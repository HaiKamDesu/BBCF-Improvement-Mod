#pragma once

#include "Overlay/Window/IWindow.h"

// Every ranked match recorded by RankHistory, grouped by day, with the day's net LP and record,
// and totals for today, the last seven days and all time. Opened from the ranked progress
// window's right-click menu and from the mod menu's ranked section.
class RankHistoryWindow : public IWindow
{
public:
	RankHistoryWindow(const std::string& windowTitle, bool windowClosable, ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags)
	{
	}

protected:
	void BeforeDraw() override;
	void Draw() override;
	void AfterDraw() override;
};
