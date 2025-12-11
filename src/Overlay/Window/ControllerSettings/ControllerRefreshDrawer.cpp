#include "ControllerRefreshDrawer.h"

#include "Core/ControllerOverrideManager.h"
#include "Core/Settings.h"
#include "Core/logger.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace ControllerSettings
{
        void DrawControllerRefresh(ControllerOverrideManager& controllerManager, bool inDevelopmentFeaturesEnabled, bool steamInputLikely)
        {
                ImGui::VerticalSpacing(5);

                ImGui::HorizontalSpacing();
                if (ImGui::Button("Refresh controllers"))
                {
                        LOG(1, "MainWindow::DrawControllers - Refresh controllers clicked\n");
                        controllerManager.RefreshDevicesAndReinitializeGame();
                }
                ImGui::SameLine();
                ImGui::ShowHelpMarker("Reload the controller list and reinitialize input slots to match connected devices.");

                if (inDevelopmentFeaturesEnabled)
                {
                        ImGui::SameLine();
                        if (steamInputLikely)
                        {
                                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                        }
                        if (ImGui::Button("Open Joy.cpl"))
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

                ImGui::VerticalSpacing(5);

                ImGui::HorizontalSpacing();
                bool autoRefreshEnabled = controllerManager.IsAutoRefreshEnabled();
                if (ImGui::Checkbox("Automatically Update Controllers", &autoRefreshEnabled))
                {
                        controllerManager.SetAutoRefreshEnabled(autoRefreshEnabled);
                        Settings::settingsIni.autoUpdateControllers = autoRefreshEnabled;
                        Settings::changeSetting("AutomaticallyUpdateControllers", autoRefreshEnabled ? "1" : "0");
                }
                ImGui::SameLine();
                ImGui::ShowHelpMarker("Automatically refresh controller slots when devices change. The internal call to refresh controllers may freeze the game for a few moments, so only enable this if you are okay with short pauses.");

                if (inDevelopmentFeaturesEnabled)
                {
                        ImGui::VerticalSpacing(3);
                        ImGui::HorizontalSpacing();
                        ImGui::TextDisabled("STEAM INPUT: %s", steamInputLikely ? "ON" : "OFF");
                }
        }
}
