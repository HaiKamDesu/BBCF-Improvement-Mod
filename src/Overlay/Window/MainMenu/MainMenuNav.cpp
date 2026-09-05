#include "MainMenuNav.h"

#include "imgui_internal.h"

#include "Core/Localization.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"

namespace MainMenu
{
	namespace
	{
		const PageInfo kPages[Page_Count] = {
			{ "Game",         "The match you are in, and the game's own values.",             "match versus stage hud money" },
			{ "Training",     "Lab tools: what the dummy does, recordings, and save states.",  "training lab dummy practice" },
			{ "Overlays",     "Extra information drawn on top of the game while you play.",    "overlay hud display boxes frames" },
			{ "Online",       "Rooms, ranked, and everything that only matters with people.",  "netplay lobby room ranked online" },
			{ "Replays",      "Watching, rewinding, taking over and sharing replays.",         "replay theater rewind takeover" },
			{ "Look & Sound", "Palettes and music - how the game looks and what it plays.",    "palette colour color music bgm" },
			{ "Controllers",  "Which device each side plays on, and how the mod reads them.",  "controller gamepad pad keyboard input device" },
		};

		const EntryInfo kEntries[Entry_Count] = {
			{ Page_Game, Item_Loose,   "Player money (P$)",     "money cash p$ unlock gallery shop buy" },
			{ Page_Game, Item_Group,   "Current match",         "match round versus" },
			{ Page_Game, Item_Loose,   "Game mode",             "versus training switch change gameplay settings" },
			{ Page_Game, Item_Loose,   "Stage",                 "background level stage select" },
			{ Page_Game, Item_Loose,   "Hide the game HUD",     "hud health bar timer meter clean screenshot recording" },

			{ Page_Training, Item_Loose,   "Character positions", "swap coordinates sides corner switch position" },
			{ Page_Training, Item_Section, "Dummy actions",       "states scr script reversal wakeup gap on hit throw tech ai action" },
			{ Page_Training, Item_Section, "Recording slots",     "playback slot record loop unlimited playback editor" },
			{ Page_Training, Item_Group,   "Save states",         "snapshot save load state situation reset drill" },
			{ Page_Training, Item_Group,   "Wake-up timing",      "wakeup delay emergency tech roll okizeme knockdown" },
			{ Page_Training, Item_Loose,   "TAS combo editor",    "tas frame by frame combo editor rewind record movie" },

			{ Page_Overlays, Item_Section, "Hitboxes",            "hitbox hurtbox collision throw range origin box overlay" },
			{ Page_Overlays, Item_Group,   "Freeze & frame step", "freeze pause frame step advance counter slow motion" },
			{ Page_Overlays, Item_Group,   "Frame advantage",     "framedata frame advantage plus minus stagger" },
			{ Page_Overlays, Item_Group,   "Frame history",       "framehistory startup active recovery timeline bar" },
			{ Page_Overlays, Item_Group,   "HP numbers",          "hp number health bar damage abyss figure training" },
			{ Page_Overlays, Item_Loose,   "Input display",       "input buffer p1 p2 inputs notation display" },
			{ Page_Overlays, Item_Loose,   "Combo data",          "combo damage proration counter hits" },

			{ Page_Online, Item_Loose,   "Open the online window",              "room window who is in the room players list spectator" },
			{ Page_Online, Item_Section, "Ranked",                              "ranked lp rank ladder leaderboard prediction square color progress" },
			{ Page_Online, Item_Group,   "Room settings",                       "rematch ft2 ft3 ft5 ft10 room host" },
			{ Page_Online, Item_Group,   "Lobby avatar",                        "avatar icon color accessory lobby" },
			{ Page_Online, Item_Loose,   "Load other players' custom palettes", "foreign palettes crash ranked stability stopgap" },

			{ Page_Replays, Item_Loose,   "Rewind",           "rewind replay theater scrub back" },
			{ Page_Replays, Item_Section, "Replay files",     "local replays load archive replay theater database download" },
			{ Page_Replays, Item_Section, "Replay takeover",  "takeover urt unlimited replay state p1 p2" },
			{ Page_Replays, Item_Loose,   "Replay database",  "upload replay db website share" },

			{ Page_LookAndSound, Item_Group, "Palettes", "palette colour color custom editor import share hpl" },
			{ Page_LookAndSound, Item_Group, "Music",    "music bgm jukebox song track replace playlist astral" },

			{ Page_Controllers, Item_Loose, "Controller setup", "controller gamepad pad keyboard steam input separation override refresh" },
		};

		PageId g_currentPage = Page_Game;

		// Which page the menu was left on, kept in ImGui's own menus.ini next to the
		// window sizes and positions - it is the same kind of "where things were" state,
		// and putting it in settings.ini would add a row nobody wants to edit by hand.
		//
		// ImGui parses that file on the first frame, so the handler has to be registered
		// during init; one added later never sees its own lines.
		void* MenuLayout_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
		{
			return strcmp(name, "Layout") == 0 ? (void*)1 : nullptr;
		}

		void MenuLayout_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
		{
			int page = 0;
			if (sscanf_s(line, "Page=%d", &page) == 1 && page >= 0 && page < Page_Count)
				g_currentPage = (PageId)page;
		}

		void MenuLayout_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
		{
			buf->appendf("[%s][Layout]\n", handler->TypeName);
			buf->appendf("Page=%d\n\n", (int)g_currentPage);
		}
		EntryId g_pendingFocus = Entry_Count;

		// True on the one frame the search sent the user to this entry. Consumes the request,
		// so an entry that is drawn twice cannot swallow it from the real one.
		bool TakeFocus(EntryId entry)
		{
			if (g_pendingFocus != entry)
				return false;

			g_pendingFocus = Entry_Count;
			return true;
		}

		void ScrollHere()
		{
			// 0.25 rather than 0.0: leaving a little above the target reads as "here it is"
			// instead of "the list jumped".
			ImGui::SetScrollHereY(0.25f);
		}
	}

	const PageInfo& GetPage(PageId page)
	{
		return kPages[page < Page_Count ? page : 0];
	}

	const EntryInfo& GetEntry(EntryId entry)
	{
		return kEntries[entry < Entry_Count ? entry : 0];
	}

	bool BeginSection(EntryId entry, bool available)
	{
		const EntryInfo& info = GetEntry(entry);
		const bool focused = TakeFocus(entry);

		if (focused)
			ImGui::SetNextItemOpen(true);

		if (!available)
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

		ImGui::PushID(static_cast<int>(entry));
		const bool open = ImGui::CollapsingHeader(L(info.label).c_str());
		ImGui::PopID();

		if (!available)
			ImGui::PopStyleColor();

		if (focused)
			ScrollHere();

		if (open)
			ImGui::VerticalSpacing(2);

		return open;
	}

	void GroupLabel(EntryId entry, bool available)
	{
		const EntryInfo& info = GetEntry(entry);
		const bool focused = TakeFocus(entry);

		if (!available)
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

		ImGui::SeparatorText(L(info.label).c_str());

		if (!available)
			ImGui::PopStyleColor();

		if (focused)
			ScrollHere();
	}

	void Anchor(EntryId entry)
	{
		if (TakeFocus(entry))
			ScrollHere();
	}

	void Unavailable(const std::string& reason)
	{
		ImGui::HorizontalSpacing();
		ImGui::TextDisabledWrapped("%s", reason.c_str());
	}

	void Hint(const std::string& text)
	{
		ImGui::HorizontalSpacing();
		ImGui::TextDisabledWrapped("%s", text.c_str());
	}

	void RegisterLayoutSettings()
	{
		// AddSettingsHandler asserts on a duplicate type, and init can run again after a
		// device loss.
		if (ImGui::FindSettingsHandler("BBCFIMMenu"))
			return;

		ImGuiSettingsHandler handler;
		handler.TypeName = "BBCFIMMenu";
		handler.TypeHash = ImHashStr("BBCFIMMenu");
		handler.ReadOpenFn = MenuLayout_ReadOpen;
		handler.ReadLineFn = MenuLayout_ReadLine;
		handler.WriteAllFn = MenuLayout_WriteAll;
		ImGui::AddSettingsHandler(&handler);
	}

	PageId CurrentPage()
	{
		return g_currentPage;
	}

	void GoToPage(PageId page)
	{
		if (g_currentPage == page)
			return;
		g_currentPage = page;
		// Remember it for next session. Marking here rather than on every frame means the
		// ini is only rewritten when the page actually changes.
		ImGui::MarkIniSettingsDirty();
	}

	void GoToEntry(EntryId entry)
	{
		g_currentPage = GetEntry(entry).page;
		g_pendingFocus = entry;
	}
}
