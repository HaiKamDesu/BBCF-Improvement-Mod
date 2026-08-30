#include "TasWindow.h"

#include "Core/HotkeyManager.h"
#include "Core/Localization.h"
#include "Core/interfaces.h"
#include "Game/TasManager.h"
#include "Game/gamestates.h"
#include "Overlay/WindowContainer/WindowContainer.h"

#include <Windows.h>
#include <algorithm>
#include <cstdio>

namespace {
void RefreshTasFiles(std::vector<std::string>* files) {
    files->clear();
    WIN32_FIND_DATAA data{};
    HANDLE handle = FindFirstFileA("*.txt", &data);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files->push_back(data.cFileName);
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
    std::sort(files->begin(), files->end());
}

std::string NextTasFileName() {
    for (unsigned int number = 1;; ++number) {
        char name[64]{};
        std::snprintf(name, sizeof(name), "tas_movie_%u.txt", number);
        if (GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES) return name;
    }
}
}

// TAS playback uses the game's native virtual stick display.

void TasWindow::Update() {
    TasManager& manager = TasManager::Instance();
    if (!manager.IsActive()) {
        if (IsOpen()) {
            manager.Exit();
        }
        return;
    }

    if (manager.IsPlaying() && manager.IsPlaybackUiHidden()) {
        if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_TasParse)) {
            manager.StopPlayback();
        }
        return;
    }
    if (!IsOpen()) {
        manager.Exit();
        return;
    }

    if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_TasParse)) {
        manager.SetInputText(m_p1Input, m_p2Input);
    }
    if (!manager.IsPlaybackRunning() && HotkeyManager::WasPressed(HotkeyManager::Hotkey_TasRewind)) {
        manager.RewindFrames(m_frameCount);
    }
    if (!manager.IsPlaybackRunning() && HotkeyManager::WasPressed(HotkeyManager::Hotkey_TasAdvance)) {
        if (manager.IsEditingRecording()) {
            manager.EditAndAdvanceFrames(m_frameCount);
        } else {
            manager.AdvanceFrames(m_frameCount);
        }
    }
    IWindow::Update();
}

void TasWindow::Draw() {
    TasManager& manager = TasManager::Instance();

    if (!manager.IsActive()) {
        if (IsOpen()) {
            Close();
        }
        ImGui::TextUnformatted(L("TAS mode is not active.").c_str());
        if (ImGui::Button(L("Enter TAS mode").c_str())) {
            manager.Enter();
        }
        if (!manager.GetError().empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", manager.GetError().c_str());
        }
        return;
    }

    ImGui::TextUnformatted(L("TAS mode: frame editor for training matches").c_str());
    const bool playbackRunning = manager.IsPlaybackRunning();
    ImGui::NewLine();
    ImGui::TextUnformatted(L("Export:").c_str());
    ImGui::SameLine();
    ImGui::Checkbox((L("Include initial conditions") + "##tas_export_conditions").c_str(), &m_includeInitialConditions);
    ImGui::SameLine();
    ImGui::InputText("##tas_export_filename", m_exportFileName, sizeof(m_exportFileName));
    ImGui::SameLine();
    if (ImGui::Button((L("Export .txt") + "##tas_export").c_str()) && !playbackRunning) {
        std::string path = m_exportFileName[0] ? m_exportFileName : NextTasFileName();
        if (path.size() < 4 || path.substr(path.size() - 4) != ".txt") path += ".txt";
        manager.ExportMovie(path, m_includeInitialConditions);
        RefreshTasFiles(&m_importFiles);
    }
    ImGui::NewLine();
    ImGui::TextUnformatted(L("Import:").c_str());
    ImGui::SameLine();
    if (ImGui::BeginCombo("##tas_import_file", m_selectedImportFile.empty() ? L("Select .txt file").c_str() : m_selectedImportFile.c_str())) {
        RefreshTasFiles(&m_importFiles);
        for (const std::string& file : m_importFiles) {
            const bool selected = file == m_selectedImportFile;
            if (ImGui::Selectable(file.c_str(), selected)) m_selectedImportFile = file;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button((L("Import") + "##tas_import").c_str()) && !playbackRunning && !m_selectedImportFile.empty()) {
        manager.ImportMovie(m_selectedImportFile);
    }
    ImGui::Separator();
    ImGui::Text(Messages.Current_frame_u(), manager.GetCurrentFrame());
    ImGui::Text(Messages.Base_frame_s_u(), manager.HasBaseSnapshot() ? "" : L("not saved / ").c_str(), manager.GetBaseFrame());
    ImGui::Text(Messages.Input_progress_u_u(),
        static_cast<unsigned int>(manager.GetCursor()),
        static_cast<unsigned int>(manager.GetFrameCount()));
    ImGui::Text(Messages.Rerecord_count_u(), manager.GetRerecordCount());
    bool autoLoad = manager.IsAutoLoadAfterPlayback();
    if (ImGui::Checkbox(L("Auto load base state and freeze after playback").c_str(), &autoLoad)) {
        manager.SetAutoLoadAfterPlayback(autoLoad);
    }
    if (ImGui::Button(L("Save base state").c_str())) {
        manager.SaveBaseSnapshot();
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Load base state").c_str())) {
        manager.LoadBaseSnapshot();
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Resume game").c_str())) {
        manager.ResumeGame();
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Exit TAS mode").c_str())) {
        manager.Exit();
        Close();
        return;
    }

    ImGui::SameLine();
    if (manager.IsPlaying()) {
        if (ImGui::Button(L("Stop playback").c_str())) {
            manager.StopPlayback();
        }
    } else {
        if (ImGui::Button(L("Preview playback").c_str())) {
            manager.StartPlayback(false);
        }
        ImGui::SameLine();
        if (ImGui::Button(L("Presentation playback").c_str())) {
            manager.StartPlayback(true);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Reset movie").c_str())) {
        manager.ResetMovie();
    }
    ImGui::Text(Messages.Movie_frames_u(), static_cast<unsigned int>(manager.GetRecordedFrameCount()));
    if (manager.IsEditingRecording()) {
        ImGui::Text(Messages.Movie_edit_frame_u(), static_cast<unsigned int>(manager.GetCursor()));
    }

    ImGui::Separator();
    ImGui::TextUnformatted(L("Input commands").c_str());
    ImGui::TextWrapped("%s", Messages.Examples_5C_4C_28D_623C_656_dash_Each_digit_uses_one_frame_66_means_two_frames_holding_6_while_656_performs_a_dash());

    if (ImGui::InputText("P1##tas_p1", m_p1Input, sizeof(m_p1Input))) {
        manager.SetP1Text(m_p1Input);
    }
    if (ImGui::InputText("P2##tas_p2", m_p2Input, sizeof(m_p2Input))) {
        manager.SetP2Text(m_p2Input);
    }

    if (ImGui::Button(L("Parse input").c_str())) {
        manager.SetInputText(m_p1Input, m_p2Input);
    }
    ImGui::SameLine();
    ImGui::InputInt((L("Frame count") + "##tas_frame_count").c_str(), &m_frameCount);
    if (m_frameCount < 1) {
        m_frameCount = 1;
    }

    ImGui::SameLine();
    if (ImGui::Button(L("Advance N frames").c_str()) && !playbackRunning) {
        if (manager.IsEditingRecording()) {
            manager.EditAndAdvanceFrames(m_frameCount);
        } else {
            manager.AdvanceFrames(m_frameCount);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Rewind N frames").c_str()) && !playbackRunning) {
        manager.RewindFrames(m_frameCount);
    }
    ImGui::SameLine();
    if (ImGui::Button(L("Reset parsed input").c_str())) {
        manager.ResetParsedInputs();
    }

    const TasFrameInput input = manager.GetCurrentInput();
    ImGui::Text(Messages.Current_input_P1_u_P2_u(), input.p1, input.p2);

    ImGui::Separator();
    ImGui::TextUnformatted(L("Instructions").c_str());
    ImGui::TextWrapped("%s", L("1. Enter a training match and save a base state.").c_str());
    ImGui::TextWrapped("%s", L("2. Use numpad directions and buttons, for example 623C means 6, 2, then 3+C.").c_str());
    ImGui::TextWrapped("%s", L("3. Set a frame count and use Advance N Frames. Inputs are applied frame by frame.").c_str());
    ImGui::TextWrapped("%s", L("4. Preview starts immediately and freezes at the movie end for continued frame editing. Presentation hides TAS UI, holds neutral for 60 game frames, plays the movie, then holds neutral for 240 frames.").c_str());
    ImGui::TextWrapped("%s", L("5. Rewind N Frames reloads the base state, replays to the target, and deletes all later movie frames.").c_str());
    ImGui::TextWrapped("%s", L("6. Export creates a numbered .txt file when the filename is empty. Import selects a .txt file from the current game directory. Save the matching base state after import.").c_str());
    ImGui::TextWrapped("%s", L("Directions: 7 8 9 / 4 5 6 / 1 2 3; 5 means neutral. Parsed commands hold neutral after their final frame.").c_str());

    if (!manager.GetStatus().empty()) {
        ImGui::TextWrapped("%s", manager.GetStatus().c_str());
    }
    if (!manager.GetError().empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", manager.GetError().c_str());
    }
}

// Lifecycle cleanup is handled by Update(), including a close via the window X button.
