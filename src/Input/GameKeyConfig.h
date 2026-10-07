#pragma once

// The game's own Button Settings, remembered per physical device.
//
// Players set their buttons in the game's key-config screens (main menu Options, character
// select, the pause and training menus). The game keeps one record per controller slot and a
// single keyboard record (docs/Decompilation/Controllers.md section 6); this module keeps those
// records per device instead:
//
//   - optData (the bbsave.dat image) is a staging window. When an editor opens for a player,
//     the records of the device that player is editing with are copied in, and every edit the
//     screen makes is found by diffing optData against what was staged, once per frame, and
//     stored back into that device.
//   - The bridge builds each device's word from its own records with the game's semantics
//     (class defaults + record of the profile the game selected), so a keyboard on Player 2
//     gets its own keyboard config, two keyboards get two configs, and so on.
//   - The rebind screens are pointed at the device being edited (pad capture and keyboard
//     capture reads), and the keyboard panel opens for Player 2 too.
//
// A device that was never configured uses the records the game had when the save loaded
// ("inherited"), so existing configs carry over.

#include "LocalInputTypes.h"

#include <Windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace LocalInput
{
	// One device's copy of the four records the game would hold for it, in the record format
	// of its class (pad or keyboard), plus its two per-player options.
	struct GameBinds
	{
		uint8_t setB[2][20] = {};   // battle, profile 0 (Type A) / 1 (Type B)
		uint8_t setA[2][20] = {};   // menus, Current Set 0 / 1
		uint8_t typeB = 0;          // "Type B" enabled (char select may pick it)
		uint8_t menuSet = 0;        // menu Current Set
	};

	namespace GameKeyConfig
	{
		enum class BindClass { Pad, Keyboard };
		inline BindClass ClassOf(DeviceKind kind) { return kind == DeviceKind::Keyboard ? BindClass::Keyboard : BindClass::Pad; }

		// Patches the capture reads, the keyboard-panel switch and the save-loaded signal.
		// Each patch is checked against the RE'd bytes and skipped alone when it does not match.
		void Install();

		// The game's save data (and with it the key config) has been loaded.
		bool SaveLoaded();

		GameBinds Defaults(BindClass cls);
		// What a never-configured device uses: the records the game had at load.
		GameBinds Inherited(BindClass cls, int side);
		// Replaces values vanilla could never produce (they are written into bbsave.dat).
		bool Sanitize(GameBinds& binds, BindClass cls);

		// Held game keys of a device, in its class's game-key space (128 bits).
		struct HeldKeys { uint32_t bits[4] = {}; bool Test(int gk) const { return gk >= 0 && gk < 128 && (bits[gk >> 5] >> (gk & 31)) & 1; } };
		HeldKeys HeldGameKeys(DeviceKind kind, const ControlSet& controls);

		// The device's word as the game would build it for that set.
		uint32_t Word(BindClass cls, const HeldKeys& held, const GameBinds& binds, bool battle, int side);

		// control -> game buttons, for labels (the battle set in use).
		std::map<int, uint32_t> EffectiveMapping(DeviceKind kind, const GameBinds& binds, int side);

		// ---- per frame (game thread) ----
		struct DeviceRef
		{
			std::string id;
			DeviceKind kind = DeviceKind::Keyboard;
			int side = 0;
			bool usable = false;        // connected, not hidden
			bool idle = false;          // a keyboard nobody has typed on
			ULONGLONG lastInputMs = 0;
			ControlSet controls;
			GameBinds binds;            // effective (own or inherited)
		};
		// Absorbs edits made by the game's screens and stages records for editors that just
		// opened. Returns the devices whose binds changed (to be stored as their own).
		void Frame(const std::vector<DeviceRef>& devices, std::vector<std::pair<std::string, GameBinds>>& changed);

		// A game key-config editor is open for that side (either family).
		bool EditorOpen(int side);
	}
}
