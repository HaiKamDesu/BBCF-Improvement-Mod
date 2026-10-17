#pragma once

// Log lines an automated test can wait for, so every input it sends has an observable result
// to check ("press Right on stage select -> expect [Probe] stage x=3"). Off unless the
// environment variable BBCF_IM_STATE_TRACE=1 is set; costs one branch per frame otherwise.
//
//   [Probe] stage x=X y=Y      the stage select cursor moved (character select only)
//   [Probe] hp P1 P2 pos X1,Y1 X2,Y2   either character's HP changed (in a match)
//
// A stock run and a stressed run of the same replay give identical hp lines, which is how
// replay determinism above 2 GB was checked (docs/Decompilation/Memory.md).

namespace TestProbes
{
	// Once per game frame (GetFrameCounter hook).
	void OnGameFrame();

	// Once per rendered frame.
	void OnRenderFrame();
}
