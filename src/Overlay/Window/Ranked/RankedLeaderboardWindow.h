#pragma once

#include "Overlay/Window/IWindow.h"

// Custom ranked-leaderboard browser. Reads the Steam "RANK_ALL" board (and the
// per-character "RANK_<code>" boards) directly and shows far more than the
// in-game screen: exact score, decoded rank tier + LP, the opponent's last-used
// character, Steam level, online status, and a Steam-profile shortcut.
//
// Everything is paged so we never download the whole ladder at once. Filters
// mirror the game (All / per-character, plus a Friends-only view) and add a
// "Jump to me" control that scrolls straight to the local player's position.
//
// All Steam async state lives in the .cpp (see the file-local model) so this
// header stays free of Steamworks includes.
class RankedLeaderboardWindow : public IWindow
{
public:
	RankedLeaderboardWindow(const std::string& windowTitle, bool windowClosable, ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags)
	{
	}

protected:
	void BeforeDraw() override;
	void Draw() override;
};
