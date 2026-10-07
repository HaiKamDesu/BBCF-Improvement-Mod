#pragma once

// Shared vocabulary of the local input system (see LocalInputManager.h).
//
// A device is anything a person plays with: a keyboard, an XInput pad, a DirectInput pad or
// stick. Every physical control on it - a key, a button, a hat direction, half of an axis -
// has a small integer "control code" that is only meaningful together with the device's kind.
// Backends (Windows today, Linux later) produce DeviceSnapshots in this vocabulary; nothing
// above the backend knows which OS API a device came from.

#include <bitset>
#include <cstdint>
#include <string>

namespace LocalInput
{
	enum class DeviceKind : uint8_t
	{
		Keyboard,
		XInputPad,
		DInputPad,
	};

	// Control codes.
	//   Keyboard  : the Windows virtual-key code, 1..254.
	//   XInputPad : 0..15 the buttons in kXInputControlNames order, 16..19 left stick
	//               up/down/left/right, 20..23 right stick.
	//   DInputPad : 0..127 buttons, 128..143 hat directions (hat*4 + up/right/down/left),
	//               160..175 axis halves (axis*2 + negative/positive) for X Y Z Rx Ry Rz S0 S1.
	constexpr int kMaxControls = 256;
	using ControlSet = std::bitset<kMaxControls>;

	constexpr int kXInputControlCount = 24;
	constexpr int kDInputButtonCount = 128;
	constexpr int kDInputPovBase = 128;
	constexpr int kDInputPovHats = 4;
	constexpr int kDInputAxisBase = 160;
	constexpr int kDInputAxisCount = 8;

	struct DeviceSnapshot
	{
		std::string id;           // stable across sessions where the OS allows it
		std::string defaultName;  // what the OS calls it
		DeviceKind kind = DeviceKind::Keyboard;
		uint16_t vendorId = 0;    // USB ids of DirectInput HID devices, 0 when unknown
		uint16_t productId = 0;
		uint8_t subType = 0;      // XInput subtype (XINPUT_DEVSUBTYPE_*), 0 when unknown
		bool connected = false;
		ControlSet controls;      // what is held right now
		// Raw analog values for the "is my controller working" view, -1000..1000.
		int16_t axes[kDInputAxisCount] = {};
		int axisCount = 0;
	};

	// The game's own buttons: bit i of the controller word is the game's action index i
	// (docs/Decompilation/Controllers.md section 0). Menus read the same bits: D (12) confirms,
	// C (13) goes back, Start (26) starts, Reset (27) is menu back/select, directions move the
	// cursor. 22-25 are the game's own button macros.
	enum GameButton : uint32_t
	{
		Btn_Up = 0x00000001,
		Btn_Right = 0x00000002,
		Btn_Down = 0x00000004,
		Btn_Left = 0x00000008,
		Btn_D = 0x00001000,
		Btn_C = 0x00002000,
		Btn_A = 0x00004000,
		Btn_B = 0x00008000,
		Btn_Fn1 = 0x00010000,
		Btn_Taunt = 0x00020000,
		Btn_Fn2 = 0x00040000,
		Btn_Special = 0x00080000,
		Btn_MacroAB = 0x00400000,
		Btn_MacroBC = 0x00800000,
		Btn_MacroABC = 0x01000000,
		Btn_MacroABCD = 0x02000000,
		Btn_Start = 0x04000000,
		Btn_Reset = 0x08000000,
	};

	constexpr uint32_t kDirectionMask = Btn_Up | Btn_Right | Btn_Down | Btn_Left;

	struct GameButtonInfo
	{
		GameButton bit;
		const char* name;      // shown in the mapping screen
		const char* menuRole;  // what it does in menus, or nullptr
	};

	constexpr GameButtonInfo kGameButtons[] = {
		{ Btn_Up,      "Up",      nullptr },
		{ Btn_Down,    "Down",    nullptr },
		{ Btn_Left,    "Left",    nullptr },
		{ Btn_Right,   "Right",   nullptr },
		{ Btn_A,       "A",       nullptr },
		{ Btn_B,       "B",       nullptr },
		{ Btn_C,       "C",       "back" },
		{ Btn_D,       "D",       "confirm" },
		{ Btn_Taunt,   "Taunt",   nullptr },
		{ Btn_Special, "Special", nullptr },
		{ Btn_Fn1,     "Fn1",     nullptr },
		{ Btn_Fn2,     "Fn2",     nullptr },
		{ Btn_MacroAB,   "A+B",     nullptr },
		{ Btn_MacroBC,   "B+C",     nullptr },
		{ Btn_MacroABC,  "A+B+C",   nullptr },
		{ Btn_MacroABCD, "A+B+C+D", nullptr },
		{ Btn_Start,   "Start",   nullptr },
		{ Btn_Reset,   "Reset positions", "back/select" },
	};
	constexpr int kGameButtonCount = sizeof(kGameButtons) / sizeof(kGameButtons[0]);

	// Opposing directions cancel out (SOCD neutral), the way the game's own keyboard path
	// resolves them.
	inline uint32_t CleanDirections(uint32_t word)
	{
		if ((word & Btn_Left) && (word & Btn_Right))
			word &= ~(Btn_Left | Btn_Right);
		if ((word & Btn_Up) && (word & Btn_Down))
			word &= ~(Btn_Up | Btn_Down);
		return word;
	}
}
