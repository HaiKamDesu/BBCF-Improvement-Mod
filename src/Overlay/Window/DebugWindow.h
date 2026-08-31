#pragma once
#include "IWindow.h"

#include <string>

class DebugWindow : public IWindow
{
public:
	DebugWindow(const std::string& windowTitle, bool windowClosable,
		ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags) {}
	~DebugWindow() override = default;

protected:
	void Draw() override;

private:
	void DrawImGuiSection();
	void DrawGameValuesSection();
	void DrawRoomSection();
	void DrawSpectatorSyncSection();
	void DrawSettingsSection();
	void DrawNotificationSection();
	void DrawBgmTableProbeSection();

	bool m_showDemoWindow = false;
	std::string m_bgmProbeStatus;
	int m_bgmProbeIndex = 0;
	char m_bgmProbeFilter[64] = {};
	int m_bgmProbeMp3Index = 0;
};
