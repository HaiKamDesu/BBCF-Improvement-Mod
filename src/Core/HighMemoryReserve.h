#pragma once

// The upper 2 GB of a large-address-aware BBCF, held back until the game would run out.
//
// With the large-address-aware bit, WOW64 gives BBCF 4 GB instead of 2. Some of the game's code
// was never meant to see a pointer above 2 GB (docs/Decompilation/Memory.md), and not every such
// place has been found. So the whole upper half is reserved from DllMain, before the game's
// WinMain runs: the game, the GPU driver and everything else then lay memory out exactly as on a
// stock 2 GB exe. It is released only at the moment something would otherwise fail to fit, which
// on a stock exe is a crash, so whatever breaks above 2 GB can only break in a session that would
// have ended anyway.
//
// Released by:
//   - any failed allocation the mod already catches (operator new's retry, the netplay arena and
//     file-buffer sites, D3D texture creation), through ReleaseNetplayArenaReserve;
//   - a background watcher, when the largest free block below 2 GB drops under kLowWaterBytes,
//     for the plain malloc callers that would just get a null back.
//
// Does nothing on a stock exe, on 32-bit Windows, or while LaaStress plugs the low half.

namespace HighMemoryReserve
{
	// From DllMain, after LaaStress::OnProcessAttach.
	void OnProcessAttach();

	// Once the log is open: what OnProcessAttach did. Also starts the low-water watcher.
	void LogSummary();

	// Gives the upper half back. `why` completes "released ...: <why>", e.g. "a 62 MB file buffer
	// did not fit". True only for the call that released it.
	bool Release(const char* why);

	bool IsHeld();
}
