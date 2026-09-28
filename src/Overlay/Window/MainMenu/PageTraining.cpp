#include "MainMenuPages.h"

#include "Core/HotkeyManager.h"
#include "Core/NativeFileDialog.h"
#include "Core/logger.h"
#include "Game/Playbacks/DummyActionManager.h"
#include "Game/Playbacks/UnlimitedPlaybackManager.h"
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

#include <Windows.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace
{
	const char* kTrainingFileToken = "TrainingFile";
	// Whether the open picker is saving or loading, since one token serves both.
	bool g_trainingFileSaving = false;
	// What the last export / import did, shown under the buttons until the next one.
	std::string g_trainingFileStatus;
	bool g_trainingFileStatusIsError = false;

	std::string TrainingFileFolder()
	{
		const std::string folder = GamePath("BBCF_IM/training_files");
		CreateDirectoryA(folder.c_str(), nullptr);
		return folder;
	}

	// The part of a training file that is not the dummy: the Positions & timings section
	// and the input delay. Written as extras of the dummy-action file, which already knows
	// how to carry the triggers, their actions and the loop settings.
	DummyActionManager::Extras CollectTrainingExtras(ScrWindow* scr)
	{
		DummyActionManager::Extras extras;
		ScrWindow::TrainingSetup setup;
		scr->GetTrainingSetup(&setup);
		extras.push_back({ "swapsides", setup.swapSidesOnReset ? "1" : "0" });
		extras.push_back({ "wakeupoverride", setup.wakeupOverride ? "1" : "0" });
		extras.push_back({ "wakeupdelay", std::to_string(setup.wakeupDelay) });
		extras.push_back({ "wakeupskew", std::to_string(setup.wakeupSkew) });
		std::string types;
		for (int i = 0; i < 6; ++i) { types += setup.wakeupTypes[i] ? '1' : '0'; }
		extras.push_back({ "wakeuptypes", types });
		extras.push_back({ "inputdelay", std::to_string(Settings::settingsIni.trainingInputDelay) });
		extras.push_back({ "inputdelayignorefrozen", Settings::settingsIni.trainingInputDelayIgnoreWhenFrozen ? "1" : "0" });
		return extras;
	}

	void ApplyTrainingExtras(ScrWindow* scr, const DummyActionManager::Extras& extras)
	{
		ScrWindow::TrainingSetup setup;
		scr->GetTrainingSetup(&setup);
		for (const auto& kv : extras)
		{
			const std::string& key = kv.first;
			const std::string& value = kv.second;
			if (key == "swapsides")            { setup.swapSidesOnReset = atoi(value.c_str()) != 0; }
			else if (key == "wakeupoverride")  { setup.wakeupOverride = atoi(value.c_str()) != 0; }
			else if (key == "wakeupdelay")     { setup.wakeupDelay = atoi(value.c_str()); }
			else if (key == "wakeupskew")      { setup.wakeupSkew = atoi(value.c_str()); }
			else if (key == "wakeuptypes")
			{
				for (int i = 0; i < 6; ++i) { setup.wakeupTypes[i] = i < (int)value.size() && value[i] == '1'; }
			}
			else if (key == "inputdelay")
			{
				const int delay = (std::max)(0, (std::min)(10, atoi(value.c_str())));
				Settings::settingsIni.trainingInputDelay = delay;
				Settings::changeSetting("TrainingInputDelay", std::to_string(delay));
			}
			else if (key == "inputdelayignorefrozen")
			{
				const bool ignore = atoi(value.c_str()) != 0;
				Settings::settingsIni.trainingInputDelayIgnoreWhenFrozen = ignore;
				Settings::changeSetting("TrainingInputDelayIgnoreWhenFrozen", ignore ? "1" : "0");
			}
		}
		scr->ApplyTrainingSetup(setup);
	}

	void ResetTrainingToDefaults(ScrWindow* scr)
	{
		DummyActionManager::Instance().ResetToDefaults();
		scr->ApplyTrainingSetup(ScrWindow::TrainingSetup{});
		Settings::settingsIni.trainingInputDelay = 0;
		Settings::changeSetting("TrainingInputDelay", "0");
		Settings::settingsIni.trainingInputDelayIgnoreWhenFrozen = true;
		Settings::changeSetting("TrainingInputDelayIgnoreWhenFrozen", "1");
	}

	void BeginTrainingFilePick(bool saving)
	{
		if (NativeFileDialog::IsOpen()) { return; }
		NativeFileDialog::Request request;
		request.save = saving;
		request.title = saving ? L("Save the training file") : L("Open a training file");
		request.filters.push_back({ L("BBCF training file"), "*.bbcflab" });
		request.defaultExtension = "bbcflab";
		request.initialPath = saving ? TrainingFileFolder() + "\\training.bbcflab" : TrainingFileFolder();
		if (NativeFileDialog::Open(kTrainingFileToken, request))
		{
			g_trainingFileSaving = saving;
		}
	}

	// The picker answers on a later frame, on a worker thread. Claimed here every frame the
	// page is drawn.
	void PumpTrainingFilePick(ScrWindow* scr)
	{
		NativeFileDialog::Result result;
		if (!NativeFileDialog::Consume(kTrainingFileToken, &result) || !result.accepted || result.path.empty())
		{
			return;
		}
		DummyActionManager& actions = DummyActionManager::Instance();
		if (g_trainingFileSaving)
		{
			const DummyActionManager::Extras extras = CollectTrainingExtras(scr);
			const bool ok = actions.ExportTo(result.path, &extras);
			g_trainingFileStatusIsError = !ok;
			g_trainingFileStatus = ok
				? FormatText(L("Saved to %s").c_str(), result.path.c_str())
				: FormatText(L("Could not write %s").c_str(), result.path.c_str());
			LOG(1, "[TrainingFile] export %s -> '%s'\n", ok ? "ok" : "FAILED", result.path.c_str());
		}
		else
		{
			DummyActionManager::Extras extras;
			const bool ok = actions.ImportFrom(result.path, &extras);
			if (ok)
			{
				ApplyTrainingExtras(scr, extras);
			}
			g_trainingFileStatusIsError = !ok;
			g_trainingFileStatus = ok
				? FormatText(L("Loaded %s: %d trigger(s)").c_str(), result.path.c_str(), actions.AssignedCount())
				: FormatText(L("Could not read %s").c_str(), result.path.c_str());
			LOG(1, "[TrainingFile] import %s <- '%s'\n", ok ? "ok" : "FAILED", result.path.c_str());
		}
	}

	void DrawTrainingFileSection(ScrWindow* scr)
	{
		PumpTrainingFilePick(scr);

		MainMenu::Hint(L("Everything on this page in one file - the dummy actions, positions and timings, and the input delay - so a setup can be sent to someone and loaded in one go."));

		if (ImGui::Button(L("Export...").c_str()))
		{
			BeginTrainingFilePick(true);
		}
		ImGui::ShowHelpMarkerSameLine(L("Writes the current setup to a file you choose.").c_str());
		ImGui::SameLine(0.0f, 12.0f);
		if (ImGui::Button(L("Import...").c_str()))
		{
			BeginTrainingFilePick(false);
		}
		ImGui::ShowHelpMarkerSameLine(L("Replaces the current setup with the one in a file. Captured recordings travel inside the file; a playback file or CF slot an action points at has to exist on this machine.").c_str());
		ImGui::SameLine(0.0f, 12.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.45f, 0.45f, 1.0f));
		const bool resetPressed = ImGui::Button(L("Reset all to defaults").c_str());
		ImGui::PopStyleColor();
		ImGui::ShowHelpMarkerSameLine(L("Clears every dummy action and puts every setting on this page back to how a fresh install has it.").c_str());
		if (resetPressed)
		{
			ImGui::OpenPopup(L("Reset the whole training page?").c_str());
		}

		if (!g_trainingFileStatus.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, g_trainingFileStatusIsError
				? ImVec4(0.90f, 0.45f, 0.45f, 1.0f) : ImVec4(0.58f, 0.58f, 0.58f, 1.0f));
			ImGui::TextWrapped("%s", g_trainingFileStatus.c_str());
			ImGui::PopStyleColor();
		}

		// Centred on the frame it appears; see docs/ImGuiModalCentering.md.
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (ImGui::BeginPopupModal(L("Reset the whole training page?").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted(L("This removes every dummy action and resets positions, wake-up timing and input delay. Export first if you want to keep this setup.").c_str());
			ImGui::VerticalSpacing(6);
			if (ImGui::Button(L("Reset all to defaults").c_str()))
			{
				ResetTrainingToDefaults(scr);
				g_trainingFileStatus = L("Everything is back to defaults.");
				g_trainingFileStatusIsError = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button(L("Cancel").c_str()))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
}

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

		// The whole page as a file, for sharing a setup or wiping it. Last of the sections
		// because it acts on all of the ones above it.
		if (BeginSection(Training_File, true))
		{
			DrawTrainingFileSection(scr);
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
