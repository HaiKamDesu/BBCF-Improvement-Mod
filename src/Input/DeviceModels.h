#pragma once

// What a device physically is, for the UI only: its product name, whether it is an arcade
// stick, and which raw control sits where with which printed name ("Cross" bottom, "Options"
// right of centre...). Never used to decide what a control does: bindings stay exactly what
// the player set, raw control k is game key k like in vanilla.
//
// Pads are recognised by USB vendor/product id; XInput devices by their XInput subtype.
// Raw orders were taken from the device's own HID report where a faithful one was available
// (DualShock 4, DualSense, Switch 2 Pro probed through HIDMaestro virtual pads on 2026-10-07),
// otherwise from the controller's published report format (PS4-licensed pads and sticks send
// the DualShock 4 report; HORI-style Switch pads and the Switch Pro's simple HID report).

#include "LocalInputTypes.h"

#include <cstdint>
#include <string>

namespace LocalInput
{
	enum class PadFamily : uint8_t
	{
		Keyboard,
		Xbox,
		PlayStation,
		Nintendo,
		Generic,     // unknown: controls are shown by their number
	};

	// Physical places, named after an Xbox pad's positions.
	enum PadRole : uint8_t
	{
		Role_South, Role_East, Role_West, Role_North,
		Role_L1, Role_R1, Role_L2, Role_R2,
		Role_Select, Role_Start, Role_L3, Role_R3,
		Role_Home, Role_Touchpad, Role_Capture,
		Role_DpadUp, Role_DpadDown, Role_DpadLeft, Role_DpadRight,
		Role_Count
	};

	struct PadLayout
	{
		PadFamily family;
		int16_t control[Role_Count];   // control code at that place, -1 when the device has none
		const char* name[Role_Count];  // what is printed on it
		int8_t rightX, rightY;         // axis indices (DeviceSnapshot::axes) of the right stick, -1 none
	};

	struct DeviceModel
	{
		const char* name = nullptr;        // product name when the OS name is unhelpful, else null
		bool arcadeStick = false;
		const PadLayout* layout = nullptr; // null for keyboards
		PadFamily Family() const { return layout ? layout->family : PadFamily::Keyboard; }
	};

	// XInput subtypes worth telling apart (XINPUT_DEVSUBTYPE_*).
	constexpr uint8_t kXInputSubTypeArcadeStick = 0x03;
	constexpr uint8_t kXInputSubTypeArcadePad = 0x13;

	DeviceModel IdentifyDevice(const DeviceSnapshot& snap);

	// The printed name of a raw control on this layout ("Cross", "Options"), or null.
	const char* LayoutControlName(const PadLayout* layout, int control);
}
