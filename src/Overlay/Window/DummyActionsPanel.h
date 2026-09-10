#pragma once

// The training page's "Dummy actions" section.
//
// One row per armed trigger, and a "+ Add Action" button offering the triggers that are still
// free. Adding is two decisions: which trigger, then where its inputs come from - so the menu
// is a trigger list of source submenus, and picking a pair opens that source's own modal.
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
