#pragma once

bool placeHooks_detours();

// The two D3D9 factory exports on their own, installed as early as the init thread
// can manage and idempotent. Must run before anything slow, or the game creates its
// device before the hook exists and the overlay never attaches. See the definition
// in hooks_detours.cpp for the measurement.
bool placeD3D9FactoryHooks_detours();

// The D3D9 import-table fallback, decided from D3D9IatFallbackHook. Separate from the
// detours because those go in before settings.ini is read. Call once settings are loaded;
// idempotent.
bool applyD3D9IatFallbackPolicy_detours();

// SteamAPI_Init on its own, installed as early as the D3D factory hooks and
// idempotent. Missing this one call leaves every Steam interface pointer null for
// the life of the process, which stops WindowManager::Render before it draws
// anything. See the definition in hooks_detours.cpp for the measurement.
bool placeSteamInitHook_detours();
