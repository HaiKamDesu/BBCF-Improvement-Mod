#pragma once

#include "Overlay/Window/IWindow.h"

// Lists the players currently hidden from the ranked search list by
// RankedListConnectionFilter, with per-player Restore buttons.
//
// Auto-visibility: opens by itself while the ranked search list is on screen
// (and the filter plus the ShowRankedListFilterWindow setting are enabled),
// and closes when the list goes away. The user closing the window with its X
// is treated as unchecking ShowRankedListFilterWindow, persisted to
// settings.ini - see UpdateAutoVisibility / AfterDraw.
class RankedListFilterWindow : public IWindow
{
public:
	RankedListFilterWindow(const std::string& windowTitle, bool windowClosable, ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags)
	{
	}

	// Called once per frame (MatchState::OnUpdate) to drive automatic
	// open/close based on filter state and list visibility.
	void UpdateAutoVisibility();

protected:
	void Draw() override;
	void AfterDraw() override;
};
