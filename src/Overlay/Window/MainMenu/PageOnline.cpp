#include "MainMenuPages.h"

#include "Core/HotkeyManager.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include "Overlay/Window/Ranked/RankedProgressWindow.h"
#include "Overlay/Window/ScrWindow.h"
#include "Network/LobbyAvatarManager.h"
#include "Network/ProfileBlobSeal.h"

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

			// Every slider writes straight into the game's avatar fields, so an edit is
			// only visible to the persistence code as "the values changed". Telling it
			// explicitly keeps a re-apply that is still running from dragging the slider
			// back out from under the user.
			bool edited = false;

			ImGui::HorizontalSpacing(); edited |= ImGui::SliderInt(Messages.Avatar(), g_gameVals.playerAvatarAddr, 0, 0x2F);
			ImGui::ShowHelpMarkerSameLine(Messages.Avatar_icon_tooltip());
			ImGui::HorizontalSpacing(); edited |= ImGui::SliderInt(Messages.Color(), g_gameVals.playerAvatarColAddr, 0, 0x3);
			ImGui::ShowHelpMarkerSameLine(Messages.Avatar_color_tooltip());
			ImGui::HorizontalSpacing(); edited |= ImGui::SliderByte(Messages.Accessory_1(), g_gameVals.playerAvatarAcc1, 0, 0xCF);
			ImGui::ShowHelpMarkerSameLine(Messages.Avatar_accessory1_tooltip());
			ImGui::HorizontalSpacing(); edited |= ImGui::SliderByte(Messages.Accessory_2(), g_gameVals.playerAvatarAcc2, 0, 0xCF);
			ImGui::ShowHelpMarkerSameLine(Messages.Avatar_accessory2_tooltip());

			if (edited)
			{
				// The sliders write into the player's own network profile blob, which is
				// checksummed and uploaded to Steam. Resealing here is what stops a drag
				// from killing profile uploads for the rest of the session; this hazard
				// predates the persistence feature below. See ProfileBlobSeal.h.
				ProfileBlobSeal::Reseal();
				LobbyAvatarManager::GetInstance().OnUserEdited();
			}

			ImGui::VerticalSpacing(4);

			// Lives next to the sliders rather than only in Settings because this is where
			// people notice the problem it solves.
			ImGui::HorizontalSpacing();
			static bool rememberAvatar = Settings::settingsIni.rememberLobbyAvatar;
			if (ImGui::CheckboxWrapped(L("Put this back on automatically next launch").c_str(), &rememberAvatar))
			{
				Settings::settingsIni.rememberLobbyAvatar = rememberAvatar;
				Settings::changeSetting("RememberLobbyAvatar", rememberAvatar ? "1" : "0");
				if (rememberAvatar)
				{
					LobbyAvatarManager::GetInstance().OnUserEdited();
				}
			}
			ImGui::ShowHelpMarkerSameLine(L("The game only saves the accessories its own equip menu offers, so the hidden ones are gone every time you relaunch. With this on, whatever you last had equipped is re-applied when you connect to network mode.").c_str());
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
		if (ImGui::CheckboxWrapped(L("Load other players' custom palettes").c_str(), &loadForeignPalettes))
		{
			g_modVals.enableForeignPalettes = loadForeignPalettes;
		}
		ImGui::ShowHelpMarkerSameLine(L("Turn this off if your game crashes when you search for a ranked match from training mode. It only stops other players' custom colours from loading; yours are unaffected. This is a stopgap, not the real fix.").c_str());
	}
}
