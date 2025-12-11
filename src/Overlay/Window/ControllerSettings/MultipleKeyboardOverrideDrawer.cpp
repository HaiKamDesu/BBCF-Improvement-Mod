#include "MultipleKeyboardOverrideDrawer.h"

#include "Core/ControllerOverrideManager.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cstring>
#include <utility>
#include <vector>

namespace ControllerSettings
{
        void DrawMultipleKeyboardOverride(ControllerOverrideManager& controllerManager)
        {
                ImGui::HorizontalSpacing();
                bool multiKeyboardOverride = controllerManager.IsMultipleKeyboardOverrideEnabled();
                if (ImGui::Checkbox("Multiple keyboards override", &multiKeyboardOverride))
                {
                        controllerManager.SetMultipleKeyboardOverrideEnabled(multiKeyboardOverride);
                }
                ImGui::SameLine();
                ImGui::ShowHelpMarker("Choose which physical keyboards should be treated as Player 1 when multiple keyboards are connected. All other keyboards will drive Player 2 using their saved mappings (defaults to WASD/JIKL).");

                if (!multiKeyboardOverride)
                {
                        return;
                }

                bool requestRenamePopup = false;
                bool requestMappingPopup = false;
                static bool renamePopupOpen = false;
                static KeyboardDeviceInfo renameTarget{};
                static char renameBuffer[128] = { 0 };
                static bool ignoredListOpen = false;
                static bool mappingPopupOpen = false;
                static KeyboardDeviceInfo mappingTarget{};
                struct MappingCaptureState
                {
                        bool capturing = false;
                        bool isMenu = true;
                        MenuAction menuAction = MenuAction::Up;
                        BattleAction battleAction = BattleAction::Up;

                        std::array<BYTE, 256> baselineState{};
                        bool baselineValid = false;
                };
                static MappingCaptureState captureState{};

                struct MappingNavState
                {
                        bool initialized = false;
                        int selectedIndex = 0;                // index in "all rows" (menu+battle)
                        std::array<BYTE, 256> lastKeyState{}; // for edge detection (new presses)

                        // NEW: tracking for "hold Right for 2 seconds to clear"
                        float rightHoldSeconds = 0.0f;
                        bool  rightWasDown = false;
                };
                static MappingNavState navState{};

                ImGui::VerticalSpacing(3);

                ImGui::HorizontalSpacing();
                if (ImGui::Button("Refresh keyboard list"))
                {
                        controllerManager.RefreshDevices();
                }
                ImGui::SameLine();
                ImGui::TextUnformatted("Players that are using the keyboard to play the game will be forced to use their respective keyboard");

                ImGui::VerticalSpacing(1);
                ImGui::HorizontalSpacing();
                const auto& summaryKeyboards = controllerManager.GetKeyboardDevices();
                const auto& summaryHandles = controllerManager.GetP1KeyboardHandles();
                std::vector<std::string> p1Labels;
                for (HANDLE handle : summaryHandles)
                {
                        auto it = std::find_if(summaryKeyboards.begin(), summaryKeyboards.end(), [&](const KeyboardDeviceInfo& dev) { return dev.deviceHandle == handle; });
                        if (it != summaryKeyboards.end())
                        {
                                p1Labels.push_back(it->displayName);
                        }
                }

                std::string p1Summary = p1Labels.empty() ? std::string("No keyboards selected") : p1Labels.front();
                for (size_t i = 1; i < p1Labels.size(); ++i)
                {
                        p1Summary += ", " + p1Labels[i];
                }

                ImGui::Text("%s will become Player 1", p1Summary.c_str());
                ImGui::Text("Other keyboards will become Player 2");
                ImGui::VerticalSpacing(3);

                auto drawKeyboardList = [&](const char* title, bool isP1)
                        {
                                ImGui::TextUnformatted(title);

                                std::vector<KeyboardDeviceInfo> devices = controllerManager.GetKeyboardDevices();
                                std::vector<HANDLE> p1Handles = controllerManager.GetP1KeyboardHandles();

                                if (devices.empty())
                                {
                                        ImGui::TextDisabled("No keyboards detected.");
                                        return;
                                }

                                if (ImGui::ListBoxHeader(isP1 ? "##keyboard_list_p1" : "##keyboard_list_p2", ImVec2(-FLT_MIN, 200)))
                                {
                                        for (auto& dev : devices)
                                        {
                                                if (dev.ignored)
                                                {
                                                        continue;
                                                }

                                                const std::string mappedName = dev.displayName.empty() ? dev.baseName : dev.displayName;
                                                const bool isPlayer1 = controllerManager.IsP1KeyboardHandle(dev.deviceHandle);
                                                if (isP1 != isPlayer1)
                                                {
                                                        continue;
                                                }

                                                if (!isPlayer1 && controllerManager.IsP1KeyboardHandle(dev.deviceHandle))
                                                {
                                                        continue;
                                                }

                                                ImGui::PushID(dev.deviceId.c_str());
                                                const bool selected = isPlayer1 && controllerManager.IsP1KeyboardHandle(dev.deviceHandle);
                                                if (ImGui::Selectable(mappedName.c_str(), selected))
                                                {
                                                        if (isP1 && !selected)
                                                        {
                                                                controllerManager.SetP1KeyboardHandleEnabled(dev.deviceHandle, true);
                                                                p1Handles.push_back(dev.deviceHandle);
                                                        }
                                                        else if (!isP1 && selected)
                                                        {
                                                                controllerManager.SetP1KeyboardHandleEnabled(dev.deviceHandle, false);
                                                                p1Handles.erase(std::remove(p1Handles.begin(), p1Handles.end(), dev.deviceHandle), p1Handles.end());
                                                        }
                                                }
                                                ImGui::SameLine();
                                                if (ImGui::SmallButton("Rename"))
                                                {
                                                        requestRenamePopup = true;
                                                        renameTarget = dev;
                                                        strncpy_s(renameBuffer, dev.displayName.c_str(), _TRUNCATE);
                                                }
                                                ImGui::SameLine();
                                                if (ImGui::SmallButton("Mapping"))
                                                {
                                                        requestMappingPopup = true;
                                                        mappingTarget = dev;
                                                }
                                                ImGui::SameLine();
                                                const bool isIgnored = dev.ignored;
                                                const bool ignorePressed = ImGui::SmallButton(isIgnored ? "Unignore" : "Ignore");
                                                if (ignorePressed)
                                                {
                                                        if (isIgnored)
                                                        {
                                                                controllerManager.UnignoreKeyboard(dev.canonicalId);
                                                        }
                                                        else
                                                        {
                                                                controllerManager.IgnoreKeyboard(dev);
                                                                controllerManager.SetP1KeyboardHandleEnabled(dev.deviceHandle, false);
                                                        }
                                                }
                                                ImGui::PopID();
                                        }

                                        ImGui::ListBoxFooter();

                                        // Save P1 handle list if anything changed.
                                        controllerManager.SetP1KeyboardHandles(p1Handles);
                                }
                        };

                ImGui::HorizontalSpacing();
                drawKeyboardList("Keyboards set to Player 1", true);
                ImGui::HorizontalSpacing();
                drawKeyboardList("Keyboards set to Player 2", false);

                if (ImGui::Button("Show ignored keyboards"))
                {
                        ignoredListOpen = true;
                }

                if (requestRenamePopup)
                {
                        renamePopupOpen = true;
                        ImGui::OpenPopup("Rename keyboard");
                }
                if (requestMappingPopup)
                {
                        mappingPopupOpen = true;
                        ImGui::OpenPopup("Configure keyboard mapping");
                        captureState.capturing = false;
                        captureState.baselineValid = false;
                        navState.initialized = false;
                        navState.rightHoldSeconds = 0.0f;
                        navState.rightWasDown = false;
                }

                if (renamePopupOpen)
                {
                        ImGui::SetNextWindowSize(ImVec2(440.0f, 150.0f), ImGuiCond_FirstUseEver);
                        if (ImGui::BeginPopupModal("Rename keyboard", &renamePopupOpen))
                        {
                                ImGui::InputText("", renameBuffer, sizeof(renameBuffer));
                                ImGui::VerticalSpacing(3);
                                if (ImGui::Button("Save"))
                                {
                                        controllerManager.RenameKeyboard(renameTarget, std::string(renameBuffer));
                                        renamePopupOpen = false;
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Cancel"))
                                {
                                        renamePopupOpen = false;
                                        ImGui::CloseCurrentPopup();
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Clear"))
                                {
                                        controllerManager.RenameKeyboard(renameTarget, "");
                                        renamePopupOpen = false;
                                        ImGui::CloseCurrentPopup();
                                }

                                ImGui::EndPopup();
                        }
                }

                if (ImGui::BeginPopupModal("Configure keyboard mapping", &mappingPopupOpen, ImGuiWindowFlags_AlwaysAutoResize))
                {
                        // Tell the controller manager that mapping is active this frame.
                        controllerManager.SetMappingPopupActive(true);

                        auto describeBindings = [](const std::vector<uint32_t>& bindings)
                                {
                                        if (bindings.empty())
                                        {
                                                return std::string("Unbound");
                                        }

                                        std::string text = ControllerOverrideManager::VirtualKeyToLabel(bindings.front());
                                        for (size_t i = 1; i < bindings.size(); ++i)
                                        {
                                                text += ", " + ControllerOverrideManager::VirtualKeyToLabel(bindings[i]);
                                        }

                                        return text;
                                };

                        KeyboardMapping mapping = controllerManager.GetKeyboardMapping(mappingTarget);

                        auto commitMenuBinding = [&](MenuAction action, const std::vector<uint32_t>& keys)
                                {
                                        mapping.menuBindings[action] = keys;
                                        controllerManager.SetKeyboardMapping(mappingTarget, mapping);
                                };

                        auto commitBattleBinding = [&](BattleAction action, const std::vector<uint32_t>& keys)
                                {
                                        mapping.battleBindings[action] = keys;
                                        controllerManager.SetKeyboardMapping(mappingTarget, mapping);
                                };

                        auto detectCapturedKey = [&]() -> uint32_t
                                {
                                        if (!captureState.capturing || !mappingTarget.deviceHandle)
                                        {
                                                return 0;
                                        }

                                        std::array<BYTE, 256> currentState{};
                                        if (!controllerManager.GetKeyboardStateSnapshot(mappingTarget.deviceHandle, currentState))
                                        {
                                                return 0;
                                        }

                                        if (!captureState.baselineValid)
                                        {
                                                captureState.baselineState = currentState;
                                                captureState.baselineValid = true;
                                                return 0;
                                        }

                                        for (uint32_t vk = 1; vk < 256; ++vk)
                                        {
                                                const bool wasPressed = (captureState.baselineState[vk] & 0x80) != 0;
                                                const bool isPressed = (currentState[vk] & 0x80) != 0;
                                                if (isPressed && !wasPressed)
                                                {
                                                        captureState.baselineState = currentState;
                                                        return vk;
                                                }
                                        }

                                        captureState.baselineState = currentState;
                                        return 0;
                                };

                        const uint32_t capturedKey = detectCapturedKey();
                        bool suppressNavThisFrame = false;
                        if (capturedKey != 0)
                        {
                                if (captureState.isMenu)
                                {
                                        commitMenuBinding(captureState.menuAction, { capturedKey });
                                }
                                else
                                {
                                        commitBattleBinding(captureState.battleAction, { capturedKey });
                                }

                                captureState.capturing = false;
                                captureState.baselineValid = false;

                                // NEW: don't let this key also act as navigation in this frame,
                                // and sync navState so it's not treated as a "new press" next frame.
                                suppressNavThisFrame = true;
                                if (mappingTarget.deviceHandle)
                                {
                                        std::array<BYTE, 256> navStateSnapshot{};
                                        if (controllerManager.GetKeyboardStateSnapshot(mappingTarget.deviceHandle, navStateSnapshot))
                                        {
                                                navState.lastKeyState = navStateSnapshot;
                                        }
                                }
                        }

                        // Extra handling while in bind mode: ESC cancels, ENTER clears
                        if (captureState.capturing && mappingTarget.deviceHandle)
                        {
                                std::array<BYTE, 256> currentState{};
                                if (controllerManager.GetKeyboardStateSnapshot(mappingTarget.deviceHandle, currentState))
                                {
                                        const bool escPressed = (currentState[VK_ESCAPE] & 0x80) != 0;
                                        const bool enterPressed = (currentState[VK_RETURN] & 0x80) != 0;

                                        if (escPressed)
                                        {
                                                // Just exit bind mode, keep existing binding
                                                captureState.capturing = false;
                                                captureState.baselineValid = false;

                                                // NEW: also suppress nav this frame and sync navState
                                                suppressNavThisFrame = true;
                                                navState.lastKeyState = currentState;
                                        }
                                        else if (enterPressed)
                                        {
                                                // Clear binding for the action we were editing
                                                if (captureState.isMenu)
                                                        commitMenuBinding(captureState.menuAction, {});
                                                else
                                                        commitBattleBinding(captureState.battleAction, {});

                                                captureState.capturing = false;
                                                captureState.baselineValid = false;

                                                // NEW: also suppress nav this frame and sync navState
                                                suppressNavThisFrame = true;
                                                navState.lastKeyState = currentState;
                                        }
                                }
                        }

                        // ---- Navigation using the device's menu bindings (Up/Down/Confirm/Return) ----
                        const int totalRows =
                                static_cast<int>(ControllerOverrideManager::GetMenuActions().size()) +
                                static_cast<int>(ControllerOverrideManager::GetBattleActions().size());

                        if (!navState.initialized)
                        {
                                navState.selectedIndex = 0;
                                navState.lastKeyState.fill(0);
                                navState.initialized = true;
                        }

                        if (totalRows > 0 && navState.selectedIndex >= totalRows)
                        {
                                navState.selectedIndex = totalRows - 1;
                        }

                        bool navConfirm = false;
                        bool navClose = false;
                        bool navClear = false;   // NEW: clear binding on selected row when true

                        if (!captureState.capturing && !suppressNavThisFrame && mappingTarget.deviceHandle && totalRows > 0)
                        {
                                std::array<BYTE, 256> currentState{};
                                if (controllerManager.GetKeyboardStateSnapshot(mappingTarget.deviceHandle, currentState))
                                {
                                        auto isPressedNow = [&](uint32_t vk)
                                                {
                                                        return (currentState[vk] & 0x80) != 0;
                                                };

                                        auto wasPressed = [&](uint32_t vk)
                                                {
                                                        return (navState.lastKeyState[vk] & 0x80) != 0;
                                                };

                                        auto isNewPress = [&](uint32_t vk)
                                                {
                                                        return isPressedNow(vk) && !wasPressed(vk);
                                                };

                                        auto actionNewPress = [&](MenuAction action) -> bool
                                                {
                                                        auto it = mapping.menuBindings.find(action);
                                                        if (it == mapping.menuBindings.end())
                                                                return false;

                                                        for (uint32_t key : it->second)
                                                        {
                                                                if (isNewPress(key))
                                                                        return true;
                                                        }
                                                        return false;
                                                };

                                        // NEW: "is this action currently held down?"
                                        auto actionHeld = [&](MenuAction action) -> bool
                                                {
                                                        auto it = mapping.menuBindings.find(action);
                                                        if (it == mapping.menuBindings.end())
                                                                return false;

                                                        for (uint32_t key : it->second)
                                                        {
                                                                if (isPressedNow(key))
                                                                        return true;
                                                        }
                                                        return false;
                                                };

                                        // Move selection with Up/Down
                                        if (actionNewPress(MenuAction::Up))
                                        {
                                                navState.selectedIndex = (navState.selectedIndex + totalRows - 1) % totalRows;
                                        }
                                        if (actionNewPress(MenuAction::Down))
                                        {
                                                navState.selectedIndex = (navState.selectedIndex + 1) % totalRows;
                                        }

                                        // Confirm or close with Confirm/Return
                                        if (actionNewPress(MenuAction::Confirm))
                                        {
                                                navConfirm = true;
                                        }
                                        if (actionNewPress(MenuAction::ReturnAction))
                                        {
                                                navClose = true;
                                        }

                                        // NEW: hold Right for 2+ seconds to clear binding on selected row
                                        if (actionHeld(MenuAction::Right))
                                        {
                                                navState.rightHoldSeconds += ImGui::GetIO().DeltaTime;
                                                navState.rightWasDown = true;
                                                if (navState.rightHoldSeconds >= 2.0f)
                                                {
                                                        navClear = true;
                                                }
                                        }
                                        else if (navState.rightWasDown)
                                        {
                                                // Reset timer when Right is released
                                                navState.rightHoldSeconds = 0.0f;
                                                navState.rightWasDown = false;
                                        }

                                        navState.lastKeyState = currentState;
                                }
                        }

                        ImGui::TextUnformatted("Menu action");
                        ImGui::Separator();

                        ImGui::PushID("MenuSection");
                        int rowIndex = 0;
                        auto drawMenuRow = [&](MenuAction action, int thisRowIndex, bool confirmForRow, bool clearForRow)
                                {
                                        const bool capturingThisRow = captureState.capturing && captureState.isMenu && (captureState.menuAction == action);
                                        const auto findIt = mapping.menuBindings.find(action);
                                        const bool hasBinding = findIt != mapping.menuBindings.end();
                                        const bool selectedForNav = navState.initialized && (thisRowIndex == navState.selectedIndex);
                                        std::string buttonLabel;
                                        if (!hasBinding)
                                        {
                                                buttonLabel = "Unbound";
                                        }
                                        else
                                        {
                                                buttonLabel = describeBindings(findIt->second);
                                        }
                                        if (capturingThisRow)
                                        {
                                                buttonLabel = "Press a key... (ESC=Cancel, ENTER=Clear)";
                                        }
                                        else if (selectedForNav)
                                        {
                                                buttonLabel += " *";
                                        }

                                        if (selectedForNav)
                                        {
                                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.25f, 0.45f, 1.0f));
                                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.35f, 0.55f, 1.0f));
                                                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.13f, 0.20f, 0.38f, 1.0f));
                                        }

                                        if (ImGui::Button(ControllerOverrideManager::GetMenuActionLabel(action), ImVec2(150, 0)))
                                        {
                                                captureState.capturing = false;
                                                captureState.baselineValid = false;

                                                captureState.capturing = true;
                                                captureState.isMenu = true;
                                                captureState.menuAction = action;
                                        }
                                        ImGui::SameLine();
                                        if (confirmForRow || ImGui::Button(buttonLabel.c_str(), ImVec2(300, 0)))
                                        {
                                                if (capturingThisRow)
                                                {
                                                        captureState.capturing = false;
                                                        captureState.baselineValid = false;
                                                }
                                                else
                                                {
                                                        captureState.capturing = true;
                                                        captureState.isMenu = true;
                                                        captureState.menuAction = action;
                                                        captureState.baselineValid = false;
                                                }
                                        }
                                        if (selectedForNav)
                                        {
                                                ImGui::PopStyleColor(3);
                                        }

                                        ImGui::SameLine();
                                        if (clearForRow || ImGui::Button("Clear##menu"))
                                        {
                                                commitMenuBinding(action, {});
                                                captureState.capturing = false;
                                                captureState.baselineValid = false;
                                        }
                                };

                        for (MenuAction action : ControllerOverrideManager::GetMenuActions())
                        {
                                ImGui::PushID(static_cast<int>(action));
                                const bool confirmForRow = navConfirm && (rowIndex == navState.selectedIndex);
                                const bool clearForRow = navClear && (rowIndex == navState.selectedIndex);
                                drawMenuRow(action, rowIndex, confirmForRow, clearForRow);
                                ImGui::PopID();
                                ++rowIndex;
                        }
                        ImGui::PopID();

                        ImGui::Separator();

                        ImGui::TextUnformatted("Battle action");
                        ImGui::Separator();

                        ImGui::PushID("BattleSection");
                        for (BattleAction action : ControllerOverrideManager::GetBattleActions())
                        {
                                ImGui::PushID(static_cast<int>(action));
                                const bool confirmForRow = navConfirm && (rowIndex == navState.selectedIndex);
                                const bool clearForRow = navClear && (rowIndex == navState.selectedIndex);
                                auto drawBattleRow = [&](BattleAction battleAction, int thisRowIndex, bool confirmForThisRow, bool clearForThisRow)
                                        {
                                                const bool capturingThisRow = captureState.capturing && !captureState.isMenu && (captureState.battleAction == battleAction);
                                                const auto findIt = mapping.battleBindings.find(battleAction);
                                                const bool hasBinding = findIt != mapping.battleBindings.end();
                                                const bool selectedForNav = navState.initialized && (thisRowIndex == navState.selectedIndex);
                                                std::string buttonLabel;
                                                if (!hasBinding)
                                                {
                                                        buttonLabel = "Unbound";
                                                }
                                                else
                                                {
                                                        buttonLabel = describeBindings(findIt->second);
                                                }
                                                if (capturingThisRow)
                                                {
                                                        buttonLabel = "Press a key... (ESC=Cancel, ENTER=Clear)";
                                                }
                                                else if (selectedForNav)
                                                {
                                                        buttonLabel += " *";
                                                }

                                                if (selectedForNav)
                                                {
                                                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.25f, 0.45f, 1.0f));
                                                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.35f, 0.55f, 1.0f));
                                                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.13f, 0.20f, 0.38f, 1.0f));
                                                }

                                                if (ImGui::Button(ControllerOverrideManager::GetBattleActionLabel(battleAction), ImVec2(150, 0)))
                                                {
                                                        captureState.capturing = false;
                                                        captureState.baselineValid = false;

                                                        captureState.capturing = true;
                                                        captureState.isMenu = false;
                                                        captureState.battleAction = battleAction;
                                                }
                                                ImGui::SameLine();
                                                if (confirmForThisRow || ImGui::Button(buttonLabel.c_str(), ImVec2(300, 0)))
                                                {
                                                        if (capturingThisRow)
                                                        {
                                                                captureState.capturing = false;
                                                                captureState.baselineValid = false;
                                                        }
                                                        else
                                                        {
                                                                captureState.capturing = true;
                                                                captureState.isMenu = false;
                                                                captureState.battleAction = battleAction;
                                                                captureState.baselineValid = false;
                                                        }
                                                }
                                                if (selectedForNav)
                                                {
                                                        ImGui::PopStyleColor(3);
                                                }

                                                ImGui::SameLine();
                                                if (clearForThisRow || ImGui::Button("Clear##battle"))
                                                {
                                                        commitBattleBinding(battleAction, {});
                                                        captureState.capturing = false;
                                                        captureState.baselineValid = false;
                                                }
                                        };
                                drawBattleRow(action, rowIndex, confirmForRow, clearForRow);
                                ImGui::PopID();
                                ++rowIndex;
                        }
                        ImGui::PopID();

                        ImGui::Separator();

                        // Close button
                        if (ImGui::Button("Close"))
                        {
                                mappingPopupOpen = false;
                                captureState.capturing = false;
                                captureState.baselineValid = false;
                                ImGui::CloseCurrentPopup();
                        }

                        // Put "Set all to default" to the right of "Close"
                        ImGui::SameLine();
                        if (ImGui::Button("Set all to default"))
                        {
                                // Reset this keyboard to full BBCF defaults
                                KeyboardMapping defaultMapping = KeyboardMapping::CreateDefault();
                                mapping = defaultMapping; // update our local copy
                                controllerManager.SetKeyboardMapping(mappingTarget, mapping);

                                // Kill any ongoing capture
                                captureState.capturing = false;
                                captureState.baselineValid = false;
                        }

                        // NEW: handle navClose *before* ending the popup
                        if (!captureState.capturing && !suppressNavThisFrame && navClose)
                        {
                                mappingPopupOpen = false;
                                captureState.capturing = false;
                                captureState.baselineValid = false;
                                ImGui::CloseCurrentPopup();
                        }

                        // Only ONE EndPopup here
                        ImGui::EndPopup();

                        if (!mappingPopupOpen)
                        {
                                controllerManager.SetMappingPopupActive(false);
                        }
                }

                if (ignoredListOpen)
                {
                        ImGui::SetNextWindowSize(ImVec2(520.0f, 240.0f), ImGuiCond_FirstUseEver);
                        if (ImGui::Begin("Ignored keyboards", &ignoredListOpen))
                        {
                                auto ignoredDevices = controllerManager.GetIgnoredKeyboardSnapshot();
                                if (ignoredDevices.empty())
                                {
                                        ImGui::TextDisabled("No ignored keyboards.");
                                }
                                else
                                {
                                        for (const auto& dev : ignoredDevices)
                                        {
                                                if (ImGui::Button(std::string("Unignore##" + dev.canonicalId).c_str()))
                                                {
                                                        controllerManager.UnignoreKeyboard(dev.canonicalId);
                                                }
                                                ImGui::SameLine();
                                                ImGui::TextDisabled(dev.connected ? "(connected)" : "(not connected)");
                                                ImGui::SameLine();
                                                ImGui::Text("%s", dev.displayName.c_str());
                                        }
                                }
                        }
                        ImGui::End();
                }

                ImGui::VerticalSpacing(3);
        }
}
