#include "ControllerRefreshDrawer.h"

#include "Core/ControllerOverrideManager.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/logger.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace ControllerSettings
{
        void DrawControllerRefresh(ControllerOverrideManager& controllerManager, bool inDevelopmentFeaturesEnabled, bool steamInputLikely)
        {
                ImGui::HorizontalSpacing();
                if (ImGui::Button(Messages.Refresh_controllers()))
                {
                        LOG(1, "MainWindow::DrawControllers - Refresh controllers clicked\n");
                        controllerManager.RefreshDevicesAndReinitializeGame();
                }
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Reload_the_controller_list_and_reinitialize_input_slots_to_match_connected_devices());

                if (inDevelopmentFeaturesEnabled)
                {
                        ImGui::SameLine();
                        if (steamInputLikely)
                        {
                                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                        }

                        if (ImGui::Button(Messages.Open_Joy_cpl()))
                        {
                                LOG(1, "MainWindow::DrawControllers - Joy.cpl clicked\n");
                                controllerManager.OpenControllerControlPanel();
                        }

                        if (steamInputLikely)
                        {
                                ImGui::PopStyleVar();
                                ImGui::PopItemFlag();
                        }
                }

                ImGui::SameLine();
                bool autoRefreshEnabled = controllerManager.IsAutoRefreshEnabled();
                if (ImGui::Checkbox(Messages.Auto_refresh(), &autoRefreshEnabled))
                {
                        controllerManager.SetAutoRefreshEnabled(autoRefreshEnabled);
                        Settings::settingsIni.autoUpdateControllers = autoRefreshEnabled;
                        Settings::changeSetting("AutomaticallyUpdateControllers", autoRefreshEnabled ? "1" : "0");
                }
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Automatically_refresh_controller_slots_when_devices_change_The_internal_call_to_refresh_controllers_may_freeze_the_game_for_a_few_moments_so_only_enable_this_if_you_are_okay_with_short_pauses());

                if (inDevelopmentFeaturesEnabled)
                {
                        ImGui::VerticalSpacing(3);
                        ImGui::HorizontalSpacing();
                        ImGui::TextDisabled(Messages.STEAM_INPUT_s(), steamInputLikely ? Messages.ON() : Messages.OFF());
                }
        }
}
