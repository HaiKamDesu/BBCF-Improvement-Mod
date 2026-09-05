#include "RankedMainMenuSection.h"

#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"

namespace RankedUi
{
	// Body only - the mod menu's Online page owns the "Ranked" header this used to draw
	// for itself, and the room/online-window door now lives next to the room settings.
	uint32_t DrawMainMenuSection()
	{
		uint32_t actions = RankedMainMenuAction_None;

		ImGui::HorizontalSpacing();
		bool showRankedProgress = Settings::settingsIni.showRankedProgress;
		if (ImGui::CheckboxWrapped(L("Show ranked progress").c_str(), &showRankedProgress))
		{
			Settings::settingsIni.showRankedProgress = showRankedProgress;
			Settings::changeSetting("ShowRankedProgress", showRankedProgress ? "1" : "0");
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Shows a movable ranked progress window during ranked character select, ranked menu flow, and after a successful ranked LP upload.").c_str());

		ImGui::HorizontalSpacing();
		bool showSquareColorProgress = Settings::settingsIni.showSquareColorProgress;
		if (ImGui::CheckboxWrapped(L("Show square color progress").c_str(), &showSquareColorProgress))
		{
			Settings::settingsIni.showSquareColorProgress = showSquareColorProgress;
			Settings::changeSetting("ShowSquareColorProgress", showSquareColorProgress ? "1" : "0");
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Shows a movable network square color progress window while inside network mode.").c_str());

		ImGui::HorizontalSpacing();
		bool showRankedPrediction = Settings::settingsIni.showRankedPrediction;
		if (ImGui::CheckboxWrapped(L("Show ranked prediction").c_str(), &showRankedPrediction))
		{
			Settings::settingsIni.showRankedPrediction = showRankedPrediction;
			Settings::changeSetting("ShowRankedPrediction", showRankedPrediction ? "1" : "0");
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Shows win/loss ranked outcome predictions during ranked match confirmation and ranked rematch screens when opponent rank data is available.").c_str());

		ImGui::HorizontalSpacing();
		bool showRankedListFilterWindow = Settings::settingsIni.showRankedListFilterWindow;
		if (ImGui::CheckboxWrapped(L("Show ranked list config window").c_str(), &showRankedListFilterWindow))
		{
			Settings::settingsIni.showRankedListFilterWindow = showRankedListFilterWindow;
			Settings::changeSetting("ShowRankedListFilterWindow", showRankedListFilterWindow ? "1" : "0");
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Automatically opens a window while the ranked search list is on screen, holding the connection filter toggle, list sorting, and the list of hidden players. Closing that window is the same as unchecking this.").c_str());

		ImGui::VerticalSpacing(8);
		ImGui::HorizontalSpacing();
		if (ImGui::Button(L("Ranked ladder").c_str()))
		{
			actions |= RankedMainMenuAction_OpenLadder;
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Opens the ranked ladder window, including known LP thresholds and population estimates.").c_str());

		ImGui::HorizontalSpacing();
		if (ImGui::Button(L("Ranked leaderboard").c_str()))
		{
			actions |= RankedMainMenuAction_OpenLeaderboard;
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Opens a detailed, paged leaderboard browser (all players or per character) with exact scores, rank + LP, last-used character, Steam level, online status, and a profile shortcut.").c_str());

		ImGui::HorizontalSpacing();
		if (ImGui::Button(L("How does ranked work?").c_str()))
		{
			actions |= RankedMainMenuAction_OpenRulesSelector;
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Choose any rank and open an explanation of its LP, promotion, and demotion rules.").c_str());

		return actions;
	}
}
