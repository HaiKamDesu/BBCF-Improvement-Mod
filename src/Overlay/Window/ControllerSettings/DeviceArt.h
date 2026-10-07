#pragma once

// Vector drawings for the Controllers page: a small icon per device type, the game-input
// display (direction gate + A/B/C/D, like the in-game training input display), and a picture
// of the device itself with every control lit while it is held and labelled with the game
// button it sends. Drawn with ImDrawList so nothing has to ship as an image.

#include "Input/LocalInputManager.h"

#include "imgui.h"

namespace DeviceArt
{
	enum class IconStyle
	{
		Keyboard,
		XboxPad,
		ArcadeStick,
		GenericPad,
	};

	IconStyle StyleFor(LocalInput::DeviceKind kind, const std::string& name);

	// Draws into the current window at `pos`, `height` tall (width = height * 1.6).
	void DrawIcon(ImDrawList* draw, ImVec2 pos, float height, IconStyle style, ImU32 color);

	// What the game sees: a direction gate with the numpad notation and lit A B C D, then the
	// other buttons as chips. Reserves its own space (ImGui::Dummy).
	void DrawGameInput(uint32_t buttons, float height);

	// The device with its controls lit, sized to `width`. Reserves its own space.
	void DrawDevice(const LocalInput::DeviceView& device, const LocalInput::Mapping& mapping, float width);

	// The accent used for activity (a lit tile, a lit header dot, the selected side).
	ImU32 PlayerColor(LocalInput::Side side, float alpha = 1.0f);
}
