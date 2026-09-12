#pragma once
#include "IWindow.h"
#include <vector>
#include <chrono>
#include <string>
#include "Game/Scr/ScrStateReader.h"
#include "Game/Playbacks/PlaybackManager.h"
#include "Core/utils.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include "Game/SnapshotApparatus/SnapshotApparatus.h"
// Drawn every frame by WindowManager, not from a section body: a state loaded by hotkey
// with the mod menu closed still has to show its setup-time countdown, and the bodies only
// run while the menu is sitting on the page that owns them.
void DrawSaveStateSetupDelayStandalone();

class ScrWindow : public IWindow
{
public:
	ScrWindow(const std::string& windowTitle, bool windowClosable,
		WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable,windowFlags), m_pWindowContainer(&windowContainer) {}
	~ScrWindow() override = default;

	// Runs every game frame regardless of whether this window is open, so the
	// hold-Up-while-resetting-the-lab shortcut works with the mod menu closed.
	static void TickTrainingResetSwap();

	// Per-frame side effects that used to ride along with drawing this window. Now that the
	// controls live on mod-menu pages that are only drawn when selected, these have to be
	// pumped independently or the features would stop the moment you looked elsewhere.
	static void Tick();

	// Parses the dummy's script if it has not been read for the character now loaded, so a
	// UI that needs the move list can ask for it instead of depending on some other panel
	// having been drawn first. Safe to call every frame: it is a no-op once loaded.
	static void EnsureDummyScriptLoadedForUi();

	// Polled from WindowManager::HandleButtons, the mod's single hotkey poll site, so a
	// save-state bind works with the mod menu closed. It only latches a request: the work
	// happens in RunPendingSaveStateRequests.
	void TickSaveStateHotkeys();

	// The dummy's own per-frame work: the registered gap/wakeup/tech/on-hit actions, burst
	// on hit, and the Naoto EN toggle. All of this used to sit at the bottom of
	// DrawDummyActionsBody, so the dummy simply stopped doing anything the moment the mod
	// menu was closed or moved off the Training page.
	void TickDummyActions();

	// Whether anything the dummy does on its own is actually switched on. Used to decide
	// whether a character swap is worth re-parsing the script for outside the menu.
	bool DummyFeaturesInUse() const;

	// Copies the animation pools out of the dummy-action table into the registers the
	// firing conditions below read.
	void SyncAnimationRegistersFromActions();

	// Drops everything parsed for the previous dummy and, if allowed, re-parses for the
	// current one. The registers hold scrState* whose addr points into a specific
	// character's script, so handing one to the game after a swap is a jump into the wrong
	// script - they have to be dropped the moment the character changes, menu or no menu.
	// Returns whether the parsed state list is usable afterwards.
	bool EnsureDummyScriptFresh(bool allowReparse);

	// Runs the latched request, called after the overlay's windows have been drawn.
	//
	// Deliberately not done straight from HandleButtons. That runs before ImGui::NewFrame,
	// which is a frame phase the snapshot code had never been entered from - the buttons
	// that used to be its only trigger live in the draw pass. SnapshotApparatus's
	// constructor NOPs three sites in the game's code and calls into its network init, so
	// it is not something to invoke from a new phase on a hunch. This keeps the hotkey
	// reading a fresh press edge while doing the work where the buttons always did it.
	void RunPendingSaveStateRequests();

	// Remaining/total of the post-load "setup time" pause, for the standalone indicator.
	bool GetSetupDelayCountdown(float* remaining, float* total) const;

	// Section bodies drawn by the mod menu's Training, Replays and Online pages. Each of
	// these renders the CONTENT only - the page owns the header and the layout around it.
	void DrawPositionsBody();
	void DrawWakeupBody();
	void DrawSaveStatesBody();
	void DrawLocalReplaysBody();
	// idScope keeps this body's modal unique when more than one host draws it in the same
	// frame (the Replay Extras window and the mod menu's Replays page both do). compact
	// drops the asides that only earn their space with the mod menu's width behind them.
	void DrawReplayTakeoverBody(const char* idScope, bool compact);
	// True while a replay takeover is running, so the menu page can stay reachable after the
	// mode has flipped to training - which is exactly when the user needs its two buttons.
	bool IsReplayTakeoverActive() const { return takeover_active; }
	// Replay takeover. One button in the replay opens the setup dialog; accepting it does
	// the whole thing (snapshot, side swap, input capture, state load) in one go.
	void DrawTakeoverSetupModal(const char* popupId);
	void BeginReplayTakeover(bool asP1);
	// Same moment, different options. Puts the replay back - the snapshot IS the moment you
	// took over - and takes it over again, so switching side mid-takeover is free.
	void ReconfigureReplayTakeover(bool asP1);
	void EndReplayTakeover();
	void TickReplayTakeover();
	SnapshotApparatus* EnsureTakeoverSnapshot();
	// Capture a stretch of the replay you are watching as a playback file. Lives on the
	// Replays page rather than in the playback library, because it is a replay job: the
	// file it writes can then be imported into a slot, a library, or a dummy action.
	void DrawReplayPlaybackCaptureBody(const char* idScope, bool compact);
	void DrawRoomSettingsBody();
	void DrawInputBufferButton();
	void DrawComboDataButton();
	void DrawTasComboToolButton();
	// Import a playback file into one of the game's four recording slots, or write a slot
	// back out to a file. The pair that lets everything else here exchange playbacks: a
	// file exported from a replay capture or a library can be loaded straight into a slot.
	void DrawPlaybackTransferButtons();

protected:
	void Draw() override;

	static void swap_character_coordinates();
	void check_wakeup_delay();

private:
	static void TickLocalReplayRedirect();

	static bool s_swapCoordsToggle;
	static bool s_wakeupOverrideEnabled;
	static bool s_localReplayLoaded;
	static std::string s_localReplayLoadedName;
	void draw_playback_slot_section(int slot);
	void DrawReplayRewind();
	void DrawReplayRewind_old();
	void DrawWakeupDelayControl();
	PlaybackManager playback_manager;
	bool m_showDemoWindow = false;
	void* p2_old_char_data = NULL;
	std::vector<scrState*> gap_register{};
	std::vector<int> gap_register_delays{};
	std::vector<scrState*> wakeup_register{};
	std::vector<int> wakeup_register_delays{};
	std::vector<scrState*> onhit_register{};
	std::vector<int> onhit_register_delays{};
	std::vector<scrState*> throwtech_register{};
	std::vector<int> throwtech_register_delays{};
	// Uninitialised until a script parse finds them, and null for a character whose script
	// has no burst state, so both the burst logic and every user must null-check.
	scrState* burst_action = nullptr;
	scrState* air_burst_action = nullptr;



	int states_wakeup_random_pos = 0;
	int states_gap_random_pos = 0;
	int states_onhit_random_pos = 0;
	int states_throwtech_random_pos = 0;


	int frame_to_burst_onhit;
	int states_wakeup_frame_to_do_action = 0;
	int states_gap_frame_to_do_action = 0;
	int states_throwtech_frame_to_do_action = 0;


	char fpath_s1[1200] = "fpath";
	char fpath_s2[1200] = "fpath";
	char fpath_s3[1200] = "fpath";
	char fpath_s4[1200] = "fpath";
	int slot_gap = 0; //0 for disabled, 1 for slot 1, 2 for slot 2, etc
	int slot_wakeup = 0; //0 for disabled, 1 for slot 1, 2 for slot 2, etc
	int slot_onblock = 0; //0 for disabled, 1 for slot 1, 2 for slot 2, etc
	int slot_onhit = 0; //0 for disabled, 1 for slot 1, 2 for slot 2, etc
	int slot_throwtech = 0; //0 for disabled, 1 for slot 1, 2 for slot 2, etc
	
	bool random_gap_slot1 = false;
	bool random_gap_slot2 = false;
	bool random_gap_slot3 = false;
	bool random_gap_slot4 = false;

	bool random_wakeup_slot1 = false;
	bool random_wakeup_slot2 = false;
	bool random_wakeup_slot3 = false;
	bool random_wakeup_slot4 = false;
	std::vector<int> slot_buffer{ 0,0,0,0 }; //holds the amount of frames each slot should buffer their actions for before coming out of hitstun(not implemented) or being able to act after waking up
	std::vector<int> random_gap{}; //holds the slots to be random for gap
	std::vector<int> random_wakeup{}; //holds the slots to be random for wakeup

	std::string prev_action;


	// Whether the Naoto EN flag was held last frame, so it is cleared exactly once when the
	// action that wanted it goes away. Everything else about dummy actions now lives in
	// DummyActionManager; these were draw-body statics of a panel that no longer exists.
	bool dummy_naoto_en_specials_old = false;

	int32_t wakeup_type = 0;
	int wakeup_delay_skew = 0;
	bool wakeup_delay_skew_change_flag = false;
	int wakeup_delay = 0;



	// Save states, and the "setup time" pause that follows a load. Both used to live entirely
	// inside the draw bodies, which are only called while the mod menu sits on the page that
	// owns them: the hotkeys fired only then, and the countdown that clears isFrameFrozen
	// only advanced while it was on screen, so closing the menu mid-delay froze the game.
	// The hotkeys are now polled by TickSaveStateHotkeys and the countdown by TickSetupDelay.
	void SaveTrainingState();
	void LoadTrainingState();
	void LoadReplayTakeoverState();
	bool HasTrainingSnapshot() const;
	bool HasReplayTakeoverSnapshot() const;

	SnapshotApparatus* EnsureTrainingSnapshot();
	void BeginSetupDelay(float seconds);
	void TickSetupDelay();

	bool pending_save_state = false;
	bool pending_load_state = false;
	bool pending_load_replay_state = false;

	SnapshotApparatus* snap_apparatus = nullptr;
	SnapshotApparatus* snap_apparatus_takeover = nullptr;
	std::vector<char> replay_action_load{};
	int facing_left_replay_takeover = 0;
	bool takeover_active = false;
	bool takeover_as_p1 = true;
	// Which side the setup dialog currently has picked, remembered between opens.
	int takeover_modal_side = 0;
	// One reload per round end, not one per frame for as long as the KO animation lasts.
	bool takeover_round_reset_armed = false;
	// Side to re-take the moment with, or -1. Latched like the state loads: it reloads a
	// snapshot, which must not happen from the draw pass.
	int pending_takeover_reconfigure = -1;
	float wait_before_exec_s = 0;
	float wait_before_exec_s2 = 0;
	unsigned long long setup_delay_last_tick = 0;
	float setup_delay_total = 0;

	std::chrono::steady_clock::time_point start_time;
	float base_time = 0;
	bool is_setup_time_running = false;
	char* bbcf_base_adress = GetBbcfBaseAdress();
	WindowContainer* m_pWindowContainer = nullptr;
};