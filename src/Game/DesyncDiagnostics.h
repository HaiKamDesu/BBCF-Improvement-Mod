#pragma once

// Puts BBCF's own GGPO desync checker into DEBUG.txt, so a "DESYNC DETECTED" report can be
// read from the log alone: when the session started and against whom, every checksum
// mismatch with its frame and both checksums, when the overlay came up, when the game was
// about to drop the match, and the last Jukebox change before each of those.
//
// The checker (docs/Decompilation/Lobby_E_ConnectionNetcode.md, "Desync checker"): every 90
// confirmed frames both peers hash their rollback snapshot and compare. More than 1 mismatch
// in a row shows the overlay (0x783730), more than 5 is treated as a disconnect (0x7836A0).
// This only reads it.

namespace DesyncDiagnostics
{
	// Once per game frame (GetFrameCounter hook).
	void Tick();

	// Frames since the current GGPO session's checker appeared, or -1 outside a session.
	// The checker's mismatch frames count from the same start, give or take the frame delay.
	int GetSessionFrame();

	// A one-line description of a music change, kept so the next mismatch can name the last
	// change before it. Also counted for the end-of-session summary.
	void NoteMusicChange(const char* description);

	// What MatchState::OnMatchInit decided about us (palettes.ini side, palette sends). When the
	// game then builds its GGPO session, Tick logs a [SeatCheck] line comparing the two: player
	// or spectator, P1 or P2, and who the remote peer is. The session is built from the game's
	// own seat logic (0x4E5740), so a MISMATCH means our room reading is wrong.
	void NoteMatchInitRole(bool spectator, int matchPlayerIndex);
}
