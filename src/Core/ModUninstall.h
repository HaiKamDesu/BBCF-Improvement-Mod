#pragma once

#include <string>

// Settings > Uninstall. The game has dinput8.dll loaded (and in 4 GB mode BBCF_IM_4GB.exe is the
// running exe), so the files can only go once it has closed: this extracts the updater embedded
// in the DLL to %TEMP%, starts it with --uninstall, and closes the game. The updater waits for
// both processes, removes the files, the mirrored registry values and (with one admin prompt)
// the copied firewall rules, shows what it did, and deletes its own temp folder.
// See BBCFIMUpdater.cpp, RunUninstall.
namespace ModUninstall
{
	// keepUserData: settings.ini, palettes.ini and BBCF_IM's palettes, music, recordings and
	// save backups stay, so reinstalling picks them up. Otherwise everything the mod made goes.
	// Only returns on failure (the game exits otherwise), with the reason in *error.
	bool UninstallAndExit(bool keepUserData, std::string* error);
}
