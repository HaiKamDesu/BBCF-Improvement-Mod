#pragma once

// The training page's "Dummy actions" section.
//
// One block per trigger that holds anything: the trigger row (its on/off switch, name, timing,
// and settings / add / remove buttons), then one indented line per action in it, each with its
// own switch once there is more than one, and play / edit / remove buttons. Adding is two
// decisions: which trigger, then where the inputs come from - so the "+ Add Action" menu is a
// trigger list of source submenus, and a trigger row's "+" skips the first decision.
//
// The rows are the whole point of the layout: what the dummy will do, and when, without
// opening anything. See docs/DummyActionsRework.md.
namespace DummyActionsPanel
{
	// Rows, the add button, and whichever source modal is open. Drawn by the Training page.
	void Draw();

	// The dummy's script was re-parsed, so any animation an action holds is pointing into
	// memory for a character that is no longer loaded.
	void OnDummyScriptReloaded();
}
