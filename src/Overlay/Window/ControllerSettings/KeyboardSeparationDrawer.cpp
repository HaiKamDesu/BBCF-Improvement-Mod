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
                if (ImGui::Checkbox("Swap P1 Controller to P2", &controllerPositionSwapped))
                {
                        controllerManager.SetControllerPosSwap(controllerPositionSwapped);
                        Settings::settingsIni.swapControllerPos = controllerPositionSwapped;
                }
                ImGui::SameLine();
                ImGui::ShowHelpMarker("Changes P1 Controller's assignment so it is recognized as P2. Internally, this is just swapping p1 with p2's game controllers.");
        }
}
