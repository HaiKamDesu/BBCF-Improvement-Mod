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

		if (multiKeyboardOverride)
		{
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
			const auto& keyboards = controllerManager.GetKeyboardDevices();
			std::vector<const KeyboardDeviceInfo*> selectedInfos;
			selectedInfos.reserve(keyboards.size());

			for (const auto& device : keyboards)
			{
				if (controllerManager.IsP1KeyboardHandle(device.deviceHandle))
				{
					selectedInfos.push_back(&device);
				}
			}

			std::string preview;
			if (selectedInfos.empty())
			{
				preview = "No keyboards selected";
			}
			else
			{
				preview = selectedInfos.front()->displayName;
				for (size_t i = 1; i < selectedInfos.size(); ++i)
				{
					preview += ", " + selectedInfos[i]->displayName;
				}
			}

			if (keyboards.empty())
			{
				ImGui::TextDisabled("No keyboards detected.");
			}
			else
			{
				if (ImGui::BeginCombo("P1 Keyboards", preview.c_str()))
				{
					for (const auto& device : keyboards)
					{
						bool selected = controllerManager.IsP1KeyboardHandle(device.deviceHandle);
						std::string rowId = !device.canonicalId.empty() ? device.canonicalId : (!device.deviceId.empty() ? device.deviceId : std::to_string(reinterpret_cast<uintptr_t>(device.deviceHandle)));
						ImGui::PushID(rowId.c_str());
						if (ImGui::Checkbox("##p1-keyboard", &selected))
						{
							controllerManager.SetP1KeyboardHandleEnabled(device.deviceHandle, selected);
						}
						ImGui::SameLine();
						ImGui::TextUnformatted(device.displayName.c_str());

						ImGui::SameLine();
						if (ImGui::SmallButton("Map"))
						{
							mappingTarget = device;
							mappingPopupOpen = true;
							captureState.capturing = false;
							requestMappingPopup = true;
						}

						ImGui::SameLine();
						if (ImGui::SmallButton("Rename"))
						{
							renameTarget = device;
							std::string currentLabel = controllerManager.GetKeyboardLabelForId(renameTarget.canonicalId);
							strncpy(renameBuffer, currentLabel.c_str(), sizeof(renameBuffer));
							renameBuffer[sizeof(renameBuffer) - 1] = '\0';
							renamePopupOpen = true;
							requestRenamePopup = true;
						}

						ImGui::SameLine();
						if (ImGui::SmallButton("Ignore"))
						{
							controllerManager.IgnoreKeyboard(device);
						}
						ImGui::PopID();
					}

					ImGui::EndCombo();
				}
				if (requestRenamePopup)
				{
					ImGui::OpenPopup("Rename keyboard");
				}

				if (requestMappingPopup)
				{
					ImGui::OpenPopup("Configure keyboard mapping");
				}

				ImGui::VerticalSpacing(1);
				ImGui::HorizontalSpacing();
				if (ImGui::Button("Ignored keyboards"))
				{
					ignoredListOpen = true;
				}

				if (ImGui::BeginPopupModal("Rename keyboard", &renamePopupOpen, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text("Set a custom name for this keyboard.");
					ImGui::InputText("##RenameKeyboardInput", renameBuffer, sizeof(renameBuffer));

					ImGui::Separator();
					if (ImGui::Button("Save") && renameTarget.deviceHandle)
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

							// Confirm to enter bind mode on selected row
							navConfirm = actionNewPress(MenuAction::Confirm);

							// Return (C button) to close popup
							bool navCloseByReturn = actionNewPress(MenuAction::ReturnAction);

							// NEW: ESC closes popup when NOT binding
							bool escNow = isPressedNow(VK_ESCAPE);
							bool escWas = wasPressed(VK_ESCAPE);
							bool navCloseByEsc = escNow && !escWas;

							navClose = navCloseByReturn || navCloseByEsc;

							// NEW: Right hold-to-clear: only when not binding
							bool rightHeldThisFrame = actionHeld(MenuAction::Right);
							if (rightHeldThisFrame)
							{
								// accumulate hold time while Right is down
								navState.rightHoldSeconds += ImGui::GetIO().DeltaTime;
								navState.rightWasDown = true;

								if (navState.rightHoldSeconds >= 2.0f)
								{
									// Trigger clear on the currently selected row
									navClear = true;
									navState.rightHoldSeconds = 0.0f; // reset so you can do it again later

									// Optional: prevent this same frame from also doing Up/Down/Confirm nav
									suppressNavThisFrame = true;
								}
							}
							else
							{
								navState.rightHoldSeconds = 0.0f;
								navState.rightWasDown = false;
							}

							navState.lastKeyState = currentState;
						}
					}


					// Adjust these widths if action labels overlap the binding/action buttons.
					// Increase labelColumnWidth to give long action names more room.
					const float labelColumnWidth = 250.0f;
					const float bindingColumnWidth = 150.0f;

					// rowIndex is a running index across all rows (menu then battle)
					auto drawMenuRow = [&](MenuAction action, int rowIndex, bool confirmForRow, bool clearForRow)
						{
							const float rowStart = ImGui::GetCursorPosX();
							const bool isSelected = (rowIndex == navState.selectedIndex);

							if (isSelected)
							{
								ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.6f, 1.0f));
							}

							ImGui::TextUnformatted(ControllerOverrideManager::GetMenuActionLabel(action));
							ImGui::SameLine(rowStart + labelColumnWidth);
							ImGui::TextUnformatted(describeBindings(mapping.menuBindings[action]).c_str());
							ImGui::SameLine(rowStart + labelColumnWidth + bindingColumnWidth);

							const bool isCapturing = captureState.capturing && captureState.isMenu && captureState.menuAction == action;
							if (isCapturing)
							{
								ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Press a key...");
							}
							else
							{
								bool triggerBind = false;

								if (ImGui::SmallButton("Bind"))
									triggerBind = true;

								if (isSelected && confirmForRow && !captureState.capturing)
									triggerBind = true;

								if (triggerBind)
								{
									captureState.capturing = true;
									captureState.isMenu = true;
									captureState.menuAction = action;
									captureState.battleAction = BattleAction::Up;
									captureState.baselineState.fill(0);
									if (mappingTarget.deviceHandle)
									{
										controllerManager.GetKeyboardStateSnapshot(mappingTarget.deviceHandle, captureState.baselineState);
									}
								}
							}

							ImGui::SameLine();
							if (ImGui::SmallButton("Clear") || clearForRow)
							{
								commitMenuBinding(action, {});
							}

							if (isSelected)
							{
								ImGui::PopStyleColor();
							}
						};

					auto drawBattleRow = [&](BattleAction action, int rowIndex, bool confirmForRow, bool clearForRow)
						{
							const float rowStart = ImGui::GetCursorPosX();
							const bool isSelected = (rowIndex == navState.selectedIndex);

							if (isSelected)
							{
								ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.6f, 1.0f));
							}

							ImGui::TextUnformatted(ControllerOverrideManager::GetBattleActionLabel(action));
							ImGui::SameLine(rowStart + labelColumnWidth);
							ImGui::TextUnformatted(describeBindings(mapping.battleBindings[action]).c_str());
							ImGui::SameLine(rowStart + labelColumnWidth + bindingColumnWidth);

							const bool isCapturing = captureState.capturing && !captureState.isMenu && captureState.battleAction == action;
							if (isCapturing)
							{
								ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Press a key...");
							}
							else
							{
								bool triggerBind = false;

								if (ImGui::SmallButton("Bind"))
									triggerBind = true;

								if (isSelected && confirmForRow && !captureState.capturing)
									triggerBind = true;

								if (triggerBind)
								{
									captureState.capturing = true;
									captureState.isMenu = false;
									captureState.battleAction = action;
									captureState.menuAction = MenuAction::Up;
									captureState.baselineState.fill(0);
									if (mappingTarget.deviceHandle)
									{
										controllerManager.GetKeyboardStateSnapshot(mappingTarget.deviceHandle, captureState.baselineState);
									}
								}
							}

							ImGui::SameLine();
							if (ImGui::SmallButton("Clear") || clearForRow)
							{
								commitBattleBinding(action, {});
							}

							if (isSelected)
							{
								ImGui::PopStyleColor();
							}
						};


					ImGui::Text("Mapping for %s", mappingTarget.displayName.c_str());
					ImGui::Separator();

					int rowIndex = 0;

					ImGui::TextUnformatted("Menu action");
					ImGui::Separator();

					ImGui::PushID("MenuSection");
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
			}
		}
	}
}