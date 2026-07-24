#pragma once

// Fix for the spectator desync bug. See docs/Research/SpectatorDesyncInvestigation.md.
//
// ROOT CAUSE (confirmed from a real captured desync, Cuack rachel-vs-kokonoe 2026-07-23):
// during a transition scene state, when the next input hasn't arrived over the network
// yet, vanilla advances the fight with a ZEROED input instead of waiting -- inserting a
// phantom frame the real match never had. A brief network stall (observed: 18 frames /
// ~0.28s) during such a state inserts many phantom frames and permanently desyncs the
// spectator's deterministic re-simulation. Benign sessions had 0-1 of these; the real
// desync had 45, including an 18-in-a-row burst. The stalls are TEMPORARY (the input
// arrives ~0.3s later), which is what makes freezing viable.
//
// THE FIX: only in those advance-with-zero states, FREEZE the fight (do not advance)
// while the input is missing, so it arrives with nothing inserted -> zero phantom frames
// -> no desync. Each frozen frame we pump the network drain (DoPoll) so the event queue
// can't overflow (the first freeze attempt crashed for lack of this) and the awaited
// input can arrive. Bounded to kMaxAbsorbFrames (well under the game's 5000ms spectator
// disconnect timeout) so a permanent stall (match end / dead connection) resumes vanilla
// -- which needs advance-with-zero to play the ending and reach the victory screen before
// the disconnect fires (a longer cap re-broke match-end; this short one does not).
// Normal (non-transition) starvation is left entirely to vanilla, which already waits
// correctly there.

// Set from naked asm at SpectatorSyncInputEntry; the live spectator backend pointer.
extern "C" unsigned long g_spectatorBackendPtrRaw;

// TEST ONLY: while > 0, SpectatorSyncInputEntry forces SyncInput to report starvation
// AND flags the frame as injected, so the thunk treats it as the desync-prone
// advance-with-zero situation on demand (regardless of the real scene state). This lets
// the fix be A/B tested without having to time an injection into a real transition.
extern "C" unsigned long g_spectatorSyncInjectFramesRemaining;
extern "C" unsigned long g_spectatorInjectingThisFrame;  // set by entry hook, read by thunk
// When set, an injected frame BYPASSES the fix and forces the raw phantom-frame advance
// (reproduces the desync). When clear, injected frames go through the fix (freeze/absorb),
// so you can confirm the fix prevents the desync and doesn't break the match.
extern "C" unsigned long g_spectatorInjectBypassFix;

// Called from the caller-side gate hook on every starved frame. Returns:
//   0 = let vanilla decide (normal-state stall, or advance-with-zero past the absorb window)
//   1 = FREEZE (caller must NOT advance the fight) -- the fix, in advance-with-zero states
//   2 = FORCE advance-with-zero (phantom frame) -- TEST ONLY, bypass-fix desync repro
extern "C" int __cdecl SpectatorSyncOnStarvationThunk();

namespace SpectatorSyncDiagnostics
{
	struct Status
	{
		unsigned long backendPtr;
		int starvationCount;      // frames SyncInput reported starvation
		int framesAbsorbed;       // advance-with-zero frames we froze instead (desync prevented)
		int zeroInputAdvancesLeaked; // advance-with-zero we let through (absorb window exceeded)
		int stalls;               // normal-state starvations vanilla stalls (left to vanilla)
		int currentFreezeStreak;  // consecutive frozen frames right now
		int maxLagSeen;
		int injectRemaining;
		int nextInputToSend;
		int maxReceivedFrame;
		int sceneA, sceneB, sceneC;
	};
	Status GetStatus();
	void InjectDesync(int frames);
}
