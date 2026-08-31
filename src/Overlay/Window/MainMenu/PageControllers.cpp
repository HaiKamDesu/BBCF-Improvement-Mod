#include "MainMenuPages.h"

#include "Core/Localization.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Window/ControllerSettings/ControllerSettingsSection.h"

#include "imgui.h"

namespace MainMenu
{
	// A page rather than a section under Mod: which pad each side is on is not something you
	// think of as a "setting", it is a thing you go and sort out, usually in a hurry, usually
	// with someone waiting.
	void DrawControllersPage(const PageContext& ctx)
	{
		(void)ctx;

		Anchor(Controllers_Setup);
		ControllerSettings::DrawSection();
	}
}
