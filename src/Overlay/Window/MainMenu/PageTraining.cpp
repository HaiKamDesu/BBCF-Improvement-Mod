#include "MainMenuPages.h"

#include "Core/HotkeyManager.h"
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

		// Two checkboxes; a category of its own would be all frame and no picture.
		Anchor(Training_Positions);
		scr->DrawPositionsBody();

		ImGui::VerticalSpacing(8);

		if (BeginSection(Training_Dummy, inTraining))
		{
			Hint(L("Pick one of the dummy's own moves and tell it when to use that move: after waking up, in a gap in your pressure, when it gets hit, or when it techs a throw."));
			scr->DrawDummyActionsBody();
		}

		if (BeginSection(Training_Slots, inTraining))
		{
			Hint(L("Record what you do into a slot, then have the dummy replay it. Four classic slots, plus the longer Unlimited Playback recorder."));
			scr->DrawRecordingSlotsBody();
		}

		GroupLabel(Training_SaveStates, inTraining);
		Hint(FormatText(L("Save the exact moment you are in and jump back to it later. Hotkeys: %s to save, %s to load.").c_str(),
			HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_SaveState)).c_str(),
			HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_LoadState)).c_str()));
		scr->DrawSaveStatesBody();

		ImGui::VerticalSpacing(4);
		GroupLabel(Training_Wakeup, inTraining);
		scr->DrawWakeupBody();

		ImGui::VerticalSpacing(8);
		ImGui::Separator();
		ImGui::VerticalSpacing(4);

		Anchor(Training_Tas);
		scr->DrawTasComboToolButton();
	}
}
