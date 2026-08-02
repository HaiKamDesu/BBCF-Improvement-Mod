#pragma once

#include "SemVersion.h"
#include "UpdateModels.h"

#include <string>

namespace Updater
{
	class GitHubReleaseClient
	{
	public:
		GitHubReleaseClient();

		UpdateCheckResult CheckLatestRelease(const SemVersion& currentVersion) const;
		UpdateCheckResult CheckForUpdates(const SemVersion& currentVersion, bool includePrereleases) const;
		// forceRefresh bypasses the releases cache (see GetReleasesJson). Use it for an explicit
		// user-driven Refresh, not for automatic checks.
		bool FetchAllReleases(std::vector<GitHubRelease>& outReleases, std::string& error, bool forceRefresh = false) const;
		bool FetchText(const std::wstring& url, std::string& outText, std::string& error) const;

	private:
		bool GetText(const std::wstring& url, std::string& outText, std::string& error) const;

		// Releases list, served from an in-memory + on-disk cache (15 minute TTL) so the startup
		// check and the All Releases window do not each spend a request from GitHub's 60/hour
		// unauthenticated budget. Falls back to a stale cache when the request fails.
		bool GetReleasesJson(std::string& outJson, std::string& error, bool forceRefresh) const;
	};
}
