#include "KeyboardSeparationDrawer.h"

#include "Core/ControllerOverrideManager.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"

namespace ControllerSettings
{
        void DrawKeyboardSeparation(ControllerOverrideManager& controllerManager)
        {
                ImGui::HorizontalSpacing();
                bool controllerPositionSwapped = controllerManager.IsKeyboardControllerSeparated();
                if (ImGui::Checkbox(Messages.Swap_P1_Controller_to_P2(), &controllerPositionSwapped))
                {
                        controllerManager.SetControllerPosSwap(controllerPositionSwapped);
                        Settings::settingsIni.swapControllerPos = controllerPositionSwapped;
                }
                ImGui::SameLine();
                ImGui::ShowHelpMarker(Messages.Changes_P1_Controller_s_assignment_so_it_is_recognized_as_P2_Internally_this_is_just_swapping_p1_with_p2_s_game_controllers());
        }
}
