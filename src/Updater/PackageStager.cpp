#include "PackageStager.h"

#include "FileUtil.h"

#include <Windows.h>
#include <wincrypt.h>
#include <wininet.h>

// miniz, for inflate. stb_image's inflate was tried first, since the DLL already
// compiles it, but it rejects valid deflate streams: anything compressed heavily
// enough to leave a single distance code (repetitive text, long runs) comes back
// as an error, while ordinary binaries decode fine. A decoder that works or not
// depending on what the compressor happened to pick is not something an updater
// can be built on, so this uses a complete implementation instead.
// Without this miniz defines zlib-compatible aliases, one of which is a bare
// `crc32` macro that rewrites the ZipEntryRecord field of the same name.
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wininet.lib")

namespace Updater
{
	namespace
	{
		const unsigned int MaxZipEntries = 64;
		const unsigned long long MaxTotalUncompressedSize = 128ULL * 1024ULL * 1024ULL;
		const unsigned long long MaxSingleUncompressedSize = 64ULL * 1024ULL * 1024ULL;

		unsigned short ReadU16(const unsigned char* p)
		{
			return static_cast<unsigned short>(p[0] | (p[1] << 8));
		}

		unsigned int ReadU32(const unsigned char* p)
		{
			return static_cast<unsigned int>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
		}

		// The shell used to verify entry CRCs for us. Doing it ourselves is the
		// difference between "extracted" and "extracted correctly".
		unsigned int Crc32(const unsigned char* data, size_t length)
		{
			static unsigned int table[256];
			static bool built = false;
			if (!built)
			{
				for (unsigned int i = 0; i < 256; ++i)
				{
					unsigned int c = i;
					for (int bit = 0; bit < 8; ++bit)
						c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
					table[i] = c;
				}
				built = true;
			}
			unsigned int c = 0xFFFFFFFFu;
			for (size_t i = 0; i < length; ++i)
				c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
			return c ^ 0xFFFFFFFFu;
		}

		std::string ToLower(std::string value)
		{
			for (size_t i = 0; i < value.size(); ++i)
				value[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
			return value;
		}

		bool IsAllowedEntryPath(const std::string& path)
		{
			if (path.empty())
				return false;
			if (path[0] == '/' || path[0] == '\\')
				return false;
			if (path.find(':') != std::string::npos)
				return false;

			std::string normalized = path;
			std::replace(normalized.begin(), normalized.end(), '\\', '/');
			if (normalized.find("//") != std::string::npos)
				return false;

			size_t start = 0;
			while (start <= normalized.size())
			{
				const size_t slash = normalized.find('/', start);
				const std::string part = normalized.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
				if (part == "." || part == "..")
					return false;
				if (slash == std::string::npos)
					break;
				start = slash + 1;
			}

			if (!normalized.empty() && normalized[normalized.size() - 1] == '/')
				return true;

			// A release is only the DLL, the updater and the readme. The ini templates are
			// embedded in the DLL and written out by the mod itself on first run, so they no
			// longer appear in the package.
			const std::string lower = ToLower(normalized);
			return lower == "dinput8.dll" ||
				lower == "bbcfimupdater.exe" ||
				lower == "user_readme.txt";
		}

		std::wstring GetParentDirectory(const std::wstring& path)
		{
			const size_t pos = path.find_last_of(L"\\/");
			return pos == std::wstring::npos ? L"" : path.substr(0, pos);
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

			std::string result = prefix;
			result += " Windows error ";
			char code[32] = {};
			std::snprintf(code, sizeof(code), "%lu", errorCode);
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

		bool LooksLikeSecuritySoftwareBlock(DWORD errorCode)
		{
			return errorCode == ERROR_ACCESS_DENIED ||
				errorCode == ERROR_SHARING_VIOLATION ||
				errorCode == ERROR_LOCK_VIOLATION ||
				errorCode == ERROR_FILE_NOT_FOUND ||
				errorCode == ERROR_PATH_NOT_FOUND
#ifdef ERROR_VIRUS_INFECTED
				|| errorCode == ERROR_VIRUS_INFECTED
#endif
#ifdef ERROR_VIRUS_DELETED
				|| errorCode == ERROR_VIRUS_DELETED
#endif
				;
		}

		void AppendSecuritySoftwareHint(std::string& error)
		{
			error += " Antivirus or security software may be blocking the updater. Add your BlazBlue Centralfiction installation folder as an antivirus exclusion, then retry.";
		}
	}

	bool DownloadFileWithProgress(
		const std::string& url,
		const std::wstring& tempPath,
		const std::wstring& finalPath,
		const std::string& userAgent,
		volatile long* progressPercent,
		std::string& error)
	{
		error.clear();
		if (url.compare(0, 8, "https://") != 0)
		{
			error = "Download URL is not HTTPS.";
			return false;
		}

		EnsureDirectoryRecursive(GetParentDirectory(tempPath));
		EnsureDirectoryRecursive(GetParentDirectory(finalPath));
		if (!DeleteFileW(tempPath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
		{
			const DWORD errorCode = GetLastError();
			error = FormatWindowsError("Could not remove old temporary download file.", errorCode);
			if (LooksLikeSecuritySoftwareBlock(errorCode))
				AppendSecuritySoftwareHint(error);
			return false;
		}
		if (!DeleteFileW(finalPath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
		{
			const DWORD errorCode = GetLastError();
			error = FormatWindowsError("Could not remove old downloaded package.", errorCode);
			if (LooksLikeSecuritySoftwareBlock(errorCode))
				AppendSecuritySoftwareHint(error);
			return false;
		}

		HINTERNET internet = InternetOpenW(Utf8ToWide(userAgent).c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
		if (!internet)
		{
			error = FormatWindowsError("Could not initialize internet connection.", GetLastError());
			return false;
		}

		const wchar_t* headers = L"Accept: application/octet-stream\r\n";
		HINTERNET request = InternetOpenUrlW(
			internet,
			Utf8ToWide(url).c_str(),
			headers,
			static_cast<DWORD>(wcslen(headers)),
			INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE,
			0);

		if (!request)
		{
			const DWORD errorCode = GetLastError();
			InternetCloseHandle(internet);
			error = FormatWindowsError("Could not open update download URL.", errorCode);
			return false;
		}

		DWORD contentLength = 0;
		DWORD contentLengthSize = sizeof(contentLength);
		HttpQueryInfoW(request, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &contentLength, &contentLengthSize, nullptr);

		HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			const DWORD errorCode = GetLastError();
			InternetCloseHandle(request);
			InternetCloseHandle(internet);
			error = FormatWindowsError("Could not create update download file.", errorCode);
			if (LooksLikeSecuritySoftwareBlock(errorCode))
				AppendSecuritySoftwareHint(error);
			return false;
		}

		char buffer[32768];
		DWORD bytesRead = 0;
		unsigned long long totalRead = 0;
		while (true)
		{
			if (!InternetReadFile(request, buffer, sizeof(buffer), &bytesRead))
			{
				const DWORD errorCode = GetLastError();
				CloseHandle(file);
				InternetCloseHandle(request);
				InternetCloseHandle(internet);
				DeleteFileW(tempPath.c_str());
				error = FormatWindowsError("Update download was interrupted before it completed.", errorCode);
				AppendSecuritySoftwareHint(error);
				return false;
			}
			if (!bytesRead)
				break;

			DWORD bytesWritten = 0;
			if (!WriteFile(file, buffer, bytesRead, &bytesWritten, nullptr) || bytesWritten != bytesRead)
			{
				const DWORD errorCode = GetLastError();
				CloseHandle(file);
				InternetCloseHandle(request);
				InternetCloseHandle(internet);
				DeleteFileW(tempPath.c_str());
				error = FormatWindowsError("Could not write update download file.", errorCode);
				if (LooksLikeSecuritySoftwareBlock(errorCode))
					AppendSecuritySoftwareHint(error);
				return false;
			}

			totalRead += bytesRead;
			if (progressPercent && contentLength > 0)
				InterlockedExchange(const_cast<long*>(progressPercent), static_cast<long>((totalRead * 100ULL) / contentLength));
		}

		CloseHandle(file);
		InternetCloseHandle(request);
		InternetCloseHandle(internet);

		if (contentLength > 0 && totalRead != contentLength)
		{
			DeleteFileW(tempPath.c_str());
			error = "Update download did not complete. Antivirus, firewall, or proxy software may have blocked it. Add your BlazBlue Centralfiction installation folder as an antivirus exclusion, then retry.";
			return false;
		}

		if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			const DWORD errorCode = GetLastError();
			DeleteFileW(tempPath.c_str());
			error = FormatWindowsError("Could not finalize update download file.", errorCode);
			if (LooksLikeSecuritySoftwareBlock(errorCode))
				AppendSecuritySoftwareHint(error);
			return false;
		}

		if (progressPercent)
			InterlockedExchange(const_cast<long*>(progressPercent), 100);
		return true;
	}

	bool ComputeFileSha256Hex(const std::wstring& path, std::string& outSha256, std::string& error)
	{
		outSha256.clear();
		error.clear();

		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			const DWORD errorCode = GetLastError();
			error = FormatWindowsError("Could not open downloaded package for SHA-256 verification.", errorCode);
			if (LooksLikeSecuritySoftwareBlock(errorCode))
				AppendSecuritySoftwareHint(error);
			return false;
		}

		HCRYPTPROV provider = 0;
		HCRYPTHASH hash = 0;
		if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
			!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
		{
			CloseHandle(file);
			if (provider)
				CryptReleaseContext(provider, 0);
			error = "Could not initialize SHA-256.";
			return false;
		}

		BYTE buffer[32768];
		DWORD bytesRead = 0;
		while (ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead)
		{
			if (!CryptHashData(hash, buffer, bytesRead, 0))
			{
				const DWORD errorCode = GetLastError();
				CryptDestroyHash(hash);
				CryptReleaseContext(provider, 0);
				CloseHandle(file);
				error = FormatWindowsError("Could not hash downloaded package.", errorCode);
				if (LooksLikeSecuritySoftwareBlock(errorCode))
					AppendSecuritySoftwareHint(error);
				return false;
			}
		}

		BYTE digest[32];
		DWORD digestSize = sizeof(digest);
		const BOOL ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0);
		CryptDestroyHash(hash);
		CryptReleaseContext(provider, 0);
		CloseHandle(file);
		if (!ok || digestSize != sizeof(digest))
		{
			error = "Could not finish SHA-256.";
			return false;
		}

		char hex[65] = {};
		for (DWORD i = 0; i < digestSize; ++i)
			std::sprintf(hex + (i * 2), "%02x", digest[i]);
		outSha256 = hex;
		return true;
	}

	ZipValidationResult ValidateUpdateZip(const std::wstring& zipPath)
	{
		ZipValidationResult result;
		std::string bytes;
		if (!ReadBinaryFile(zipPath, bytes))
		{
			result.error = "Could not read zip file.";
			return result;
		}

		if (bytes.size() < 22)
		{
			result.error = "Zip file too small.";
			return result;
		}

		const unsigned char* data = reinterpret_cast<const unsigned char*>(bytes.data());
		size_t eocd = std::string::npos;
		const size_t minSearch = bytes.size() > 66000 ? bytes.size() - 66000 : 0;
		for (size_t i = bytes.size() - 22; i + 4 <= bytes.size() && i >= minSearch; --i)
		{
			if (ReadU32(data + i) == 0x06054b50)
			{
				eocd = i;
				break;
			}
			if (i == 0)
				break;
		}

		if (eocd == std::string::npos)
		{
			result.error = "Zip end-of-central-directory not found.";
			return result;
		}

		const unsigned int entryCount = ReadU16(data + eocd + 10);
		const unsigned int cdSize = ReadU32(data + eocd + 12);
		const unsigned int cdOffset = ReadU32(data + eocd + 16);
		if (entryCount == 0 || entryCount > MaxZipEntries || cdOffset + cdSize > bytes.size())
		{
			result.error = "Zip central directory bounds are invalid.";
			return result;
		}

		size_t pos = cdOffset;
		std::set<std::string> seen;
		for (unsigned int i = 0; i < entryCount; ++i)
		{
			if (pos + 46 > bytes.size() || ReadU32(data + pos) != 0x02014b50)
			{
				result.error = "Zip central directory entry is invalid.";
				return result;
			}

			const unsigned short method = ReadU16(data + pos + 10);
			const unsigned int entryCrc = ReadU32(data + pos + 16);
			const unsigned int compressedSize = ReadU32(data + pos + 20);
			const unsigned int uncompressedSize = ReadU32(data + pos + 24);
			const unsigned short nameLen = ReadU16(data + pos + 28);
			const unsigned short extraLen = ReadU16(data + pos + 30);
			const unsigned short commentLen = ReadU16(data + pos + 32);
			const unsigned int externalAttrs = ReadU32(data + pos + 38);
			const unsigned int localOffset = ReadU32(data + pos + 42);
			if (pos + 46 + nameLen + extraLen + commentLen > bytes.size() || localOffset >= bytes.size())
			{
				result.error = "Zip entry bounds are invalid.";
				return result;
			}

			const std::string name(reinterpret_cast<const char*>(data + pos + 46), nameLen);
			const bool isDirectory = !name.empty() && (name[name.size() - 1] == '/' || name[name.size() - 1] == '\\');
			const unsigned int unixMode = (externalAttrs >> 16) & 0170000;
			if (unixMode == 0120000)
			{
				result.error = "Zip symlink entries are not allowed.";
				return result;
			}

			if (method != 0 && method != 8)
			{
				result.error = "Zip entry uses unsupported compression method.";
				return result;
			}

			// Zip64 puts the real sizes in an extra field and leaves these saturated.
			// The package is capped far below 4 GB, so treat it as malformed rather
			// than growing a second size path that would never be exercised.
			if (compressedSize == 0xFFFFFFFFu || uncompressedSize == 0xFFFFFFFFu ||
				localOffset == 0xFFFFFFFFu)
			{
				result.error = "Zip64 entries are not supported.";
				return result;
			}

			if (!IsAllowedEntryPath(name))
			{
				result.error = "Zip entry path is not allowed: " + name;
				return result;
			}

			if (!isDirectory)
			{
				if (compressedSize > MaxSingleUncompressedSize || uncompressedSize > MaxSingleUncompressedSize)
				{
					result.error = "Zip entry is too large.";
					return result;
				}
				result.totalUncompressedSize += uncompressedSize;
				if (result.totalUncompressedSize > MaxTotalUncompressedSize)
				{
					result.error = "Zip total uncompressed size is too large.";
					return result;
				}
				if (!seen.insert(ToLower(name)).second)
				{
					result.error = "Zip contains duplicate entry path.";
					return result;
				}
			}

			ZipEntryRecord record;
			record.name = name;
			record.isDirectory = isDirectory;
			record.method = method;
			record.compressedSize = compressedSize;
			record.uncompressedSize = uncompressedSize;
			record.crc32 = entryCrc;
			record.localHeaderOffset = localOffset;
			result.records.push_back(record);

			result.entries.push_back(name);
			pos += 46 + nameLen + extraLen + commentLen;
		}

		result.valid = true;
		return result;
	}

	namespace
	{
		// Turns "a/b/c.dll" into an absolute path under `destination`, creating the
		// directories on the way. The name has already passed IsAllowedEntryPath, so it
		// cannot climb out with ".." or an absolute prefix.
		std::wstring ResolveEntryPath(const std::wstring& destination, const std::string& name)
		{
			std::wstring relative = Utf8ToWide(name);
			std::replace(relative.begin(), relative.end(), L'/', L'\\');
			return CombinePath(destination, relative);
		}

		bool WriteEntryFile(const std::wstring& path, const unsigned char* data, size_t length, std::string& error)
		{
			const std::wstring parent = GetParentDirectory(path);
			if (!parent.empty())
				EnsureDirectoryRecursive(parent);

			const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				error = FormatWindowsError("Could not create staged file.", GetLastError());
				AppendSecuritySoftwareHint(error);
				return false;
			}

			size_t written = 0;
			while (written < length)
			{
				const DWORD chunk = static_cast<DWORD>(
					(length - written) > 0x100000u ? 0x100000u : (length - written));
				DWORD wroteNow = 0;
				if (!WriteFile(file, data + written, chunk, &wroteNow, nullptr) || wroteNow == 0)
				{
					const DWORD errorCode = GetLastError();
					CloseHandle(file);
					error = FormatWindowsError("Could not write staged file.", errorCode);
					AppendSecuritySoftwareHint(error);
					return false;
				}
				written += wroteNow;
			}

			CloseHandle(file);
			return true;
		}
	}

	bool ExtractUpdateZip(
		const std::wstring& zipPath,
		const ZipValidationResult& validated,
		const std::wstring& destination,
		std::string& error)
	{
		error.clear();

		// Extracting an archive nobody vetted would hand an attacker the path checks for
		// free, so refuse rather than fall back to parsing it here.
		if (!validated.valid)
		{
			error = "Refusing to extract a zip that failed validation.";
			return false;
		}

		std::string bytes;
		if (!ReadBinaryFile(zipPath, bytes))
		{
			error = "Could not read the downloaded package.";
			return false;
		}

		if (!EnsureDirectoryRecursive(destination))
		{
			error = FormatWindowsError("Could not create the staging folder.", GetLastError());
			AppendSecuritySoftwareHint(error);
			return false;
		}

		const unsigned char* data = reinterpret_cast<const unsigned char*>(bytes.data());
		const size_t total = bytes.size();
		std::vector<unsigned char> output;
		unsigned int filesWritten = 0;

		for (size_t i = 0; i < validated.records.size(); ++i)
		{
			const ZipEntryRecord& entry = validated.records[i];
			const std::wstring outPath = ResolveEntryPath(destination, entry.name);

			if (entry.isDirectory)
			{
				EnsureDirectoryRecursive(outPath);
				continue;
			}

			// The local header repeats the name and extra fields, and its lengths are
			// allowed to differ from the central directory's. The payload starts after
			// whatever the LOCAL header says, so it has to be read here rather than
			// assumed from the entry we already parsed.
			const size_t local = entry.localHeaderOffset;
			if (local + 30 > total || ReadU32(data + local) != 0x04034b50)
			{
				error = "Zip local header is invalid for entry: " + entry.name;
				return false;
			}

			const unsigned short localNameLen = ReadU16(data + local + 26);
			const unsigned short localExtraLen = ReadU16(data + local + 28);
			const size_t payload = local + 30 + localNameLen + localExtraLen;
			if (payload + entry.compressedSize > total)
			{
				error = "Zip entry data runs past the end of the file: " + entry.name;
				return false;
			}

			const unsigned char* compressed = data + payload;

			if (entry.method == 0)
			{
				if (entry.compressedSize != entry.uncompressedSize)
				{
					error = "Stored zip entry has mismatched sizes: " + entry.name;
					return false;
				}
				output.assign(compressed, compressed + entry.uncompressedSize);
			}
			else
			{
				// Zip stores raw deflate with no zlib header, so the header flag is off.
				output.assign(entry.uncompressedSize, 0);
				const size_t produced = tinfl_decompress_mem_to_mem(
					output.empty() ? nullptr : &output[0],
					entry.uncompressedSize,
					compressed,
					entry.compressedSize,
					0);
				if (produced == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ||
					produced != entry.uncompressedSize)
				{
					error = "Could not decompress zip entry: " + entry.name;
					return false;
				}
			}

			// The shell verified entry CRCs on our behalf. Now that we inflate the bytes
			// ourselves, dropping this would let a corrupt download install silently.
			const unsigned int actual = Crc32(output.empty() ? nullptr : &output[0], output.size());
			if (actual != entry.crc32)
			{
				error = "Zip entry failed its checksum: " + entry.name;
				return false;
			}

			if (!WriteEntryFile(outPath, output.empty() ? nullptr : &output[0], output.size(), error))
				return false;

			++filesWritten;
		}

		if (filesWritten == 0)
		{
			error = "The update package contained no files to install.";
			return false;
		}

		return true;
	}

	bool WriteUpdaterHandoff(
		const AvailableUpdate& update,
		const std::wstring& stagedRoot,
		const std::wstring& packagePath,
		std::wstring& outHandoffPath,
		std::string& error)
	{
		error.clear();
		const std::wstring installRoot = GetInstallRoot();
		const std::wstring updaterRoot = CombinePath(installRoot, L"BBCF_IM\\Updater");
		const std::wstring handoffRoot = CombinePath(updaterRoot, L"handoff");
		EnsureDirectoryRecursive(handoffRoot);
		outHandoffPath = CombinePath(handoffRoot, Utf8ToWide(update.release.tagName + "-" + GetUtcTimestampForFileName() + ".ini"));

		const DWORD pid = GetCurrentProcessId();
		std::string content;
		content += "[Update]\n";
		content += "InstallRoot=" + WideToUtf8(installRoot) + "\n";
		content += "StagedRoot=" + WideToUtf8(stagedRoot) + "\n";
		content += "PackagePath=" + WideToUtf8(packagePath) + "\n";
		content += "BackupRoot=" + WideToUtf8(CombinePath(updaterRoot, L"backups")) + "\n";
		content += "LogPath=" + WideToUtf8(CombinePath(updaterRoot, L"logs\\updater.log")) + "\n";
		content += "ParentPid=" + std::to_string(pid) + "\n";
		content += "Tag=" + update.release.tagName + "\n";
		content += "Version=" + update.manifest.version + "\n";
		content += "EntryDll=" + update.manifest.entryDll + "\n";
		content += "SteamAppId=586140\n";
		content += "BbcfExePath=" + WideToUtf8(GetBbcfExePath()) + "\n";
		content += "Relaunch=1\n";

		if (!WriteTextFile(outHandoffPath, content))
		{
			error = "Could not write updater handoff file.";
			return false;
		}
		return true;
	}

	bool LaunchUpdaterAndExitGame(const std::wstring& handoffPath, std::string& error)
	{
		error.clear();
		std::wstring updaterPath = CombinePath(GetInstallRoot(), L"BBCF_IM\\Updater\\stage\\BBCFIMUpdater.exe");
		if (!FileExists(updaterPath))
			updaterPath = CombinePath(GetInstallRoot(), L"BBCFIMUpdater.exe");
		if (!FileExists(updaterPath))
		{
			error = "Staged updater executable was not found.";
			return false;
		}

		std::wstring command = L"\"" + updaterPath + L"\" --handoff \"" + handoffPath + L"\"";
		STARTUPINFOW si = {};
		PROCESS_INFORMATION pi = {};
		si.cb = sizeof(si);
		if (!CreateProcessW(updaterPath.c_str(), &command[0], nullptr, nullptr, FALSE, 0, nullptr, GetInstallRoot().c_str(), &si, &pi))
		{
			error = "Could not launch updater executable.";
			return false;
		}

		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		ExitProcess(0);
		return true;
	}
}
