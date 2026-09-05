#include "MainMenuPages.h"

#include "Core/HotkeyManager.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Widget/ActiveGameModeWidget.h"
#include "Overlay/Widget/GameModeSelectWidget.h"
#include "Overlay/Widget/StageSelectWidget.h"

#include "imgui.h"

namespace MainMenu
{
	void DrawGamePage(const PageContext& ctx)
	{
		(void)ctx;

		// Money is a one-line control that has nothing to do with the match in progress, so it
		// sits loose above the match block rather than pretending to be a category.
		Anchor(Game_Money);
		if (g_gameVals.pGameMoney)
		{
			ImGui::SetNextItemWidth(160.0f);
			ImGui::InputInt("P$##money", *&g_gameVals.pGameMoney);
			ImGui::ShowHelpMarkerSameLine(Messages.Player_money_tooltip());
		}
		else
		{
			ImGui::BeginDisabled();
			int placeholder = 0;
			ImGui::SetNextItemWidth(160.0f);
			ImGui::InputInt("P$##money", &placeholder);
			ImGui::EndDisabled();
			const std::string note = L("Reachable once the game has loaded its save data.");
			ImGui::SameLineOrWrap(ImGui::CalcTextSize(note.c_str()).x);
			ImGui::TextDisabled("%s", note.c_str());
		}

		ImGui::VerticalSpacing(6);
		GroupLabel(Game_CurrentMatch);

		const bool inMatchFlow = isInMatch() || isOnVersusScreen() || isOnCharacterSelectionScreen();

		Anchor(Game_Mode);
		if (!inMatchFlow)
		{
			Unavailable(L("Game mode: available from character select onwards."));
		}
		else
		{
			ImGui::HorizontalSpacing();
			ActiveGameModeWidget();

			const bool isSpectator = g_interfaces.pRoomManager->IsRoomFunctional()
				&& g_interfaces.pRoomManager->IsThisPlayerSpectator();

			if (isGameModeSelectorEnabledInCurrentState() && !isSpectator)
			{
				ImGui::HorizontalSpacing();
				GameModeSelectWidget();
			}
			else if (isSpectator)
			{
				Unavailable(L("Spectators cannot change the game mode."));
			}
		}

		ImGui::VerticalSpacing(6);

		Anchor(Game_Stage);
		if (!isStageSelectorEnabledInCurrentState())
		{
			Unavailable(L("Stage: pick one from character select, the versus screen, or the replay menu."));
		}
		else
		{
			ImGui::HorizontalSpacing();
			StageSelectWidget();
		}

		ImGui::VerticalSpacing(6);

		Anchor(Game_Hud);
		if (!isInMatch() || !g_gameVals.pIsHUDHidden)
		{
			ImGui::BeginDisabled();
			bool placeholder = false;
			ImGui::HorizontalSpacing();
			ImGui::CheckboxWrapped(Messages.Hide_HUD_checkbox(), &placeholder);
			ImGui::EndDisabled();
			const std::string note = L("(in a match)");
			ImGui::SameLineOrWrap(ImGui::CalcTextSize(note.c_str()).x);
			ImGui::TextDisabled("%s", note.c_str());
		}
		else
		{
			ImGui::HorizontalSpacing();
			ImGui::CheckboxWrapped(Messages.Hide_HUD_checkbox(), (bool*)g_gameVals.pIsHUDHidden);
			ImGui::ShowHelpMarkerSameLine(Messages.Hide_HUD_tooltip());
		}
	}
}
