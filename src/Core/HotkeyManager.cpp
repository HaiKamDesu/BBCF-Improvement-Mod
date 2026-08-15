#include "HotkeyManager.h"

#include "InputDevices.h"
#include "Settings.h"
#include "interfaces.h"
#include "logger.h"
#include "utils.h"

#include <Windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
	struct KeyName
	{
		int virtualKey;
		const char* name;
	};

	// Canonical names used both for settings.ini serialization and for the UI. Every name the
	// pre-8.4 keycodes.h table accepted is still here, so an untouched settings.ini keeps
	// working; the rest is what a rebind prompt can now actually produce.
	const KeyName kKeyNames[] = {
		{ VK_F1, "F1" }, { VK_F2, "F2" }, { VK_F3, "F3" }, { VK_F4, "F4" },
		{ VK_F5, "F5" }, { VK_F6, "F6" }, { VK_F7, "F7" }, { VK_F8, "F8" },
		{ VK_F9, "F9" }, { VK_F10, "F10" }, { VK_F11, "F11" }, { VK_F12, "F12" },
		{ VK_F13, "F13" }, { VK_F14, "F14" }, { VK_F15, "F15" }, { VK_F16, "F16" },
		{ VK_F17, "F17" }, { VK_F18, "F18" }, { VK_F19, "F19" }, { VK_F20, "F20" },
		{ VK_F21, "F21" }, { VK_F22, "F22" }, { VK_F23, "F23" }, { VK_F24, "F24" },

		{ 'A', "A" }, { 'B', "B" }, { 'C', "C" }, { 'D', "D" }, { 'E', "E" },
		{ 'F', "F" }, { 'G', "G" }, { 'H', "H" }, { 'I', "I" }, { 'J', "J" },
		{ 'K', "K" }, { 'L', "L" }, { 'M', "M" }, { 'N', "N" }, { 'O', "O" },
		{ 'P', "P" }, { 'Q', "Q" }, { 'R', "R" }, { 'S', "S" }, { 'T', "T" },
		{ 'U', "U" }, { 'V', "V" }, { 'W', "W" }, { 'X', "X" }, { 'Y', "Y" },
		{ 'Z', "Z" },

		{ '0', "0" }, { '1', "1" }, { '2', "2" }, { '3', "3" }, { '4', "4" },
		{ '5', "5" }, { '6', "6" }, { '7', "7" }, { '8', "8" }, { '9', "9" },

		{ VK_NUMPAD0, "NUMPAD_0" }, { VK_NUMPAD1, "NUMPAD_1" }, { VK_NUMPAD2, "NUMPAD_2" },
		{ VK_NUMPAD3, "NUMPAD_3" }, { VK_NUMPAD4, "NUMPAD_4" }, { VK_NUMPAD5, "NUMPAD_5" },
		{ VK_NUMPAD6, "NUMPAD_6" }, { VK_NUMPAD7, "NUMPAD_7" }, { VK_NUMPAD8, "NUMPAD_8" },
		{ VK_NUMPAD9, "NUMPAD_9" },
		{ VK_MULTIPLY, "NUMPAD_MULTIPLY" }, { VK_ADD, "NUMPAD_PLUS" },
		{ VK_SUBTRACT, "NUMPAD_MINUS" }, { VK_DECIMAL, "NUMPAD_PERIOD" },
		{ VK_DIVIDE, "NUMPAD_SLASH" },

		{ VK_OEM_3, "TILDE" }, { VK_OEM_MINUS, "MINUS" }, { VK_OEM_PLUS, "EQUAL" },
		{ VK_BACK, "BACKSPACE" }, { VK_TAB, "TAB" }, { VK_RETURN, "ENTER" },
		{ VK_SHIFT, "SHIFT" }, { VK_CONTROL, "CTRL" }, { VK_MENU, "ALT" },
		{ VK_SPACE, "SPACE" },

		{ VK_LEFT, "ARROW_LEFT" }, { VK_UP, "ARROW_UP" },
		{ VK_RIGHT, "ARROW_RIGHT" }, { VK_DOWN, "ARROW_DOWN" },

		{ VK_OEM_4, "OPEN_BRACKET" }, { VK_OEM_6, "CLOSE_BRACKET" }, { VK_OEM_5, "BACKSLASH" },
		{ VK_OEM_1, "SEMICOLON" }, { VK_OEM_7, "QUOTE" }, { VK_OEM_COMMA, "COMMA" },
		{ VK_OEM_PERIOD, "PERIOD" }, { VK_OEM_2, "SLASH" },

		{ VK_ESCAPE, "ESC" }, { VK_CAPITAL, "CAPSLOCK" }, { VK_INSERT, "INSERT" },
		{ VK_DELETE, "DELETE" }, { VK_HOME, "HOME" }, { VK_END, "END" },
		{ VK_PRIOR, "PAGE_UP" }, { VK_NEXT, "PAGE_DOWN" },
		{ VK_SCROLL, "SCROLL_LOCK" }, { VK_PAUSE, "PAUSE" }, { VK_NUMLOCK, "NUM_LOCK" },
	};

	// The legacy Unlimited Playback binds stored controller buttons as pseudo virtual-key
	// codes starting here, indexing the same 16-entry XInput table InputDevices uses.
	constexpr int kLegacyControllerBindBase = 0x1000;

	struct ActionMetadata
	{
		const char* iniKey;
		const char* defaultBinding;
		const char* displayName;
		const char* tooltip;
	};

	const ActionMetadata kActions[HotkeyManager::Hotkey_Count] = {
#define HOTKEY(_id, _var, _ini, _default, _display, _tooltip) { _ini, _default, _display, _tooltip },
#include "hotkeys.def"
#undef HOTKEY
	};

	// Matches ImGui's key repeat feel, which is what the frame-step hotkey used to inherit
	// from ImGui::IsVirtualKeyPressed(repeat = true).
	constexpr float kRepeatDelaySeconds = 0.275f;
	constexpr float kRepeatRateSeconds = 0.05f;

	struct ActionState
	{
		HotkeyBinding binding;
		bool down = false;
		bool prevDown = false;
		bool suppressedUntilRelease = false;
		float heldSeconds = 0.0f;
		bool repeated = false;
	};

	ActionState g_actions[HotkeyManager::Hotkey_Count];
	bool g_loaded = false;
	// True when at least one action is bound to a controller. While it is false and no rebind
	// prompt is open, the device layer is never polled at all, so a keyboard-only setup pays
	// nothing for this feature.
	bool g_anyDeviceBinding = false;
	bool g_anyDirectInputBinding = false;

	bool g_capturing = false;
	bool g_captureKeyBaseline[256] = {};

	void RefreshDeviceBindingFlag()
	{
		g_anyDeviceBinding = false;
		g_anyDirectInputBinding = false;
		for (int i = 0; i < HotkeyManager::Hotkey_Count; ++i)
		{
			if (g_actions[i].binding.source != HotkeyBinding::Source_Device)
				continue;

			g_anyDeviceBinding = true;
			// Only a binding that names a DirectInput device justifies paying for
			// DirectInput enumeration; XInput pads are polled directly. Keeping
			// these separate is what stops a single controller bind from costing
			// a periodic ~100ms freeze on machines with a large device tree.
			if (!InputDevices::IsXInputDeviceId(g_actions[i].binding.deviceId))
				g_anyDirectInputBinding = true;
		}
	}

	const char* KeyNameFromVirtualKey(int virtualKey)
	{
		for (const KeyName& entry : kKeyNames)
			if (entry.virtualKey == virtualKey)
				return entry.name;
		return nullptr;
	}

	int VirtualKeyFromName(const std::string& name)
	{
		for (const KeyName& entry : kKeyNames)
			if (_stricmp(entry.name, name.c_str()) == 0)
				return entry.virtualKey;
		return 0;
	}

	std::string Trim(const std::string& text)
	{
		size_t begin = 0;
		size_t end = text.size();
		while (begin < end && isspace((unsigned char)text[begin]))
			++begin;
		while (end > begin && isspace((unsigned char)text[end - 1]))
			--end;
		return text.substr(begin, end - begin);
	}

	bool IsAllDigits(const std::string& text)
	{
		if (text.empty())
			return false;
		for (char c : text)
			if (!isdigit((unsigned char)c))
				return false;
		return true;
	}

	bool IsModifierVirtualKey(int virtualKey)
	{
		switch (virtualKey)
		{
		case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
		case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
		case VK_MENU: case VK_LMENU: case VK_RMENU:
			return true;
		default:
			return false;
		}
	}

	bool IsGameWindowFocused()
	{
		return g_gameProc.hWndGameWindow && GetForegroundWindow() == g_gameProc.hWndGameWindow;
	}

	bool IsKeyHeld(int virtualKey)
	{
		return virtualKey > 0 && virtualKey < 256 && (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
	}

	// Raw "is this binding physically held", before focus/typing/suppression gating.
	bool IsBindingHeld(const HotkeyBinding& binding)
	{
		if (binding.source == HotkeyBinding::Source_Device)
			return InputDevices::IsControlDown(binding.deviceId, binding.control);

		if (binding.source != HotkeyBinding::Source_Keyboard || binding.virtualKey == 0)
			return false;

		if (!IsKeyHeld(binding.virtualKey))
			return false;

		// Modifiers must match exactly. Without this a binding of "C" fires on Ctrl+C too,
		// which is precisely how the old freeze-frame key collided with the copy-room-link
		// shortcut. A binding whose main key IS a modifier skips its own check.
		const bool ctrlHeld = IsKeyHeld(VK_CONTROL);
		const bool shiftHeld = IsKeyHeld(VK_SHIFT);
		const bool altHeld = IsKeyHeld(VK_MENU);

		if (binding.virtualKey != VK_CONTROL && ctrlHeld != binding.ctrl)
			return false;
		if (binding.virtualKey != VK_SHIFT && shiftHeld != binding.shift)
			return false;
		if (binding.virtualKey != VK_MENU && altHeld != binding.alt)
			return false;

		return true;
	}

	// Reads the settings.ini string that backs an action.
	const std::string& SettingValueForAction(HotkeyManager::Action action)
	{
		switch (action)
		{
#define HOTKEY(_id, _var, _ini, _default, _display, _tooltip) \
		case HotkeyManager::Hotkey_##_id: return Settings::settingsIni._var;
#include "hotkeys.def"
#undef HOTKEY
		default: break;
		}
		static const std::string empty;
		return empty;
	}

	void StoreSettingValueForAction(HotkeyManager::Action action, const std::string& value)
	{
		switch (action)
		{
#define HOTKEY(_id, _var, _ini, _default, _display, _tooltip) \
		case HotkeyManager::Hotkey_##_id: Settings::settingsIni._var = value; break;
#include "hotkeys.def"
#undef HOTKEY
		default: break;
		}
	}
}

std::string HotkeyManager::BindingToString(const HotkeyBinding& binding)
{
	if (binding.source == HotkeyBinding::Source_Device)
	{
		char buf[128];
		std::snprintf(buf, sizeof(buf), "Pad:%d:%s", binding.control, binding.deviceId.c_str());
		return buf;
	}

	if (binding.source != HotkeyBinding::Source_Keyboard || binding.virtualKey == 0)
		return "";

	std::string text;
	if (binding.ctrl)
		text += "Ctrl+";
	if (binding.shift)
		text += "Shift+";
	if (binding.alt)
		text += "Alt+";

	if (const char* name = KeyNameFromVirtualKey(binding.virtualKey))
	{
		text += name;
	}
	else
	{
		// Unnamed key (an exotic OEM key on a non-US layout). Round-trips as a number so the
		// binding survives a settings.ini rewrite even though we cannot name it.
		char buf[16];
		std::snprintf(buf, sizeof(buf), "VK%d", binding.virtualKey);
		text += buf;
	}

	return text;
}

HotkeyBinding HotkeyManager::BindingFromString(const std::string& rawText)
{
	HotkeyBinding binding;
	const std::string text = Trim(rawText);
	if (text.empty())
		return binding;

	if (_strnicmp(text.c_str(), "Pad:", 4) == 0)
	{
		const size_t controlEnd = text.find(':', 4);
		if (controlEnd == std::string::npos)
			return binding;

		binding.control = atoi(text.substr(4, controlEnd - 4).c_str());
		binding.deviceId = text.substr(controlEnd + 1);
		if (binding.deviceId.empty() || binding.control < 0 || binding.control >= InputDevices::kControlMax)
			return HotkeyBinding();

		binding.source = HotkeyBinding::Source_Device;
		return binding;
	}

	// Named keys win over the legacy numeric form below, because the number-row keys are named
	// "0".."9" and would otherwise be read as raw virtual-key codes: a binding on the 1 key
	// serializes to "1", and parsing that as VK code 1 gives VK_LBUTTON, which then re-displays
	// as the meaningless "VK1" and never fires. Legacy codes for real hotkeys are all >= 10
	// (the 1 key was stored as 49), so they still reach the numeric path untouched.
	if (const int namedKey = VirtualKeyFromName(text))
	{
		binding.source = HotkeyBinding::Source_Keyboard;
		binding.virtualKey = namedKey;
		return binding;
	}

	// Legacy numeric form: Unlimited Playback stored raw virtual-key codes, and controller
	// buttons as 0x1000 + index against the XInput table.
	if (IsAllDigits(text))
	{
		const int code = atoi(text.c_str());
		if (code >= kLegacyControllerBindBase && code < kLegacyControllerBindBase + InputDevices::kXInputButtonCount)
		{
			binding.source = HotkeyBinding::Source_Device;
			binding.deviceId = "XI*";
			binding.control = code - kLegacyControllerBindBase;
			return binding;
		}
		if (code > 0 && code < 256)
		{
			binding.source = HotkeyBinding::Source_Keyboard;
			binding.virtualKey = code;
			return binding;
		}
		return binding;
	}

	std::string remaining = text;
	for (;;)
	{
		if (_strnicmp(remaining.c_str(), "Ctrl+", 5) == 0)
		{
			binding.ctrl = true;
			remaining = remaining.substr(5);
		}
		else if (_strnicmp(remaining.c_str(), "Shift+", 6) == 0)
		{
			binding.shift = true;
			remaining = remaining.substr(6);
		}
		else if (_strnicmp(remaining.c_str(), "Alt+", 4) == 0)
		{
			binding.alt = true;
			remaining = remaining.substr(4);
		}
		else
		{
			break;
		}
	}

	remaining = Trim(remaining);
	if (remaining.empty())
		return HotkeyBinding();

	int virtualKey = 0;
	if (_strnicmp(remaining.c_str(), "VK", 2) == 0 && IsAllDigits(remaining.substr(2)))
		virtualKey = atoi(remaining.c_str() + 2);
	else
		virtualKey = VirtualKeyFromName(remaining);

	if (virtualKey <= 0 || virtualKey >= 256)
		return HotkeyBinding();

	binding.source = HotkeyBinding::Source_Keyboard;
	binding.virtualKey = virtualKey;
	return binding;
}

std::string HotkeyManager::DisplayString(const HotkeyBinding& binding)
{
	if (!binding.IsBound())
		return "Not bound";

	if (binding.source == HotkeyBinding::Source_Device)
		return InputDevices::DeviceDisplayName(binding.deviceId) + ": " +
			InputDevices::ControlName(binding.deviceId, binding.control);

	return BindingToString(binding);
}

void HotkeyManager::ReloadFromSettings()
{
	for (int i = 0; i < Hotkey_Count; ++i)
	{
		const Action action = (Action)i;
		// By value: the migration below rewrites the very setting this reads from.
		const std::string stored = SettingValueForAction(action);
		HotkeyBinding binding = BindingFromString(stored);

		// Migration: the Unlimited Playback binds stored 0 to mean "nothing saved, use the
		// built-in default", where an unbound hotkey is now an empty value. Read literally, a
		// 0 would silently take those two hotkeys away from everyone who never rebound them.
		if (stored == "0")
		{
			binding = BindingFromString(kActions[action].defaultBinding);
			const std::string migrated = BindingToString(binding);
			Settings::changeSetting(kActions[action].iniKey, migrated);
			StoreSettingValueForAction(action, migrated);
			LOG(2, "[hotkeys] Migrated %s from unset (0) to its default '%s'\n",
				kActions[action].iniKey, migrated.c_str());
		}

		// Migration: the room-link shortcuts used to store a bare letter and hardcode Ctrl in
		// the polling code. Read literally, an existing "C" would now be a plain C - which is
		// also the default freeze-frame key. Re-add the Ctrl those settings always implied.
		if ((action == Hotkey_CopyLobbyLink || action == Hotkey_JoinLobbyLink) &&
			binding.source == HotkeyBinding::Source_Keyboard &&
			!binding.ctrl && !binding.shift && !binding.alt)
		{
			binding.ctrl = true;
			const std::string migrated = BindingToString(binding);
			Settings::changeSetting(kActions[action].iniKey, migrated);
			StoreSettingValueForAction(action, migrated);
			LOG(2, "[hotkeys] Migrated %s from '%s' to '%s'\n",
				kActions[action].iniKey, stored.c_str(), migrated.c_str());
		}

		g_actions[i].binding = binding;
		// A binding that is already held while settings are (re)loaded must not count as a
		// fresh press on the next frame.
		g_actions[i].down = false;
		g_actions[i].prevDown = false;
		g_actions[i].suppressedUntilRelease = true;
	}

	g_loaded = true;
	RefreshDeviceBindingFlag();
}

void HotkeyManager::Update()
{
	if (!g_loaded)
		ReloadFromSettings();

	if (g_anyDeviceBinding || g_capturing)
		InputDevices::Update(g_anyDirectInputBinding);

	const bool focused = IsGameWindowFocused();
	const bool typing = IsTypingInImGuiTextField();
	const float deltaTime = GetImGuiDeltaTime();

	for (int i = 0; i < Hotkey_Count; ++i)
	{
		ActionState& state = g_actions[i];
		const bool held = IsBindingHeld(state.binding);

		if (state.suppressedUntilRelease)
		{
			if (held)
			{
				state.prevDown = state.down = false;
				continue;
			}
			state.suppressedUntilRelease = false;
		}

		// Typing in an overlay text field must not fire keyboard hotkeys; a controller button
		// is never "typing", so device bindings ignore that gate (see utils.h).
		// Nothing fires while a rebind prompt is listening either, or assigning F1 would
		// toggle the menu shut in the middle of assigning it.
		const bool gated = !focused || g_capturing ||
			(typing && state.binding.source == HotkeyBinding::Source_Keyboard);

		state.prevDown = state.down;
		state.down = held && !gated;

		state.repeated = false;
		if (!state.down)
		{
			state.heldSeconds = 0.0f;
			continue;
		}

		const float previousHeld = state.heldSeconds;
		state.heldSeconds += deltaTime;
		if (previousHeld >= kRepeatDelaySeconds)
		{
			// Fire once per elapsed repeat interval, measured from the end of the initial
			// delay, so a slow frame does not shift the rhythm permanently.
			const float sincePrevious = previousHeld - kRepeatDelaySeconds;
			const float sinceCurrent = state.heldSeconds - kRepeatDelaySeconds;
			state.repeated = (int)(sinceCurrent / kRepeatRateSeconds) > (int)(sincePrevious / kRepeatRateSeconds);
		}
	}
}

bool HotkeyManager::WasPressed(Action action)
{
	if (action < 0 || action >= Hotkey_Count)
		return false;
	const ActionState& state = g_actions[action];
	return state.down && !state.prevDown;
}

bool HotkeyManager::WasPressedOrRepeated(Action action)
{
	if (action < 0 || action >= Hotkey_Count)
		return false;
	const ActionState& state = g_actions[action];
	return (state.down && !state.prevDown) || state.repeated;
}

bool HotkeyManager::ConsumePress(Action action)
{
	if (action < 0 || action >= Hotkey_Count)
		return false;

	ActionState& state = g_actions[action];
	if (!state.down || state.prevDown)
		return false;

	state.prevDown = true;
	return true;
}

bool HotkeyManager::IsDown(Action action)
{
	if (action < 0 || action >= Hotkey_Count)
		return false;
	return g_actions[action].down;
}

const HotkeyBinding& HotkeyManager::GetBinding(Action action)
{
	static const HotkeyBinding empty;
	if (action < 0 || action >= Hotkey_Count)
		return empty;
	return g_actions[action].binding;
}

void HotkeyManager::SetBinding(Action action, const HotkeyBinding& binding)
{
	if (action < 0 || action >= Hotkey_Count)
		return;

	const std::string text = BindingToString(binding);
	Settings::changeSetting(kActions[action].iniKey, text);
	StoreSettingValueForAction(action, text);

	g_actions[action].binding = binding;
	g_actions[action].down = false;
	g_actions[action].prevDown = false;
	// The button the user just pressed to assign this is still held. Without this the bind
	// would fire immediately on the very press that created it.
	g_actions[action].suppressedUntilRelease = true;
	RefreshDeviceBindingFlag();

	LOG(2, "[hotkeys] %s bound to '%s'\n", kActions[action].iniKey, text.c_str());
}

void HotkeyManager::ClearBinding(Action action)
{
	SetBinding(action, HotkeyBinding());
}

void HotkeyManager::ResetToDefault(Action action)
{
	if (action < 0 || action >= Hotkey_Count)
		return;
	SetBinding(action, BindingFromString(kActions[action].defaultBinding));
}

void HotkeyManager::ResetAllToDefaults()
{
	for (int i = 0; i < Hotkey_Count; ++i)
		ResetToDefault((Action)i);
}

const char* HotkeyManager::IniKey(Action action)
{
	return (action >= 0 && action < Hotkey_Count) ? kActions[action].iniKey : "";
}

const char* HotkeyManager::DisplayName(Action action)
{
	return (action >= 0 && action < Hotkey_Count) ? kActions[action].displayName : "";
}

const char* HotkeyManager::Tooltip(Action action)
{
	return (action >= 0 && action < Hotkey_Count) ? kActions[action].tooltip : "";
}

const char* HotkeyManager::DefaultBindingString(Action action)
{
	return (action >= 0 && action < Hotkey_Count) ? kActions[action].defaultBinding : "";
}

HotkeyManager::Action HotkeyManager::ActionFromIniKey(const char* iniKey)
{
	if (!iniKey)
		return Hotkey_Count;
	for (int i = 0; i < Hotkey_Count; ++i)
		if (_stricmp(kActions[i].iniKey, iniKey) == 0)
			return (Action)i;
	return Hotkey_Count;
}

bool HotkeyManager::BindingsEqual(const HotkeyBinding& a, const HotkeyBinding& b)
{
	if (!a.IsBound() || a.source != b.source)
		return false;

	if (a.source == HotkeyBinding::Source_Keyboard)
		return a.virtualKey == b.virtualKey && a.ctrl == b.ctrl &&
			a.shift == b.shift && a.alt == b.alt;

	return a.control == b.control && a.deviceId == b.deviceId;
}

bool HotkeyManager::CanShareBinding(Action a, Action b)
{
	if (a == b)
		return true;

	// Unlimited Playback's trigger and loop hotkeys. The Key Press trigger only acts while
	// that trigger type is the selected one, and the loop hotkey only while the On Loop type
	// is; they are two modes of the same feature, never both live, so putting them on one
	// button is a normal way to set it up.
	static const Action kCompatiblePairs[][2] = {
		{ Hotkey_UnlimitedPlaybackTrigger, Hotkey_UnlimitedPlaybackLoop },
	};

	for (const auto& pair : kCompatiblePairs)
	{
		if ((pair[0] == a && pair[1] == b) || (pair[0] == b && pair[1] == a))
			return true;
	}

	return false;
}

HotkeyManager::Action HotkeyManager::FindConflict(const HotkeyBinding& binding, Action ignore)
{
	if (!binding.IsBound())
		return Hotkey_Count;

	for (int i = 0; i < Hotkey_Count; ++i)
	{
		if (i == ignore || CanShareBinding(ignore, (Action)i))
			continue;
		if (BindingsEqual(binding, g_actions[i].binding))
			return (Action)i;
	}

	return Hotkey_Count;
}

bool HotkeyManager::IsControllerBinding(const HotkeyBinding& binding)
{
	return binding.source == HotkeyBinding::Source_Device;
}

void HotkeyManager::BeginCapture()
{
	g_capturing = true;

	// Baseline whatever is already held so a key the user happens to be resting on is not
	// mistaken for their choice.
	for (int vk = 0; vk < 256; ++vk)
		g_captureKeyBaseline[vk] = IsKeyHeld(vk);

	InputDevices::BeginCapture();
}

void HotkeyManager::CancelCapture()
{
	g_capturing = false;
	InputDevices::EndCapture();
}

bool HotkeyManager::IsCapturing()
{
	return g_capturing;
}

bool HotkeyManager::PollCapture(HotkeyBinding& outBinding)
{
	if (!g_capturing)
		return false;

	if (IsKeyHeld(VK_ESCAPE) && !g_captureKeyBaseline[VK_ESCAPE])
	{
		CancelCapture();
		return false;
	}

	std::string deviceId;
	int control = 0;
	if (InputDevices::PollCapture(deviceId, control))
	{
		outBinding = HotkeyBinding();
		outBinding.source = HotkeyBinding::Source_Device;
		outBinding.deviceId = deviceId;
		outBinding.control = control;
		g_capturing = false;
		return true;
	}

	for (int vk = 1; vk < 256; ++vk)
	{
		// Mouse buttons would make the bind button itself a binding.
		if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
			vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
			continue;

		// A modifier held alone is a modifier, not the key being bound - otherwise reaching
		// for Ctrl+Shift+P would bind Ctrl the moment it went down.
		if (IsModifierVirtualKey(vk))
			continue;

		if (!IsKeyHeld(vk) || g_captureKeyBaseline[vk])
			continue;

		outBinding = HotkeyBinding();
		outBinding.source = HotkeyBinding::Source_Keyboard;
		outBinding.virtualKey = vk;
		outBinding.ctrl = IsKeyHeld(VK_CONTROL);
		outBinding.shift = IsKeyHeld(VK_SHIFT);
		outBinding.alt = IsKeyHeld(VK_MENU);
		g_capturing = false;
		InputDevices::EndCapture();
		return true;
	}

	// Keys released since capture started become available again, so a user who was holding
	// Ctrl when they clicked "Rebind" can still bind Ctrl+something afterwards.
	for (int vk = 0; vk < 256; ++vk)
		if (g_captureKeyBaseline[vk] && !IsKeyHeld(vk))
			g_captureKeyBaseline[vk] = false;

	return false;
}
