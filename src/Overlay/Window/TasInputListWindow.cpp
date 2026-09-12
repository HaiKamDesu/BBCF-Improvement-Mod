#include "TasInputListWindow.h"

#include "Core/Localization.h"
#include "Game/TasManager.h"
#include "Overlay/imgui_utils.h"

#include <cfloat>

void TasInputListWindow::Update() {
    TasManager& manager = TasManager::Instance();

    // The list is a companion to the editor, so it follows it: closed outright when TAS mode
    // ends, and skipped without closing while a presentation hides the TAS interface, so it
    // comes back exactly as the user left it.
    if (!manager.IsActive()) {
        if (IsOpen()) {
            Close();
        }
        m_list.ClearSelection();
        return;
    }
    if (manager.IsPlaying() && manager.IsPlaybackUiHidden()) {
        return;
    }

    IWindow::Update();
}

void TasInputListWindow::BeforeDraw() {
    // Tall and narrow by default: this is a list of frames, not a table of data.
    ImGui::SetNextWindowSize(ImVec2(300.0f, 520.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f, 200.0f), ImVec2(FLT_MAX, FLT_MAX));
}

void TasInputListWindow::Draw() {
    if (!TasManager::Instance().IsActive()) {
        ImGui::TextDisabled("%s", L("TAS mode is not active.").c_str());
        return;
    }

    m_list.Draw(m_document);
}
