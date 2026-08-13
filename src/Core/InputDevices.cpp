#include "InputDevices.h"

#include "dllmain.h"
#include "interfaces.h"
#include "logger.h"
#include "XInputRuntime.h"

#include <dinput.h>
#include <Xinput.h>
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace
{
	// Re-enumerating DirectInput devices costs a few milliseconds, so it happens on a timer
	// rather than every frame - and the timer is much shorter while a rebind prompt is open,
	// where plugging a stick in and having it work seconds later is the whole point, than it
	// is during normal play, where a stall is worse than noticing a hotplug late.
	constexpr DWORD kEnumIntervalCapturingMs = 1000;
	constexpr DWORD kEnumIntervalIdleMs = 15000;

	// XInputGetState on an empty slot is measurably slow on some systems, so slots that
	// answered "nothing plugged in" are only re-checked occasionally rather than every frame.
	constexpr DWORD kDisconnectedRecheckMs = 2000;

	// Axis half-press threshold on the -1000..1000 range every axis is normalized to. Well
	// past the resting slop of a worn analog stick, well below a deliberate push.
	constexpr LONG kAxisThreshold = 600;

	// XInput triggers are 0..255 analog; the game's own trigger handling uses the same cut.
	constexpr BYTE kTriggerThreshold = 128;

	struct XInputButtonDef
	{
		WORD mask;              // 0 for the analog triggers, which are not in wButtons
		const char* name;
		bool isLeftTrigger;
		bool isRightTrigger;
	};

	// Order is load-bearing: it is the control-code order, and it matches the legacy
	// Unlimited Playback controller table (0x1000 + index) that old settings.ini files
	// still contain. Never reorder, only append.
	const XInputButtonDef kXInputButtons[InputDevices::kXInputButtonCount] = {
		{ XINPUT_GAMEPAD_A,              "Pad A",     false, false },
		{ XINPUT_GAMEPAD_B,              "Pad B",     false, false },
		{ XINPUT_GAMEPAD_X,              "Pad X",     false, false },
		{ XINPUT_GAMEPAD_Y,              "Pad Y",     false, false },
		{ XINPUT_GAMEPAD_LEFT_SHOULDER,  "Pad LB",    false, false },
		{ XINPUT_GAMEPAD_RIGHT_SHOULDER, "Pad RB",    false, false },
		{ XINPUT_GAMEPAD_BACK,           "Pad Back",  false, false },
		{ XINPUT_GAMEPAD_START,          "Pad Start", false, false },
		{ XINPUT_GAMEPAD_LEFT_THUMB,     "Pad LS",    false, false },
		{ XINPUT_GAMEPAD_RIGHT_THUMB,    "Pad RS",    false, false },
		{ XINPUT_GAMEPAD_DPAD_UP,        "Pad Up",    false, false },
		{ XINPUT_GAMEPAD_DPAD_DOWN,      "Pad Down",  false, false },
		{ XINPUT_GAMEPAD_DPAD_LEFT,      "Pad Left",  false, false },
		{ XINPUT_GAMEPAD_DPAD_RIGHT,     "Pad Right", false, false },
		{ 0,                             "Pad L2",    true,  false },
		{ 0,                             "Pad R2",    false, true  },
	};

	struct XPadState
	{
		bool connected = false;
		DWORD lastDisconnectedCheckTick = 0;
		bool everChecked = false;
		bool down[InputDevices::kXInputButtonCount] = {};
		bool captureBaseline[InputDevices::kXInputButtonCount] = {};
	};

	struct AxisInfo
	{
		bool present = false;
		LONG min = -1000;
		LONG max = 1000;
	};

	struct DIDeviceState
	{
		LPDIRECTINPUTDEVICE8A device = nullptr;
		std::string id;
		std::string displayName;
		bool connected = false;
		bool seenThisEnum = false;
		AxisInfo axes[InputDevices::kControlAxisCount];
		bool down[InputDevices::kControlMax] = {};
		bool captureBaseline[InputDevices::kControlMax] = {};
	};

	std::recursive_mutex g_mutex;
	bool g_shutdown = false;
	LPDIRECTINPUT8A g_dinput = nullptr;
	bool g_dinputInitFailed = false;
	DWORD g_lastEnumTick = 0;
	bool g_everEnumerated = false;
	XPadState g_pads[XUSER_MAX_COUNT];
	std::map<std::string, DIDeviceState> g_diDevices;
	bool g_capturing = false;

	std::string GuidToId(const GUID& guid)
	{
		char buf[64];
		std::snprintf(buf, sizeof(buf),
			"DI:%08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X",
			(unsigned long)guid.Data1, guid.Data2, guid.Data3,
			guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
			guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
		return buf;
	}

	// Maps a DIJOYSTATE2 member offset onto our flat axis index. Anything we do not
	// recognise (the second set of sliders, force-feedback actuators) is simply not bindable.
	int AxisIndexFromOffset(DWORD ofs)
	{
		switch (ofs)
		{
		case DIJOFS_X:         return 0;
		case DIJOFS_Y:         return 1;
		case DIJOFS_Z:         return 2;
		case DIJOFS_RX:        return 3;
		case DIJOFS_RY:        return 4;
		case DIJOFS_RZ:        return 5;
		case DIJOFS_SLIDER(0): return 6;
		case DIJOFS_SLIDER(1): return 7;
		default:               return -1;
		}
	}

	LONG AxisValueFromState(const DIJOYSTATE2& js, int axisIndex)
	{
		switch (axisIndex)
		{
		case 0: return js.lX;
		case 1: return js.lY;
		case 2: return js.lZ;
		case 3: return js.lRx;
		case 4: return js.lRy;
		case 5: return js.lRz;
		case 6: return js.rglSlider[0];
		case 7: return js.rglSlider[1];
		default: return 0;
		}
	}

	BOOL CALLBACK EnumAxesCallback(LPCDIDEVICEOBJECTINSTANCEA instance, LPVOID context)
	{
		DIDeviceState* state = (DIDeviceState*)context;
		const int axisIndex = AxisIndexFromOffset(instance->dwOfs);
		if (axisIndex < 0 || axisIndex >= InputDevices::kControlAxisCount)
			return DIENUM_CONTINUE;

		state->axes[axisIndex].present = true;

		// Prefer a normalized -1000..1000 range so one threshold fits every device. Devices
		// that refuse the range (some HID gamepads report DIERR_UNSUPPORTED) keep their own,
		// which is why the real range is read back and used for scaling instead of assumed.
		DIPROPRANGE range = {};
		range.diph.dwSize = sizeof(DIPROPRANGE);
		range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
		range.diph.dwHow = DIPH_BYID;
		range.diph.dwObj = instance->dwType;
		range.lMin = -1000;
		range.lMax = 1000;
		state->device->SetProperty(DIPROP_RANGE, &range.diph);

		DIPROPRANGE actual = {};
		actual.diph.dwSize = sizeof(DIPROPRANGE);
		actual.diph.dwHeaderSize = sizeof(DIPROPHEADER);
		actual.diph.dwHow = DIPH_BYID;
		actual.diph.dwObj = instance->dwType;
		if (SUCCEEDED(state->device->GetProperty(DIPROP_RANGE, &actual.diph)) && actual.lMax > actual.lMin)
		{
			state->axes[axisIndex].min = actual.lMin;
			state->axes[axisIndex].max = actual.lMax;
		}

		return DIENUM_CONTINUE;
	}

	// True when this DirectInput device is really an XInput pad. Such devices show up on both
	// APIs, and listing the same controller twice under two different button numberings is
	// exactly the kind of confusion this whole feature is meant to remove. Microsoft's own
	// documented marker is "IG_" in the HID device path.
	bool IsXInputDevice(LPDIRECTINPUTDEVICE8A device)
	{
		DIPROPGUIDANDPATH path = {};
		path.diph.dwSize = sizeof(DIPROPGUIDANDPATH);
		path.diph.dwHeaderSize = sizeof(DIPROPHEADER);
		path.diph.dwHow = DIPH_DEVICE;
		if (FAILED(device->GetProperty(DIPROP_GUIDANDPATH, &path.diph)))
			return false;

		for (int i = 0; i < MAX_PATH - 2 && path.wszPath[i]; ++i)
		{
			if ((path.wszPath[i] == L'i' || path.wszPath[i] == L'I') &&
				(path.wszPath[i + 1] == L'g' || path.wszPath[i + 1] == L'G') &&
				path.wszPath[i + 2] == L'_')
				return true;
		}
		return false;
	}

	BOOL CALLBACK EnumDevicesCallback(LPCDIDEVICEINSTANCEA instance, LPVOID)
	{
		const std::string id = GuidToId(instance->guidInstance);

		auto existing = g_diDevices.find(id);
		if (existing != g_diDevices.end())
		{
			existing->second.seenThisEnum = true;
			return DIENUM_CONTINUE;
		}

		LPDIRECTINPUTDEVICE8A device = nullptr;
		if (FAILED(g_dinput->CreateDevice(instance->guidInstance, &device, nullptr)) || !device)
			return DIENUM_CONTINUE;

		if (IsXInputDevice(device))
		{
			device->Release();
			return DIENUM_CONTINUE;
		}

		if (FAILED(device->SetDataFormat(&c_dfDIJoystick2)))
		{
			device->Release();
			return DIENUM_CONTINUE;
		}

		// BACKGROUND | NONEXCLUSIVE is the whole safety story: the game keeps full access to
		// the same device, and hotkeys still work when the window is not focused (the focus
		// check lives in the hotkey layer, not here).
		if (g_gameProc.hWndGameWindow)
			device->SetCooperativeLevel(g_gameProc.hWndGameWindow, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);

		DIDeviceState state;
		state.device = device;
		state.id = id;
		state.displayName = instance->tszInstanceName[0] ? instance->tszInstanceName : "Controller";
		state.seenThisEnum = true;

		device->EnumObjects(EnumAxesCallback, &state, DIDFT_AXIS);
		device->Acquire();

		g_diDevices[id] = state;
		LOG(2, "[hotkeys] DirectInput device added: %s (%s)\n", state.displayName.c_str(), id.c_str());
		return DIENUM_CONTINUE;
	}

	void EnsureDirectInput()
	{
		if (g_dinput || g_dinputInitFailed || g_shutdown)
			return;

		// Deliberately the pointer resolved from the real system dinput8.dll rather than the
		// import-lib DirectInput8Create: this module IS dinput8.dll, so the import would
		// resolve back into our own forwarding export.
		if (!orig_DirectInput8Create)
		{
			g_dinputInitFailed = true;
			return;
		}

		LPDIRECTINPUT8A dinput = nullptr;
		const HRESULT hr = orig_DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION,
			IID_IDirectInput8A, (LPVOID*)&dinput, nullptr);
		if (FAILED(hr) || !dinput)
		{
			g_dinputInitFailed = true;
			LOG(1, "[hotkeys] DirectInput8Create failed (hr=0x%08X); controller binds limited to XInput\n", hr);
			return;
		}

		g_dinput = dinput;
	}

	void EnumerateDevices()
	{
		EnsureDirectInput();
		if (!g_dinput)
			return;

		for (auto& entry : g_diDevices)
			entry.second.seenThisEnum = false;

		g_dinput->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumDevicesCallback, nullptr, DIEDFL_ATTACHEDONLY);

		for (auto it = g_diDevices.begin(); it != g_diDevices.end(); )
		{
			if (it->second.seenThisEnum)
			{
				++it;
				continue;
			}

			LOG(2, "[hotkeys] DirectInput device removed: %s\n", it->second.displayName.c_str());
			if (it->second.device)
			{
				it->second.device->Unacquire();
				it->second.device->Release();
			}
			it = g_diDevices.erase(it);
		}
	}

	void PollXInput()
	{
		const DWORD now = GetTickCount();
		for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot)
		{
			XPadState& pad = g_pads[slot];

			if (pad.everChecked && !pad.connected &&
				(now - pad.lastDisconnectedCheckTick) < kDisconnectedRecheckMs)
				continue;

			XINPUT_STATE state = {};
			if (XInputRuntime::GetState(slot, &state) != ERROR_SUCCESS)
			{
				pad.connected = false;
				pad.everChecked = true;
				pad.lastDisconnectedCheckTick = now;
				std::memset(pad.down, 0, sizeof(pad.down));
				continue;
			}

			pad.everChecked = true;

			pad.connected = true;
			for (int i = 0; i < InputDevices::kXInputButtonCount; ++i)
			{
				const XInputButtonDef& def = kXInputButtons[i];
				if (def.isLeftTrigger)
					pad.down[i] = state.Gamepad.bLeftTrigger > kTriggerThreshold;
				else if (def.isRightTrigger)
					pad.down[i] = state.Gamepad.bRightTrigger > kTriggerThreshold;
				else
					pad.down[i] = (state.Gamepad.wButtons & def.mask) != 0;
			}
		}
	}

	void PollDirectInput()
	{
		for (auto& entry : g_diDevices)
		{
			DIDeviceState& dev = entry.second;
			if (!dev.device)
				continue;

			HRESULT hr = dev.device->Poll();
			if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
			{
				if (FAILED(dev.device->Acquire()))
				{
					dev.connected = false;
					std::memset(dev.down, 0, sizeof(dev.down));
					continue;
				}
				dev.device->Poll();
			}

			DIJOYSTATE2 js = {};
			if (FAILED(dev.device->GetDeviceState(sizeof(js), &js)))
			{
				dev.connected = false;
				std::memset(dev.down, 0, sizeof(dev.down));
				continue;
			}

			dev.connected = true;
			std::memset(dev.down, 0, sizeof(dev.down));

			for (int i = 0; i < InputDevices::kControlButtonCount && i < 128; ++i)
				dev.down[InputDevices::kControlButtonBase + i] = (js.rgbButtons[i] & 0x80) != 0;

			for (int hat = 0; hat < InputDevices::kControlPovHats; ++hat)
			{
				const DWORD pov = js.rgdwPOV[hat];
				if (LOWORD(pov) == 0xFFFF)
					continue;

				// Hundredths of a degree, clockwise from up. Each direction owns a 135 degree
				// window so a diagonal reports both of its neighbours -- matching what the
				// user sees on the stick, and letting a hat be used as four usable binds.
				const int angle = (int)(pov % 36000);
				const bool up = angle >= 29250 || angle <= 6750;
				const bool right = angle >= 2250 && angle <= 15750;
				const bool downDir = angle >= 11250 && angle <= 24750;
				const bool left = angle >= 20250 && angle <= 33750;

				const int base = InputDevices::kControlPovBase + hat * 4;
				dev.down[base + 0] = up;
				dev.down[base + 1] = right;
				dev.down[base + 2] = downDir;
				dev.down[base + 3] = left;
			}

			for (int axis = 0; axis < InputDevices::kControlAxisCount; ++axis)
			{
				if (!dev.axes[axis].present)
					continue;

				const LONG raw = AxisValueFromState(js, axis);
				const LONG min = dev.axes[axis].min;
				const LONG max = dev.axes[axis].max;
				const LONG center = min + (max - min) / 2;
				const LONG halfRange = (max - min) / 2;
				if (halfRange <= 0)
					continue;

				// Scale onto -1000..1000 so kAxisThreshold means the same thing on a device
				// that kept its native 0..65535 range as on one that accepted ours.
				const LONG normalized = ((raw - center) * 1000) / halfRange;
				const int base = InputDevices::kControlAxisBase + axis * 2;
				dev.down[base + 0] = normalized <= -kAxisThreshold;
				dev.down[base + 1] = normalized >= kAxisThreshold;
			}
		}
	}

	bool IsXInputId(const std::string& deviceId)
	{
		return deviceId.size() >= 3 && deviceId[0] == 'X' && deviceId[1] == 'I';
	}

	// -1 when the id means "any XInput pad".
	int XInputSlotFromId(const std::string& deviceId)
	{
		if (deviceId.size() < 3 || deviceId[2] == '*')
			return -1;
		const int slot = deviceId[2] - '0';
		return (slot >= 0 && slot < XUSER_MAX_COUNT) ? slot : -1;
	}
}

void InputDevices::Update()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (g_shutdown)
		return;

	const DWORD now = GetTickCount();
	const DWORD enumInterval = g_capturing ? kEnumIntervalCapturingMs : kEnumIntervalIdleMs;
	if (!g_everEnumerated || (now - g_lastEnumTick) >= enumInterval)
	{
		g_lastEnumTick = now;
		g_everEnumerated = true;
		EnumerateDevices();
	}

	PollXInput();
	PollDirectInput();
}

void InputDevices::Shutdown()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	g_shutdown = true;

	for (auto& entry : g_diDevices)
	{
		if (!entry.second.device)
			continue;
		entry.second.device->Unacquire();
		entry.second.device->Release();
	}
	g_diDevices.clear();

	if (g_dinput)
	{
		g_dinput->Release();
		g_dinput = nullptr;
	}
}

std::vector<InputDevices::DeviceInfo> InputDevices::GetDevices()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::vector<DeviceInfo> devices;

	for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot)
	{
		if (!g_pads[slot].connected)
			continue;
		DeviceInfo info;
		info.id = "XI" + std::to_string(slot);
		info.displayName = "Xbox pad " + std::to_string(slot + 1);
		info.isXInput = true;
		info.connected = true;
		devices.push_back(info);
	}

	for (const auto& entry : g_diDevices)
	{
		DeviceInfo info;
		info.id = entry.second.id;
		info.displayName = entry.second.displayName;
		info.isXInput = false;
		info.connected = entry.second.connected;
		devices.push_back(info);
	}

	return devices;
}

bool InputDevices::IsControlDown(const std::string& deviceId, int control)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (deviceId.empty() || control < 0 || control >= kControlMax)
		return false;

	if (IsXInputId(deviceId))
	{
		if (control >= kXInputButtonCount)
			return false;

		const int slot = XInputSlotFromId(deviceId);
		if (slot >= 0)
			return g_pads[slot].connected && g_pads[slot].down[control];

		for (int i = 0; i < XUSER_MAX_COUNT; ++i)
			if (g_pads[i].connected && g_pads[i].down[control])
				return true;
		return false;
	}

	auto it = g_diDevices.find(deviceId);
	if (it == g_diDevices.end() || !it->second.connected)
		return false;
	return it->second.down[control];
}

void InputDevices::BeginCapture()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	g_capturing = true;

	// Snapshot rather than "wait for everything to be released": a hitbox with a resting
	// trigger axis or a stuck POV would otherwise make the capture prompt hang forever.
	for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot)
		std::memcpy(g_pads[slot].captureBaseline, g_pads[slot].down, sizeof(g_pads[slot].down));

	for (auto& entry : g_diDevices)
		std::memcpy(entry.second.captureBaseline, entry.second.down, sizeof(entry.second.down));
}

void InputDevices::EndCapture()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	g_capturing = false;
}

bool InputDevices::PollCapture(std::string& outDeviceId, int& outControl)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (!g_capturing)
		return false;

	for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot)
	{
		if (!g_pads[slot].connected)
			continue;
		for (int i = 0; i < kXInputButtonCount; ++i)
		{
			if (g_pads[slot].down[i] && !g_pads[slot].captureBaseline[i])
			{
				outDeviceId = "XI" + std::to_string(slot);
				outControl = i;
				g_capturing = false;
				return true;
			}
		}
	}

	for (auto& entry : g_diDevices)
	{
		if (!entry.second.connected)
			continue;
		for (int control = 0; control < kControlMax; ++control)
		{
			if (entry.second.down[control] && !entry.second.captureBaseline[control])
			{
				outDeviceId = entry.second.id;
				outControl = control;
				g_capturing = false;
				return true;
			}
		}
	}

	return false;
}

bool InputDevices::AnyControlDown()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);

	for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot)
	{
		if (!g_pads[slot].connected)
			continue;
		for (int i = 0; i < kXInputButtonCount; ++i)
			if (g_pads[slot].down[i])
				return true;
	}

	for (const auto& entry : g_diDevices)
	{
		if (!entry.second.connected)
			continue;
		for (int control = 0; control < kControlMax; ++control)
			if (entry.second.down[control])
				return true;
	}

	return false;
}

std::string InputDevices::ControlName(const std::string& deviceId, int control)
{
	if (control < 0 || control >= kControlMax)
		return "None";

	if (IsXInputId(deviceId))
	{
		if (control < kXInputButtonCount)
			return kXInputButtons[control].name;
		return "Pad button";
	}

	char buf[64];
	if (control >= kControlAxisBase)
	{
		const int axis = (control - kControlAxisBase) / 2;
		const bool positive = ((control - kControlAxisBase) % 2) != 0;
		std::snprintf(buf, sizeof(buf), "Axis %d %s", axis + 1, positive ? "+" : "-");
		return buf;
	}

	if (control >= kControlPovBase)
	{
		static const char* const kPovNames[4] = { "Up", "Right", "Down", "Left" };
		const int hat = (control - kControlPovBase) / 4;
		const int dir = (control - kControlPovBase) % 4;
		std::snprintf(buf, sizeof(buf), "Hat %d %s", hat + 1, kPovNames[dir]);
		return buf;
	}

	std::snprintf(buf, sizeof(buf), "Button %d", control + 1);
	return buf;
}

std::string InputDevices::DeviceDisplayName(const std::string& deviceId)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);

	if (IsXInputId(deviceId))
	{
		const int slot = XInputSlotFromId(deviceId);
		if (slot < 0)
			return "Any controller";
		return "Xbox pad " + std::to_string(slot + 1);
	}

	auto it = g_diDevices.find(deviceId);
	if (it != g_diDevices.end())
		return it->second.displayName;

	return "Unplugged controller";
}
