#pragma once

// Opt-in (LogFrameStalls=1 in settings.ini) frame-time instrumentation for
// tracking down the "FPS drops mid-match" reports (2026-08-02, atv/chickzama
// in the Discord). Measures real frame-to-frame time (what a player's FPS
// counter reflects) alongside the time spent inside the mod's own EndScene
// hook code, so a captured stall can be attributed to the mod or ruled out
// in favor of the base game/driver/OS. See src/D3D9EXWrapper/ID3D9EXWrapper_Device.cpp
// EndScene() for the call sites.
namespace FrameStallDiagnostics
{
	void OnFrameBegin();
	void OnAfterMatchStateUpdate();
	void OnFrameEnd();
}
