#pragma once

// The local input system's controls page: two boxes, Player 1 and Player 2, listing every
// keyboard and pad that plays for that side. Drag a device to the other box to move it; each
// row lights up while that device is being used; right-click or Configure to rename it, see
// its live input, and remap it. See Input/LocalInputManager.h.
namespace LocalInputPanel
{
	void Draw();
}
