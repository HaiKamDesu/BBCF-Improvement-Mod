#pragma once

// What the mod does on a BBCF.exe it was not written for (GameBuildGuard.h): every hook is off,
// so the overlay and its update popup do not exist. This runs instead, on its own thread, with
// native Windows dialogs.
//
// - A newer release whose update-manifest.json lists this game build: offered with the same
//   choices as the overlay popup (update now / later / skip this version). Updating downloads,
//   verifies and stages it through the normal pipeline, then hands over to BBCFIMUpdater.exe,
//   which closes the game, installs and restarts it through Steam.
// - No such release yet, update checks off, or Wine/Proton: one notice that the mod is off for
//   this game version, with "don't show this again for this game version".
//
// Reads CheckUpdates, EnableInDevelopmentFeatures and Language straight from settings.ini:
// the settings system belongs to the normal startup path, which did not run.

namespace Updater
{
	void RunUnsupportedBuildFlow(const char* gameBuildId);
}
