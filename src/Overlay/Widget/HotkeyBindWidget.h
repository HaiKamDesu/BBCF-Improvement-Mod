#pragma once

/*
	The "click, then press what you want" rebind control, shared by every place that shows a
	hotkey. Draws:

		[ Ctrl+F5 ]  [Clear]  [Default]

	Clicking the first button turns it into "Press any key or button..." and the next thing
	the user presses - keyboard key with whatever modifiers they are holding, or a button /
	hat direction / stick direction on any controller - becomes the binding. Escape cancels.

	The widget only edits the HotkeyBinding it is handed. Persisting it is the caller's job,
	so the Settings window can keep its Cancel button honest by binding into its draft copy.
*/

#include "Core/HotkeyManager.h"

#include <string>

namespace ImGuiHotkey
{
	// Returns true on the frame `binding` changed. `defaultBindingText` is what the Default
	// button restores; `warning` (may be null) is shown under the row in amber, used for
	// "this is already used by X" and the controller-during-match caveat.
	bool BindWidget(const char* id, HotkeyBinding& binding, const char* defaultBindingText,
		const char* warning);

	// True while any bind widget is waiting for a key. Callers that draw other hotkey-driven
	// UI can use this to explain why nothing else is responding.
	bool IsAnyCapturing();
}
