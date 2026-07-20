#pragma once

// Diagnostics for the spectator desync bug. See
// docs/Research/SpectatorDesyncInvestigation.md for the full history, including why the
// "repeat last input", "force stall", and "freeze" attempts were all retired (the last
// one broke match-end: freezing prevents vanilla's advance-with-zero from playing the
// ending before the 5s connection timeout, so the spectator got "connection lost").
//
// This build is INSTRUMENTATION-ONLY: it does not change spectator behavior at all
// (vanilla decides every advance). On each frame where SyncInput reports starvation it
// logs the session mode, the battle scene-state triple the vanilla gate (FUN_0055EDB0)
// keys off, the input cursor vs newest-received frame, and whether vanilla is about to
// advance-the-fight-with-a-zeroed-input (the suspected desync mechanism) -- so a real
// desync can be correlated against these signals when it next occurs in the wild, and so
// the on-demand injection can be used to test the advance-with-zero theory directly.

// Set from naked asm at SpectatorSyncInputEntry; the live spectator backend pointer.
extern "C" unsigned long g_spectatorBackendPtrRaw;

// TEST ONLY: while > 0, SpectatorSyncInputEntry forces SyncInput to report starvation
// (return 4) even though the real input is present, so the vanilla starvation path
// (stall in normal scene states, advance-with-zero in transition states) can be
// exercised on demand to observe whether it visibly desyncs.
extern "C" unsigned long g_spectatorSyncInjectFramesRemaining;

// Called from the caller-side gate hook on every starved frame. Instrumentation only:
// logs and ALWAYS returns 0 (let vanilla decide the advance). Never freezes.
extern "C" int __cdecl SpectatorSyncOnStarvationThunk();

namespace SpectatorSyncDiagnostics
{
	struct Status
	{
		unsigned long backendPtr;
		int starvationCount;      // frames SyncInput reported starvation
		int zeroInputAdvances;    // of those, how many vanilla will advance-with-zero (desync-prone)
		int stalls;               // of those, how many vanilla will stall (safe wait)
		int maxLagSeen;           // peak buffered-input backlog (toward the 256-ring limit)
		int injectRemaining;
		int nextInputToSend;      // -1 if no backend captured
		int maxReceivedFrame;     // -1 if no backend captured
		int sceneA, sceneB, sceneC; // last-seen gate scene-state triple (-999 if unknown)
	};
	Status GetStatus();
	void InjectDesync(int frames);
}
