#pragma once

#include <string>

// Runs the game as a 4 GB (large-address-aware) process without touching BBCF.exe.
//
// BBCF.exe stays exactly as Steam installed it. Once the player accepts, the mod keeps a copy of
// it next to it, BBCF_IM_4GB.exe, identical except for the large-address-aware flag. Every time
// the mod loads into the stock BBCF.exe it starts that copy instead - same command line, same
// folder, Steam's environment inherited - and the stock process just waits for it and exits with
// its exit code, so Steam still sees the game it launched running until the player quits.
//
// Why a copy and not a patched BBCF.exe: an exe with the flag crashes at boot without this mod's
// fixes (D3DX effects, LargeAddressAware.h). With the copy, deleting the mod, or going back to a
// version that does not know about it, simply runs the stock BBCF.exe again. Steam's "verify
// files" never touches the copy either.
//
// Fallbacks, all ending in the stock 2 GB game with the mod: no consent, 32-bit Windows, the copy
// cannot be written or started, or it dies within its first seconds (then redirecting turns itself
// off and the stock exe is started in its place). Everything the stock process decides goes to
// BBCF_IM\LargeAddressSpace.log, since DEBUG.txt belongs to the game process.

namespace LaaRedirect
{
	// From DllMain, once GameBuildGuard accepted the build. True when this process is the stock
	// BBCF.exe handing the game to the copy: DllMain must then skip the rest of the mod's init,
	// and the game's WinMain only waits for the copy.
	bool OnProcessAttach();

	// True in the process that only waits for the copy (OnProcessAttach returned true).
	bool IsRedirectingProcess();

	// Once the log is open: how this process came to be running.
	void LogSummary();

	// The consent prompt: a supported stock exe on 64-bit Windows, LargeAddressSpace still -1.
	bool ShouldAsk();

	// Accept: write the copy, save LargeAddressSpace=1, and close the game so the updater can
	// start it again through Steam. Does not return on success.
	bool AcceptAndRestart(std::string& error);
}
