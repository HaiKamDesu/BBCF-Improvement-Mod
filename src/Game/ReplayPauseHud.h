#pragma once

// Keeps the game's input display on screen while a replay is paused.
//
// Pausing a replay hides the input display - the scrolling A/B/C/D columns down both edges and
// the two "Player 1 / Player 2" stick-and-button panels - which is exactly what a player pauses
// to read. Everything else (health, timer, portraits, the seek bar) stays.
//
// WHAT NOT TO PATCH. The predicate at BBCF.exe+0xD1FC0 answers "is the game paused right now",
// and its first reason is "Replay Theater AND playback state 1". It is tempting, because every
// hidden display asks it. It is also what the world update asks before stepping the game
// (0x5505D1: call it, and skip the update if it says yes). Forcing it to 0 does make the display
// stay up - by preventing the pause from happening at all. That was tried, and from screenshots
// alone it looked like a fix, because a running game and a paused game with its HUD restored
// photograph the same way.
//
// WHAT THIS PATCHES. Only the branches that consume that answer to hide a display, leaving the
// answer itself alone so the pause still pauses:
//
//   * four "is this display switched on" getters at 0x699BC0 / BE0 / C00 / C20, each of which
//     reads its own on/off global and then ANDs in "and we are not paused";
//   * the input-info panel gate at 0x6CFA40, which skips the whole panel - the one that pushes
//     the resource name "TRI_InputInfo" on the line right after the check.
//
// Each patch is the `jne` that jumps over the display work, turned into NOPs so the work always
// runs; the game's own on/off globals still decide, exactly as they do while the replay plays.
// Restored byte for byte when the option is turned off. Nothing is written to game state.
//
// Full write-up, including the five theories that were wrong first: docs/Research/ReplayPauseHud.md
namespace ReplayPauseHud
{
	// Finds the branches. Call once after the Steam DRM has unpacked the executable, alongside
	// the other hook placement; before that the bytes to scan for are not in memory yet.
	void Locate();

	// Applies or removes the patches. Cheap and idempotent - it writes only on a real change, so
	// calling it every frame from the settings mirror costs a comparison.
	void SetEnabled(bool enabled);

	// The replay's playback state, straight from the field the game pauses on: 0 while playing,
	// 1 while paused. -1 when it cannot be read. Logged so a test can assert that a pause
	// actually happened rather than inferring it from a screenshot - which is how the broken
	// version of this feature passed.
	int PlaybackState();
}
