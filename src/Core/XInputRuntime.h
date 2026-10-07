#pragma once

#include <Windows.h>
#include <Xinput.h>

namespace XInputRuntime
{
        DWORD GetState(DWORD userIndex, XINPUT_STATE* state);
        // Subtype (arcade stick, wheel...) needs xinput1_3 or later; 9_1_0 reports every
        // device as a gamepad.
        DWORD GetCapabilities(DWORD userIndex, XINPUT_CAPABILITIES* capabilities);
}

