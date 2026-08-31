#include "MainMenuPages.h"

namespace MainMenu
{
	void DrawPage(PageId page, const PageContext& ctx)
	{
		switch (page)
		{
		case Page_Game:         DrawGamePage(ctx); break;
		case Page_Training:     DrawTrainingPage(ctx); break;
		case Page_Overlays:     DrawOverlaysPage(ctx); break;
		case Page_Online:       DrawOnlinePage(ctx); break;
		case Page_Replays:      DrawReplaysPage(ctx); break;
		case Page_LookAndSound: DrawLookAndSoundPage(ctx); break;
		case Page_Controllers:  DrawControllersPage(ctx); break;
		default: break;
		}
	}
}
