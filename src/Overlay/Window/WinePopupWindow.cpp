#include "WinePopupWindow.h"

#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Overlay/imgui_utils.h"

#include <imgui.h>

// Both answers are final. Whichever the user picks, the prompt is done asking: it is the
// same question with the same answer on every launch, and it was being asked on every
// launch because nothing recorded that it had been answered.
static void RememberAnswer()
{
    Settings::changeSetting("WineControllerPromptAnswered", "1");
    Settings::settingsIni.wineControllerPromptAnswered = 1;
}

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
        ImGui::TextWrapped(Messages.Wine_Proton_detected_The_mod_turned_its_controller_tools_off_so_the_game_does_not_crash_at_startup_This_choice_is_remembered_you_can_change_it_later_under_Controller_in_the_mod_settings_Forcing_them_on_is_unsupported_and_applies_the_next_time_you_start_the_game());
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::AlignItemHorizontalCenter(buttonSize.x);
        if (ImGui::Button(Messages.Enable_anyway(), buttonSize))
        {
            // The force flag, not EnableControllerHooks. Turning the plain setting back on
            // achieved nothing: the startup gate rewrites it to 0 on the next Wine launch
            // before anything reads it, so "Enable anyway" quietly undid itself. The force
            // flag is the one that survives that gate and overrides the platform check on
            // its own - it is also, deliberately, the unsupported option.
            Settings::changeSetting("ForceEnableControllerSettingHooks", "1");
            Settings::settingsIni.ForceEnableControllerSettingHooks = 1;
            RememberAnswer();
            ImGui::CloseCurrentPopup();
            Close();
        }

        ImGui::AlignItemHorizontalCenter(buttonSize.x);
        if (ImGui::Button(Messages.Keep_disabled(), buttonSize))
        {
            Settings::changeSetting("EnableControllerHooks", "0");
            Settings::settingsIni.EnableControllerHooks = 0;
            Settings::changeSetting("ForceEnableControllerSettingHooks", "0");
            Settings::settingsIni.ForceEnableControllerSettingHooks = 0;
            RememberAnswer();
            ImGui::CloseCurrentPopup();
            Close();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

