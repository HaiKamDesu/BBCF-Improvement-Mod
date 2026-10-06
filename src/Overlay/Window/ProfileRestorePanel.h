#pragma once

// The "go back to before this update" controls (Core/SaveDataSafety.h, Network/ProfileSync.h).
//
// Deliberately out of the way: a collapsed header at the bottom of Settings > Advanced, kept
// out of search, with every button disabled until the player ticks that they understand a
// restore cannot be undone. It is an escape hatch for a profile an update broke, not a
// feature anyone should meet while browsing.
namespace ProfileRestorePanel
{
	void Draw();
}
