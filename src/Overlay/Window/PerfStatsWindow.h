#pragma once
#include "IWindow.h"

#include <string>

// PC & connection stats for online matches (src/Game/PerformanceMonitor.h), in its own window
// so it can be sized independently of the online window. Players see You / Opp.; spectators
// see P1 / P2. Never opens by itself: the online window's button, Ctrl+F2 or the mod menu's
// Online page. While it is open (even collapsed) the mod asks the other side for its numbers;
// closed, nothing is exchanged.
//
// It fits its content at a fixed width (not resizable). Position and collapsed state are
// ImGui's own (menus.ini, id ###PerfStats); whether "Details" is open is kept next to them by
// RegisterLayoutSettings. The first thing shown is a plain-language answer; the full table
// sits behind "Details".
class PerfStatsWindow : public IWindow
{
public:
	PerfStatsWindow(const std::string& windowTitle, bool windowClosable, ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags)
	{
	}

	// Must run before the first ImGui frame (that is when menus.ini is parsed).
	static void RegisterLayoutSettings();

	// Open and bring to the front (buttons).
	void OpenFocused();
	// Ctrl+F2: open and focus, or close if it is already open.
	void ToggleFromHotkey();

protected:
	void BeforeDraw() override;
	void Draw() override;

private:
	void DrawStats();
	std::string BuildStatsText() const;

	bool m_focusNextFrame = false;
};
