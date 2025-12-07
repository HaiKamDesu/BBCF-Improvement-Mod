#include "LocalControllerOverrideDrawer.h"

#include "Core/ControllerOverrideManager.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"

#include <Windows.h>

namespace ControllerSettings
{
        void DrawLocalControllerOverride(ControllerOverrideManager& controllerManager, bool inDevelopmentFeaturesEnabled, bool steamInputLikely)
        {
                if (!inDevelopmentFeaturesEnabled && controllerManager.IsOverrideEnabled())
                {
                        controllerManager.SetOverrideEnabled(false);
                }

                if (!inDevelopmentFeaturesEnabled)
                {
                        return;
                }

                ImGui::HorizontalSpacing();
                bool overrideEnabled = controllerManager.IsOverrideEnabled();
                if (steamInputLikely && overrideEnabled)
                {
                        controllerManager.SetOverrideEnabled(false);
                        overrideEnabled = false;
                }
                if (steamInputLikely)
                {
                        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                }
                if (ImGui::Checkbox("Local Controller Override", &overrideEnabled))
                {
                        controllerManager.SetOverrideEnabled(overrideEnabled);
                }
                ImGui::SameLine();
                ImGui::ShowHelpMarker("Choose which connected controller or the keyboard should be Player 1 and Player 2. Use Refresh when devices change.");

                bool showOverrideControls = overrideEnabled && !steamInputLikely;

                if (steamInputLikely)
                {
                        ImGui::PopStyleVar();
                        ImGui::PopItemFlag();
                }

                if (!showOverrideControls)
                {
                        return;
                }

                ImGui::VerticalSpacing(3);
                ImGui::HorizontalSpacing();
                const auto& devices = controllerManager.GetDevices();
                if (devices.empty())
                {
                        ImGui::TextDisabled("No input devices detected.");
                        return;
                }

                auto renderPlayerSelector = [&](const char* label, int playerIndex) {
                        GUID selection = controllerManager.GetPlayerSelection(playerIndex);
                        const ControllerDeviceInfo* selectedInfo = nullptr;
                        std::string preview = devices.front().name;
                        for (const auto& device : devices)
                        {
                                if (IsEqualGUID(device.guid, selection))
                                {
                                        preview = device.name;
                                        selectedInfo = &device;
                                        break;
                                }
                        }

                        if (ImGui::BeginCombo(label, preview.c_str()))
                        {
                                for (const auto& device : devices)
                                {
                                        bool selected = IsEqualGUID(device.guid, selection);
                                        if (ImGui::Selectable(device.name.c_str(), selected))
                                        {
                                                controllerManager.SetPlayerSelection(playerIndex, device.guid);
                                                selection = device.guid;
                                                selectedInfo = &device;
                                        }

                                        if (selected)
                                        {
                                                ImGui::SetItemDefaultFocus();
                                        }
                                }

                                ImGui::EndCombo();
                        }

                        bool disableTest = (selectedInfo && selectedInfo->isKeyboard);
                        if (disableTest)
                        {
                                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                        }

                        ImGui::SameLine();
                        std::string testLabel = std::string("Test##player") + std::to_string(playerIndex + 1);
                        if (ImGui::Button(testLabel.c_str()))
                        {
                                controllerManager.OpenDeviceProperties(selection);
                        }

                        if (disableTest)
                        {
                                ImGui::PopStyleVar();
                                ImGui::PopItemFlag();
                        }
                        };

                renderPlayerSelector("Player 1 Controller", 0);
                ImGui::HorizontalSpacing();
                renderPlayerSelector("Player 2 Controller", 1);
        }
}
