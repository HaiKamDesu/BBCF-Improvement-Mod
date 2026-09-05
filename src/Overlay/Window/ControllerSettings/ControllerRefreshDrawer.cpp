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
                ImGui::ShowHelpMarkerSameLine(Messages.Controller_refresh_help());

                // Every step wraps: this row can hold three controls, and "Auto-refresh" is the
                // last of them, so on a narrow menu it was simply pushed off the right edge.
                if (inDevelopmentFeaturesEnabled)
                {
                        ImGui::SameLineOrWrap(ImGui::ButtonWidth(Messages.Open_Joy_cpl()));
                        if (steamInputLikely)
                        {
                                ImGui::BeginDisabled();
                        }

                        if (ImGui::Button(Messages.Open_Joy_cpl()))
                        {
                                LOG(1, "MainWindow::DrawControllers - Joy.cpl clicked\n");
                                controllerManager.OpenControllerControlPanel();
                        }

                        if (steamInputLikely)
                        {
                                ImGui::EndDisabled();
                        }
                }

                ImGui::SameLineOrWrap(ImGui::CheckboxWidth(Messages.Auto_refresh()));
                bool autoRefreshEnabled = controllerManager.IsAutoRefreshEnabled();
                if (ImGui::CheckboxWrapped(Messages.Auto_refresh(), &autoRefreshEnabled))
                {
                        controllerManager.SetAutoRefreshEnabled(autoRefreshEnabled);
                        Settings::settingsIni.autoUpdateControllers = autoRefreshEnabled;
                        Settings::changeSetting("AutomaticallyUpdateControllers", autoRefreshEnabled ? "1" : "0");
                }
                ImGui::ShowHelpMarkerSameLine(Messages.Auto_refresh_warning());

                if (inDevelopmentFeaturesEnabled)
                {
                        ImGui::VerticalSpacing(3);
                        ImGui::HorizontalSpacing();
                        ImGui::TextDisabled(Messages.STEAM_INPUT_s(), steamInputLikely ? Messages.ON() : Messages.OFF());
                }
        }
}
