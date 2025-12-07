#include "KeyboardSeparationDrawer.h"

#include "Core/ControllerOverrideManager.h"
#include "Core/Settings.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"

namespace ControllerSettings
{
        void DrawKeyboardSeparation(ControllerOverrideManager& controllerManager)
        {
                ImGui::HorizontalSpacing();
                bool controllerPositionSwapped = controllerManager.IsKeyboardControllerSeparated();
                if (ImGui::Checkbox("Separate Keyboard and Controllers", &controllerPositionSwapped))
                {
                        controllerManager.SetKeyboardControllerSeparated(controllerPositionSwapped);
                        Settings::settingsIni.separateKeyboardAndControllers = controllerPositionSwapped;
                }
                ImGui::SameLine();
                ImGui::ShowHelpMarker("Separates keyboard input from controller slots so they can map to different players.");
        }
}
