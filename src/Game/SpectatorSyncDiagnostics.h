#pragma once

// Diagnostic instrumentation for the spectator desync bug. See
// docs/Research/SpectatorDesyncInvestigation.md for the RE writeup this is
// built from.
//
// Theory under test: at the in-match SynchronizeInput call site (VA 0x4E60D5),
// a failed sync normally stalls the frame, but helper FUN_0055EDB0 lets the
// sim ADVANCE WITH ZERO INPUTS in spectator mode (session mode 3) during
// certain scene-state windows. The spectator's GGPO input cursor
// (_next_input_to_send) does not move on failure, so each such frame
// permanently shifts input consumption and the re-simulated match diverges.
//
// Two hooks feed this module (installed in hooks_bbcf.cpp):
//  - SpectatorSyncInputEntry (spectator SyncInput, VA 0x77E340): captures the
//    SteamSpectatorBackend pointer into g_spectatorBackendPtrRaw.
//  - SpectatorSyncInputError (error branch at VA 0x4E60F1): calls
//    SpectatorSyncErrorThunk() on every SynchronizeInput failure. The thunk
//    logs the decision inputs and returns nonzero to force the stall path
//    (the candidate fix, gated on Settings::settingsIni.spectatorSyncFailStall).

// Set from naked asm at spectator SyncInput entry; the live backend pointer.
extern "C" unsigned long g_spectatorBackendPtrRaw;

// TEST ONLY fault injection: while > 0, the SpectatorSyncInputEntry hook makes
// spectator SyncInput report starvation (error 4) without touching the input
// queue, and the error thunk forces the game down the zero-input-advance path
// (bypassing the scene-state gate we can't summon on demand) -- reproducing
// the desync mechanism deliberately. With spectatorSyncFailStall enabled the
// same injection must instead resolve into harmless stalls; that contrast is
// the validation that the fix is production-ready.
extern "C" unsigned long g_spectatorSyncInjectFramesRemaining;

// Return contract with the naked SpectatorSyncInputError hook:
//   0 = let the game's own gate decide (log-only)
//   1 = force the stall path (the fix)
//   2 = force the zero-input advance path (fault injection, fix disabled)
extern "C" int __cdecl SpectatorSyncErrorThunk();

namespace SpectatorSyncDiagnostics
{
	struct Status
	{
		unsigned long backendPtr;
		int spectatorErrors;
		int zeroInputAdvances;
		int forcedStalls;
		int injectRemaining;
		int nextInputToSend;   // -1 if no backend captured
		int maxReceivedFrame;  // -1 if no backend captured
	};
	Status GetStatus();
	void InjectDesync(int frames);
}
