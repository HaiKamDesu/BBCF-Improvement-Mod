#pragma once
#include <imgui.h>

#include <string>

class IWindow
{
public:
	IWindow(const std::string& windowTitle, bool windowClosable, ImGuiWindowFlags windowFlags = 0)
		: m_windowTitle(windowTitle),
		m_windowClosable(windowClosable),
		m_windowFlags(windowFlags) {}
	virtual ~IWindow() = default;
	virtual void Update();
	void Open();
	void Close();
	void ToggleOpen();
	bool IsOpen() const;

	// Whether this window needs the mod's mouse cursor on screen right now.
	//
	// This is deliberately not the same question as IsOpen(). A HUD-style overlay can be
	// "open" for its whole session and still not want a cursor - either because it never
	// takes mouse input at all, or because it is open but not currently being drawn. Basing
	// the cursor on IsOpen() left one on screen on the title screen and the main menu for
	// anyone whose frame history was enabled, since that window self-opens from settings.ini
	// and only stops drawing, never closes.
	virtual bool WantsMouseCursor() const { return m_windowOpen; }
	void SetWindowFlag(ImGuiWindowFlags flag);
	void ClearWindowFlag(ImGuiWindowFlags flag);
protected:
	virtual void BeforeDraw() {}
	virtual void Draw() = 0;
	virtual void AfterDraw() {}
private:
	bool* GetWindowOpenPointer();
protected:
	std::string       m_windowTitle;
	bool              m_windowClosable;
	ImGuiWindowFlags  m_windowFlags;
	bool              m_windowOpen = false;
};
