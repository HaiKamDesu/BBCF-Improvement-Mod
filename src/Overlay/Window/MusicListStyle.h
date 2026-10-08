#pragma once
#include "imgui.h"

#include <string>

// The look the two music windows share - the Jukebox's track list and the music
// replacement list are the same kind of list (the game's tracks, grouped by category, a
// checkbox per row), so they are drawn by the same pieces and read as one design.
namespace MusicListStyle
{
	// One colour per track category (btl red, vs blue, boss purple, ...).
	ImVec4 CategoryColor(const std::string& category);

	struct HeaderResult
	{
		bool checkboxClicked = false;
		bool checkTo = false;      // the state the user asked for
		bool open = true;          // whether the category is expanded now
	};

	// A category's header row: a checkbox for the whole category (checkState 1 all on,
	// 0 all off, -1 mixed - drawn as a dash), then the coloured collapsing header.
	// `open` is the state to draw it in; the result says what the user did.
	HeaderResult CategoryHeader(const std::string& category, int checkState, bool checkboxEnabled,
		bool open, const char* checkboxTip);

	// The same header with no checkbox, for lists that only pick from the categories.
	// Returns whether it is expanded now.
	bool CategoryHeaderPlain(const std::string& category, bool open);

	// The small per-row volume button: "vol" at 0 dB, the offset otherwise.
	bool VolumeButton(float db, const char* id);

	// A borderless "label  v" button that opens a menu or picker, the width given.
	bool DropdownButton(const char* label, const char* id, float width, const ImVec4& textColor);

	// Opens the next popup under the item just drawn, like a dropdown, rather than at the
	// mouse. Call right after the button that opened it.
	void AnchorNextPopupBelowLastItem();

	// The modal look the music windows use: fully opaque over the slightly see-through
	// mod windows, so nothing behind shows through a dialog's text.
	struct OpaquePopup
	{
		OpaquePopup();
		~OpaquePopup();
	};
}
