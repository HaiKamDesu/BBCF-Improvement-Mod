#pragma once

// Shared scaffolding for the mod menu.
//
// The menu used to be two flat lists of collapsing headers (the F1 window and the separate
// "States" window), which meant a newcomer had to already know a feature existed before they
// could find it. Everything now lives on one of a handful of pages.
//
// Every entry a page can show is declared once in the table in MainMenuNav.cpp, so the search
// box and the pages can never drift apart. An entry is one of three weights:
//
//   Item_Section  a collapsing header. Only for blocks big or noisy enough that you want them
//                 shut by default (the dummy's move list, the replay browser).
//   Item_Group    a SeparatorText heading, always visible. The default for anything that just
//                 needs a name over it - no click required to see what is in there.
//   Item_Loose    a single control or a row of buttons with no heading of its own, sitting
//                 loose at the top or bottom of a page. Still indexed, so search finds it.
//
// Reach for Item_Loose first. A category that exists only to hold one button is the problem
// this reorganisation is trying to fix, not the shape of the fix.

#include <string>

class WindowContainer;
class PalettesConfigWindow;

namespace MainMenu
{
	enum PageId
	{
		Page_Game,
		Page_Training,
		Page_Overlays,
		Page_Online,
		Page_Replays,
		Page_LookAndSound,
		Page_Controllers,
		Page_Count
	};

	enum EntryId
	{
		Game_Money,
		Game_CurrentMatch,
		Game_Mode,
		Game_Stage,
		Game_Hud,

		Training_Positions,
		Training_Dummy,
		Training_Slots,
		Training_SaveStates,
		Training_Wakeup,
		Training_Tas,

		Overlays_Hitboxes,
		Overlays_FrameStep,
		Overlays_FrameAdvantage,
		Overlays_FrameHistory,
		Overlays_InputDisplay,
		Overlays_ComboData,

		Online_Window,
		Online_Ranked,
		Online_RoomSettings,
		Online_Avatar,
		Online_ForeignPalettes,

		Replays_Rewind,
		Replays_Files,
		Replays_Takeover,
		Replays_Database,

		Look_Palettes,
		Look_Music,

		Controllers_Setup,

		Entry_Count
	};

	enum EntryKind
	{
		Item_Section,
		Item_Group,
		Item_Loose,
	};

	struct EntryInfo
	{
		PageId page;
		EntryKind kind;
		const char* label;    // English source string; also the localization key
		const char* keywords; // extra English search terms, including the pre-8.5 menu names
	};

	struct PageInfo
	{
		const char* label;
		const char* blurb;    // one line under the page title, so a page explains itself
		const char* keywords;
	};

	const PageInfo& GetPage(PageId page);
	const EntryInfo& GetEntry(EntryId entry);

	// Everything the page bodies need in order to reach the rest of the overlay. The two
	// window pointers are modal helpers owned by MainWindow rather than by WindowContainer.
	struct PageContext
	{
		WindowContainer* container = nullptr;
		PalettesConfigWindow* palettesConfigWindow = nullptr;
	};

	// A collapsing header for a declared Item_Section. Pass available=false when nothing
	// inside it can be used right now: the heading greys out, but it is never hidden, and it
	// still opens - people have to be able to find out a feature exists before they can want
	// it.
	bool BeginSection(EntryId entry, bool available = true);

	// An always-visible heading for a declared Item_Group.
	void GroupLabel(EntryId entry, bool available = true);

	// Scroll target for a declared Item_Loose, called immediately before the control. Draws
	// nothing.
	void Anchor(EntryId entry);

	// Dim, wrapped explanation of why the surrounding controls do nothing right now.
	void Unavailable(const std::string& reason);

	// Dim, wrapped supporting text (what a section is for, where a thing ended up).
	void Hint(const std::string& text);

	// Registers the handler that remembers which page the menu was left on. Must run
	// before the first frame, which is when ImGui parses its ini.
	void RegisterLayoutSettings();

	PageId CurrentPage();
	void GoToPage(PageId page);
	void GoToEntry(EntryId entry);
}
