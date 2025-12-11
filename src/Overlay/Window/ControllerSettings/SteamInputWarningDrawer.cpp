#include "SteamInputWarningDrawer.h"

#include "Core/ControllerOverrideManager.h"
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
                        "Steam Input appears to be active.\n"
                        "This will disable some of this section's features.");
                ImGui::SameLine();
                ImGui::ShowHelpMarker(
                        "The internal behavior of Steam Input hides some controllers from the game's process, thus making some controller related features impossible/work in unintended ways.\n"
                        "\n"
                        "The disabled features include:\n"
                        "- Local Controller Override\n"
                        "- Opening Joy.cpl"
                );
                ImGui::VerticalSpacing(5);
        }
}
