#include "DeviceModels.h"

#include <cctype>

namespace LocalInput
{
	namespace
	{
		// DirectInput hat 0 as control codes.
		constexpr int16_t kHatUp = kDInputPovBase + 0;
		constexpr int16_t kHatRight = kDInputPovBase + 1;
		constexpr int16_t kHatDown = kDInputPovBase + 2;
		constexpr int16_t kHatLeft = kDInputPovBase + 3;
		// DeviceSnapshot::axes order: X Y Z Rx Ry Rz S0 S1.
		constexpr int8_t kAxisZ = 2, kAxisRx = 3, kAxisRy = 4, kAxisRz = 5;

		// Role order: South East West North | L1 R1 L2 R2 | Select Start L3 R3 | Home Touchpad
		// Capture | d-pad up down left right.

		// XInput control codes (kXInputControlNames order); the backend puts the right stick
		// in axes 2 and 3.
		const PadLayout kXbox = { PadFamily::Xbox,
			{ 0, 1, 2, 3, 4, 5, 14, 15, 6, 7, 8, 9, -1, -1, -1, 10, 11, 12, 13 },
			{ "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", "LS", "RS", nullptr, nullptr, nullptr, "Up", "Down", "Left", "Right" },
			kAxisZ, kAxisRx };

		// DualShock 4 report, also sent by PS4-licensed pads and sticks. Probed.
		const PadLayout kDualShock4 = { PadFamily::PlayStation,
			{ 1, 2, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, -1, kHatUp, kHatDown, kHatLeft, kHatRight },
			{ "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "Share", "Options", "L3", "R3", "PS", "Touchpad", nullptr, "Up", "Down", "Left", "Right" },
			kAxisZ, kAxisRz };

		// DualSense: same order, Share is called Create. Probed.
		const PadLayout kDualSense = { PadFamily::PlayStation,
			{ 1, 2, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, -1, kHatUp, kHatDown, kHatLeft, kHatRight },
			{ "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "Create", "Options", "L3", "R3", "PS", "Touchpad", nullptr, "Up", "Down", "Left", "Right" },
			kAxisZ, kAxisRz };

		// Switch Pro Controller's simple HID report (what Windows reads without Steam).
		const PadLayout kSwitchPro = { PadFamily::Nintendo,
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, -1, 13, kHatUp, kHatDown, kHatLeft, kHatRight },
			{ "B", "A", "Y", "X", "L", "R", "ZL", "ZR", "-", "+", "LS", "RS", "Home", nullptr, "Capture", "Up", "Down", "Left", "Right" },
			kAxisRx, kAxisRy };

		// Switch 2 Pro Controller: the d-pad is four buttons, shoulders interleaved. Probed
		// except ZL/ZR, which follow the report's byte layout (R ZR + RS / L ZL - LS).
		const PadLayout kSwitch2Pro = { PadFamily::Nintendo,
			{ 0, 1, 2, 3, 12, 4, 13, 5, 14, 6, 15, 7, 16, -1, 17, 11, 8, 10, 9 },
			{ "B", "A", "Y", "X", "L", "R", "ZL", "ZR", "-", "+", "LS", "RS", "Home", nullptr, "Capture", "Up", "Down", "Left", "Right" },
			kAxisRx, kAxisRz };

		// HORI-style licensed Switch pads: Y B A X first.
		const PadLayout kHoriSwitch = { PadFamily::Nintendo,
			{ 1, 2, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, -1, 13, kHatUp, kHatDown, kHatLeft, kHatRight },
			{ "B", "A", "Y", "X", "L", "R", "ZL", "ZR", "-", "+", "LS", "RS", "Home", nullptr, "Capture", "Up", "Down", "Left", "Right" },
			kAxisZ, kAxisRz };

		// Unknown DirectInput pads: numbered like Windows numbers them, placed where most
		// generic pads put buttons 1-4.
		const PadLayout kGeneric = { PadFamily::Generic,
			{ 1, 2, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1, -1, -1, kHatUp, kHatDown, kHatLeft, kHatRight },
			{ "2", "3", "1", "4", "5", "6", "7", "8", "9", "10", "11", "12", nullptr, nullptr, nullptr, "Up", "Down", "Left", "Right" },
			kAxisZ, kAxisRz };

		struct KnownDevice
		{
			uint16_t vid, pid;
			const char* name;
			bool stick;
			const PadLayout* layout;
		};

		const KnownDevice kKnown[] = {
			// Sony
			{ 0x054C, 0x05C4, "DualShock 4", false, &kDualShock4 },
			{ 0x054C, 0x09CC, "DualShock 4", false, &kDualShock4 },
			{ 0x054C, 0x0BA0, "DualShock 4 (wireless adapter)", false, &kDualShock4 },
			{ 0x054C, 0x0CE6, "DualSense", false, &kDualSense },
			{ 0x054C, 0x0DF2, "DualSense Edge", false, &kDualSense },
			// PS4/PS5-licensed pads and sticks
			{ 0x0F0D, 0x0055, "HORIPAD 4 FPS", false, &kDualShock4 },
			{ 0x0F0D, 0x0066, "HORIPAD 4 FPS Plus", false, &kDualShock4 },
			{ 0x0F0D, 0x005E, "HORI Fighting Commander 4", false, &kDualShock4 },
			{ 0x0F0D, 0x0084, "HORI Fighting Commander", false, &kDualShock4 },
			{ 0x0F0D, 0x00EE, "HORI Mini Wired Gamepad", false, &kDualShock4 },
			{ 0x0F0D, 0x0087, "HORI Fighting Stick mini 4", true, &kDualShock4 },
			{ 0x0F0D, 0x008A, "HORI Real Arcade Pro 4", true, &kDualShock4 },
			{ 0x0F0D, 0x0184, "HORI Fighting Stick Alpha", true, &kDualSense },
			{ 0x1532, 0x0401, "Razer Panthera", true, &kDualShock4 },
			{ 0x1532, 0x1008, "Razer Panthera Evo", true, &kDualShock4 },
			{ 0x1532, 0x1000, "Razer Raiju", false, &kDualShock4 },
			{ 0x1532, 0x1004, "Razer Raiju Ultimate", false, &kDualShock4 },
			{ 0x1532, 0x1007, "Razer Raiju Tournament", false, &kDualShock4 },
			{ 0x0738, 0x8384, "Mad Catz FightStick TE S+", true, &kDualShock4 },
			{ 0x0738, 0x8481, "Mad Catz FightStick TE2+", true, &kDualShock4 },
			{ 0x0738, 0x8250, "Mad Catz FightPad PRO", false, &kDualShock4 },
			{ 0x2C22, 0x2000, "Qanba Drone", true, &kDualShock4 },
			{ 0x2C22, 0x2300, "Qanba Obsidian", true, &kDualShock4 },
			{ 0x0C12, 0x0EF6, "Hit Box", true, &kDualShock4 },
			{ 0x146B, 0x0D01, "Nacon Revolution Pro", false, &kDualShock4 },
			{ 0x146B, 0x0D02, "Nacon Revolution Pro 2", false, &kDualShock4 },
			// Nintendo and Switch-licensed
			{ 0x057E, 0x2009, "Switch Pro Controller", false, &kSwitchPro },
			{ 0x057E, 0x2069, "Switch 2 Pro Controller", false, &kSwitch2Pro },
			{ 0x0F0D, 0x00C1, "HORIPAD for Nintendo Switch", false, &kHoriSwitch },
			{ 0x0F0D, 0x0092, "HORI Pokken Controller", false, &kHoriSwitch },
			{ 0x20D6, 0xA711, "PowerA Switch Controller", false, &kHoriSwitch },
		};

		bool NameSaysStick(const std::string& name)
		{
			std::string lower = name;
			for (char& c : lower)
				c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			for (const char* word : { "stick", "arcade", "hitbox", "hit box", "fight", "joystick", "vjoy", "qanba", "mayflash", "snack box", "leverless" })
				if (lower.find(word) != std::string::npos)
					return true;
			return false;
		}
	}

	DeviceModel IdentifyDevice(const DeviceSnapshot& snap)
	{
		DeviceModel model;
		switch (snap.kind)
		{
		case DeviceKind::Keyboard:
			return model;
		case DeviceKind::XInputPad:
			model.layout = &kXbox;
			model.arcadeStick = snap.subType == kXInputSubTypeArcadeStick || snap.subType == kXInputSubTypeArcadePad;
			return model;
		case DeviceKind::DInputPad:
			break;
		}
		for (const KnownDevice& known : kKnown)
		{
			if (known.vid == snap.vendorId && known.pid == snap.productId)
			{
				model.name = known.name;
				model.arcadeStick = known.stick;
				model.layout = known.layout;
				return model;
			}
		}
		model.layout = &kGeneric;
		model.arcadeStick = NameSaysStick(snap.defaultName);
		return model;
	}

	const char* LayoutControlName(const PadLayout* layout, int control)
	{
		if (!layout || layout->family == PadFamily::Generic)
			return nullptr;
		for (int role = 0; role < Role_Count; ++role)
			if (layout->control[role] == control && layout->name[role])
				return layout->name[role];
		return nullptr;
	}
}
