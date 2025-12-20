#include "SteamInputWarningDrawer.h"

#include "Core/ControllerOverrideManager.h"
#include "Core/Localization.h"
#include "Core/logger.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"

namespace ControllerSettings
{
        void DrawSteamInputWarning(bool steamInputLikely, bool inDevelopmentFeaturesEnabled)
        {
                if (!inDevelopmentFeaturesEnabled)
                {
                        return;
                }

                static bool loggedSteamInputState = false;
                static bool lastSteamInputState = false;
                if (!loggedSteamInputState || lastSteamInputState != steamInputLikely)
                {
                        LOG(1, "MainWindow::DrawControllerSettingSection - steamInputLikely=%d\n", steamInputLikely ? 1 : 0);
                        loggedSteamInputState = true;
                        lastSteamInputState = steamInputLikely;
                }

                if (!steamInputLikely)
                {
                        return;
                }

                ImGui::HorizontalSpacing();
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                        Messages.Steam_Input_appears_to_be_active_This_will_disable_some_of_this_section_s_features());
                ImGui::SameLine();
                ImGui::ShowHelpMarker(
                        Messages.The_internal_behavior_of_Steam_Input_hides_some_controllers_from_the_game_s_process_thus_making_some_controller_related_features_impossible_work_in_unintended_ways_The_disabled_features_include_Local_Controller_Override_Opening_Joy_cpl());
                ImGui::VerticalSpacing(5);
        }
}
