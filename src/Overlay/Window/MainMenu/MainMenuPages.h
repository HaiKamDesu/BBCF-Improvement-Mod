#pragma once

#include "MainMenuNav.h"

namespace MainMenu
{
	void DrawGamePage(const PageContext& ctx);
	void DrawTrainingPage(const PageContext& ctx);
	void DrawOverlaysPage(const PageContext& ctx);
	void DrawOnlinePage(const PageContext& ctx);
	void DrawReplaysPage(const PageContext& ctx);
	void DrawLookAndSoundPage(const PageContext& ctx);
	void DrawControllersPage(const PageContext& ctx);
	void DrawModPage(const PageContext& ctx);

	void DrawPage(PageId page, const PageContext& ctx);
}
