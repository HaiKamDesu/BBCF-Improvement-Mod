#pragma once

#include "Overlay/Window/IWindow.h"

// "Ranked List Config" window. Holds all ranked-list-filter controls: the
// enable checkbox, the sort-order dropdown, and the live list of hidden
// players with per-player Restore. These were moved here out of the F1
// Network Matches section, which now only carries the toggle that shows this
// window.
//
// Auto-visibility: opens by itself whenever the ranked search list is on
// screen (and the ShowRankedListConfigWindow setting is enabled), and closes
// promptly when the list goes away. The user closing the window with its X is
// treated as unchecking ShowRankedListConfigWindow, persisted to settings.ini.
class RankedListFilterWindow : public IWindow
{
public:
	RankedListFilterWindow(const std::string& windowTitle, bool windowClosable, ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags)
	{
	}

	// Called once per frame (MatchState::OnUpdate) to drive automatic
	// open/close based on the setting and list visibility.
	void UpdateAutoVisibility();

protected:
	void Draw() override;
	void AfterDraw() override;

private:
	// Draws the separate "Hidden players" modal (opened by clicking the
	// "N hidden" status text) with its scrollable restore list.
	void DrawHiddenPlayersPopup();

	// True while the hidden-players modal is open. Seeded from and kept in
	// sync with ShowRankedListHiddenPopup: opening/closing it (by click or by
	// its own X) persists the new state, so it reopens on its own next time
	// unless the user closed it last.
	bool m_showHiddenPopup = false;
};
