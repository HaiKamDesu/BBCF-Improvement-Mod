#include "MainMenuPages.h"

#include "Core/HotkeyManager.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include "Overlay/Window/DummyActionsPanel.h"
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

		ImGui::VerticalSpacing(8);
		ImGui::Separator();
		ImGui::VerticalSpacing(4);

		Anchor(Training_Tas);
		scr->DrawTasComboToolButton();
		ImGui::VerticalSpacing(4);
		scr->DrawPlaybackTransferButtons();
	}
}
