#include "ControllerSettingsSection.h"

#include "Core/ControllerOverrideManager.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Overlay/Window/ControllerSettings/ControllerRefreshDrawer.h"
#include "Overlay/Window/ControllerSettings/KeyboardSeparationDrawer.h"
#include "Overlay/Window/ControllerSettings/LocalControllerOverrideDrawer.h"
#include "Overlay/Window/ControllerSettings/MultipleKeyboardOverrideDrawer.h"
#include "Overlay/Window/ControllerSettings/SteamInputWarningDrawer.h"

#include "imgui.h"

namespace ControllerSettings
{
        void DrawSection()
        {
                auto& controllerManager = ControllerOverrideManager::GetInstance();
                controllerManager.TickAutoRefresh();
                const bool inDevelopmentFeaturesEnabled = Settings::settingsIni.enableInDevelopmentFeatures;
                const bool steamInputLikely = inDevelopmentFeaturesEnabled ? controllerManager.IsSteamInputLikelyActive() : false;

                if (!Settings::settingsIni.EnableWineBreakingFeatures)
                {
                        ImGui::HorizontalSpacing();
                        ImGui::TextWrapped(Messages.Controller_overrides_are_disabled_because_Wine_Proton_was_detected_Edit_EnableWineBreakingFeatures_in_settings_ini_to_enable_them_at_your_own_risk());
                        return;
                }

                DrawSteamInputWarning(steamInputLikely, inDevelopmentFeaturesEnabled);
                DrawKeyboardSeparation(controllerManager);
                DrawMultipleKeyboardOverride(controllerManager);
                DrawLocalControllerOverride(controllerManager, inDevelopmentFeaturesEnabled, steamInputLikely);
                DrawControllerRefresh(controllerManager, inDevelopmentFeaturesEnabled, steamInputLikely);
        }
}
