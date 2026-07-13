#pragma once

// Diagnostic instrumentation for the D-Code display freeze / ranked progress
// rollback bug. See docs/Research/DCodeNetworkStallBug.md for the RE writeup
// this is built from.
//
// Two layers:
//  - OnUpdate(): 200ms poll of the room-member fetch states, the auto-save
//    trigger and the save manager (verbose parts gated behind
//    Settings::settingsIni.enableInDevelopmentFeatures).
//  - OnFetchTickEnter(): called from the DCodeFetchTick hook at the entry of
//    the game's per-slot async-fetch tick (FUN_004A25C0). Snapshots the
//    0x6800 profile blob while a fetch is in flight so that when the game
//    rejects a response (state 6, which wipes the blob and wedges the slot
//    for the rest of the process lifetime), the offending payload has already
//    been captured and can be dumped. Also hosts the optional auto-recovery
//    watchdog (Settings::settingsIni.dcodeAutoRecover).
namespace NetworkStallDiagnostics
{
	void OnUpdate();
	void OnFetchTickEnter(void* row);
}

// Plain-C thunk for the naked asm hook in hooks_bbcf.cpp.
extern "C" void __cdecl DCodeFetchTickEnterThunk(void* row);
