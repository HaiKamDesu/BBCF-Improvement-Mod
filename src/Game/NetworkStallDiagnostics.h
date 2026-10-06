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
#include <Windows.h>
#include <cstdint>

// The last moment the profile server CONFIRMED a write of the player's profile
// (work-manager result 8). Everything the player earns after this is only in
// RAM and is discarded on restart if no later write succeeds -- which is the
// whole cost of the D-Code wedge. Serial 0 means nothing has been confirmed
// this session yet.
struct ProfilePersistState
{
	uint64_t serial = 0;
	bool haveTime = false;
	SYSTEMTIME whenLocal = {};
	ULONGLONG whenTickMs = 0;
};

namespace NetworkStallDiagnostics
{
	void OnUpdate();
	void OnFetchTickEnter(void* row);
	void GetProfilePersistState(ProfilePersistState* out);

	// Asks for a fresh user/login. Safe from any thread: the login itself only ever
	// starts on the thread that ticks the web strategies, on its next tick. Subject to
	// the same cooldown and per-window cap as the automatic re-login.
	void RequestRelogin(const char* reason);

	// Appends a line to DCodeIncidents.log (and the debug log), for subsystems that
	// share this one's incident trail.
	void IncidentLog(const char* fmt, ...);
}

// Plain-C thunk for the naked asm hook in hooks_bbcf.cpp.
extern "C" void __cdecl DCodeFetchTickEnterThunk(void* row);
