#include "TasWindow.h"

#include "Core/HotkeyManager.h"
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
        ImGui::TextUnformatted("TAS mode is not active.");
        if (ImGui::Button("Enter TAS mode")) {
            manager.Enter();
        }
        if (!manager.GetError().empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", manager.GetError().c_str());
        }
        return;
    }

    ImGui::TextUnformatted("TAS mode: frame editor for training matches");
    const bool playbackRunning = manager.IsPlaybackRunning();
    ImGui::NewLine();
    ImGui::TextUnformatted("Export:");
    ImGui::SameLine();
    ImGui::Checkbox("Include initial conditions##tas_export_conditions", &m_includeInitialConditions);
    ImGui::SameLine();
    ImGui::InputText("##tas_export_filename", m_exportFileName, sizeof(m_exportFileName));
    ImGui::SameLine();
    if (ImGui::Button("Export .txt##tas_export") && !playbackRunning) {
        std::string path = m_exportFileName[0] ? m_exportFileName : NextTasFileName();
        if (path.size() < 4 || path.substr(path.size() - 4) != ".txt") path += ".txt";
        manager.ExportMovie(path, m_includeInitialConditions);
        RefreshTasFiles(&m_importFiles);
    }
    ImGui::NewLine();
    ImGui::TextUnformatted("Import:");
    ImGui::SameLine();
    if (ImGui::BeginCombo("##tas_import_file", m_selectedImportFile.empty() ? "Select .txt file" : m_selectedImportFile.c_str())) {
        RefreshTasFiles(&m_importFiles);
        for (const std::string& file : m_importFiles) {
            const bool selected = file == m_selectedImportFile;
            if (ImGui::Selectable(file.c_str(), selected)) m_selectedImportFile = file;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Import##tas_import") && !playbackRunning && !m_selectedImportFile.empty()) {
        manager.ImportMovie(m_selectedImportFile);
    }
    ImGui::Separator();
    ImGui::Text("Current frame: %u", manager.GetCurrentFrame());
    ImGui::Text("Base frame: %s%u", manager.HasBaseSnapshot() ? "" : "not saved / ", manager.GetBaseFrame());
    ImGui::Text("Input progress: %u / %u",
        static_cast<unsigned int>(manager.GetCursor()),
        static_cast<unsigned int>(manager.GetFrameCount()));
    ImGui::Text("Rerecord count: %u", manager.GetRerecordCount());
    bool autoLoad = manager.IsAutoLoadAfterPlayback();
    if (ImGui::Checkbox("Auto load base state and freeze after playback", &autoLoad)) {
        manager.SetAutoLoadAfterPlayback(autoLoad);
    }
    if (ImGui::Button("Save base state")) {
        manager.SaveBaseSnapshot();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load base state")) {
        manager.LoadBaseSnapshot();
    }
    ImGui::SameLine();
    if (ImGui::Button("Resume game")) {
        manager.ResumeGame();
    }
    ImGui::SameLine();
    if (ImGui::Button("Exit TAS mode")) {
        manager.Exit();
        Close();
        return;
    }

    ImGui::SameLine();
    if (manager.IsPlaying()) {
        if (ImGui::Button("Stop playback")) {
            manager.StopPlayback();
        }
    } else {
        if (ImGui::Button("Preview playback")) {
            manager.StartPlayback(false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Presentation playback")) {
            manager.StartPlayback(true);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset movie")) {
        manager.ResetMovie();
    }
    ImGui::Text("Movie frames: %u", static_cast<unsigned int>(manager.GetRecordedFrameCount()));
    if (manager.IsEditingRecording()) {
        ImGui::Text("Movie edit frame: %u", static_cast<unsigned int>(manager.GetCursor()));
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Input commands");
    ImGui::TextUnformatted("Examples: 5C, 4C, 28D, 623C, 656 (dash). Each digit uses one frame; 66 means two frames holding 6, while 656 performs a dash.");

    if (ImGui::InputText("P1##tas_p1", m_p1Input, sizeof(m_p1Input))) {
        manager.SetP1Text(m_p1Input);
    }
    if (ImGui::InputText("P2##tas_p2", m_p2Input, sizeof(m_p2Input))) {
        manager.SetP2Text(m_p2Input);
    }

    if (ImGui::Button("Parse input")) {
        manager.SetInputText(m_p1Input, m_p2Input);
    }
    ImGui::SameLine();
    ImGui::InputInt("Frame count##tas_frame_count", &m_frameCount);
    if (m_frameCount < 1) {
        m_frameCount = 1;
    }

    ImGui::SameLine();
    if (ImGui::Button("Advance N frames") && !playbackRunning) {
        if (manager.IsEditingRecording()) {
            manager.EditAndAdvanceFrames(m_frameCount);
        } else {
            manager.AdvanceFrames(m_frameCount);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Rewind N frames") && !playbackRunning) {
        manager.RewindFrames(m_frameCount);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset parsed input")) {
        manager.ResetParsedInputs();
    }

    const TasFrameInput input = manager.GetCurrentInput();
    ImGui::Text("Current input: P1=%u P2=%u", input.p1, input.p2);

    ImGui::Separator();
    ImGui::TextUnformatted("Instructions");
    ImGui::TextWrapped("1. Enter a training match and save a base state.");
    ImGui::TextWrapped("2. Use numpad directions and buttons, for example 623C means 6, 2, then 3+C.");
    ImGui::TextWrapped("3. Set a frame count and use Advance N Frames. Inputs are applied frame by frame.");
    ImGui::TextWrapped("4. Preview starts immediately and freezes at the movie end for continued frame editing. Presentation hides TAS UI, holds neutral for 60 game frames, plays the movie, then holds neutral for 240 frames.");
    ImGui::TextWrapped("5. Rewind N Frames reloads the base state, replays to the target, and deletes all later movie frames.");
    ImGui::TextWrapped("6. Export creates a numbered .txt file when the filename is empty. Import selects a .txt file from the current game directory. Save the matching base state after import.");
    ImGui::TextWrapped("Directions: 7 8 9 / 4 5 6 / 1 2 3; 5 means neutral. Parsed commands hold neutral after their final frame.");

    if (!manager.GetStatus().empty()) {
        ImGui::TextWrapped("%s", manager.GetStatus().c_str());
    }
    if (!manager.GetError().empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", manager.GetError().c_str());
    }
}

// Lifecycle cleanup is handled by Update(), including a close via the window X button.
