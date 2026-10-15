#include "MainMenuPages.h"

#include "Core/HotkeyManager.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Game/OnlineInputDelay.h"
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
		// The one netcode number the game exposes, and the one place it is safe to expose it.
		//
		// The slider only goes down to 2 because below-stock delay is not a setting that
		// affects the person changing it - it shifts rollback onto the opponent and onto
		// spectators. The reasoning, and the byte this writes, are in
		// src/Game/OnlineInputDelay.h; do not widen the range here without reading it.
		void DrawOnlineInputDelay()
		{
			Anchor(Online_InputDelay);

			const bool available = OnlineInputDelay::IsAvailable();
			const bool belowStock = OnlineInputDelay::BelowStockAllowed();
			const int minimum = belowStock ? 0 : OnlineInputDelay::kStockDelay;

			ImGui::TextUnformatted(L("Online input delay").c_str());
			ImGui::ShowHelpMarkerSameLine(L("How many frames your inputs are held back in an online match. The game always uses 2. More delay means less rollback for both players and more lag for you - the trade worth making on a bad connection. It takes effect on your next online match, not this one.").c_str());

			if (!available)
			{
				Unavailable(L("This build cannot find the place in the game where the delay is set, so this does nothing. Check DEBUG.txt."));
				return;
			}

			int delay = Settings::settingsIni.onlineInputDelay;
			if (delay < minimum) { delay = minimum; }
			if (delay > OnlineInputDelay::kMaxDelay) { delay = OnlineInputDelay::kMaxDelay; }

			// The slider takes whatever width the page has, between a floor that keeps the
			// handle usable and a ceiling that stops nine notches being stretched across a
			// maximised window. A fixed width was wrong at both ends: it ran off the right
			// edge of a narrow mod menu and looked stranded in a wide one.
			ImGui::HorizontalSpacing();
			float sliderWidth = ImGui::GetContentRegionAvail().x;
			if (sliderWidth > 260.0f) { sliderWidth = 260.0f; }
			if (sliderWidth < 90.0f) { sliderWidth = 90.0f; }
			ImGui::SetNextItemWidth(sliderWidth);

			if (ImGui::SliderInt("##online_input_delay", &delay, minimum, OnlineInputDelay::kMaxDelay,
					delay == OnlineInputDelay::kStockDelay
						? L("%d frames (the game's own)").c_str()
						: L("%d frames").c_str()))
			{
				Settings::settingsIni.onlineInputDelay = delay;
				Settings::changeSetting("OnlineInputDelay", std::to_string(delay));
			}

			// Say what it costs, in the direction they are actually moving it. All of these
			// are full sentences, so they wrap with the page rather than being clipped.
			if (delay > OnlineInputDelay::kStockDelay)
			{
				Hint(FormatText(
					L("%d frames more lag than normal, and less rollback for both of you.").c_str(),
					delay - OnlineInputDelay::kStockDelay));
			}
			else if (delay < OnlineInputDelay::kStockDelay)
			{
				ImGui::HorizontalSpacing();
				ImGui::TextColoredWrapped(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
					L("Below the game's own delay. This does not make you faster - it makes your inputs reach your opponent later, so they and anyone spectating get the extra rollback. For testing only.").c_str());
			}
			else
			{
				Hint(L("What the game does on its own."));
			}

			if (!belowStock)
			{
				Hint(L("2 is the lowest this goes. Lower values are a testing knob and need in-development features turned on."));
			}
		}

		// Same window as the ranked list's, with its own settings.
		void DrawPlayerRoomListToggle()
		{
			ImGui::HorizontalSpacing();
			bool show = Settings::settingsIni.showPlayerRoomListFilterWindow;
			if (ImGui::CheckboxWrapped(L("Show player room list config window").c_str(), &show))
			{
				Settings::settingsIni.showPlayerRoomListFilterWindow = show;
				Settings::changeSetting("ShowPlayerRoomListFilterWindow", show ? "1" : "0");
			}
			ImGui::ShowHelpMarkerSameLine(L("Automatically opens a window while the player room list is on screen, with sorting (connection, player count, room size, name) and filters for that list. It keeps its own settings, separate from the ranked list's. Closing that window is the same as unchecking this.").c_str());
		}

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

		// Room settings and the avatar live in here, so a search that lands on either has
		// to open the section first or the jump has nothing to scroll to.
		if (IsFocusPending(Online_RoomSettings) || IsFocusPending(Online_Avatar))
			ImGui::SetNextItemOpen(true);
		if (BeginSection(Online_PlayerRooms))
		{
			DrawPlayerRoomListToggle();

			const bool inRoom = g_interfaces.pRoomManager && g_interfaces.pRoomManager->IsRoomFunctional();

			ImGui::VerticalSpacing(4);
			GroupLabel(Online_RoomSettings, inRoom);
			if (scr)
			{
				ImGui::HorizontalSpacing();
				scr->DrawRoomSettingsBody();
			}

			ImGui::VerticalSpacing(4);
			GroupLabel(Online_Avatar, g_gameVals.playerAvatarAddr != NULL);
			DrawAvatar();
		}

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

		ImGui::VerticalSpacing(8);
		DrawOnlineInputDelay();
	}
}
