#include "ControllerSettingsSection.h"

#include "Core/ControllerOverrideManager.h"
#include "Core/Settings.h"
#include "Overlay/Window/ControllerSettings/ControllerRefreshDrawer.h"
#include "Overlay/Window/ControllerSettings/KeyboardSeparationDrawer.h"
#include "Overlay/Window/ControllerSettings/LocalControllerOverrideDrawer.h"
#include "Overlay/Window/ControllerSettings/MultipleKeyboardOverrideDrawer.h"
#include "Overlay/Window/ControllerSettings/SteamInputWarningDrawer.h"

namespace ControllerSettings
{
        void DrawSection()
        {
                auto& controllerManager = ControllerOverrideManager::GetInstance();
                controllerManager.TickAutoRefresh();
                const bool inDevelopmentFeaturesEnabled = Settings::settingsIni.enableInDevelopmentFeatures;
                const bool steamInputLikely = inDevelopmentFeaturesEnabled ? controllerManager.IsSteamInputLikelyActive() : false;

                DrawSteamInputWarning(steamInputLikely, inDevelopmentFeaturesEnabled);
                DrawKeyboardSeparation(controllerManager);
                DrawMultipleKeyboardOverride(controllerManager);
                DrawLocalControllerOverride(controllerManager, inDevelopmentFeaturesEnabled, steamInputLikely);
                DrawControllerRefresh(controllerManager, inDevelopmentFeaturesEnabled, steamInputLikely);
        }
}
