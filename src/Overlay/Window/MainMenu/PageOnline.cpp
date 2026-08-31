#include "MainMenuPages.h"

#include "Core/HotkeyManager.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include "Overlay/Window/Ranked/RankedProgressWindow.h"
#include "Overlay/Window/ScrWindow.h"

#include "imgui.h"

namespace MainMenu
{
	namespace
	{
		void DrawAvatar()
		{
			if (g_gameVals.playerAvatarAddr == NULL && g_gameVals.playerAvatarColAddr == NULL
				&& g_gameVals.playerAvatarAcc1 == NULL && g_gameVals.playerAvatarAcc2 == NULL)
			{
				Unavailable(Messages.CONNECT_TO_NETWORK_MODE_FIRST());
				return;
			}

			ImGui::HorizontalSpacing(); ImGui::SliderInt(Messages.Avatar(), g_gameVals.playerAvatarAddr, 0, 0x2F);
			ImGui::ShowHelpMarkerSameLine(Messages.Avatar_icon_tooltip());
			ImGui::HorizontalSpacing(); ImGui::SliderInt(Messages.Color(), g_gameVals.playerAvatarColAddr, 0, 0x3);
			ImGui::ShowHelpMarkerSameLine(Messages.Avatar_color_tooltip());
			ImGui::HorizontalSpacing(); ImGui::SliderByte(Messages.Accessory_1(), g_gameVals.playerAvatarAcc1, 0, 0xCF);
			ImGui::ShowHelpMarkerSameLine(Messages.Avatar_accessory1_tooltip());
			ImGui::HorizontalSpacing(); ImGui::SliderByte(Messages.Accessory_2(), g_gameVals.playerAvatarAcc2, 0, 0xCF);
			ImGui::ShowHelpMarkerSameLine(Messages.Avatar_accessory2_tooltip());
		}
	}

	void DrawOnlinePage(const PageContext& ctx)
	{
		ScrWindow* scr = ctx.container->GetWindow<ScrWindow>(WindowType_Scr);

		// One button, so it stays loose at the top where it is the first thing you see.
		Anchor(Online_Window);
		if (ImGui::Button(L("Open the online window").c_str()))
		{
			ctx.container->GetWindow(WindowType_Room)->ToggleOpen();
		}
		ImGui::ShowHelpMarkerSameLine(L("A small movable window listing who is in the room with you, who is playing, and who is spectating.").c_str());

		ImGui::VerticalSpacing(8);

		// Ranked is the one block here big enough to be worth collapsing, and it is always
		// usable: the toggles configure what happens next time you play ranked, and the
		// ladder, leaderboard and rules windows read data you can browse from anywhere.
		if (BeginSection(Online_Ranked))
		{
			DrawRankedMatchesMainMenuSection();
		}

		const bool inRoom = g_interfaces.pRoomManager && g_interfaces.pRoomManager->IsRoomFunctional();

		GroupLabel(Online_RoomSettings, inRoom);
		if (scr)
		{
			ImGui::HorizontalSpacing();
			scr->DrawRoomSettingsBody();
		}

		ImGui::VerticalSpacing(4);
		GroupLabel(Online_Avatar, g_gameVals.playerAvatarAddr != NULL);
		DrawAvatar();

		ImGui::VerticalSpacing(8);
		ImGui::Separator();
		ImGui::VerticalSpacing(4);

		// A netplay crash workaround that used to sit, unlabelled and unexplained, above the
		// training tools in the old States window. One checkbox, so it stays one checkbox.
		Anchor(Online_ForeignPalettes);
		static bool loadForeignPalettes = g_modVals.enableForeignPalettes;
		if (ImGui::Checkbox(L("Load other players' custom palettes").c_str(), &loadForeignPalettes))
		{
			g_modVals.enableForeignPalettes = loadForeignPalettes;
		}
		ImGui::ShowHelpMarkerSameLine(L("Turn this off if your game crashes when you search for a ranked match from training mode. It only stops other players' custom colours from loading; yours are unaffected. This is a stopgap, not the real fix.").c_str());
	}
}
