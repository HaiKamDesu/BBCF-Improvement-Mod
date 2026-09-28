#pragma once

#include "Game/Player.h"

// Which side is the human and which is the dummy.
//
// Everything that drives the training dummy used to read g_interfaces.player2 outright, so
// picking Player 2 on the training character select - or taking a replay over as P2 - left
// every dummy feature acting on the wrong character: triggers watched the human, forced
// animations hit the human, and the move list was parsed from the human's script.
//
// The game keeps its own answer: a dword at BBCF.exe+0x891A38 holding the index (0 or 1) of
// the side the training player controls. It is what indexes the per-side training tables
// (0xC904D4 + side * 0x710) and what the replay takeover already rewrites to hand a side
// to the human. Outside a mode with a dummy it is left as it was, so callers should still
// gate on the game mode themselves.
namespace TrainingSide
{
	// 0 = Player 1 is the human, 1 = Player 2 is. Anything else read from the game is
	// treated as 0.
	int HumanIndex();
	int DummyIndex();

	Player& Human();
	Player& Dummy();

	// parse_scr's player number (1 or 2) for the dummy's script.
	int DummyPlayerNum();

	// The game's own byte, for those that need to write it (the replay takeover).
	unsigned char* SidePointer();
}
