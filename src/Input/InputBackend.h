#pragma once

// Where devices come from. One backend per platform; LocalInputManager only ever talks to this
// interface, so supporting Linux natively later means writing a backend (evdev / SDL), not
// touching assignment, mapping or the UI.
//
// Today the Windows backend is used everywhere, including under Wine/Proton, where XInput and
// DirectInput are emulated well. LinuxInputBackend.cpp is the placeholder for a native one.

#include "LocalInputTypes.h"

#include <Windows.h>

#include <memory>
#include <vector>

namespace LocalInput
{
	class IInputBackend
	{
	public:
		virtual ~IInputBackend() = default;

		virtual const char* Name() const = 0;

		// Called once from the game thread. May start worker threads.
		virtual void Start() = 0;
		virtual void Stop() = 0;

		// Called once per game frame from the game thread. Fills `out` with every device the
		// backend knows about, connected or not: a device that disappears stays in the list
		// with connected = false until Stop, so the manager can keep its assignment.
		virtual void Poll(std::vector<DeviceSnapshot>& out) = 0;

		// The game window's messages (raw keyboard input, device arrival/removal, focus).
		// Return true when the message was consumed.
		virtual bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) = 0;

		// The game window lost or regained focus. Keyboards must release every key on loss.
		virtual void OnFocusChanged(bool focused) = 0;

		// Hot-plug on/off. Off: devices already open keep working, but nothing new is looked
		// for until Refresh().
		virtual void SetAutoDetect(bool enabled) = 0;
		// Look for devices now (the "Refresh controllers" button).
		virtual void Refresh() = 0;
		// Steam Input is presenting controllers through its own virtual pads.
		virtual bool SteamInputLikely() const = 0;
	};

	std::unique_ptr<IInputBackend> CreateWindowsInputBackend();
	std::unique_ptr<IInputBackend> CreateLinuxInputBackend();
}
