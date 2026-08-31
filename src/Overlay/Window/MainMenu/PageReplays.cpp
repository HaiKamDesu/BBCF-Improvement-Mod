#include "MainMenuPages.h"

#include "Core/HotkeyManager.h"
#include "Core/info.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include "Overlay/Window/ScrWindow.h"

#include "imgui.h"

namespace MainMenu
{
	void DrawReplaysPage(const PageContext& ctx)
	{
		ScrWindow* scr = ctx.container->GetWindow<ScrWindow>(WindowType_Scr);
		const bool inTheater = g_gameVals.pGameMode && *g_gameVals.pGameMode == GameMode_ReplayTheater;

		Anchor(Replays_Rewind);
		ImGui::BeginDisabled(!inTheater);
		if (ImGui::Button(Messages.Toggle_Rewind()))
			ctx.container->GetWindow(WindowType_ReplayRewind)->ToggleOpen();
		ImGui::EndDisabled();
		ImGui::ShowHelpMarkerSameLine(L("Opens the rewind bar, so you can step a replay backwards instead of restarting it.").c_str());
		if (!inTheater)
		{
			const std::string note = L("(while watching a replay)");
			ImGui::SameLineOrWrap(ImGui::CalcTextSize(note.c_str()).x);
			ImGui::TextDisabled("%s", note.c_str());
		}

		ImGui::VerticalSpacing(8);

		if (BeginSection(Replays_Files))
		{
			Hint(L("Load a replay file the game's own theater cannot see, browse the archive, or pull one down from the replay database."));
			if (scr)
				scr->DrawLocalReplaysBody();
		}

		if (BeginSection(Replays_Takeover, inTheater))
		{
			Hint(FormatText(L("Jump into a replay and play it out yourself from a moment you saved. Hotkey: %s loads that moment.").c_str(),
				HotkeyManager::DisplayString(
					HotkeyManager::GetBinding(HotkeyManager::Hotkey_LoadReplayState)).c_str()));
			if (scr)
				scr->DrawReplayTakeoverBody();
		}

		ImGui::VerticalSpacing(8);
		ImGui::Separator();
		ImGui::VerticalSpacing(4);

		Anchor(Replays_Database);
		ImGui::ButtonUrl(Messages.Replay_Database(), REPLAY_DB_FRONTEND);
		ImGui::SameLineOrWrap(ImGui::ButtonWidth(Messages.Enable_Disable_Upload()));
		if (ImGui::Button(Messages.Enable_Disable_Upload()))
		{
			ctx.container->GetWindow(WindowType_ReplayDBPopup)->ToggleOpen();
		}
		ImGui::ShowHelpMarkerSameLine(L("Choose whether your finished matches are uploaded to the community replay database.").c_str());
	}
}
