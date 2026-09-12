#include "ScrWindow.h"

#include "Overlay/Widget/HotkeyBindWidget.h"

#include "Overlay/Window/TasWindow.h"
#include "Game/Scr/ScrStateNames.h"
#include "Overlay/Window/DummyActionsPanel.h"
#include "Game/Playbacks/DummyActionManager.h"

#include "Core/HotkeyManager.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/NativeFileDialog.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Game/ReplayStates/FrameState.h"
#include "Game/ReplayFiles/ReplayFile.h"
#include "Game/ReplayFiles/ReplayList.h"
#include "Game/ReplayFiles/ReplayFileManager.h"
#include "Game/Menus/TrainingSetupMenu.h"
#include "Game/ScenesManager/ScenesManager.h"
#include "Game/TasManager.h"
#include "Overlay/NotificationBar/NotificationBar.h"
#include "Overlay/WindowManager.h"
#include "Overlay/Window/HitboxOverlay.h"
#include "Overlay/imgui_utils.h"
#include "Psapi.h"
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <array>
#include "Core/info.h"
#include <windows.h>
#include "Game/Playbacks/PlaybackManager.h"
#include "Game/Playbacks/UnlimitedPlaybackManager.h"
#include "Game/ReplayTakeover/ReplayTakeoverFeatureFlags.h"
#include "Hooks/hooks_battle_input.h"
#include "Hooks/hooks_system_input.h"
#include "Core/logger.h"
#include "Overlay/imgui_utils.h"
#include <cstdlib>
#include <ctime>
#include <algorithm>




void ScrWindow::Draw()
{
    if (m_showDemoWindow)
    {
        ImGui::ShowDemoWindow(&m_showDemoWindow);
    }
    // Everything this window used to host now lives in the mod menu's Training, Replays and
    // Online pages, which call the section bodies below directly. The window itself is kept
    // only as the owner of that state; nothing opens it any more.
    ImGui::TextWrapped("%s", L("These tools moved into the mod menu.").c_str());
}
void ScrWindow::DrawComboDataButton() {
    if (ImGui::Button("Combo Data"))
    {
        ScrWindow::m_pWindowContainer->GetWindow(WindowType_ComboData)->ToggleOpen();
    }
    ImGui::ShowHelpMarkerSameLine(Messages.Combo_data_button_tooltip());
}
void ScrWindow::DrawReplayPlaybackCaptureBody(const char* idScope, bool compact)
{
    static const char* kToken = "ScrWindowReplayCapture";
    // Visible label before ##, host-unique id after it, so two hosts drawing this in one
    // frame do not both claim the same modal.
    const std::string whosePopupId = L("Capture whose inputs?") + std::string("##") + (idScope ? idScope : "");
    static std::vector<char> s_captured;
    static char s_capturedFacing = 0;
    static std::string s_status;

    UnlimitedPlaybackManager& mgr = UnlimitedPlaybackManager::Instance();

    const bool inReplayMatch =
        g_gameVals.pGameMode && g_gameVals.pGameState &&
        (*g_gameVals.pGameMode == GameMode_ReplayTheater) &&
        (*g_gameVals.pGameState == GameState_InMatch);
    const bool busy = NativeFileDialog::IsOpen();

    const std::string captureTip =
        L("Records one player's inputs from the replay as it plays, then saves them as a playback file. Start it where you want the capture to begin and stop it where you want it to end.");

    if (!mgr.IsReplayRecording())
    {
        ImGui::BeginDisabled(!inReplayMatch || busy);
        if (ImGui::Button(L("Capture playback from replay").c_str()))
        {
            ImGui::OpenPopup(whosePopupId.c_str());
        }
        ImGui::EndDisabled();

        // One row in a compact host. The (?) is still there - without it nothing says the
        // explanation exists - but the "(while watching a replay)" aside is dropped, because
        // that is what the greyed-out button already says.
        if (compact)
        {
            ImGui::HoverTooltipEvenDisabled(inReplayMatch
                ? captureTip.c_str()
                : L("Open a replay to capture a stretch of it.").c_str());
            ImGui::ShowHelpMarkerSameLine(captureTip.c_str());
        }
        else
        {
            ImGui::ShowHelpMarkerSameLine(captureTip.c_str());

            if (!inReplayMatch)
            {
                const std::string note = L("(while watching a replay)");
                ImGui::SameLineOrWrap(ImGui::CalcTextSize(note.c_str()).x);
                ImGui::TextDisabled("%s", note.c_str());
            }
        }

        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (display.x > 0.0f && display.y > 0.0f)
        {
            ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        }
        if (ImGui::BeginPopupModal(whosePopupId.c_str(), nullptr,
                ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(
                L("Recording starts now and runs until you stop it.").c_str());
            ImGui::VerticalSpacing(6);
            if (ImGui::Button(L("Player 1").c_str()))
            {
                if (mgr.StartReplayRecording(true)) { s_status.clear(); }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(L("Player 2").c_str()))
            {
                if (mgr.StartReplayRecording(false)) { s_status.clear(); }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(L("Cancel").c_str()))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (!compact && !s_status.empty())
        {
            ImGui::TextDisabled("%s", s_status.c_str());
        }
    }
    else
    {
        if (compact)
        {
            // Row 1 of two: what is being recorded, and since when.
            ImGui::TextColored(ImVec4(1.00f, 0.45f, 0.45f, 1.00f), "%s", L("Capturing").c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", FormatText(L("%s, from frame %d").c_str(),
                mgr.IsReplayRecordingAsP1() ? L("Player 1").c_str() : L("Player 2").c_str(),
                mgr.GetReplayRecordingStartFrame()).c_str());
        }
        else
        {
            ImGui::TextDisabled(L("Recording %s inputs from frame %d...").c_str(),
                mgr.IsReplayRecordingAsP1() ? "P1" : "P2",
                mgr.GetReplayRecordingStartFrame());
        }

        ImGui::BeginDisabled(busy);
        if (ImGui::Button(L("Stop and Save...").c_str()))
        {
            // Stopped here, not after the picker answers: the replay keeps playing while a
            // dialog is up, so the captured range would grow by however long the user spent
            // typing a filename.
            if (mgr.StopReplayRecordingToBuffer(&s_captured, &s_capturedFacing))
            {
                NativeFileDialog::Request request;
                request.save = true;
                request.title = L("Save the captured playback");
                request.filters.push_back({ L("Playback file"), "*.playback" });
                request.defaultExtension = "playback";
                request.initialPath = "replay_capture.playback";
                if (!NativeFileDialog::Open(kToken, request))
                {
                    s_status = L("Could not open the save dialog. The capture was kept.");
                }
            }
            else
            {
                s_status = L("Nothing was captured.");
            }
        }
        ImGui::EndDisabled();
        if (compact)
        {
            ImGui::ShowHelpMarkerSameLine(
                L("Stops recording and asks where to save the captured inputs as a playback file.").c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button(L("Cancel").c_str()))
        {
            mgr.CancelReplayRecording();
            s_captured.clear();
        }
        if (compact)
        {
            ImGui::ShowHelpMarkerSameLine(
                L("Throws the capture away and stops recording.").c_str());
        }
    }

    // The picker answers on a later frame; the capture is already in hand by then.
    NativeFileDialog::Result result;
    if (NativeFileDialog::Consume(kToken, &result))
    {
        if (result.accepted && !s_captured.empty())
        {
            if (PlaybackManager::save_playback_to_path(result.path, s_captured, s_capturedFacing))
            {
                s_status = FormatText(L("Saved %u frames.").c_str(),
                    static_cast<unsigned int>(s_captured.size()));
                LOG(1, "[ReplayCapture] wrote %u frames to '%s'\n",
                    static_cast<unsigned int>(s_captured.size()), result.path.c_str());
            }
            else
            {
                s_status = L("That playback could not be saved.");
                LOG(2, "[ReplayCapture] failed to write '%s'\n", result.path.c_str());
            }
        }
        else if (!result.accepted)
        {
            s_status = L("Save cancelled - the capture was discarded.");
        }
        s_captured.clear();
    }
}

void ScrWindow::DrawPlaybackTransferButtons()
{
    // Owner token for this pair of pickers, so a result here is never confused with the
    // library window's own dialogs.
    static const char* kToken = "ScrWindowPlaybackTransfer";
    // contextId, handed back with the result, says which of the two flows answered.
    const int kImportPick = 0;
    const int kExportPick = 1;

    static int s_slot = 1;
    static bool s_openImportSlot = false;
    static bool s_openExportSlot = false;
    static bool s_openEditSlot = false;
    static std::vector<char> s_importFrames;
    static char s_importFacing = 0;
    static std::string s_importName;
    static std::string s_status;

    const bool inTraining = g_gameVals.pGameMode && *g_gameVals.pGameMode == GameMode_Training;
    const bool busy = NativeFileDialog::IsOpen();

    ImGui::BeginDisabled(!inTraining || busy);
    if (ImGui::Button(L("Import Playback").c_str()))
    {
        NativeFileDialog::Request request;
        request.title = L("Choose a playback file to import");
        request.filters.push_back({ L("Playback file"), "*.playback" });
        request.defaultExtension = "playback";
        request.contextId = kImportPick;
        NativeFileDialog::Open(kToken, request);
    }
    ImGui::EndDisabled();
    ImGui::ShowHelpMarkerSameLine(
        L("Load a playback file into one of the game's four recording slots, so you can play it back from the training menu like anything you recorded yourself.").c_str());

    ImGui::SameLine();
    ImGui::BeginDisabled(!inTraining || busy);
    if (ImGui::Button(L("Export Playback").c_str()))
    {
        s_openExportSlot = true;
    }
    ImGui::EndDisabled();
    ImGui::ShowHelpMarkerSameLine(
        L("Write one of the four recording slots out to a playback file, which can then be imported anywhere else - a dummy action, a library, or another slot.").c_str());

    ImGui::SameLine();
    ImGui::BeginDisabled(!inTraining || busy);
    if (ImGui::Button(L("Edit Playback").c_str()))
    {
        s_openEditSlot = true;
    }
    ImGui::EndDisabled();
    ImGui::ShowHelpMarkerSameLine(
        L("Open one of the four recording slots in the frame editor, where you can retype, insert, delete and reorder single frames. Saving there writes straight back into the slot.").c_str());

    if (!inTraining)
    {
        ImGui::TextDisabled("%s", L("(training mode only)").c_str());
    }
    else if (!s_status.empty())
    {
        ImGui::TextDisabled("%s", s_status.c_str());
    }

    // Both pickers answer here, on a later frame, because the dialog runs on its own thread.
    NativeFileDialog::Result result;
    if (NativeFileDialog::Consume(kToken, &result) && result.accepted)
    {
        if (result.contextId == kImportPick)
        {
            // Read and checked BEFORE asking which slot, so an unreadable file is refused
            // rather than being turned into a slot choice that then fails.
            if (PlaybackManager::load_playback_from_path(result.path, &s_importFrames, &s_importFacing))
            {
                const size_t at = result.path.find_last_of("/\\");
                s_importName = at == std::string::npos ? result.path : result.path.substr(at + 1);
                s_openImportSlot = true;
                s_status.clear();
            }
            else
            {
                s_status = L("That file could not be read as a playback.");
                LOG(2, "[PlaybackTransfer] import failed to read '%s'\n", result.path.c_str());
            }
        }
        else if (result.contextId == kExportPick)
        {
            std::vector<char> trimmed = playback_manager.slots[s_slot - 1].get_slot_buffer();
            const char facing = playback_manager.slots[s_slot - 1].get_facing_direction();
            if (PlaybackManager::save_playback_to_path(result.path, trimmed, facing))
            {
                s_status = FormatText(L("Exported slot %d, %u frames.").c_str(),
                    s_slot, static_cast<unsigned int>(trimmed.size()));
            }
            else
            {
                s_status = L("That playback could not be saved.");
                LOG(2, "[PlaybackTransfer] export failed to write '%s'\n", result.path.c_str());
            }
        }
    }

    const auto drawSlotChooser = [](const char* title, const char* prompt, int* slot) -> int {
        // Returns 1 to go ahead, -1 to cancel, 0 while still open.
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (display.x > 0.0f && display.y > 0.0f)
        {
            ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        }
        if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return 0;
        }
        int answer = 0;
        ImGui::TextUnformatted(prompt);
        ImGui::VerticalSpacing(4);
        for (int i = 1; i <= 4; ++i)
        {
            if (i > 1) { ImGui::SameLine(); }
            if (ImGui::RadioButton(FormatText(L("Slot %d").c_str(), i).c_str(), *slot == i))
            {
                *slot = i;
            }
        }
        ImGui::VerticalSpacing(6);
        if (ImGui::Button(L("OK").c_str()))
        {
            answer = 1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(L("Cancel").c_str()))
        {
            answer = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return answer;
    };

    const std::string importTitle = L("Import into which slot?");
    if (s_openImportSlot)
    {
        ImGui::OpenPopup(importTitle.c_str());
        s_openImportSlot = false;
    }
    const int importAnswer = drawSlotChooser(importTitle.c_str(),
        s_importName.empty() ? L("Choose a slot.").c_str()
                             : FormatText(L("Loading '%s'.").c_str(), s_importName.c_str()).c_str(),
        &s_slot);
    if (importAnswer == 1)
    {
        playback_manager.load_into_slot(s_importFrames, s_importFacing, s_slot);
        s_status = FormatText(L("Imported '%s' into slot %d, %u frames.").c_str(),
            s_importName.c_str(), s_slot, static_cast<unsigned int>(s_importFrames.size()));
        LOG(1, "[PlaybackTransfer] imported '%s' into slot %d (%u frames)\n",
            s_importName.c_str(), s_slot, static_cast<unsigned int>(s_importFrames.size()));
        s_importFrames.clear();
    }
    else if (importAnswer == -1)
    {
        s_importFrames.clear();
    }

    // Editing asks for the slot and then hands it to the frame editor - the same editor the
    // TAS tool uses, switched to editing plain recording data.
    const std::string editTitle = L("Edit which slot?");
    if (s_openEditSlot)
    {
        ImGui::OpenPopup(editTitle.c_str());
        s_openEditSlot = false;
    }
    if (drawSlotChooser(editTitle.c_str(), L("Choose a slot to edit.").c_str(), &s_slot) == 1)
    {
        if (auto* tas = ScrWindow::m_pWindowContainer->GetWindow<TasWindow>(WindowType_Tas))
        {
            // No status line on success: the editor window appearing is the feedback, and a
            // note saying what you are already looking at outlives the window that caused it.
            s_status = tas->OpenPlaybackSlot(s_slot) ? std::string()
                : L("That slot could not be opened for editing.");
        }
    }

    // Export asks for the slot first, then where to put it.
    const std::string exportTitle = L("Export which slot?");
    if (s_openExportSlot)
    {
        ImGui::OpenPopup(exportTitle.c_str());
        s_openExportSlot = false;
    }
    if (drawSlotChooser(exportTitle.c_str(), L("Choose a slot to write out.").c_str(), &s_slot) == 1)
    {
        NativeFileDialog::Request request;
        request.save = true;
        request.title = L("Save the playback file");
        request.filters.push_back({ L("Playback file"), "*.playback" });
        request.defaultExtension = "playback";
        request.initialPath = FormatText("slot%d.playback", s_slot);
        request.contextId = kExportPick;
        NativeFileDialog::Open(kToken, request);
    }
}

void ScrWindow::DrawTasComboToolButton() {
    TasManager& tas = TasManager::Instance();

    // Greyed out off training, like everything else on the page. TasManager::Enter does
    // refuse and explain itself, but a button that looks usable and then tells you off is
    // worse than one that plainly is not available yet. Exiting stays enabled, so a tool
    // left running cannot be stranded by walking out of training.
    const bool inTraining = g_gameVals.pGameMode && *g_gameVals.pGameMode == GameMode_Training;
    ImGui::BeginDisabled(!inTraining && !tas.IsActive());
    if (ImGui::Button(tas.IsActive() ? L("Exit TAS Combo tool").c_str()
                                     : L("TAS Combo tool").c_str()))
    {
        if (tas.IsActive())
        {
            tas.Exit();
            ScrWindow::m_pWindowContainer->GetWindow(WindowType_Tas)->Close();
        }
        else
        {
            tas.Enter();
            if (tas.IsActive())
            {
                ScrWindow::m_pWindowContainer->GetWindow(WindowType_Tas)->Open();
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::ShowHelpMarkerSameLine(L("Frame-by-frame combo editor: build a combo one input at a time, rewind and re-record any part of it, then play the whole thing back. Training mode only.").c_str());

    if (!tas.IsActive() && !tas.GetError().empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", tas.GetError().c_str());
    }
}

void ScrWindow::DrawInputBufferButton() {
    if (ImGui::Button("Input Buffer P1"))
    {
        ScrWindow::m_pWindowContainer->GetWindow(WindowType_InputBufferP1)->ToggleOpen();
    }
    ImGui::ShowHelpMarkerSameLine(Messages.Input_buffer_button_tooltip());
    ImGui::SameLineOrWrap(ImGui::ButtonWidth("Input Buffer P2"));
    if (ImGui::Button("Input Buffer P2"))
    {
        ScrWindow::m_pWindowContainer->GetWindow(WindowType_InputBufferP2)->ToggleOpen();
    }
    ImGui::ShowHelpMarkerSameLine(Messages.Input_buffer_button_tooltip());
}
void ScrWindow::DrawWakeupDelayControl() {
    //ImGui::BeginChild("zbmjxc");
    const char* items[] = { "Disabled", "Neutral", "Forward", "Backward", "Quick", "Random" };
    
    static int32_t wakeup_delay_current_item = 0;
    //gets the start of the fourth page of training setup menu
    ImGui::BeginChild("wakeup_delay_child##wakeup_delay", ImVec2(160, 30));
    ImGui::Text("Delay: ");
    ImGui::SameLine();
    if (ImGui::InputInt("##wakeup_delay", &(ScrWindow::wakeup_delay))) {

        if (ScrWindow::wakeup_delay > 39) {
            ScrWindow::wakeup_delay = 39;
        }
        if (ScrWindow::wakeup_delay < 0) {
            ScrWindow::wakeup_delay = 0;
        }
    }
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Wakeup_control_delay_tooltip());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("wakeup_skew_child##wakeup_delay", ImVec2(263, 30));
    ImGui::Text("Skew range: ");
    ImGui::SameLine();
    if (ImGui::InputInt("##wakeup_skew", &(ScrWindow::wakeup_delay_skew))) {
        ScrWindow::wakeup_delay_skew_change_flag = true;
        if (ScrWindow::wakeup_delay_skew > 39) {
            ScrWindow::wakeup_delay_skew = 39;
        }
        if (ScrWindow::wakeup_delay_skew < -39) {
            ScrWindow::wakeup_delay_skew = -39;
        }
    };
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Wakeup_control_skew_tooltip());
    ImGui::EndChild();
    //ImGui::SameLine(); 
    //ImGui::HorizontalSpacing(30);
    //ImGui::BeginChild("wakeup_type_child##wakeup_delay", ImVec2(190, 30));
    ImGui::Text("Wake-up: ");
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Wakeup_type_selection_tooltip());
    ImGui::SameLine();
    ImGui::BeginChild("tst##wakeup_delay", ImVec2(263, 100));
    static bool selected[6] = {};
    for (int i = 0; i < 6; i++)
    {
        char label[15];
        sprintf(label, items[i]);
        //if (i % 2) { ImGui::NextColumn(); ImGui::SameLine(); }  //ImGui::NextColumn(); }
        ImGui::Selectable(label, &selected[i]); // FIXME-TABLE: Selection overlap
    }
    //I should probably make it so that all cases are random tbh
    std::vector<int> selected_index = {};
    for (int i = 0; i < 6; i++) {
        if (selected[i]) {
            selected_index.push_back(i);
        }
    }
    if (!selected_index.empty()) {
        ScrWindow::wakeup_type = selected_index[std::rand() % selected_index.size()];
    }
    ImGui::EndChild();

   // if (ImGui::Combo("##wakeup_delay", &wakeup_delay_current_item, items, IM_ARRAYSIZE(items)))
  //  {
   //     ScrWindow::wakeup_type= wakeup_delay_current_item;
  //  }
    //ImGui::EndChild();
    
  
    //ImGui::SameLine();
    
    
    
    //ImGui::EndChild();

}
void ScrWindow::check_wakeup_delay() {
    TrainingSetupMenu* menu = (TrainingSetupMenu*)(ScrWindow::bbcf_base_adress + 0x902BDC);
    menu->emergency_roll = 0;
    auto current_action = std::string(g_interfaces.player2.GetData()->currentAction);
    auto action_time = g_interfaces.player2.GetData()->actionTime;
    /*finds the random "skew" both for positiveand negative "skews".Yes I know this doesnt fit 
    the true statistical definition of skew but I couldn't think of a better name fuck off*/
    static int wakeup_delay_random_skew = 0;
    //this is necessary to make sure people dont get stuck in unreacheable action_times when changing the skew
    if (ScrWindow::wakeup_delay_skew_change_flag) {
        wakeup_delay_random_skew = 0;
        ScrWindow::wakeup_delay_skew_change_flag = false;
    }
    
    if (ScrWindow::wakeup_delay_skew && !wakeup_delay_random_skew) {
        wakeup_delay_random_skew = (std::rand() % std::abs(ScrWindow::wakeup_delay_skew))
                                                            * (ScrWindow::wakeup_delay_skew / std::abs(ScrWindow::wakeup_delay_skew));
    }
    //searches for CmnActFDownLoop and CmnActBDownLoop, this state after 10f is the one that allows for non emergency tech action
    auto wakeup_action_trigger_find = current_action.find("DownLoop");
    if (ScrWindow::wakeup_type
        && wakeup_action_trigger_find != std::string::npos
        && action_time == 10 + ScrWindow::wakeup_delay + wakeup_delay_random_skew) {
        menu->wake_up = wakeup_type;
        wakeup_delay_random_skew = 0;
    }
    else {
        menu->wake_up = 0;
    }

}
// Body of "Training > Wake-up". The enable flag is a class member rather than a local
// static so Tick() can keep enforcing the override every frame: the section used to be
// permanently on screen inside the old States window, so nobody noticed that it only
// applied while it was being drawn. Now that it lives behind a menu page, it has to work
// with the menu closed.
bool ScrWindow::s_wakeupOverrideEnabled = false;

void ScrWindow::DrawWakeupBody() {
    if (!g_gameVals.pGameMode || *g_gameVals.pGameMode != GameMode_Training
        || g_interfaces.player2.IsCharDataNullPtr()) {
        ImGui::TextDisabled("%s", L("Load into training mode to use this.").c_str());
        return;
    }

    ImGui::CheckboxWrapped(L("Override the dummy's wake-up timing").c_str(), &s_wakeupOverrideEnabled);
    ImGui::ShowHelpMarkerSameLine(Messages.Enable_wakeup_delay_override_tooltip());
    if (s_wakeupOverrideEnabled) {
        DrawWakeupDelayControl();
    }
}

// Per-frame work that used to happen as a side effect of drawing the States window, and so
// must not depend on any menu page being visible.
void ScrWindow::Tick() {
    // The dummy's random wake-up/gap picks use std::rand; this used to be seeded the first
    // time the States window drew itself, which no longer happens.
    static bool random_seeded = false;
    if (!random_seeded) {
        std::srand(static_cast<unsigned int>(std::time(nullptr)));
        random_seeded = true;
    }

    WindowContainer* container = WindowManager::GetInstance().GetWindowContainer();
    if (!container)
        return;

    ScrWindow* self = container->GetWindow<ScrWindow>(WindowType_Scr);
    if (!self)
        return;

    if (self->s_wakeupOverrideEnabled
        && g_gameVals.pGameMode && *g_gameVals.pGameMode == GameMode_Training
        && !g_interfaces.player1.IsCharDataNullPtr()
        && !g_interfaces.player2.IsCharDataNullPtr()) {
        self->check_wakeup_delay();
    }

    // Only the countdown runs here. The hotkeys are polled from WindowManager::HandleButtons
    // instead, because WasPressed is an edge computed by HotkeyManager::Update and this
    // function runs before it - and, worse, keeps running when Render() bails out for the
    // Steam overlay or a minimized window, which would leave a stale press edge re-firing
    // every frame. The countdown does belong here: it clears isFrameFrozen, so it has to
    // keep advancing exactly in those cases.
    self->TickSetupDelay();
    self->TickDummyActions();
    self->TickReplayTakeover();

    TickLocalReplayRedirect();
}

// The save-state controls live on a mod-menu page. Binding their hotkeys to the buttons
// meant a bind only worked while that page was on screen, and the setup-delay countdown -
// the thing that clears isFrameFrozen - only advanced while it was being drawn, so closing
// the menu mid-delay left the game frozen. Both now run from Tick() every frame.

SnapshotApparatus* ScrWindow::EnsureTrainingSnapshot()
{
    if (snap_apparatus == nullptr) {
        snap_apparatus = new SnapshotApparatus();
        snap_apparatus->ReserveSlots("training_states", 1);
    }
    else if (!snap_apparatus->check_if_valid(g_interfaces.player1.GetData(),
        g_interfaces.player2.GetData())) {
        delete snap_apparatus;
        snap_apparatus = new SnapshotApparatus();
        snap_apparatus->ReserveSlots("training_states", 1);
    }
    return snap_apparatus;
}

bool ScrWindow::HasTrainingSnapshot() const
{
    return snap_apparatus && snap_apparatus->snapshot_count != 0;
}

bool ScrWindow::HasReplayTakeoverSnapshot() const
{
    return snap_apparatus_takeover && snap_apparatus_takeover->snapshot_count > 0;
}

void ScrWindow::BeginSetupDelay(float seconds)
{
    if (seconds <= 0) {
        return;
    }
    g_gameVals.isFrameFrozen = true;
    is_setup_time_running = true;
    base_time = seconds;
    setup_delay_total = seconds;
    setup_delay_last_tick = GetTickCount64();
}

void ScrWindow::TickSetupDelay()
{
    if (!is_setup_time_running) {
        return;
    }

    // Wall-clock rather than ImGui's frame delta: this runs outside the draw pass, where
    // DeltaTime belongs to whatever frame ImGui last built. The game is frozen while the
    // delay runs, so a frame-count clock would not advance either.
    const unsigned long long now = GetTickCount64();
    if (setup_delay_last_tick != 0 && now > setup_delay_last_tick) {
        base_time -= (now - setup_delay_last_tick) / 1000.0f;
    }
    setup_delay_last_tick = now;

    if (base_time < 0) {
        g_gameVals.isFrameFrozen = false;
        is_setup_time_running = false;
    }
}

bool ScrWindow::GetSetupDelayCountdown(float* remaining, float* total) const
{
    if (!is_setup_time_running || setup_delay_total <= 0) {
        return false;
    }
    if (remaining) { *remaining = base_time; }
    if (total) { *total = setup_delay_total; }
    return true;
}

void DrawSaveStateSetupDelayStandalone()
{
    WindowContainer* container = WindowManager::GetInstance().GetWindowContainer();
    if (!container) {
        return;
    }
    ScrWindow* scr = container->GetWindow<ScrWindow>(WindowType_Scr);
    if (!scr) {
        return;
    }

    float remaining = 0.0f;
    float total = 0.0f;
    if (!scr->GetSetupDelayCountdown(&remaining, &total)) {
        return;
    }

    // Not a modal. A focused ImGui window sets WantCaptureKeyboard, which freezes the game's
    // keyboard state, and the whole point of this pause is to get your hands into position.
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.3f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("##savestate_setup_delay", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::ProgressBar(remaining / total, ImVec2(360.0f, 0.0f));
        ImGui::Text("%s", FormatText(L("Setup time: %.1fs").c_str(), remaining).c_str());
    }
    ImGui::End();
}

void ScrWindow::SaveTrainingState()
{
    if (*(bbcf_base_adress + 0x8F7758) != 0) {
        LOG(2, "[SaveState] save refused: searching for a ranked match\n");
        return;
    }
    if (g_interfaces.player1.IsCharDataNullPtr() || g_interfaces.player2.IsCharDataNullPtr()) {
        LOG(2, "[SaveState] save refused: char data not available\n");
        return;
    }
    LOG(2, "[SaveState] saving\n");

    SnapshotApparatus* apparatus = EnsureTrainingSnapshot();
    if (apparatus) {
        apparatus->save_snapshot(0);
    }
}

void ScrWindow::LoadTrainingState()
{
    if (*(bbcf_base_adress + 0x8F7758) != 0) {
        return;
    }
    if (g_interfaces.player1.IsCharDataNullPtr() || g_interfaces.player2.IsCharDataNullPtr()) {
        return;
    }
    if (!HasTrainingSnapshot()) {
        LOG(2, "[SaveState] load refused: nothing saved yet\n");
        return;
    }

    LOG(2, "[SaveState] loading\n");
    SnapshotApparatus* apparatus = EnsureTrainingSnapshot();
    if (apparatus) {
        apparatus->load_snapshot(0);
    }
    BeginSetupDelay(wait_before_exec_s);
}

void ScrWindow::LoadReplayTakeoverState()
{
    if (!g_gameVals.pGameMode || *g_gameVals.pGameMode != GameMode_Training) {
        return;
    }
    if (!HasReplayTakeoverSnapshot()) {
        return;
    }
    if (g_interfaces.player1.IsCharDataNullPtr() || g_interfaces.player2.IsCharDataNullPtr()) {
        return;
    }

    snap_apparatus_takeover->load_snapshot(0);

    playback_manager.load_into_slot(replay_action_load, facing_left_replay_takeover, 1);
    playback_manager.set_active_slot(1);
    playback_manager.set_playback_type(0); //forces playback type to be "normal" instead of "random"
    playback_manager.set_playback_position(0); //makes sure the playback is in frame zero
    playback_manager.set_playback_control(3); //activates the playback
    BeginSetupDelay(wait_before_exec_s2);
}

void ScrWindow::TickSaveStateHotkeys()
{
    // Latch only. See the header for why the work is not done from here.
    if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_SaveState)) {
        pending_save_state = true;
        LOG(2, "[SaveState] save requested by hotkey\n");
    }
    if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_LoadState)) {
        pending_load_state = true;
        LOG(2, "[SaveState] load requested by hotkey\n");
    }
    if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_LoadReplayState)) {
        pending_load_replay_state = true;
        LOG(2, "[SaveState] replay-takeover load requested by hotkey\n");
    }
}

void ScrWindow::RunPendingSaveStateRequests()
{
    // First: it ends with a state load of its own, which the block below then runs.
    if (pending_takeover_reconfigure >= 0) {
        const bool asP1 = pending_takeover_reconfigure == 0;
        pending_takeover_reconfigure = -1;
        ReconfigureReplayTakeover(asP1);
    }
    if (pending_save_state) {
        pending_save_state = false;
        SaveTrainingState();
    }
    if (pending_load_state) {
        pending_load_state = false;
        LoadTrainingState();
    }
    if (pending_load_replay_state) {
        pending_load_replay_state = false;
        LoadReplayTakeoverState();
    }
}

bool ScrWindow::s_swapCoordsToggle = false;

namespace {

// True while either battle input slot is holding an up direction (packed digits 7/8/9).
//
// Both slots, not just P1: the detection this replaces OR-ed every controller object the
// game owns, so a player sitting on the P2 side could already toggle the swap. Reading
// both keeps that working and stays strictly narrower than the old behaviour, so nobody
// this feature works for today loses it.
bool IsAnyPlayerHoldingUp()
{
    if (!IsBattleInputHookInstalled()) {
        // Without the battle-input hook there is no trustworthy source for the player's
        // direction, and the action words are exactly the thing that misreports it. Fail
        // closed - the "Swap sides on every reset" checkbox still works by hand.
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG(1, "[ResetSwap] battle input hook unavailable - Up+reset toggle disabled\n");
        }
        return false;
    }

    return GetLastObservedBattleInput(0).up || GetLastObservedBattleInput(1).up;
}

} // namespace

void ScrWindow::TickTrainingResetSwap() {
    // "Hold Up while pressing the reset bind" toggles the always-swap ("P2 mode")
    // checkbox, mirroring how the native reset treats directions: only what's held at
    // the PRESS moment matters, releasing during the fadeout is fine.
    //
    // The reset press itself is read from the game's own keyconfig-resolved logical
    // action words (PollTrainingResetPressed - reset action edge bit, identified via
    // the [ResetProbe] runs of 2026-07-19). This fires on the exact press frame, for
    // any rebound key or controller button.
    //
    // "Up" is NOT read from those action words. Their Up bit lies on a keyboard: the
    // captured controller set includes the two GAMESTEAM_SystemKeyControler objects
    // (sysMgr+0x0C/+0x18), whose Up action is fed by BBCF's second key config set -
    // untouched, and therefore still default W/D/S/A, for anyone who only rebound the
    // other one - plus the hardcoded VK_SPACE and VK_UP fallbacks in FUN_00469750.
    // Someone playing a Q/W/E+Space layout toggles this on every single reset, because
    // their "down" key is the other config set's "up". See the SystemKeyControler block
    // in GhidraDefs.h. So take the direction from the packed battle input word instead:
    // that is the player's real, in-match direction under their own battle key config,
    // identical for pad and keyboard, and it never carries the menu-side bits.
    //
    // The toggle is then applied only when the reset actually executes (frame counter
    // drops), armed for a few seconds from the press: this keeps the visual change in
    // sync with the reset, and discards phantom presses (e.g. if some controller
    // object shares the action bit with pause, pausing never drops the counter, so an
    // armed press simply expires).
    //
    // Driven from EndScene (every rendered frame - the battle-frame-counter hook goes
    // idle during the reset limbo). EndScene also fires before the game resolves
    // g_gameVals' pointers (null until then), hence the guards.
    if (!g_gameVals.pGameMode || !g_gameVals.pFrameCount) {
        return;
    }

    if (*g_gameVals.pGameMode != GameMode_Training) {
        return;
    }

    static int last_frame_count = -1;
    static ULONGLONG armed_at_ms = 0;
    static bool armed_up = false;

    int current_frame_count = *g_gameVals.pFrameCount;

    if (PollTrainingResetPressed()) {
        armed_at_ms = GetTickCount64();
        armed_up = IsAnyPlayerHoldingUp();
    }

    if (last_frame_count != -1 && current_frame_count < last_frame_count) {
        // The reset actually executed. Honor the Up state captured at press time.
        constexpr ULONGLONG kArmWindowMs = 3000;
        if (armed_at_ms != 0 && GetTickCount64() - armed_at_ms <= kArmWindowMs && armed_up) {
            s_swapCoordsToggle = !s_swapCoordsToggle;
            LOG(1, "[ResetSwap] Up+reset detected - always-swap-coordinates toggled %s\n",
                s_swapCoordsToggle ? "ON" : "OFF");
        }
        armed_at_ms = 0;
        armed_up = false;
    }

    last_frame_count = current_frame_count;

    if (s_swapCoordsToggle && current_frame_count == 5) {
        ScrWindow::swap_character_coordinates();
    }
}

void ScrWindow::swap_character_coordinates() {
    CharData* p1 = g_interfaces.player1.GetData();
    CharData* p2 = g_interfaces.player2.GetData();
    auto posx1 = p1->position_x;
    auto posy1 = p1->position_y;
    p1->facingLeft = !p1->facingLeft;
    p1->position_x = p2->position_x;
    p1->position_y = p2->position_y;
    p2->position_x = posx1;
    p2->position_y = posy1;
    p2->facingLeft = !p2->facingLeft;
}
bool find_substring_in_vector(std::vector<std::string> string_vector, std::string substr) {
    for (auto& str : string_vector) {
        if (substr.find(str) != std::string::npos) {
            return true;
        }
    };
    return false;
}
void ScrWindow::DrawPositionsBody()
{
    if (!g_gameVals.pGameMode || *g_gameVals.pGameMode != GameMode_Training) {
        ImGui::TextDisabled("%s", L("Load into training mode to use this.").c_str());
        return;
    }

    if (ImGui::Button(L("Swap sides now").c_str())) {
        ScrWindow::swap_character_coordinates();
    }
    ImGui::ShowHelpMarkerSameLine(Messages.Swap_coordinates_tooltip());
    const std::string alwaysSwapLabel = L("Swap sides on every reset");
    ImGui::SameLineOrWrap(ImGui::CheckboxWidth(alwaysSwapLabel.c_str()));
    ImGui::CheckboxWrapped(alwaysSwapLabel.c_str(), &s_swapCoordsToggle);
    ImGui::ShowHelpMarkerSameLine(L("Always swap coordinates help").c_str());
}

void ScrWindow::SyncAnimationRegistersFromActions()
{
    DummyActionManager& actions = DummyActionManager::Instance();

    struct Wiring {
        DummyActionManager::TriggerType trigger;
        std::vector<scrState*>* pool;
        std::vector<int>* delays;
    };
    const Wiring wiring[] = {
        { UnlimitedPlaybackManager::Trigger_Wakeup,    &wakeup_register,    &wakeup_register_delays },
        { UnlimitedPlaybackManager::Trigger_Gap,       &gap_register,       &gap_register_delays },
        { UnlimitedPlaybackManager::Trigger_OnHit,     &onhit_register,     &onhit_register_delays },
        { UnlimitedPlaybackManager::Trigger_ThrowTech, &throwtech_register, &throwtech_register_delays },
    };

    for (const Wiring& w : wiring) {
        const DummyActionManager::Action& action = actions.Get(w.trigger);
        if (action.source != DummyActionManager::Source_Animation) {
            if (!w.pool->empty()) {
                w.pool->clear();
                w.delays->clear();
            }
            continue;
        }
        if (*w.pool == action.animations) {
            continue;
        }
        *w.pool = action.animations;
        *w.delays = action.animationDelays;
    }
}

void ScrWindow::EnsureDummyScriptLoadedForUi()
{
    if (!g_gameVals.pGameMode || *g_gameVals.pGameMode != GameMode_Training) {
        return;
    }
    if (g_interfaces.player2.IsCharDataNullPtr() || g_interfaces.player1.IsCharDataNullPtr()) {
        return;
    }
    WindowContainer* container = WindowManager::GetInstance().GetWindowContainer();
    if (!container) {
        return;
    }
    ScrWindow* self = container->GetWindow<ScrWindow>(WindowType_Scr);
    if (!self) {
        return;
    }
    self->EnsureDummyScriptFresh(true);
}

bool ScrWindow::DummyFeaturesInUse() const
{
    // Whether anything the dummy does needs the dummy's script parsed.
    //
    // Animation obviously does - it forces one of those states. So does BURST, which is why
    // it did nothing at all: it finds CmnActBurstBegin by name in the parsed state list, and
    // with only a burst armed nothing ever asked for the parse, so the list was empty and
    // every burst resolved to nothing. The log said so plainly once it was asked:
    // "Trigger 'On Hit' fired but resolved to nothing: source=5".
    for (DummyActionManager::TriggerType trigger : DummyActionManager::TriggerOrder()) {
        const DummyActionManager::Source source =
            DummyActionManager::Instance().Get(trigger).source;
        if (source == DummyActionManager::Source_Animation
            || source == DummyActionManager::Source_Burst) {
            return true;
        }
    }
    return false;
}

bool ScrWindow::EnsureDummyScriptFresh(bool allowReparse)
{
    if (p2_old_char_data == (void*)g_interfaces.player2.GetData()) {
        return true;
    }

    // The dummy changed (or this is the first look). Everything parsed for the previous one
    // points into that character's script memory, so drop it all before anything can hand a
    // stale address to the game.
    gap_register = {};
    gap_register_delays = {};
    wakeup_register = {};
    wakeup_register_delays = {};
    onhit_register = {};
    onhit_register_delays = {};
    throwtech_register = {};
    throwtech_register_delays = {};
    burst_action = nullptr;
    air_burst_action = nullptr;
    // The dummy-action table holds scrState* of its own for animation actions, pointing into
    // the same script memory, so it has to be told too.
    DummyActionsPanel::OnDummyScriptReloaded();
    states_wakeup_frame_to_do_action = 0;
    states_wakeup_random_pos = 0;
    states_gap_frame_to_do_action = 0;
    states_gap_random_pos = 0;
    states_throwtech_frame_to_do_action = 0;
    states_throwtech_random_pos = 0;

    if (!allowReparse) {
        // Left stale deliberately: p2_old_char_data is not updated, so the next caller that
        // is allowed to parse still sees the change and does the work.
        return false;
    }

    std::vector<scrState*> states = parse_scr(GetBbcfBaseAdress(), 2);
    g_interfaces.player2.SetScrStates(states);
    g_interfaces.player2.states = states;
    p2_old_char_data = (void*)g_interfaces.player2.GetData();
    for (auto& state : states) {
        if (state->name == "CmnActBurstBegin") {
            burst_action = state;
        }
        if (state->name == "CmnActAirBurstBegin") {
            air_burst_action = state;
        }
    }
    // Animation actions hold their moves by name, so they can be pointed at this character's
    // script now that it is parsed. This is what makes a saved animation action work after a
    // restart, and survive a character swap.
    DummyActionManager::Instance().ResolveAnimations(states, p2_old_char_data);
    return true;
}

// Everything the dummy does on its own each frame. This used to live at the bottom of
// DrawDummyActionsBody, which means it only ran while the mod menu was open on the
// Training page - so a registered wakeup or gap action, burst-on-hit and the Naoto EN
// toggle all silently stopped the moment you closed the menu to actually play.
void ScrWindow::TickDummyActions()
{
    if (!g_gameVals.pGameMode || *g_gameVals.pGameMode != GameMode_Training) {
        return;
    }
    if (g_interfaces.player2.IsCharDataNullPtr() || g_interfaces.player1.IsCharDataNullPtr()) {
        return;
    }
    if (!g_gameVals.pFrameCount) {
        return;
    }

    // Cheap when nothing changed. On a swap this drops the stale registers unconditionally,
    // and only re-parses when the dummy is actually set up to do something - the parse is
    // the expensive part, and nobody who never opened the menu should pay for it.
    EnsureDummyScriptFresh(DummyFeaturesInUse());

    // Actions restored from disk hold their moves by name and arrive after the script was
    // already parsed, so there is no character swap left for the resolve to ride along with.
    // Asked every frame, done once per character - including when it matches nothing,
    // because a character without the saved move must not be retried forever.
    {
        DummyActionManager& actions = DummyActionManager::Instance();
        const void* charData = static_cast<const void*>(g_interfaces.player2.GetData());
        if (!g_interfaces.player2.states.empty() && actions.NeedsAnimationResolve(charData))
        {
            actions.ResolveAnimations(g_interfaces.player2.states, charData);
        }
    }

    // Animation actions are no longer mirrored into these registers. They fire through
    // UnlimitedPlaybackManager's own trigger path now, alongside every other source, which
    // is what lets them be used on all seven triggers instead of the four this function
    // knows about. Syncing as well would fire them twice.

    // Naoto's EN specials need the flag held down, not toggled, so it is rewritten every
    // frame while on and cleared once on the way off. Driven by whichever animation action
    // asks for it, rather than a loose toggle on a panel that no longer exists.
    bool wantNaotoEn = false;
    for (DummyActionManager::TriggerType trigger : DummyActionManager::TriggerOrder()) {
        const DummyActionManager::Action& action = DummyActionManager::Instance().Get(trigger);
        if (action.source == DummyActionManager::Source_Animation && action.naotoEnSpecials) {
            wantNaotoEn = true;
            break;
        }
    }
    if (wantNaotoEn) {
        memset(&g_interfaces.player2.GetData()->slot2_or_slot4, 0x00000018, 4);
    }
    else if (wantNaotoEn != dummy_naoto_en_specials_old) {
        memset(&g_interfaces.player2.GetData()->slot2_or_slot4, 0, 4);
    }
    dummy_naoto_en_specials_old = wantNaotoEn;

    // The old burst-on-hit toggle lived here. It is now a source you can point the On Hit
    // trigger at ("Burst"), which is the same mechanism - an action override the game acts
    // on - reached the same way as every other kind of dummy action.

        static const std::vector<std::tuple<std::string, int>> wakeup_length_pairs{
            //{"CmnActUkemiLandN",30} ,
           {"CmnActUkemiLandNLanding",1},
            {"CmnActUkemiLandF",30 },
            {"CmnActUkemiLandB",30 },
            {"CmnActFDown2Stand", 14},
            {"CmnActBDown2Stand", 14},
            {"CmnActUkemiStagger",7} }; //this in theory should be an on hit trigger, but its an ukemi so i'll consider it wakeup due to how it works
        //"CmnActFDown2Stand", 14 seems to be 20 so far
        //"CmnActFDown2Stand", 14
        if (!wakeup_register.empty()) {

            for (std::tuple<std::string, int> wakeup_length_pair : wakeup_length_pairs) {
                auto name = std::get<0>(wakeup_length_pair);
                auto len = std::get<1>(wakeup_length_pair);
                if (g_interfaces.player2.GetData()->currentAction == name
                    &&
                    g_interfaces.player2.GetData()->actionTime == len
                    &&
                    g_interfaces.player2.GetData()->lastAction != name
                    ) {
                    states_wakeup_random_pos = std::rand() % wakeup_register.size();
                    states_wakeup_frame_to_do_action = *g_gameVals.pFrameCount + wakeup_register_delays[states_wakeup_random_pos];

                    
                }
                //the hitstun check is necessary to make sure it doesnt trigger once the action is already stopped by a hit before it triggers, in the case of delayed ones
                // this used to be a check on the histun, but since sometimes the hitstun isn't reset upon knockdown it wouldn't trigger the action in some situations when it should,
                // so it'll filter out being interrupted by checking if the state itself the character is in is one of hitstun, It doesn't seem like it triggers a 1f delay as I was concerned,
                // but could be wrong
                if (states_wakeup_frame_to_do_action && std::string(g_interfaces.player2.GetData()->currentAction).find("CmnActHit") == std::string::npos){
                    //old_way && (states_wakeup_frame_to_do_action)
                   // ||
               //     hitstun_0 && (states_wakeup_frame_to_do_action && g_interfaces.player2.GetData()->hitstun == 0)
               //     || 
             //       curr_action_not_ukemi && (states_wakeup_frame_to_do_action && std::string(g_interfaces.player2.GetData()->currentAction).find("CmnActHit") == std::string::npos)
             //      {
                 


                     if (*g_gameVals.pFrameCount == states_wakeup_frame_to_do_action) {
                        memcpy(&(g_interfaces.player2.GetData()->nextScriptLineLocationInMemory), &(wakeup_register[states_wakeup_random_pos]->addr), 4);
                        g_interfaces.player2.GetData()->frameCounterCurrentSprite = g_interfaces.player2.GetData()->frameLengthCurrentSprite2 -1;
                        //memcpy(&(g_interfaces.player2.GetData()->currentAction), &(wakeup_register[states_wakeup_random_pos]->name[0]), 20);
                        states_wakeup_frame_to_do_action = 0;
                        states_wakeup_random_pos = 0;
                        break;
                    }
                    else if (*g_gameVals.pFrameCount > states_wakeup_frame_to_do_action) {
                        states_wakeup_frame_to_do_action = 0;
                        states_wakeup_random_pos = 0;
                        break;
                    }

                }
            }
        }

        if (!throwtech_register.empty()) {
            auto throwtech_action_trigger_find = std::string(g_interfaces.player2.GetData()->currentAction).find("LockReject");

                
                if (g_interfaces.player2.GetData()->timeAfterTechIsPerformed == 29 && throwtech_action_trigger_find != std::string::npos){
                    states_throwtech_random_pos = std::rand() % throwtech_register.size();
                    states_throwtech_frame_to_do_action = *g_gameVals.pFrameCount + throwtech_register_delays[states_throwtech_random_pos];


                }
               
                if (*g_gameVals.pFrameCount == states_throwtech_frame_to_do_action) {
                    memcpy(&(g_interfaces.player2.GetData()->nextScriptLineLocationInMemory), &(throwtech_register[states_throwtech_random_pos]->addr), 4);
                    g_interfaces.player2.GetData()->frameCounterCurrentSprite = g_interfaces.player2.GetData()->frameLengthCurrentSprite2 - 1;
                    //memcpy(&(g_interfaces.player2.GetData()->currentAction), &(wakeup_register[states_wakeup_random_pos]->name[0]), 20);
                    states_throwtech_frame_to_do_action = 0;
                    states_throwtech_random_pos = 0;
                }
                else if (*g_gameVals.pFrameCount > states_throwtech_frame_to_do_action && states_throwtech_frame_to_do_action != 0) {
                    states_throwtech_frame_to_do_action = 0;
                    states_throwtech_random_pos = 0;
                };

                }
            
        
        if (!gap_register.empty()) {
            // Note that you can't rely on "GuardEnd" to be there, it can be skipped if there
            // is a mash frame 1

            std::string curr_action = g_interfaces.player2.GetData()->currentAction;
            std::string prev_action = g_interfaces.player2.GetData()->lastAction;
            int prev_blockstun = g_interfaces.player2.GetData()->blockstun;

            if (
                // Doing it this way is necessary because otherwise you have a 1f delay, due to GuardEnd being skipped on frame 1 mash
                // blockstun for now seems to be the most reliable metric
                (g_interfaces.player2.GetData()->blockstun == 1
                    &&
                    curr_action.find("Guard") != std::string::npos

                    )


                ) {


                states_gap_random_pos = std::rand() % gap_register.size();
                states_gap_frame_to_do_action = *g_gameVals.pFrameCount + gap_register_delays[states_gap_random_pos];


            }
            //the hitstun check is necessary to make sure it doesnt trigger once the action is already stopped by a hit before it triggers, in the case of delayed ones
            if (states_gap_frame_to_do_action && g_interfaces.player2.GetData()->hitstun == 0) {



                if (*g_gameVals.pFrameCount == states_gap_frame_to_do_action) {
                    memcpy(&(g_interfaces.player2.GetData()->nextScriptLineLocationInMemory), &(gap_register[states_gap_random_pos]->addr), 4);
                    g_interfaces.player2.GetData()->frameCounterCurrentSprite = g_interfaces.player2.GetData()->frameLengthCurrentSprite2 - 1;
                    //memcpy(&(g_interfaces.player2.GetData()->currentAction), &(gap_register[state_gap_random_pos]->name[0]), 20);
                    states_gap_frame_to_do_action = 0;
                    states_gap_random_pos = 0;

                }
                else if (*g_gameVals.pFrameCount > states_gap_frame_to_do_action) {
                    states_gap_frame_to_do_action = 0;
                    states_gap_random_pos = 0;
                }

            }
        }

        if (!onhit_register.empty()) {
            int random_pos = std::rand() % onhit_register.size();
            static std::vector<std::string>loops_bound{ "Loop" , "Bound", "CmnActBDownCrash", "CmnActBDownDown"};/*necessary to stop the on hit actions from activating in a ukemi situation, once ukemi
                                                                                              comes into play it becomes a wakeup action.
                                                                                              Only reason CmdActBDownCrash is being fully specified and not as a substring is because 
                                                                                              a lot of moves prob have "Crash" in the name*/
            std::string curr_action = g_interfaces.player2.GetData()->currentAction;
            
            if ((g_interfaces.player2.GetData()->hitstun == 1) & !find_substring_in_vector(loops_bound,curr_action)) {
                memcpy(&(g_interfaces.player2.GetData()->nextScriptLineLocationInMemory), &(onhit_register[random_pos]->addr), 4);
                memcpy(&(g_interfaces.player2.GetData()->currentAction), &(onhit_register[random_pos]->name[0]), 20);
            }


        }
    
}

std::string interpret_move(char move) {
    //auto button_bits = move & ((4 << 1) - 1);
    auto button_bits = move & 0xf0;
    auto direction_bits = move & 0x0f;
    //auto direction_bits = move & ((8 << 1) - 1);
    std::string move_t{ "" };
    switch (direction_bits) {
    case 0x5:
        move_t += "Neutral";
        break;
    case 0x4:
        move_t += "Left";
        break;
    case 0x1:
        move_t += "DownLeft";
        break;
    case 0x2:
        move_t += "Down";
        break;
    case 0x3:
        move_t += "DownRight";
        break;
    case 0x6:
        move_t += "Right";
        break;
    case 0x9:
        move_t += "UpRight";
        break;
    case 0x8:
        move_t += "Up";
        break;
    case 0x7:
        move_t += "UpLeft";
        break;
    }
    switch (button_bits) {
    case 0x10:
        move_t += "+A";
        break;
    case 0x20:
        move_t += "+B";
        break;
    case 0x40:
        move_t += "+C";
        break;
    case 0x80:
        move_t += "+D";
        break;
    case 0x30:
        move_t += "+A+B";
        break;
    case 0x50:
        move_t += "+A+C";
        break;
    case 0x90:
        move_t += "+A+D";
        break;
    case 0x60:
        move_t += "+B+C";
        break;
    case 0xA0:
        move_t += "+B+D";
        break;
    case 0xC0:
        move_t += "+C+D";
        break;
    case 0xB0:
        move_t += "+A+B+D";
        break;
    case 0x70:
        move_t += "+A+B+D";
        break;
    case 0xD0:
        move_t += "+A+C+D";
        break;
    case 0xE0:
        move_t += "+B+C+D";
        break;
    case 0xF0:
        move_t += "+A+B+C+D";
        break;
    }

    return move_t;
}



void treat_random_slot_checkbox(std::vector<int> &slot_vec, bool slot_toggle, int slot_num) {
    if (slot_toggle) {
        slot_vec.push_back(slot_num);
    }
    else {
        auto iterator = std::find(slot_vec.begin(), slot_vec.end(), slot_num);
        if (iterator != slot_vec.end()) {
            slot_vec.erase(std::find(slot_vec.begin(), slot_vec.end(), slot_num));
        }
    }
};
void ScrWindow::draw_playback_slot_section(int slot) {
    ImGui::PushID(int("slot") + slot);
    char* fpath;
    switch (slot)
    {
    case 1:
        fpath = fpath_s1;
        break;
    case 2:
        fpath = fpath_s2;
        break;
    case 3:
        fpath = fpath_s3;
        break;
    case 4:
        fpath = fpath_s4;
        break;

    default:
        fpath = fpath_s1;
        break;
    }
    PlaybackSlot selected_slot = playback_manager.slots[slot - 1];
   

    int facing_direction;
    memcpy(&facing_direction, selected_slot.facing_direction_p, 4);



    std::vector<char> slot_recording_frames = selected_slot.get_slot_buffer();
    if (ImGui::Button("Save")) {
        playback_manager.save_to_file(slot_recording_frames, facing_direction, fpath);
    }
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Save_playback_tooltip());
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        playback_manager.load_from_file_into_slot(fpath,slot);

    }
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Load_playback_tooltip());
    ImGui::SameLine();
    if (ImGui::Button("Trim Playback")) {
        slot_recording_frames = playback_manager.trim_playback(slot_recording_frames);
        playback_manager.load_into_slot(slot_recording_frames, slot);
        //load_trimmed_playback(slot_recording_frames, selected_slot.frame_len_slot_p, start_of_slot_inputs);
    }
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Trim_playback_tooltip());

    ImGui::InputText("File Path", fpath, 1200);// IM_ARRAYSIZE(fpath));
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Playback_file_path_tooltip());
    ImGui::TextWrapped("All input files expect the .playback extension now, please add the extension to your old playback files. You can still load the files with .playback extension without writing the extension in the field.");
    ImGui::TextWrapped("If the field isn't accepting keyboard input, try alt-tabbing out and back in, if that doesn't work copy and paste should still work(or restarting the game)");
    if (ImGui::Button("Set as gap action")) {
        slot_gap = slot;
    }
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Playback_set_gap_tooltip());
    ImGui::SameLine();
    if (ImGui::Button("Set as wakeup action")) {
        slot_wakeup = slot;
    }
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Playback_set_wakeup_tooltip());
    if (ImGui::Button("Set as onblock action")) {
        slot_onblock = slot;
    }
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Playback_set_onblock_tooltip());

    ImGui::SameLine();
    if (ImGui::Button("Set as onhit action::experimental")) {
        slot_onhit = slot;
    }
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Playback_set_onhit_tooltip());
    ImGui::SameLine();
    if (ImGui::Button("Set as tech action")) {
        slot_throwtech = slot;
    }
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Playback_set_tech_tooltip());
    if (ImGui::Button("Reset")) {
        slot_gap = 0;
        slot_wakeup = 0;
        slot_onblock = 0;
        slot_onhit = 0;
        slot_throwtech = 0;
    }
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Playback_reset_tooltip());
    ImGui::InputInt("Buffer frames", &slot_buffer[slot-1]);
    ImGui::SameLine();
    ImGui::ShowHelpMarker(Messages.Playback_buffer_frames_tooltip());
    ImGui::TextWrapped("Buffer frames only works currently with wakeup actions");
    ImGui::Separator();
    auto old_val = 0; auto frame_counter = 0;
    for (auto el : slot_recording_frames) {
        frame_counter++;
        if (old_val != el) {
            std::string move_string = interpret_move(el);
            ImGui::Text("frame %d: %s (0x%x)", frame_counter, move_string.c_str(), el);
            old_val = el;
        }
    }
    ImGui::PopID();
};
void ScrWindow::DrawSaveStatesBody() {
    if (*(bbcf_base_adress + 0x8F7758) == 0) {
        if (!g_interfaces.player1.IsCharDataNullPtr() && !g_interfaces.player2.IsCharDataNullPtr()) {
            if (ImGui::Button("Save snapshot")) {
                SaveTrainingState();
            }
            ImGui::SameLine();
            ImGui::ShowHelpMarker(Messages.Save_snapshot_tooltip());
            ImGui::SameLine();
            ImGui::ShowHelpMarker("You can also use a hotkey for this. Set it under Hotkeys in the mod's Settings window - any key, key combination, or a spare controller button.");
            ImGui::SameLine();
            if (HasTrainingSnapshot()) {
                if (ImGui::Button("Load snapshot")) {
                    LoadTrainingState();
                }
            }
            else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                ImGui::Button("Load snapshot");
                ImGui::PopStyleColor();

            }
            ImGui::SameLine();
            ImGui::ShowHelpMarker(Messages.Load_snapshot_tooltip());
            ImGui::SameLine();
            ImGui::ShowHelpMarker("You can use a hotkey to activate it, default is F9 but can be changed in settings.ini between F1-9.");

            ImGui::InputFloat("Setup time(s)", &wait_before_exec_s, 0.3f);
            ImGui::SameLine();
            ImGui::ShowHelpMarker("This pauses the game once you load a state for the amount set in order to adjust hand position. Set to 0 if no delay is desired.");
            // The countdown indicator is drawn by DrawSaveStateSetupDelayStandalone, so it
            // shows for a hotkey load with this page nowhere on screen.
        }
        else {
            ImGui::TextWrapped("%s", L("You must be in a mode where state can be saved.").c_str());
        }

    }
    else {
        ImGui::TextWrapped("%s", L("You cannot use this feature while searching for a ranked match.").c_str());
    }
}
bool compareFiles(const std::string& p1, const std::string& p2) {
    std::ifstream f1(p1, std::ifstream::binary | std::ifstream::ate);
    std::ifstream f2(p2, std::ifstream::binary | std::ifstream::ate);

    if (f1.fail() || f2.fail()) {
        return false; //file problem
    }

    if (f1.tellg() != f2.tellg()) {
        return false; //size mismatch
    }

    //seek back to beginning and use std::equal to compare contents
    f1.seekg(0, std::ifstream::beg);
    f2.seekg(0, std::ifstream::beg);
    return std::equal(std::istreambuf_iterator<char>(f1.rdbuf()),
        std::istreambuf_iterator<char>(),
        std::istreambuf_iterator<char>(f2.rdbuf()));
}

void set_local_replay(char* replayname, int fname_size_max) {
    int bbcf_base = (int)GetBbcfBaseAdress();
    uintptr_t replay_file_template = bbcf_base + 0x4AA66C;
    WriteToProtectedMemory(replay_file_template, replayname, fname_size_max);

}
void restore_replays(int fname_size_max) {
    int bbcf_base = (int)GetBbcfBaseAdress();
    uintptr_t replay_file_template = bbcf_base + 0x4AA66C;
    char* original_name = "replay%02d.dat\0\0replay_list.dat";

    WriteToProtectedMemory(replay_file_template, original_name, fname_size_max);

}


#include <wininet.h> // only for InternetCanonicalizeUrlA


// The replay-file redirect is a patch on the game's replay filename template, so it has to
// be re-applied (and undone on the way out of the theater) every frame, not only while the
// Replays page happens to be open.
bool ScrWindow::s_localReplayLoaded = false;
std::string ScrWindow::s_localReplayLoadedName = "";

void ScrWindow::TickLocalReplayRedirect() {
    const int FNAME_SIZE_MAX = 31;
    if (!g_gameVals.pGameMode || !s_localReplayLoaded)
        return;

    if (*g_gameVals.pGameMode != GameMode_ReplayTheater) {
        restore_replays(FNAME_SIZE_MAX);
    }
    else {
        set_local_replay(&s_localReplayLoadedName[0], FNAME_SIZE_MAX);
    }
}

void ScrWindow::DrawLocalReplaysBody() {
    /*std::filesystem::path targetParent = "./Save/Replay/locals";
    std::filesystem::create_directories(targetParent);*/
    


    const int FNAME_SIZE_MAX = 31;
    ReplayFileManager& rep_manager = g_rep_manager;
    {
        
        static int view_type = 0; // 0 for default, 1 for archive, 2 for db
        static int page = 0;
        static int character1 = -1;
        static char player1[200] = "";
        static int character2 = -1;
        static char player2[200] = "";

       
        
        
        static char local_replay_name[FNAME_SIZE_MAX] = "fname";
        ImGui::InputText("File Name##replay_theater", local_replay_name, IM_ARRAYSIZE(local_replay_name));
        ImGui::SameLine();
        ImGui::ShowHelpMarker("The replay file must be in Save/Replay/, to load archived replays move them from Save/Replay/archive/ to Save/Replay/. Filenames must not exceed 31 chars. ");
 
        if (ImGui::Button("Load##replay_theater")) {
            set_local_replay(local_replay_name, FNAME_SIZE_MAX);
            s_localReplayLoaded = true;
            s_localReplayLoadedName = local_replay_name;
        }
        ImGui::SameLine();
        ImGui::ShowHelpMarker("Loads the specified File Name. Once done you can select any replay from the list and it will play the loaded replay file.");
        ImGui::SameLine();

        if (ImGui::Button("Restore original replays##replay_theater")) {
            restore_replays(FNAME_SIZE_MAX);
            s_localReplayLoaded = false;
        }
        ImGui::SameLine();
        ImGui::ShowHelpMarker("Restores your replays to their original files, reverting the effect of \"Load\".");


        
        
        if (ImGui::Button("Archive replay files")) {
            rep_manager.archive_replays();

        }
        ImGui::SameLine();
        ImGui::ShowHelpMarker("Archiving will copy and rename all current replays to Save/Replay/archive/ .");

        ImGui::SameLine();
        if (ImGui::CheckboxWrapped("Auto archive saved replays", &Settings::settingsIni.autoArchive)) {
            Settings::changeSetting("autoArchive", std::to_string((int)Settings::settingsIni.autoArchive));
        }
        ImGui::SameLine();
        ImGui::ShowHelpMarker(Messages.Auto_archive_replays_tooltip());





        if (ImGui::TreeNode("(Experimental)Replay database download/archive replace##local_replays")) {
            if (!g_rep_manager.template_modified && view_type == 1)
                view_type = 0; // if replay list was reset to default due to playing a real match, also reset view_type to default
                // except for db, which does not immediately modify the template

            bool view_changed = false;

            if (ImGui::RadioButton("Recent replays", &view_type, 0))
                view_changed = true;
            ImGui::SameLine();
            ImGui::ShowHelpMarker(Messages.Recent_replays_tooltip());

            if (view_type == 0) {
                ImGui::SameLine();

                if (ImGui::Button("Repair##replay_list"))
                    g_rep_manager.load_replay_list_default_repair();
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Repair_replay_list_tooltip());

                if (view_changed)
                    g_rep_manager.load_replay_list_default();
            }

            if (ImGui::RadioButton("Replay archive", &view_type, 1))
                view_changed = true;
            ImGui::SameLine();
            ImGui::ShowHelpMarker(Messages.Replay_archive_tooltip());

            ImGui::RadioButton("Replay db", &view_type, 2);
            ImGui::SameLine();
            ImGui::ShowHelpMarker(Messages.Replay_db_tooltip());


            if (view_type == 1) { // archive controls
                ImGui::TextUnformatted("page");

                ImGui::SameLine();

                if (ImGui::InputInt("##replay_list_page", &page))
                    view_changed = true;
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Replay_list_page_tooltip());

                // TODO: search?

                if (view_changed)
                    g_rep_manager.load_replay_list_from_archive(page);
            }


            if (view_type == 2) { // db controls
                if (ImGui::BeginCombo("character1##replay_db_character", character1 == -1 ? "<any>" : getCharacterNameByIndexA(character1).c_str())) {

                    if (ImGui::Selectable("<any>", character1 == -1)) character1 = -1;

                    for (int i = 0; i < getCharactersCount(); i++) {
                        if (ImGui::Selectable(getCharacterNameByIndexA(i).c_str(), character1 == i))
                            character1 = i;
                    }

                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Replay_db_character_filter_tooltip());

                ImGui::InputText("player1##replay_db_player", player1, sizeof(player1));
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Replay_db_player_filter_tooltip());


                ImGui::TextUnformatted("vs");


                if (ImGui::BeginCombo("character2##replay_db_character", character2 == -1 ? "<any>" : getCharacterNameByIndexA(character2).c_str())) {

                    if (ImGui::Selectable("<any>", character2 == -1)) character2 = -1;

                    for (int i = 0; i < getCharactersCount(); i++) {
                        if (ImGui::Selectable(getCharacterNameByIndexA(i).c_str(), character2 == i))
                            character2 = i;
                    }

                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Replay_db_character_filter_tooltip());

                ImGui::InputText("player2##replay_db_player", player2, sizeof(player2));
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Replay_db_player_filter_tooltip());


                ImGui::TextUnformatted("page");

                ImGui::SameLine();

                ImGui::InputInt("##replay_list_page", &page);
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Replay_list_page_tooltip());

                if (ImGui::Button("Load##replay_db"))
                    g_rep_manager.load_replay_list_from_db(page, character1, player1, character2, player2);
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Replay_db_search_tooltip());
                // TODO: instead of Load button, we could use view_changed and debounce
            }


            // print extra info about selected replay
            char* base = GetBbcfBaseAdress();
            ReplayList* replay_list = (ReplayList*)(base + 0xAA9808);
            int selected_order = *(int*)(base + 0xE9329C); // replay menu item index

            int selected_index = replay_list->order[selected_order]; //*(int*)(replay_list + 8 + 100 * 0x390 + selected_order * 4);

            static bool first = true;
            if (first && replay_list->count == 0) { // otherwise count is 0 until you go to replay theater
                g_rep_manager.bbcf_sort_replay_list();
                first = false;
            }

            if (selected_index != -1) {
                ReplayFile* rp = replay_list->replays[selected_index].data(); // (ReplayFile*)(replay_list + 8 + selected_index * 0x390 - 8); // only first 0x390 bytes match

                ImGui::Text("Selected: %s (lvl%d %s)%s",
                    utf16_to_utf8(rp->p1_name).c_str(), rp->p1_lvl + 1, getCharacterNameByIndexA(rp->p1_toon).c_str(), rp->winner_maybe == 0 ? " (win)" : "");
                ImGui::Text("      vs  %s (lvl%d %s)%s",
                    utf16_to_utf8(rp->p2_name).c_str(), rp->p2_lvl + 1, getCharacterNameByIndexA(rp->p2_toon).c_str(), rp->winner_maybe == 1 ? " (win)" : "");
                // TODO: draw replay levels, winner and other metadata on top of bbcf list ui?

                if (ImGui::Button("<##replay_list_prev"))
                    g_rep_manager.set_selected_replay_index(selected_order - 1, true);
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Replay_list_prev_tooltip());
                ImGui::SameLine();

                ImGui::Text("index %3d / %d", selected_order + 1, replay_list->count); // *(int*)(base + 0x8f85d8 + 0x1b1230 + 0x165d8)); // base->static_CSaveDataManager.replay_list.count
                ImGui::SameLine();

                if (ImGui::Button(">##replay_list_next"))
                    g_rep_manager.set_selected_replay_index(selected_order + 1, true);
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Replay_list_next_tooltip());

                ImGui::SameLine();
                if (ImGui::Button("Load##replay_list_next")) {
                    g_rep_manager.load_replay(selected_order, NULL);
                    g_rep_manager.unpack_replay_buffer();
                }
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Replay_list_load_tooltip());


                if (view_type == 2) { // if db
                    if (ImGui::Button("Save selected replay to archive##replay_db")) {
                        char path[32] = "";
                        char* replay_file_template = base + 0x4AA66C;
                        sprintf(path, replay_file_template, selected_index);

                        rep_manager.load_replay(path);
                        auto new_fname = rep_manager.build_file_name();
                        rep_manager.save_replay(REPLAY_ARCHIVE_FOLDER_PATH + new_fname);
                    }
                    ImGui::SameLine();
                    ImGui::ShowHelpMarker(Messages.Replay_db_save_archive_tooltip());
                }
            }


            ImGui::Separator();

            // load external replay file

            //static char filename[256] = "https://bbreplay.ovh/download?filename=082009246cfd79b5a64208ba2.dat";
            static char filename[256] = "./Save/Replay/replay00.dat";
            ImGui::InputText("##replay_filename", filename, 256);
            ImGui::SameLine();
            ImGui::ShowHelpMarker(Messages.Replay_filename_path_tooltip());
            ImGui::SameLine();

            if (ImGui::Button("Load")) {

                if(g_rep_manager.validate_url_prefix(filename))
                    g_rep_manager.download_replay(filename, NULL);

                else // load file
                    g_rep_manager.load_replay(filename, NULL);

                // TODO: check that replay in buffer is valid, show message otherwise
                g_rep_manager.unpack_replay_buffer();
            }
            ImGui::SameLine();
            ImGui::ShowHelpMarker(Messages.Replay_filename_load_tooltip());

            // load take filename from steam url, e.g. steam://run/586140/?load-replay=https%3A%2F%2Fbbreplay.ovh%2Fdownload%3Ffilename%3D082009246cfd79b5a64208ba2.dat
            ISteamApps* apps = *(ISteamApps**)((char*)base + 0x005d3230); // base->static_SteamInterfaces.apps
            const char* param = apps->GetLaunchQueryParam("load-replay");
            //ImGui::Text("steam test param %p %s", param, param);

            bool param_changed = false;
            static char last_param[256] = "";
            if (strcmp(param, last_param) != 0) {
                strncpy(last_param, param, sizeof(last_param) - 1);
                strncpy(filename, param, sizeof(filename)-1);
                param_changed = true;
                
            }
            if (param_changed) {
                DWORD n = 256;

                InternetCanonicalizeUrlA(param, filename, &n, ICU_DECODE);
                if (g_rep_manager.validate_url_prefix(filename)) { //Makes sure the urls are only from the upload endpoint and bbreplay.ovh for now due to safety.
                    if (g_rep_manager.download_replay(filename, NULL)) {
                        g_rep_manager.unpack_replay_buffer();
                        ScenesManager::PlayLoadedReplay();
                    };
                }
            
                // TODO:  Add a popup saying it failed to download the file later so it doesnt just fail silently.
            }



            ImGui::Separator();

            // print extra info about the loaded replay
            {
                static bool autoplay = false, really_autoplay = false;

                ReplayFile* rp = (ReplayFile*)(base + 0x0115b478);
                if (g_rep_manager.check_file_validity(rp)) {
                    ImGui::Separator();

                    bool is_playing = *g_gameVals.pGameMode == GameMode_ReplayTheater && (*g_gameVals.pGameState == GameState_InMatch || *g_gameVals.pGameState == GameState_VersusScreen);
                    ImGui::Text(is_playing ? "Playing: %s (lvl%d %s)%s" : "Loaded: %s (lvl%d %s)%s",
                        utf16_to_utf8(rp->p1_name).c_str(), rp->p1_lvl + 1, getCharacterNameByIndexA(rp->p1_toon).c_str(), rp->winner_maybe == 0 ? " (win)" : "");
                    ImGui::Text("      vs  %s (lvl%d %s)%s",
                        utf16_to_utf8(rp->p2_name).c_str(), rp->p2_lvl + 1, getCharacterNameByIndexA(rp->p2_toon).c_str(), rp->winner_maybe == 1 ? " (win)" : "");
                    ImGui::Text("      at %s", rp->date1);

                    if (ImGui::Button(is_playing ? "Restart##replay" : "Play##replay")) {
                        ScenesManager::PlayLoadedReplay();
                    }
                    ImGui::SameLine();
                    ImGui::ShowHelpMarker(Messages.Replay_play_restart_tooltip());

                    ImGui::SameLine();
                    ImGui::CheckboxWrapped("autoplay", &autoplay);
                    ImGui::SameLine();
                    ImGui::ShowHelpMarker(Messages.Replay_autoplay_tooltip());
                }

                if (autoplay) {
                    if (*g_gameVals.pGameState == GameState_InMatch) {
                        auto match_state = *(int*)(base + 0xdb6ae0 + 0x62b7c + 0x30); //base->static_BATTLE_CObjectManager.match_info.match_state;
                        if (match_state > 3) really_autoplay = true; // only autoplay if watched to the end
                        if (match_state < 3) really_autoplay = false;
                    }

                    char* scene = *(char**)(base + 0x8903b0 + 0x2604); // base->static_GameVals.current_scene
                    if (*g_gameVals.pGameState == GameState_ReplayMenu && really_autoplay && *(int*)(scene + 0x2c) == 9) {//scene->GameSceneState == 9) {
                        really_autoplay = false;
                        int i0 = g_rep_manager.get_selected_replay_index();
                        int i1 = g_rep_manager.set_selected_replay_index(i0 + 1, false);
                        if (i1 != i0) {
                            g_rep_manager.load_replay(i1, NULL);
                            g_rep_manager.unpack_replay_buffer();
                            ScenesManager::PlayLoadedReplay();
                        }
                    }
                }
            }

            ImGui::Separator();
            ImGui::TreePop();
        }
    }
   

}


unsigned int count_entities(bool unk_status2) {
    if (!g_interfaces.player1.IsCharDataNullPtr() && !g_interfaces.player2.IsCharDataNullPtr()) {
        std::vector<int*> entities{};
        std::vector<CharData**> entities_char_data{};
        for (int i = 0; i < 252; i++) {
            entities.push_back((g_gameVals.pEntityList + i));
            //entities_char_data.push_back((CharData*)(g_gameVals.pEntityList + i));
        }
        for (auto entity_ptr : entities) {
            entities_char_data.push_back((CharData**)entity_ptr);
        }
        auto tst = entities_char_data[0];
        auto r = 0;
        for (auto entity : entities_char_data) {
            if ((*entity)->unknownStatus1 != NULL) {
                if (unk_status2) {
                    if ((*entity)->unknown_status2 != NULL && (*entity)->unknown_status2 == 2) {
                        r++;
                    }
                }
                else {
                    r++;
                }
            }
        }
        return r;
        //auto tst = &entities_char_data[0]->frame_count_minus_1;
    }
    return 0;
}

  

namespace
{
    // Taking over rebinds which controller slot each side reads, so that whichever side
    // you take over is driven by your P1 device. Those are game globals that persist for
    // the rest of the session: leaving them rewritten means P1 and P2 input sources stay
    // swapped in ordinary Training and Versus afterwards, until something else happens to
    // set them back. So the originals are kept and put back on the way out.
    struct TakeoverInputBinding
    {
        unsigned char trainingSide;
        unsigned char slotForP1;
        unsigned char slotForP2;
        bool saved = false;
    };

    TakeoverInputBinding g_savedInputBinding;

    const int kTrainingSideOffset = 0x891A38;
    const int kSlotForP2Offset    = 0x8929A8;
    const int kSlotForP1Offset    = 0x8929A4;

    // Which controller slot drives the character during a takeover.
    //
    // Takeover binds whichever side you took over to one slot, so that a single player can
    // take over either side without swapping pads. Slot 0 is the natural choice and is
    // also the only one that merges the keyboard in, which is why it is the default - but
    // a pad configured as player 2 lives in slot 1 and is then completely dead during a
    // takeover, which is the whole of the "takeover is keyboard only" report.
    //
    // Merging both slots so any pad works regardless would have to happen inside the
    // game's own controller read, since that is the only place both pads exist as mapped
    // BBCF inputs; the mod's battle-input hook is per player, and during a takeover the
    // other player IS the replay playback.
    // Same control as the Settings window's hotkey rows: click it, then press the key or the
    // controller button. Writes straight to the real binding, so this is the setting, not a
    // copy of it.
    void DrawTakeoverHotkeyRow(const char* label, HotkeyManager::Action action)
    {
        HotkeyBinding binding = HotkeyManager::GetBinding(action);

        std::string warning;
        const HotkeyManager::Action conflict = HotkeyManager::FindConflict(binding, action);
        if (conflict != HotkeyManager::Hotkey_Count) {
            warning = std::string("Already used by \"") + HotkeyManager::DisplayName(conflict) +
                "\". Pressing it will do both.";
        }
        else if (HotkeyManager::IsControllerBinding(binding)) {
            warning = "Controller button: this also works during a match, so pick one you never "
                "press while playing.";
        }

        ImGui::TextUnformatted(label);
        ImGui::SameLine(210.0f);
        if (ImGuiHotkey::BindWidget(HotkeyManager::IniKey(action), binding,
            HotkeyManager::DefaultBindingString(action),
            warning.empty() ? nullptr : warning.c_str())) {
            // Persists to settings.ini and re-arms controller polling for the new binding.
            HotkeyManager::SetBinding(action, binding);
        }
    }

    unsigned char TakeoverInputSlot()
    {
        return Settings::settingsIni.takeoverInputSlot == 1 ? 1 : 0;
    }

    void SaveTakeoverInputBinding(char* bbcf_base)
    {
        // Only the first takeover of a session captures the real values; a second one
        // would otherwise save the rebound state as if it were the original.
        if (g_savedInputBinding.saved)
            return;
        g_savedInputBinding.trainingSide = *(bbcf_base + kTrainingSideOffset);
        g_savedInputBinding.slotForP1    = *(bbcf_base + kSlotForP1Offset);
        g_savedInputBinding.slotForP2    = *(bbcf_base + kSlotForP2Offset);
        g_savedInputBinding.saved = true;
    }

    void RestoreTakeoverInputBinding(char* bbcf_base)
    {
        if (!g_savedInputBinding.saved)
            return;
        *(bbcf_base + kTrainingSideOffset) = g_savedInputBinding.trainingSide;
        *(bbcf_base + kSlotForP1Offset)    = g_savedInputBinding.slotForP1;
        *(bbcf_base + kSlotForP2Offset)    = g_savedInputBinding.slotForP2;
        g_savedInputBinding.saved = false;
    }
}

// The takeover used to be four buttons and a rule nobody could have guessed: press
// "Takeover as P1", then press "Load Replay State", then press "Fix playback" if it came
// out mirrored. It is one button and one dialog now - the two other steps were things the
// mod already knew how to do for itself.
void ScrWindow::DrawReplayTakeoverBody(const char* idScope, bool compact) {
    const std::string setupPopupId = std::string("##takeover_setup_") + (idScope ? idScope : "");

    // The library-based BETA tool is a separate feature with its own window; it earns a link
    // from the mod menu, not a line in a window that is meant to stay out of the way.
#if BBCF_ENABLE_UNLIMITED_REPLAY_TAKEOVER
    if (!compact) {
        if (ImGui::Button("Unlimited Replay Takeover (BETA)")) {
            ScrWindow::m_pWindowContainer->GetWindow(WindowType_UnlimitedReplayTakeover)->ToggleOpen();
        }
        ImGui::SameLine();
        ImGui::ShowHelpMarker(Messages.Unlimited_replay_takeover_tooltip());
        ImGui::SameLine();
        ImGui::TextDisabled("Capture replay situations into a training library.");
        ImGui::Separator();
    }
#endif

    if (*(bbcf_base_adress + 0x8F7758) != 0) { // searching for a ranked match
        ImGui::TextWrapped("%s", L("You cannot use this feature while searching for a ranked match.").c_str());
        return;
    }

    if (!g_gameVals.pGameMode || g_interfaces.player1.IsCharDataNullPtr() || g_interfaces.player2.IsCharDataNullPtr()) {
        ImGui::TextWrapped("%s", L("Cannot access replay takeover outside of a replay.").c_str());
        return;
    }

    const bool inReplay = *g_gameVals.pGameMode == GameMode_ReplayTheater;
    const bool inTraining = *g_gameVals.pGameMode == GameMode_Training;
    const bool running = inTraining && takeover_active;

    const std::string takeover = L("Takeover from here");
    const std::string restart = L("Restart from takeover point");
    const std::string back = L("Return to replay");
    const std::string takeoverTip =
        L("Stops the replay at this exact moment and hands you one of the two players. The other side keeps doing everything it did in the replay.");
    const std::string restartTip = FormatText(
        L("Puts everything back to the moment you took over and starts the recorded side again. Hotkey: %s. This also happens by itself whenever a round ends, so a KO never kicks you out to character select.").c_str(),
        HotkeyManager::DisplayString(
            HotkeyManager::GetBinding(HotkeyManager::Hotkey_LoadReplayState)).c_str());

    // A compact host gets three rows at most and no prose. Which three depends on what is
    // happening: once you are inside a takeover the other controls are meaningless, so the
    // window stops offering them and becomes a takeover panel instead.
    if (compact) {
        if (running) {
            // 1: what mode you are in and which side is yours.
            ImGui::TextColored(ImVec4(0.35f, 0.70f, 1.00f, 1.00f), "%s", L("Takeover mode").c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", (takeover_as_p1
                ? L("- you are Player 1")
                : L("- you are Player 2")).c_str());

            // 2: the three things you can do from here.
            const std::string shortRestart = L("Restart");
            const std::string shortBack = L("Back to replay");
            const std::string settings = L("Settings");

            if (ImGui::Button(shortRestart.c_str())) {
                pending_load_replay_state = true;
            }
            ImGui::ShowHelpMarkerSameLine(restartTip.c_str());

            ImGui::SameLine();
            if (ImGui::Button(shortBack.c_str())) {
                EndReplayTakeover();
            }
            ImGui::ShowHelpMarkerSameLine(Messages.Return_to_replay_tooltip());

            ImGui::SameLine();
            if (ImGui::Button(settings.c_str())) {
                // Opens on what is actually running, so Apply with nothing changed is a
                // no-op rather than a surprise side swap.
                takeover_modal_side = takeover_as_p1 ? 0 : 1;
                ImGui::OpenPopup(setupPopupId.c_str());
            }
            ImGui::ShowHelpMarkerSameLine(
                L("Change which side you play, the setup time, or the hotkey - and take the same moment over again with the new settings.").c_str());

            // 3: the key that does the same as Restart, so it can be used without the mouse.
            ImGui::TextDisabled("%s", FormatText(L("Restart hotkey: %s").c_str(),
                HotkeyManager::DisplayString(
                    HotkeyManager::GetBinding(HotkeyManager::Hotkey_LoadReplayState)).c_str()).c_str());

            DrawTakeoverSetupModal(setupPopupId.c_str());
            return;
        }

        ImGui::BeginDisabled(!inReplay);
        if (ImGui::Button(takeover.c_str())) {
            ImGui::OpenPopup(setupPopupId.c_str());
        }
        ImGui::EndDisabled();
        ImGui::HoverTooltipEvenDisabled(inReplay
            ? takeoverTip.c_str()
            : L("Open a replay to take one over from where it is.").c_str());
        ImGui::ShowHelpMarkerSameLine(takeoverTip.c_str());

        DrawTakeoverSetupModal(setupPopupId.c_str());
        return;
    }

    if (inReplay) {
        if (ImGui::Button(takeover.c_str())) {
            ImGui::OpenPopup(setupPopupId.c_str());
        }
        ImGui::ShowHelpMarkerSameLine(takeoverTip.c_str());

        // Same window as the button, which is what BeginPopupModal requires.
        DrawTakeoverSetupModal(setupPopupId.c_str());
        return;
    }

    if (running) {
        ImGui::TextWrapped("%s", (takeover_as_p1
            ? L("You are playing as Player 1. Player 2 is replaying the recorded match.")
            : L("You are playing as Player 2. Player 1 is replaying the recorded match.")).c_str());

        if (ImGui::Button(restart.c_str())) {
            pending_load_replay_state = true;
        }
        ImGui::ShowHelpMarkerSameLine(restartTip.c_str());

        ImGui::SameLineOrWrap(ImGui::ButtonWidth(back.c_str()));
        if (ImGui::Button(back.c_str())) {
            EndReplayTakeover();
        }
        ImGui::ShowHelpMarkerSameLine(Messages.Return_to_replay_tooltip());

        // The old "Fix playback" button was a coin flip the user had to make: the mirroring
        // is worked out from the side you took over now (see BeginReplayTakeover). This is
        // kept only as a diagnostic for the case that turns out to be wrong somewhere, and
        // only for people who have already opted into unfinished tooling.
        if (Settings::settingsIni.enableInDevelopmentFeatures) {
            bool mirrored = facing_left_replay_takeover != 0;
            if (ImGui::Checkbox(L("Mirror the recorded inputs (diagnostic)").c_str(), &mirrored)) {
                facing_left_replay_takeover = mirrored ? 1 : 0;
                pending_load_replay_state = true;
            }
        }
        return;
    }

    ImGui::TextWrapped("%s", L("Open a replay and press \"Takeover from here\".").c_str());
}

// P1/P2, how long the game freezes for afterwards, and the key that puts you back at the
// takeover point - everything the feature has, in the one dialog you already have open.
// Nothing else to press: the state load the user used to have to remember is part of
// accepting this.
void ScrWindow::DrawTakeoverSetupModal(const char* popupId) {
    // Sized before it is centred, never auto-resized: a pivot needs a size, and an
    // auto-resizing popup has none on the frame it appears. See docs/ImGuiModalCentering.md.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(430.0f, 250.0f), ImGuiCond_Appearing);
    if (display.x > 0.0f && display.y > 0.0f) {
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }

    if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_NoResize)) {
        return;
    }

    const bool reconfiguring = takeover_active;

    ImGui::TextUnformatted((reconfiguring
        ? L("Takeover settings")
        : L("Take over this replay")).c_str());
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted(L("Play as").c_str());
    ImGui::RadioButton(L("Player 1").c_str(), &takeover_modal_side, 0);
    ImGui::SameLine();
    ImGui::RadioButton(L("Player 2").c_str(), &takeover_modal_side, 1);

    ImGui::Spacing();

    float setup = Settings::settingsIni.replayTakeoverSetupSeconds;
    // Narrow: it holds "1.00" and a pair of steppers, not a sentence.
    ImGui::SetNextItemWidth(86.0f);
    if (ImGui::InputFloat(L("Setup time (s)").c_str(), &setup, 0.1f, 0.5f, "%.2f")) {
        if (setup < 0.0f) {
            setup = 0.0f;
        }
        // Kept in the draft only; written to settings.ini when the dialog is accepted, so
        // arrowing through values does not rewrite the file once per click.
        Settings::settingsIni.replayTakeoverSetupSeconds = setup;
    }
    ImGui::ShowHelpMarkerSameLine(
        L("The game freezes for this long once you are in, so you can get your hands into position. 0 starts immediately.").c_str());

    ImGui::Spacing();
    ImGui::SeparatorText(L("Hotkeys").c_str());

    // The same bind widget the Settings window uses, so a stick or hitbox button can be
    // bound here too, and it writes to the same setting - this is not a second binding.
    DrawTakeoverHotkeyRow(L("Restart from takeover point").c_str(),
        HotkeyManager::Hotkey_LoadReplayState);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button((reconfiguring ? L("Apply") : L("Take over")).c_str(), ImVec2(120, 0))) {
        char setupText[32];
        snprintf(setupText, sizeof(setupText), "%g", Settings::settingsIni.replayTakeoverSetupSeconds);
        Settings::changeSetting("ReplayTakeoverSetupSeconds", setupText);
        if (reconfiguring) {
            pending_takeover_reconfigure = takeover_modal_side;
        } else {
            BeginReplayTakeover(takeover_modal_side == 0);
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Cancel").c_str(), ImVec2(120, 0))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void ScrWindow::BeginReplayTakeover(bool asP1) {
    char* bbcf_base = GetBbcfBaseAdress();

    if (!g_gameVals.pGameMode || !g_gameVals.pFrameCount) {
        return;
    }
    if (g_interfaces.player1.IsCharDataNullPtr() || g_interfaces.player2.IsCharDataNullPtr()) {
        return;
    }

    SnapshotApparatus* apparatus = EnsureTakeoverSnapshot();
    if (!apparatus) {
        return;
    }

    const char current_round = *(bbcf_base + 0x11C034C);

    *g_gameVals.pGameMode = GameMode_Training;
    apparatus->save_snapshot(0);

    // The side you did not take is the one the replay drives, so that is the input stream
    // to read. Formula for the start of a player's inputs in a round:
    //   bbcf_base + 0x115B470 + 0x8d4 + (0x7080 * player) + (0xE100 * round)
    const int player_to_playback = asP1 ? 1 : 0;
    char* rpstart = bbcf_base + 0x115B470 + 0x8d4 + (0x7080 * player_to_playback) + (0xE100 * current_round);

    replay_action_load.clear();
    replay_action_load.reserve(0x400);
    for (int i = 0; i < 0x400; i++) {
        replay_action_load.push_back(*(rpstart + (*g_gameVals.pFrameCount + i) * 2));
    }

    // The facing byte a playback slot carries is read against the training side - the side
    // you are on - not against the character replaying the inputs. Storing the replayed
    // side's facing here (what the old code did) is therefore inverted whenever the two
    // face each other, which is nearly always: that is the entire reason "Fix playback"
    // existed and why flipping it fixed things "most of the time".
    facing_left_replay_takeover = asP1
        ? (g_interfaces.player1.GetData()->facingLeft2 != 0 ? 1 : 0)
        : (g_interfaces.player2.GetData()->facingLeft2 != 0 ? 1 : 0);

    SaveTakeoverInputBinding(bbcf_base);
    *(bbcf_base + kTrainingSideOffset) = asP1 ? 0 : 1;
    // Your side reads the chosen slot; the replay's side keeps the other one, so the two
    // never point at the same device.
    *(bbcf_base + (asP1 ? kSlotForP1Offset : kSlotForP2Offset)) = TakeoverInputSlot();
    *(bbcf_base + (asP1 ? kSlotForP2Offset : kSlotForP1Offset)) = 1 - TakeoverInputSlot();

    takeover_active = true;
    takeover_as_p1 = asP1;
    takeover_round_reset_armed = false;
    wait_before_exec_s2 = Settings::settingsIni.replayTakeoverSetupSeconds;

    LOG(1, "[Takeover] begin asP1=%d round=%d frame=%d facing=%d\n",
        asP1 ? 1 : 0, (int)current_round, *g_gameVals.pFrameCount, facing_left_replay_takeover);

    // No second button press: being in takeover mode is the point of having pressed it.
    //
    // Latched rather than run from here. This is the middle of the draw pass; loading a
    // snapshot and arming the setup freeze belongs in the phase right after it, which is
    // where the hotkey path has always done it (WindowManager: RunPendingSaveStateRequests).
    // Doing it inline left the freeze to be undone by the rest of the frame, which is why
    // the setup time stopped pausing anything.
    pending_load_replay_state = true;
}

void ScrWindow::ReconfigureReplayTakeover(bool asP1) {
    if (!takeover_active || !snap_apparatus_takeover) {
        return;
    }

    // Back to the replay first, which restores the exact frame the takeover was taken at,
    // then take it over again. The input binding is left alone: SaveTakeoverInputBinding
    // only captures the first time, so the pads the user actually started with survive.
    playback_manager.set_playback_control(0);
    if (g_gameVals.pGameMode) {
        *g_gameVals.pGameMode = GameMode_ReplayTheater;
    }
    snap_apparatus_takeover->load_snapshot(0);

    LOG(1, "[Takeover] reconfiguring to asP1=%d\n", asP1 ? 1 : 0);
    BeginReplayTakeover(asP1);
}

void ScrWindow::EndReplayTakeover() {
    // Stop the playback before going back, or the replay inherits a running dummy action.
    playback_manager.set_playback_control(0);
    RestoreTakeoverInputBinding(GetBbcfBaseAdress());

    if (g_gameVals.pGameMode) {
        *g_gameVals.pGameMode = GameMode_ReplayTheater;
    }
    if (snap_apparatus_takeover) {
        snap_apparatus_takeover->load_snapshot(0);
    }

    takeover_active = false;
    takeover_round_reset_armed = false;
    LOG(1, "[Takeover] returned to replay\n");
}

SnapshotApparatus* ScrWindow::EnsureTakeoverSnapshot() {
    if (snap_apparatus_takeover == nullptr) {
        snap_apparatus_takeover = new SnapshotApparatus();
        snap_apparatus_takeover->ReserveSlots("replay_takeover", 1);
    }
    else if (!snap_apparatus_takeover->check_if_valid(g_interfaces.player1.GetData(),
        g_interfaces.player2.GetData())) {
        delete snap_apparatus_takeover;
        snap_apparatus_takeover = new SnapshotApparatus();
        snap_apparatus_takeover->ReserveSlots("replay_takeover", 1);
    }
    return snap_apparatus_takeover;
}

// Runs every frame from Tick(), not from the menu page: the two things it does have to keep
// happening while the mod menu is shut, which is when people actually play.
void ScrWindow::TickReplayTakeover() {
    if (!takeover_active) {
        return;
    }
    if (!g_gameVals.pGameMode || *g_gameVals.pGameMode != GameMode_Training) {
        return;
    }

    if (g_interfaces.player1.IsCharDataNullPtr() || g_interfaces.player2.IsCharDataNullPtr()) {
        // The match went away underneath us. Give the pads back rather than leaving the
        // takeover's input binding in place for whatever the user does next.
        RestoreTakeoverInputBinding(GetBbcfBaseAdress());
        takeover_active = false;
        takeover_round_reset_armed = false;
        LOG(1, "[Takeover] match ended underneath the takeover; bindings restored\n");
        return;
    }

    // A takeover only swaps the game MODE. Underneath it is still the replay's versus match,
    // so a KO still counts a round and the second one ends the match into character select -
    // which is where a takeover session used to die with no way back. Reload the takeover
    // point instead, which is what you wanted the moment the dummy died anyway.
    if (g_gameVals.pMatchState) {
        const int matchState = *g_gameVals.pMatchState;
        const bool roundOver = matchState == MatchState_FinishSign ||
            matchState == MatchState_WinLoseSign ||
            matchState == MatchState_VictoryScreen;

        if (roundOver && !takeover_round_reset_armed) {
            takeover_round_reset_armed = true;
            LOG(1, "[Takeover] round ended (matchState=%d); reloading the takeover point\n", matchState);
            // Tick() runs from a mid-frame hook; the reload goes through the same latch the
            // hotkey uses so the snapshot is never loaded from in there.
            pending_load_replay_state = true;
        }
        else if (matchState == MatchState_Fight) {
            takeover_round_reset_armed = false;
        }
    }

    // Keeps the clock off the match, the same way the menu page used to while it was open.
    if (g_gameVals.pMatchTimer) {
        *g_gameVals.pMatchTimer = 3597;
    }
}

void ScrWindow::DrawRoomSettingsBody() {
    const char* items[] = { "No Rematch", "No limit", "FT2", "FT3", "FT5", "FT10"};
    static int currentItem = 0;

    if (!g_gameVals.pRoom || g_gameVals.pRoom->roomStatus == RoomStatus_Unavailable 
        || !(RoomManager::GetRoomSettingsStaticBaseAdress()))
    {
        ImGui::TextDisabled("%s", L("Join or host a room to change these.").c_str());
        return;
    }
    //sets the selected value in the dropdown the actual value
    switch (g_gameVals.pRoom->rematch) {
    case RoomRematch::RematchType_Disabled:
        currentItem = 0;
        break;
    case RoomRematch::RematchType_Unlimited:
        currentItem = 1;
        break;
    case RoomRematch::RematchType_Ft2:
        currentItem = 2;
        break;
    case RoomRematch::RematchType_Ft3:
        currentItem = 3;
        break;
    case RoomRematch::RematchType_Ft5:
        currentItem = 4;
        break;
    case RoomRematch::RematchType_Ft10:
        currentItem = 5;
        break;
    default:
        break;
    }

    bool rematchSettingsChanged = ImGui::Combo("Rematch Settings##dropdown", &currentItem, items, IM_ARRAYSIZE(items));
    ImGui::ShowHelpMarkerSameLine(Messages.Rematch_settings_tooltip());
    if (rematchSettingsChanged)
    {
        switch (currentItem) {
        case 0:
            g_interfaces.pRoomManager->ChangeRematchAmnt(0);
            break;

        case 1:
            g_interfaces.pRoomManager->ChangeRematchAmnt(-1);
            break;

        case 2:
            g_interfaces.pRoomManager->ChangeRematchAmnt(2);
            break;

        case 3:
            g_interfaces.pRoomManager->ChangeRematchAmnt(3);
            break;

        case 4:
            g_interfaces.pRoomManager->ChangeRematchAmnt(5);
            break;

        case 5:
            g_interfaces.pRoomManager->ChangeRematchAmnt(10);
            break;



        default:
            break;
        }

    };
};
