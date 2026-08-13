#pragma once

/*
	Every rebindable hotkey in the mod, in one place.

	Before this existed each feature parsed its own key name out of settings.ini and polled
	it its own way (some through ImGui, some through GetAsyncKeyState, one with a hardcoded
	Ctrl), so "which key does what" was spread across six files, modifiers were impossible,
	and a controller button could not be bound at all.

	Now: one action list (hotkeys.def), one binding type, one poll site
	(WindowManager::HandleButtons -> HotkeyManager::Update), one edge-detection rule.

	A binding is either
	  - a keyboard key with optional Ctrl/Shift/Alt modifiers ("F5", "Ctrl+Shift+P"), or
	  - one button/hat direction/axis half on one controller ("Pad:XI0:A"),
	and modifiers must match EXACTLY, so a plain "C" binding no longer fires when the user
	presses Ctrl+C.

	Storage stays in settings.def / settings.ini: each action names the std::string setting
	it lives in, so saving, loading and the Settings window's draft/cancel machinery all keep
	working unchanged.
*/

#include <string>

struct HotkeyBinding
{
	enum Source
	{
		Source_None,
		Source_Keyboard,
		Source_Device,
	};

	Source source = Source_None;

	// Keyboard bindings.
	bool ctrl = false;
	bool shift = false;
	bool alt = false;
	int virtualKey = 0;

	// Controller bindings. See InputDevices.h for the id and control-code formats.
	std::string deviceId;
	int control = 0;

	bool IsBound() const { return source != Source_None; }
};

namespace HotkeyManager
{
	enum Action
	{
#define HOTKEY(_id, _var, _ini, _default, _display, _tooltip) Hotkey_##_id,
#include "hotkeys.def"
#undef HOTKEY
		Hotkey_Count
	};

	// Polls devices and recomputes press edges. Call exactly once per rendered frame.
	void Update();

	// True on the frame the binding went from released to pressed / while it is held.
	// Both return false when nothing is bound, when the game window is not focused, and -
	// for keyboard bindings only - while an overlay text field has keyboard focus.
	bool WasPressed(Action action);
	bool IsDown(Action action);
	// As WasPressed, but also fires repeatedly while the binding is held down, at the same
	// delay/rate as a held key in a text field. For hotkeys you hold to keep doing something
	// (stepping frames one at a time).
	bool WasPressedOrRepeated(Action action);
	// WasPressed, but clears the edge so a second call this frame returns false. For callers
	// driven by a game hook rather than the render loop, which can tick more than once
	// between Update() calls and would otherwise act on the same press twice.
	bool ConsumePress(Action action);

	const HotkeyBinding& GetBinding(Action action);
	// Assigns and persists to settings.ini. Also suppresses the action until every part of
	// the new binding is released, so the keypress that assigned it does not also fire it.
	void SetBinding(Action action, const HotkeyBinding& binding);
	void ClearBinding(Action action);
	void ResetToDefault(Action action);
	void ResetAllToDefaults();

	// Re-reads every binding from Settings::settingsIni. Called at startup and after the
	// Settings window saves.
	void ReloadFromSettings();

	const char* IniKey(Action action);
	const char* DisplayName(Action action);
	const char* Tooltip(Action action);
	const char* DefaultBindingString(Action action);

	// Returns Hotkey_Count when the ini key is not a hotkey action.
	Action ActionFromIniKey(const char* iniKey);

	// "Ctrl+Shift+F5", "Xbox pad 1: Pad RB", "Not bound".
	std::string DisplayString(const HotkeyBinding& binding);
	std::string BindingToString(const HotkeyBinding& binding);
	HotkeyBinding BindingFromString(const std::string& text);

	// Another action already using this exact binding, or Hotkey_Count if it is free.
	// Actions that CanShareBinding are not reported.
	Action FindConflict(const HotkeyBinding& binding, Action ignore);

	// True for pairs of actions that are fine on the same key because only one of them can
	// ever act on a given press - sharing a binding there is a deliberate setup, not a
	// collision, and warning about it would be noise.
	bool CanShareBinding(Action a, Action b);
	// Same key and same modifiers, or same control on the same device. Two unbound bindings
	// are never "equal" - nothing conflicts with nothing.
	bool BindingsEqual(const HotkeyBinding& a, const HotkeyBinding& b);

	// Rebind capture. BeginCapture() starts listening; PollCapture() returns true once the
	// user presses something. Escape cancels (reported via IsCapturing() going false with no
	// result). Modifier keys held on their own are treated as modifiers, not as the binding.
	void BeginCapture();
	void CancelCapture();
	bool IsCapturing();
	bool PollCapture(HotkeyBinding& outBinding);

	// True when this binding sits on a controller the player is likely using to play, so the
	// rebind UI can warn that it will also fire mid-match.
	bool IsControllerBinding(const HotkeyBinding& binding);
}
