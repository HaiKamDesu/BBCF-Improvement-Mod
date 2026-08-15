#pragma once

// Opt-in (LogFrameStalls=1 in settings.ini) frame-time instrumentation for
// tracking down the "FPS drops mid-match" reports (2026-08-02, atv/chickzama
// in the Discord; 2026-08-14, Ramiro). Measures real frame-to-frame time (what
// a player's FPS counter reflects) and splits it into named sections, so a
// captured stall can be attributed to a specific piece of the mod or ruled out
// in favor of the base game/driver/OS.
//
// Report 2 (Ramiro, 2026-08-14) showed why the original two-section split was
// not enough: every captured stall reported "mod hook code: 0.2ms" with the
// remaining ~119ms lumped into an unattributed "rest of the frame". That
// bucket contained the game's own Present (vsync/driver wait), our Steam
// callback pump hook, and every hooks_bbcf JMP patch, with no way to tell them
// apart. The sections below break that bucket up. See
// src/D3D9EXWrapper/ID3D9EXWrapper_Device.cpp EndScene()/Present() and
// hooks_detours.cpp hook_SteamAPI_RunCallbacks() for the call sites.
namespace FrameStallDiagnostics
{
	enum Section
	{
		// MatchState::OnUpdate - palettes, replay rewind, network stall watch.
		Section_MatchState = 0,
		// ScrWindow::TickTrainingResetSwap and anything else in EndScene that
		// is ours but sits outside the two big calls.
		Section_EndSceneMisc,
		// WindowManager::Render - the whole ImGui overlay.
		Section_OverlayRender,
		// hook_SteamAPI_RunCallbacks: our detour body, including the ranked
		// list filter pump. May run more than once per frame; time accumulates.
		Section_SteamPump,
		// Steam's own SteamAPI_RunCallbacks dispatch, called from that same
		// detour. Not our cost, but it can block, so it is counted apart from
		// both the mod total and the unattributed remainder.
		Section_SteamNative,
		// ISteamApps::GetLaunchQueryParam, issued by the mod from
		// check_and_load_replay_steam. Broken out of Section_MatchState because
		// it is the mod's only per-frame Steam IPC call and therefore the prime
		// suspect for mod-generated contention on Steam's IPC pipe.
		Section_SteamAppsPoll,
		// The game's own Present/PresentEx, measured around the real device
		// call. This is normally where vsync waiting lives - separating it out
		// is what tells a driver/compositor stall apart from a logic stall.
		Section_Present,
		Section_Count
	};

	// True when LogFrameStalls is on. Every entry point below is a no-op
	// otherwise; callers may use this to skip building arguments.
	bool IsEnabled();

	// Overlay self-A/B (OverlayAbTestSeconds, 0 = off). Returns true when the
	// overlay's whole ImGui pass should be skipped for this frame. Always
	// false when the test is off, so normal builds are unaffected.
	bool OverlayAbSkipActive();

	void OnFrameBegin();
	void OnFrameEnd();

	// Adds elapsed QPC ticks to a section's per-frame accumulator.
	void AddSectionTicks(Section section, long long ticks);

	// Times one named piece of the mod's own work and writes a line naming it if
	// it takes longer than warnMs.
	//
	// This exists because the frame-section accounting above can only say "the
	// overlay was slow" - it cannot say which call inside it was slow, and a
	// bug in the frame/section pairing once made it say nothing at all. Wrapping
	// the mod's individually expensive operations means a slow one identifies
	// itself by name no matter what the frame maths is doing.
	//
	// Deliberately NOT gated on LogFrameStalls: an operation blowing past its
	// budget on a user's machine is worth a line in DEBUG.txt whether or not
	// anyone thought to enable frame diagnostics first. Writes are globally rate
	// limited, so a permanently slow operation costs one line every few seconds
	// rather than one per frame.
	class ScopedOperation
	{
	public:
		explicit ScopedOperation(const char* name, double warnMs = 5.0);
		~ScopedOperation();

		ScopedOperation(const ScopedOperation&) = delete;
		ScopedOperation& operator=(const ScopedOperation&) = delete;

	private:
		const char* m_name;
		double m_warnMs;
		long long m_startTicks;
		bool m_active;
	};

	// RAII timer for a section. Cheap enough to leave in hot paths: when
	// logging is off it does not even read the clock.
	class ScopedSection
	{
	public:
		explicit ScopedSection(Section section);
		~ScopedSection();

		ScopedSection(const ScopedSection&) = delete;
		ScopedSection& operator=(const ScopedSection&) = delete;

	private:
		Section m_section;
		long long m_startTicks;
		bool m_active;
	};
}
