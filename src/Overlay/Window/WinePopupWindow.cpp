#include "WinePopupWindow.h"

#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Overlay/imgui_utils.h"

#include <imgui.h>

void WinePopupWindow::Update()
{
    if (!m_windowOpen)
        return;

    // The scalable way this asked for: one rule, in WindowContainer, that every exclusive
    // popup consults - including the update notifier, which this ad-hoc check never knew
    // about and which is the pairing that actually bit users.
    if (m_pWindowContainer->ShouldDeferExclusivePopup(WindowType_WinePopup))
        return;

    BeforeDraw();

    Draw();

    AfterDraw();
}

void WinePopupWindow::Draw()
{
    ImVec4 black = ImVec4(0.060f, 0.060f, 0.060f, 1.0f);
    const ImVec4 warningColor = ImVec4(1.0f, 0.95f, 0.55f, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, black);
    ImGui::OpenPopup(Messages.Wine_or_Proton_detected());

    const ImVec2 buttonSize = ImVec2(120, 23);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal(Messages.Wine_or_Proton_detected(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, warningColor);
        ImGui::TextWrapped(Messages.Wine_Proton_detected_Controller_hooks_were_disabled_to_prevent_startup_crashes_Enable_below_or_set_ForceEnableControllerSettingHooks_to_1_in_settings_ini_to_override_detection());
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::AlignItemHorizontalCenter(buttonSize.x);
        if (ImGui::Button(Messages.Enable_anyway(), buttonSize))
        {
            Settings::changeSetting("EnableControllerHooks", "1");
            Settings::settingsIni.EnableControllerHooks = 1;
            ImGui::CloseCurrentPopup();
            Close();
        }

        ImGui::AlignItemHorizontalCenter(buttonSize.x);
        if (ImGui::Button(Messages.Keep_disabled(), buttonSize))
        {
            Settings::changeSetting("EnableControllerHooks", "0");
            Settings::settingsIni.EnableControllerHooks = 0;
            ImGui::CloseCurrentPopup();
            Close();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

