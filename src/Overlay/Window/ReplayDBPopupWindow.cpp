#include "ReplayDBPopupWindow.h"

#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/utils.h"

#include "Core/info.h"
#include "Overlay/imgui_utils.h"
#include <cstdlib>
#include <string>


void ReplayDBPopupWindow::Update()
{
    if (!m_windowOpen)
        return;

    // This one used IWindow::Update, so it deferred to nothing and wrapped its modal in an
    // ImGui::Begin that never had any content of its own. Both mattered: nothing stopped it
    // fighting the update notifier for ImGui's single popup slot, and on every frame it lost
    // that fight its empty NoTitleBar wrapper was all that reached the screen.
    if (m_pWindowContainer->ShouldDeferExclusivePopup(WindowType_ReplayDBPopup))
        return;

    BeforeDraw();

    Draw();

    AfterDraw();
}

void ReplayDBPopupWindow::Draw()
{
    ImVec4 black = ImVec4(0.060, 0.060, 0.060, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, black);
    const char* popupTitle = Messages.Enable_Disable_Automatic_Replay_Uploads();
    ImGui::OpenPopup(popupTitle);

    const ImVec2 buttonSize = ImVec2(120, 23);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    // BeginPopupModal's return value has to be honoured. When it returns false it opens no
    // window scope, so the old code drew the description and both consent buttons into the
    // WRAPPER window instead - a second, unlabelled copy of the prompt - and then called
    // EndPopup on a window that is not a popup. ImGui 1.92 catches that last part itself
    // (EndPopup opens with IM_ASSERT_USER_ERROR_RET and returns), so on this version it is
    // survivable rather than fatal; under the 1.53 this branch upgraded from, EndPopup ran
    // unconditionally and ended the wrapper, leaving IWindow::Update's own End() to pop one
    // window too many. Correct on both, and no longer dependent on which one is vendored.
    if (ImGui::BeginPopupModal(popupTitle, NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(Messages.Replay_uploads_description());
        ImGui::Separator();
        ImGui::AlignItemHorizontalCenter(buttonSize.x);
        if (ImGui::Button((std::string(Messages.ON()) + "##dbpopup").c_str(), buttonSize)) {
            Settings::changeSetting("UploadReplayData", std::to_string(1));
            Settings::settingsIni.uploadReplayData = 1;
            g_modVals.uploadReplayData = 1;
            ImGui::CloseCurrentPopup();
            Close();
        }
        //ImGui::SameLine();
        ImGui::AlignItemHorizontalCenter(buttonSize.x);
        if (ImGui::Button((std::string(Messages.OFF()) + "##dbpopup").c_str(), buttonSize)) {
            Settings::changeSetting("UploadReplayData", std::to_string(0));
            Settings::settingsIni.uploadReplayData = 0;
            g_modVals.uploadReplayData = 0;
            ImGui::CloseCurrentPopup();
            Close();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}
