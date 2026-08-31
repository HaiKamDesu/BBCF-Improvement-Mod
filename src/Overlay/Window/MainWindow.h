#pragma once
#include "IWindow.h"

#include "PalettesConfigWindow.h"
#include "SettingsIniWindow.h"
#include "MainMenu/MainMenuNav.h"
#include "Overlay/WindowContainer/WindowContainer.h"

#include "imgui.h"

#include <string>

// The mod's single menu. It owns no features of its own any more: it is the shell that holds
// the page list on the left, the feature search at the top, and whichever page is selected on
// the right. The pages themselves live in Overlay/Window/MainMenu/Page*.cpp.
class MainWindow : public IWindow
{
public:
	MainWindow(const std::string& windowTitle, bool windowClosable,
		WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0);

	~MainWindow() override = default;

protected:
	void BeforeDraw() override;
	void Draw() override;

private:
	void DrawTitleBar() const;
	void DrawWatermark() const;
	void DrawSearchBox();
	void DrawNav(float width, float height);
	void DrawNavHotkeyHints() const;
	void DrawFooter();
	static void DrawFooterDivider(float nextItemWidth);
	static bool ShouldShowDebugButton();
	void DrawSearchResults();

	MainMenu::PageContext MakePageContext();

	// The visible part of the title, drawn by hand rather than handed to ImGui.
	std::string m_titleText;
	WindowContainer* m_pWindowContainer = nullptr;
	SettingsIniWindow m_settingsIniWindow;
	PalettesConfigWindow m_palettesConfigWindow;
	ImGuiTextFilter m_featureFilter;

	// Set by the feature search, acted on in DrawFooter: OpenPopup has to be issued from the
	// window that also calls BeginPopupModal, and the results list is inside a child.
	bool m_openSettingsRequested = false;
	std::string m_pendingSettingsFilter;

	// Height the footer actually took last frame; it rewraps with the window, so the panes
	// above it reserve the measured value rather than a guess. Seeded for the first frame.
	float m_lastFooterHeight = 58.0f;
};
