#pragma once

/*
	Polling layer for "everything that is not the keyboard": XInput pads and every
	DirectInput game controller attached to the machine (fightsticks, hitboxes, generic
	pads, dance mats, whatever the user has plugged in).

	This exists so hotkeys can be bound to a spare controller button instead of a keyboard
	key. It is READ-ONLY and completely separate from the input the game itself consumes:
	devices are opened non-exclusively in background mode, so opening them here never takes
	input away from BBCF, and never changes what the game sees.

	Controls (buttons, hat directions, axis halves) are addressed by an int "control code"
	so a binding can be stored as a small, stable pair of (device id, control code):

		0   .. 127  buttons             (button index)
		128 .. 143  POV hat directions  (kControlPovBase + hat*4 + dir, dir 0=U 1=R 2=D 3=L)
		160 .. 183  axis halves         (kControlAxisBase + axis*2 + (0 = negative, 1 = positive))

	Device ids are short strings that survive a settings.ini round trip:

		"XI0".."XI3"  a specific XInput pad slot
		"XI*"         any XInput pad (what the legacy Unlimited Playback binds meant)
		"DI:<guid>"   a specific DirectInput device, by instance GUID

	Update() must be called once per frame from a single site (WindowManager::Update).
*/

#include <string>
#include <vector>

namespace InputDevices
{
	constexpr int kControlButtonBase = 0;
	constexpr int kControlButtonCount = 128;
	constexpr int kControlPovBase = 128;
	constexpr int kControlPovHats = 4;
	constexpr int kControlAxisBase = 160;
	constexpr int kControlAxisCount = 12;
	constexpr int kControlMax = kControlAxisBase + kControlAxisCount * 2;

	constexpr int kXInputButtonCount = 16;

	struct DeviceInfo
	{
		std::string id;
		std::string displayName;
		bool isXInput = false;
		bool connected = false;
	};

	// Polls every connected device. Only call it when something actually needs device input
	// (a binding uses one, or a rebind prompt is listening): enumeration and polling are
	// cheap but not free, and this runs on the render thread of a 60fps fighting game.
	void Update();
	void Shutdown();

	std::vector<DeviceInfo> GetDevices();

	// deviceId "XI*" matches any connected XInput pad. An unknown/disconnected device is
	// never down, so a binding to an unplugged stick is inert rather than an error.
	bool IsControlDown(const std::string& deviceId, int control);

	// Capture support for the rebind UI. BeginCapture() snapshots the current state of every
	// control so that axes and triggers which merely rest away from centre are not reported
	// as "pressed"; PollCapture() then returns the first control the user actually moves.
	void BeginCapture();
	void EndCapture();
	bool PollCapture(std::string& outDeviceId, int& outControl);
	bool AnyControlDown();

	// "Pad A", "Button 5", "Hat 1 Up", "Axis 3 +". Never returns an empty string.
	std::string ControlName(const std::string& deviceId, int control);
	// "Xbox pad 1", "Hitbox Arcade Stick", "Unplugged controller".
	std::string DeviceDisplayName(const std::string& deviceId);
}
