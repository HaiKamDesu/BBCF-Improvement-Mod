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
	void DrawSearchBox();
	void DrawNav(float width, float height);
	void DrawNavHotkeyHints() const;
	void DrawFooter();
	void DrawSearchResults();

	MainMenu::PageContext MakePageContext();

	WindowContainer* m_pWindowContainer = nullptr;
	SettingsIniWindow m_settingsIniWindow;
	PalettesConfigWindow m_palettesConfigWindow;
	ImGuiTextFilter m_featureFilter;
};
