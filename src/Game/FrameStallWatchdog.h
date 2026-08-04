#pragma once

// Companion to FrameStallDiagnostics: FrameStallDiagnostics measures how long
// a frame took and how much of that was inside the mod's two explicitly-timed
// calls (MatchState::OnUpdate + WindowManager::Render), but it is blind to
// time spent in any of BBCF-IM's *other* hooks (JMP patches in hooks_bbcf.cpp,
// Detours on Steam/D3D9 calls, controller/palette/network hooks) that run at
// other points during the frame -- that time would silently be folded into
// "the rest of the frame" and misread as the base game's fault.
//
// This watchdog closes that gap. While a stall is actually in progress (not
// after the fact -- by then whatever was blocking has already finished), a
// background thread repeatedly and very briefly suspends the render thread,
// reads its instruction pointer, and resumes it immediately, then resolves
// which *module* owned that address: BBCF-IM's own dinput8.dll, the game's
// BBCF.exe, or a system/GPU-driver DLL (ntdll.dll = blocked on a kernel
// object, nvwgf2umx.dll/atidxx64.dll = GPU driver, an AV vendor's DLL, etc).
//
// The suspend -> read instruction pointer -> resume step never allocates
// memory, takes a lock, or calls into the loader, so it cannot deadlock
// against whatever the render thread happens to be doing when sampled; all
// string formatting and module-name lookup happens afterward, with the
// target thread already running again.
//
// Added 2026-08-02 for the FPS-drop investigation (see FrameStallDiagnostics).
// Gated behind the same LogFrameStalls setting.
namespace FrameStallWatchdog
{
	// Starts the background sampling thread if LogFrameStalls is enabled.
	// Idempotent -- safe to call more than once.
	void Start();

	// Signals the background thread to stop and waits (bounded) for it to exit.
	void Stop();

	// Call once per rendered frame, as early as possible in EndScene, before
	// any other mod work. Identifies the calling thread as the one to sample
	// (first call only) and marks the frame as having advanced.
	void Heartbeat();
}
