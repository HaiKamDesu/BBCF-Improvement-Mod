#pragma once

// The mod is written against one exact BBCF.exe (SupportedGameBuild.h). Thousands of addresses
// in src/ are only true for that file, so on any other build every patch we write lands in
// unrelated code.
//
// DllMain asks this first. On an unknown build the DLL becomes a plain dinput8 forwarder: no
// hooks, no crash handlers, no save-data snapshots, nothing written into the game. The only
// thing that still runs is UnsupportedBuildFlow, on its own thread: it tells the player, and
// offers a release that supports this build once one is published.
//
// Set the environment variable BBCF_IM_ALLOW_UNKNOWN_GAME_BUILD=1 to skip the check (porting
// work, or the LoadLibrary test host, whose main module is not BBCF.exe).

namespace GameBuildGuard
{
	// Reads the main module's PE header. Safe under the loader lock; call it from DllMain.
	void Evaluate();

	// True when the running exe is the supported build, or the override is set.
	bool IsSupported();

	// The running exe's ID in the format of BBCF_SUPPORTED_GAME_BUILD_ID. Valid after Evaluate().
	const char* CurrentBuildId();

	// From DllMain, on an unsupported build: starts UnsupportedBuildFlow on its own thread.
	void StartUnsupportedBuildFlow();

	// From DirectInput8Create: logs once that the override let an unknown build through.
	void LogOverrideOnce();
}
