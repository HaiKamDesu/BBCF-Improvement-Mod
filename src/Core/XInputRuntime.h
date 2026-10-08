#pragma once

#include <Windows.h>
#include <Xinput.h>

namespace XInputRuntime
{
        DWORD GetState(DWORD userIndex, XINPUT_STATE* state);
        // Subtype (arcade stick, wheel...) needs xinput1_3 or later; 9_1_0 reports every
        // device as a gamepad.
        DWORD GetCapabilities(DWORD userIndex, XINPUT_CAPABILITIES* capabilities);
        // Battery of the gamepad itself (wireless pads); xinput1_3 or later.
        DWORD GetBatteryInformation(DWORD userIndex, XINPUT_BATTERY_INFORMATION* battery);
}

