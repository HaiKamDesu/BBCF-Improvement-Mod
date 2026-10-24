#pragma once
#include "IWindow.h"

#include <steamclientpublic.h>

#include <string>

// The online window (F2): who is in the room and which mod version each of them runs, the
// match's settings and palettes, and the button for the separate PC & connection stats window
// (PerfStatsWindow, also Ctrl+F2).
//
// Section open/closed state is kept in menus.ini next to the window's position and size
// (RegisterLayoutSettings), like the mod menu's own layout.
class RoomWindow : public IWindow
{
public:
	RoomWindow(const std::string& windowTitle, bool windowClosable,
		ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags), m_origWindowTitle(windowTitle)
	{
	}

	~RoomWindow() override = default;

	// Must run before the first ImGui frame (that is when menus.ini is parsed).
	static void RegisterLayoutSettings();

protected:
	void BeforeDraw() override;
	void Draw() override;

private:
	void SetWindowTitleRoomType(const std::string& roomTypeName);
	void ShowClickableSteamUser(const char* playerName, const CSteamID& steamId) const;
	void DrawToolbar();
	void DrawPlayers();
	void DrawMatchSettings();

	const std::string m_origWindowTitle;
};
