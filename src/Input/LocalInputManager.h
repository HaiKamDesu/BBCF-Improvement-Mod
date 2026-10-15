#pragma once

// The local input system: every keyboard and pad on the machine, each assigned to Player 1 or
// Player 2, merged into the two words the game reads. Replaces the game's own device handling
// (fixed at boot, two pads, keyboard welded to P1) and the mod's previous override layer.
//
//   devices    come from a platform backend (InputBackend.h): keyboards one by one through
//              Raw Input, XInput pads by the index they are really on, DirectInput pads and
//              sticks with hot-plug on a worker thread.
//   assignment keyboards start on P1, the first pad joins P1, the second P2, later pads the
//              side with fewer. Dragging a device in the UI pins it to that side, remembered
//              across sessions. A device that unplugs and comes back keeps its side.
//   mapping    per device: each control -> a set of game buttons (A, B, C, D, directions...).
//              Saved per device in BBCF_IM\controllers.ini, so it follows the device, not a slot.
//   the game   FUN_004963F0 (the per-controller action-word builder) is replaced for the six
//              controllers that matter: pad slot 0 gets the P1 word, pad slot 1 the P2 word,
//              the keyboard controllers get nothing. Everything above that - menus, battle,
//              netplay, replays, the training dummy - reads the game's own structures as usual.
//
// See docs/Decompilation/Controllers.md for the game side.

#include "DeviceModels.h"
#include "LocalInputTypes.h"

#include <Windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace LocalInput
{
	enum Side : int
	{
		Side_P1 = 0,
		Side_P2 = 1,
	};

	using Mapping = std::map<int, uint32_t>; // control code -> game buttons

	struct DeviceView
	{
		std::string id;
		std::string name;         // custom name, or the default name
		std::string defaultName;  // the product's name when the device is known, else the OS name
		DeviceKind kind = DeviceKind::Keyboard;
		DeviceModel model;        // what it physically is (names, icons, drawings only)
		bool connected = false;
		Side side = Side_P1;
		bool pinned = false;
		bool idle = false;        // a keyboard that has never been typed on (hidden by default)
		bool ignored = false;     // hidden by the player: listed apart, sends nothing to the game
		ULONGLONG lastInputMs = 0;
		ControlSet controls;
		uint32_t buttons = 0;     // what its controls map to right now
		int16_t axes[kDInputAxisCount] = {};
		int axisCount = 0;
	};

	class Manager
	{
	public:
		static Manager& Get();

		// Installs the game hook and starts the backend. No-op when the setting is off or the
		// controller hooks are not allowed on this platform.
		void Start();
		void Shutdown();
		bool IsActive() const { return m_active; }

		// Polls devices. The game hook calls this at the start of each frame's controller
		// update (lowest latency); the overlay calls it too so the UI stays live when the
		// game is not ticking controllers. Cheap when called again within the same frame.
		void Tick(bool fromGameHook);

		void OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

		// The merged game-button word for a side, directions SOCD-cleaned: the menu set or the
		// battle set of each device's Button Settings.
		uint32_t PlayerWord(Side side, bool battle) const;
		bool PlayerHasDevice(Side side) const;

		// ---- UI ----
		std::vector<DeviceView> Devices() const;
		void Assign(const std::string& id, Side side);   // pins the device to that side
		void ResetAssignments();                          // forget every pin, re-run the defaults
		void Rename(const std::string& id, const std::string& name);
		// Hidden devices are listed apart and send nothing to the game ("vJoy Device" and
		// other virtual devices nobody plays on). Showing an unused keyboard counts it as used.
		void SetIgnored(const std::string& id, bool ignored);

		// Hot-plug detection (setting AutomaticallyUpdateControllers) and a manual refresh.
		void SetAutoDetect(bool enabled);
		bool AutoDetect() const;
		void Refresh();
		bool SteamInputLikely() const;
		// What each control does: the mod's own mapping when the device has one, otherwise
		// what the game's Button Settings (remembered for this device) make it do in battle.
		Mapping GetMapping(const std::string& id) const;
		bool HasCustomMapping(const std::string& id) const;
		bool HasOwnGameBinds(const std::string& id) const;
		void SetMapping(const std::string& id, const Mapping& mapping);
		void ResetMapping(const std::string& id);
		static Mapping DefaultMapping(DeviceKind kind);
		static std::string ControlName(DeviceKind kind, int control);
		// The name printed on the device when it is a known model ("Cross", "Options").
		static std::string ControlName(const DeviceView& device, int control);

		// Rebinding: while a capture is armed nothing reaches the game, and the first control
		// newly pressed on that device is returned once.
		void BeginCapture(const std::string& id);
		void EndCapture();
		bool PollCapture(int* control);

		// Keyboards also stop reaching the game while the overlay is typing into a text box.
		void SetOverlayWantsKeyboard(bool wants) { m_overlayWantsKeyboard = wants; }

		// Writes everything the input system knows right now into DEBUG.txt, with the recent
		// input history, for bug reports. Returns the report's number, 0 when logging is off.
		int LogReport(const char* why);

		struct Device; // internal device record (LocalInputManager.cpp)

	private:
		Manager() = default;
		void LoadProfiles();
		void SaveProfiles() const;
		void ApplySnapshots();
		Side ChooseSide(const Device& device, const char** why = nullptr) const;
		void LogRoster(const char* reason) const;
		void RecomputeWords();
		void SyncGameKeyConfig();

		bool m_active = false;
		ULONGLONG m_lastTickMs = 0;
		bool m_overlayWantsKeyboard = false;
	};

	// Game bridge (GameInputBridge.cpp): replaces FUN_004963F0 for the pad-slot and keyboard
	// controllers. False when the game code is not the RE'd build.
	bool InstallGameInputBridge();
	bool ResolveControllerWord(void* controller, uint32_t* word);
	// The game's own text box (GAMESTEAM_VirtualKeyboard: chat, names) is open.
	bool GameTextEntryOpen();
}
