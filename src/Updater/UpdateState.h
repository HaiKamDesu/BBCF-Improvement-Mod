#pragma once

#include <string>

namespace Updater
{
	struct UpdateState
	{
		std::string skippedReleaseTag;
		std::string skippedReleaseVersion;
		std::string lastSeenReleaseTag;
		std::string lastSeenReleaseVersion;
		std::string lastCheckUtc;
		std::string lastFailureUtc;
		// Game build ID whose "the mod is off for this game version" notice the player turned
		// off. Only that notice; an update that supports the build is still offered.
		std::string dismissedUnsupportedGameBuild;
	};
}
