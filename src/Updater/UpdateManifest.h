#pragma once

#include "SemVersion.h"

#include <string>
#include <vector>

namespace Updater
{
	struct UpdateManifest
	{
		std::string version;
		std::string tag;
		std::string channel = "stable";
		std::string os = "win";
		std::string arch = "x86";
		std::string assetName;
		std::string sha256;
		std::string entryDll = "dinput8.dll";
		std::string minimumSupportedVersion;
		// BBCF.exe builds the release works on, as BBCF_SUPPORTED_GAME_BUILD_ID strings. Empty
		// in every manifest published before the field existed (see ManifestSupportsGameBuild).
		std::vector<std::string> supportedGameBuilds;
		std::string gameSteamBuild;
		int schemaVersion = 1;
	};

	// Whether the release can run on the given game build. A manifest without the field predates
	// it, and every one of those releases was built against Steam build 11073075.
	bool ManifestSupportsGameBuild(const UpdateManifest& manifest, const std::string& gameBuildId);

	bool IsValidSha256Hex(const std::string& value);
	bool IsValidStableAssetName(const std::string& assetName);
	bool ValidateUpdateManifest(const UpdateManifest& manifest, std::string& error);
	bool ValidateManifestTagAndVersion(const UpdateManifest& manifest, SemVersion& parsedVersion, std::string& error);
}
