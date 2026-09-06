#pragma once

bool placeHooks_detours();

// The two D3D9 factory exports on their own, installed as early as the init thread
// can manage and idempotent. Must run before anything slow, or the game creates its
// device before the hook exists and the overlay never attaches. See the definition
// in hooks_detours.cpp for the measurement.
bool placeD3D9FactoryHooks_detours();
