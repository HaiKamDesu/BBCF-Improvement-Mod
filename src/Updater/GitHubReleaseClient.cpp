#include "GitHubReleaseClient.h"

#include "Core/info.h"
#include "JsonValue.h"

#include <Windows.h>
#include <wininet.h>
#include <algorithm>
#include <cwchar>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

#pragma comment(lib, "wininet.lib")

namespace Updater
{
	namespace
	{
		std::wstring ToWideAscii(const std::string& value)
		{
			return std::wstring(value.begin(), value.end());
		}

		std::wstring BuildUserAgent()
		{
			std::string version = MOD_VERSION;
			return ToWideAscii("BBCFIM/" + version);
		}

		std::string FormatWindowsError(const char* prefix, DWORD errorCode)
		{
			char message[512] = {};
			FormatMessageA(
				FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				nullptr,
				errorCode,
				0,
				message,
				sizeof(message),
				nullptr);

			char code[32] = {};
			std::snprintf(code, sizeof(code), "%lu", errorCode);
			std::string result = prefix;
			result += " Windows error ";
			result += code;
			if (message[0])
			{
				result += ": ";
				result += message;
				while (!result.empty() && (result[result.size() - 1] == '\r' || result[result.size() - 1] == '\n' || result[result.size() - 1] == '.'))
					result.erase(result.size() - 1);
			}
			result += ".";
			return result;
		}

		DWORD QueryStatusCode(HINTERNET request)
		{
			DWORD statusCode = 0;
			DWORD length = sizeof(statusCode);
			if (!HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &length, nullptr))
				return 0;
			return statusCode;
		}

		// Reads a named response header. Returns false when absent.
		bool QueryHeader(HINTERNET request, const wchar_t* name, std::wstring& outValue)
		{
			wchar_t buffer[256] = {};
			std::wcsncpy(buffer, name, (sizeof(buffer) / sizeof(buffer[0])) - 1);
			DWORD length = sizeof(buffer);
			if (!HttpQueryInfoW(request, HTTP_QUERY_CUSTOM, buffer, &length, nullptr))
				return false;
			outValue.assign(buffer);
			return true;
		}

		// GitHub reports the limit reset as a unix timestamp in X-RateLimit-Reset.
		std::string DescribeRateLimitReset(HINTERNET request)
		{
			std::wstring resetValue;
			if (!QueryHeader(request, L"X-RateLimit-Reset", resetValue) || resetValue.empty())
				return std::string();

			const long long resetEpoch = _wtoi64(resetValue.c_str());
			if (resetEpoch <= 0)
				return std::string();

			const long long remainingSeconds = resetEpoch - static_cast<long long>(std::time(nullptr));
			if (remainingSeconds <= 0)
				return " The limit should have reset already - try again.";

			char text[128] = {};
			const long long minutes = (remainingSeconds + 59) / 60;
			std::snprintf(text, sizeof(text), " It resets in about %lld minute%s.",
				minutes, minutes == 1 ? "" : "s");
			return text;
		}

		// GitHub returns errors as a JSON object with a human-readable "message".
		std::string ExtractApiMessage(const std::string& body)
		{
			if (body.empty())
				return std::string();

			JsonValue root;
			std::string parseError;
			if (!ParseJson(body, root, parseError) || !root.IsObject())
				return std::string();

			const JsonValue* message = root.Find("message");
			if (!message || !message->IsString())
				return std::string();
			return message->AsString();
		}

		// Turns a failing HTTP response into something that names the actual problem. Without
		// this, a 403 body ({"message":"API rate limit exceeded..."}) reached the release parser
		// and surfaced as "GitHub releases JSON root is not an array", which reads like a bug in
		// our code rather than a server telling us to back off.
		std::string DescribeHttpFailure(HINTERNET request, DWORD statusCode, const std::string& body)
		{
			const std::string apiMessage = ExtractApiMessage(body);

			std::wstring remaining;
			const bool hasRemaining = QueryHeader(request, L"X-RateLimit-Remaining", remaining);
			const bool rateLimited = (statusCode == 403 || statusCode == 429) &&
				(hasRemaining && remaining == L"0");

			std::string result;
			if (rateLimited)
			{
				result = "GitHub is rate limiting this connection (60 requests per hour for "
					"unauthenticated users, shared by everyone on your IP).";
				result += DescribeRateLimitReset(request);
			}
			else
			{
				char code[64] = {};
				std::snprintf(code, sizeof(code), "GitHub request failed with HTTP %lu.", statusCode);
				result = code;
			}

			if (!apiMessage.empty())
			{
				result += " GitHub said: ";
				result += apiMessage;
			}
			return result;
		}

		// ---------------------------------------------------------------------------------
		// Releases-list cache.
		//
		// Unauthenticated GitHub allows 60 requests/hour per IP. Every launch used to spend two
		// on the same URL - once for the startup update check, once when the All Releases window
		// first opened - so a testing session burned through the budget and then reported
		// confusing parse errors. Responses are cached in memory for the process and on disk
		// across launches; the Refresh button bypasses both.
		// ---------------------------------------------------------------------------------

		const long long kReleasesCacheTtlSeconds = 15 * 60;
		const wchar_t* const kReleasesCachePath = L"BBCF_IM\\Updater\\releases-cache.txt";

		std::mutex& CacheMutex()
		{
			static std::mutex mutex;
			return mutex;
		}

		struct CacheEntry
		{
			std::wstring url;
			std::string body;
			long long fetchedAt = 0;
		};

		CacheEntry& MemoryCache()
		{
			static CacheEntry entry;
			return entry;
		}

		bool EnsureParentDirectory(const std::wstring& path)
		{
			const size_t slash = path.find_last_of(L"\\/");
			if (slash == std::wstring::npos)
				return true;

			const std::wstring directory = path.substr(0, slash);
			if (directory.empty())
				return true;
			if (GetFileAttributesW(directory.c_str()) != INVALID_FILE_ATTRIBUTES)
				return true;
			if (!EnsureParentDirectory(directory))
				return false;
			return CreateDirectoryW(directory.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
		}

		// On-disk layout: first line is the fetch time (unix seconds), second is the URL, the
		// rest is the verbatim response body. Avoids escaping JSON inside JSON.
		bool ReadDiskCache(CacheEntry& outEntry)
		{
			std::ifstream file(kReleasesCachePath, std::ios::binary);
			if (!file.is_open())
				return false;

			std::string timestampLine;
			std::string urlLine;
			if (!std::getline(file, timestampLine) || !std::getline(file, urlLine))
				return false;

			std::ostringstream body;
			body << file.rdbuf();

			outEntry.fetchedAt = _atoi64(timestampLine.c_str());
			outEntry.url.assign(urlLine.begin(), urlLine.end());
			outEntry.body = body.str();
			return outEntry.fetchedAt > 0 && !outEntry.body.empty();
		}

		void WriteDiskCache(const CacheEntry& entry)
		{
			if (!EnsureParentDirectory(kReleasesCachePath))
				return;

			std::ofstream file(kReleasesCachePath, std::ios::binary | std::ios::trunc);
			if (!file.is_open())
				return;

			const std::string url(entry.url.begin(), entry.url.end());
			file << entry.fetchedAt << "\n" << url << "\n" << entry.body;
		}

		bool IsFresh(const CacheEntry& entry, const std::wstring& url)
		{
			if (entry.fetchedAt <= 0 || entry.body.empty() || entry.url != url)
				return false;
			const long long age = static_cast<long long>(std::time(nullptr)) - entry.fetchedAt;
			return age >= 0 && age < kReleasesCacheTtlSeconds;
		}
	}

	GitHubReleaseClient::GitHubReleaseClient()
	{
	}

	UpdateCheckResult GitHubReleaseClient::CheckLatestRelease(const SemVersion& currentVersion) const
	{
		return CheckForUpdates(currentVersion, false);
	}

	UpdateCheckResult GitHubReleaseClient::CheckForUpdates(const SemVersion& currentVersion, bool includePrereleases) const
	{
		UpdateCheckResult result;

		std::string releasesJson;
		std::string error;
		if (!GetReleasesJson(releasesJson, error, false))
		{
			result.status = UpdateCheckStatus_NetworkError;
			result.message = error;
			return result;
		}

		std::vector<GitHubRelease> releases;
		if (!ParseGitHubReleasesJson(releasesJson, releases, error))
		{
			result.status = UpdateCheckStatus_ParseError;
			result.message = error;
			return result;
		}

		std::vector<GitHubRelease> newerReleases;
		for (size_t i = 0; i < releases.size(); ++i)
		{
			if (releases[i].draft)
				continue;
			if (releases[i].prerelease && !includePrereleases)
				continue;

			SemVersion version;
			if (!TryParseSemVersion(releases[i].tagName, version))
				continue;
			if (CompareSemVersion(version, currentVersion) > 0)
				newerReleases.push_back(releases[i]);
		}

		std::sort(newerReleases.begin(), newerReleases.end(),
			[](const GitHubRelease& lhs, const GitHubRelease& rhs)
			{
				SemVersion left;
				SemVersion right;
				TryParseSemVersion(lhs.tagName, left);
				TryParseSemVersion(rhs.tagName, right);
				return CompareSemVersion(left, right) > 0;
			});

		if (newerReleases.empty())
		{
			result.status = UpdateCheckStatus_NoUpdate;
			result.message = "No newer release in selected update channel.";
			return result;
		}

		const GitHubRelease& release = newerReleases.front();

		const GitHubReleaseAsset* manifestAsset = nullptr;
		for (size_t i = 0; i < release.assets.size(); ++i)
		{
			if (release.assets[i].name == "update-manifest.json")
			{
				manifestAsset = &release.assets[i];
				break;
			}
		}

		if (!manifestAsset)
		{
			result.status = UpdateCheckStatus_InvalidRelease;
			result.message = "Release is missing update-manifest.json.";
			return result;
		}

		if (manifestAsset->browserDownloadUrl.compare(0, 8, "https://") != 0)
		{
			result.status = UpdateCheckStatus_InvalidRelease;
			result.message = "Manifest asset download URL is not HTTPS.";
			return result;
		}

		std::string manifestJson;
		if (!GetText(ToWideAscii(manifestAsset->browserDownloadUrl), manifestJson, error))
		{
			result.status = UpdateCheckStatus_NetworkError;
			result.message = error;
			return result;
		}

		EvaluateGitHubReleaseForUpdate(release, manifestJson, currentVersion, includePrereleases, result);
		if (result.status == UpdateCheckStatus_UpdateAvailable)
			result.update.releaseNotes = newerReleases;
		return result;
	}

	bool GitHubReleaseClient::FetchAllReleases(std::vector<GitHubRelease>& outReleases, std::string& error, bool forceRefresh) const
	{
		std::string json;
		if (!GetReleasesJson(json, error, forceRefresh))
			return false;
		return ParseGitHubReleasesJson(json, outReleases, error);
	}

	// Shared by the startup update check and the All Releases window, which both want the same
	// URL. Without the cache that is two requests per launch against a 60/hour budget.
	bool GitHubReleaseClient::GetReleasesJson(std::string& outJson, std::string& error, bool forceRefresh) const
	{
		const std::wstring url = GetGitHubReleasesApiUrl();

		if (!forceRefresh)
		{
			std::lock_guard<std::mutex> lock(CacheMutex());
			if (IsFresh(MemoryCache(), url))
			{
				outJson = MemoryCache().body;
				return true;
			}

			CacheEntry disk;
			if (ReadDiskCache(disk) && IsFresh(disk, url))
			{
				MemoryCache() = disk;
				outJson = disk.body;
				return true;
			}
		}

		if (!GetText(url, outJson, error))
		{
			// A stale cache beats an empty list when the network is down or we are rate limited.
			// Not for an explicit Refresh though - the user asked for fresh data, so silently
			// handing back the same stale list would look like the button did nothing.
			if (forceRefresh)
				return false;

			std::lock_guard<std::mutex> lock(CacheMutex());
			CacheEntry fallback = MemoryCache();
			if (fallback.body.empty() || fallback.url != url)
			{
				CacheEntry disk;
				if (ReadDiskCache(disk) && disk.url == url)
					fallback = disk;
			}
			if (!fallback.body.empty() && fallback.url == url)
			{
				outJson = fallback.body;
				error.clear();
				return true;
			}
			return false;
		}

		CacheEntry entry;
		entry.url = url;
		entry.body = outJson;
		entry.fetchedAt = static_cast<long long>(std::time(nullptr));
		{
			std::lock_guard<std::mutex> lock(CacheMutex());
			MemoryCache() = entry;
		}
		WriteDiskCache(entry);
		return true;
	}

	bool GitHubReleaseClient::FetchText(const std::wstring& url, std::string& outText, std::string& error) const
	{
		return GetText(url, outText, error);
	}

	bool GitHubReleaseClient::GetText(const std::wstring& url, std::string& outText, std::string& error) const
	{
		outText.clear();
		error.clear();

		const std::wstring userAgent = BuildUserAgent();
		HINTERNET internet = InternetOpenW(userAgent.c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
		if (!internet)
		{
			error = FormatWindowsError("Could not initialize internet connection.", GetLastError());
			return false;
		}

		const wchar_t* headers =
			L"Accept: application/vnd.github+json\r\n";

		HINTERNET request = InternetOpenUrlW(
			internet,
			url.c_str(),
			headers,
			static_cast<DWORD>(wcslen(headers)),
			INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE,
			0);

		if (!request)
		{
			const DWORD errorCode = GetLastError();
			InternetCloseHandle(internet);
			error = FormatWindowsError("Could not open GitHub update URL.", errorCode);
			return false;
		}

		char buffer[4096];
		DWORD bytesRead = 0;
		while (true)
		{
			if (!InternetReadFile(request, buffer, sizeof(buffer), &bytesRead))
			{
				const DWORD errorCode = GetLastError();
				InternetCloseHandle(request);
				InternetCloseHandle(internet);
				error = FormatWindowsError("GitHub update request was interrupted.", errorCode);
				return false;
			}
			if (!bytesRead)
				break;

			outText.append(buffer, bytesRead);
		}

		// Check the status before handing the body to a JSON parser that expects success shapes.
		const DWORD statusCode = QueryStatusCode(request);
		if (statusCode >= 400)
		{
			error = DescribeHttpFailure(request, statusCode, outText);
			outText.clear();
			InternetCloseHandle(request);
			InternetCloseHandle(internet);
			return false;
		}

		InternetCloseHandle(request);
		InternetCloseHandle(internet);

		if (outText.empty())
		{
			error = "HTTP response body was empty.";
			return false;
		}

		return true;
	}
}
