#include "MainMenuPages.h"

#include "Core/HotkeyManager.h"
#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include "Overlay/Window/DummyActionsPanel.h"
#include "Overlay/Window/ScrWindow.h"
#include "Hooks/hooks_battle_input.h"

#include "imgui.h"

#include <string>

namespace MainMenu
{
	void DrawTrainingPage(const PageContext& ctx)
	{
		ScrWindow* scr = ctx.container->GetWindow<ScrWindow>(WindowType_Scr);
		if (!scr)
			return;

		const bool inTraining = g_gameVals.pGameMode && *g_gameVals.pGameMode == GameMode_Training;
		if (!inTraining)
		{
			Hint(L("Everything on this page needs training mode. It is all listed anyway so you know what is waiting for you there."));
			ImGui::VerticalSpacing(6);
		}

		// Dummy actions come first: it is what the page is for. One row per trigger the dummy
		// is set to react to, and every kind of input the mod can feed it lives here now -
		// recorded playbacks, typed notation, a file, a CF slot, or one of the dummy's own
		// animations. The old "Recording slots" section is gone because it was the same
		// feature reached a different way. See docs/DummyActionsRework.md.
		// Collapsible, the same as the Replays page: this page has four sections now and the
		// dummy-action list grows a row at a time, so being able to fold what you are not
		// using is the difference between a page and a wall.
		if (BeginSection(Training_Dummy, inTraining))
		{
			Hint(L("Tell the dummy what to do and when. Add an action, pick the trigger, then pick where its inputs come from."));
			DummyActionsPanel::Draw();
		}

		if (BeginSection(Training_Positions, inTraining))
		{
			Hint(L("Where the two of you stand, and how long the dummy takes to get up."));
			scr->DrawPositionsBody();
			ImGui::VerticalSpacing(4);
			scr->DrawWakeupBody();
		}

		if (BeginSection(Training_SaveStates, inTraining))
		{
			Hint(FormatText(L("Save the exact moment you are in and jump back to it later. Hotkeys: %s to save, %s to load.").c_str(),
				HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_SaveState)).c_str(),
				HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_LoadState)).c_str()));
			scr->DrawSaveStatesBody();
		}

		// Input delay. The lab runs with none and online always runs with two, so the
		// habits built here are two frames early for the only place they get used. This
		// section is the knob that closes that gap; the delay itself lives in
		// src/Hooks/hooks_battle_input.cpp.
		if (BeginSection(Training_InputDelay, inTraining))
		{
			Hint(L("Hold your own inputs back by a few frames so the lab answers as late as a net match does. BBCF online adds 2 frames of input delay whatever your ping, so 2 is the setting that matches it. The dummy is never delayed."));

			if (!IsBattleInputHookInstalled())
			{
				Unavailable(L("The mod cannot read the pad right now, so the delay does nothing. It needs the mod's controller hooks, which are off or could not be installed."));
			}

			int delayFrames = Settings::settingsIni.trainingInputDelay;
			ImGui::HorizontalSpacing();
			ImGui::SetNextItemWidth(220.0f * ImGui::GetIO().FontGlobalScale);
			if (ImGui::SliderInt(L("Input delay (frames)").c_str(), &delayFrames, 0, 10))
			{
				Settings::settingsIni.trainingInputDelay = delayFrames;
				Settings::changeSetting("TrainingInputDelay", std::to_string(delayFrames));
			}
			ImGui::ShowHelpMarkerSameLine(L("0 is the game as it ships. 2 is what online feels like. Rollback and connection hitches are not simulated - only the delay is.").c_str());
		}

		ImGui::VerticalSpacing(8);
		ImGui::Separator();
		ImGui::VerticalSpacing(4);

		Anchor(Training_Tas);
		scr->DrawTasComboToolButton();
		ImGui::VerticalSpacing(4);
		scr->DrawPlaybackTransferButtons();
	}
}
