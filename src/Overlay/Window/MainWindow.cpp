#include "MainWindow.h"

#include "MainMenu/MainMenuPages.h"

#include "Core/HotkeyManager.h"
#include "Core/Settings.h"
#include "Core/logger.h"
#include "Core/info.h"
#include "Core/interfaces.h"
#include "Core/utils.h"
#include "Core/Localization.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "SteamApiWrapper/SteamApiHelper.h"

#include <Windows.h>

#include "imgui_internal.h"

#include <string>
#include <vector>

namespace
{
	constexpr float kNavWidth = 164.0f;
}

MainWindow::MainWindow(const std::string& windowTitle, bool windowClosable, WindowContainer& windowContainer, ImGuiWindowFlags windowFlags)
	: IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer)
{
	m_windowTitle = MOD_WINDOW_TITLE;
	m_windowTitle += " ";
	m_windowTitle += MOD_VERSION_NUM;

#ifdef _DEBUG
	m_windowTitle += " (DEBUG)";
#endif

	m_windowTitle += "###MainTitle"; // Set unique identifier
}

void MainWindow::BeforeDraw()
{
	ImGui::SetWindowPos(m_windowTitle.c_str(), ImVec2(12, 20), ImGuiCond_FirstUseEver);

	// The menu is a two-pane layout now, so it needs a real size instead of auto-fitting to
	// whichever section happened to be expanded. 'menusize' still picks how roomy it starts.
	ImVec2 defaultSize;
	switch (Settings::settingsIni.menusize)
	{
	case 1:
		defaultSize = ImVec2(580, 420);
		break;
	case 3:
		defaultSize = ImVec2(780, 580);
		break;
	default:
		defaultSize = ImVec2(680, 500);
	}

	ImGui::SetNextWindowSize(defaultSize, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(480, 320), ImVec2(1600, 1200));
}

MainMenu::PageContext MainWindow::MakePageContext()
{
	MainMenu::PageContext ctx;
	ctx.container = m_pWindowContainer;
	ctx.settingsIniWindow = &m_settingsIniWindow;
	ctx.palettesConfigWindow = &m_palettesConfigWindow;
	return ctx;
}

void MainWindow::Draw()
{
	DrawSearchBox();
	ImGui::VerticalSpacing(2);

	// The player count and the community links live outside the paging entirely: they are the
	// two things you might want without having gone looking for anything, so they stay put no
	// matter which page is open.
	const float footerHeight = ImGui::GetFrameHeightWithSpacing()
		+ ImGui::GetTextLineHeightWithSpacing()
		+ ImGui::GetStyle().ItemSpacing.y * 3.0f;

	DrawNav(kNavWidth, -footerHeight);
	ImGui::SameLine();

	ImGui::BeginChild("##mainmenu_content", ImVec2(0, -footerHeight), false);
	{
		if (m_featureFilter.IsActive())
		{
			DrawSearchResults();
		}
		else
		{
			const MainMenu::PageId page = MainMenu::CurrentPage();
			const MainMenu::PageInfo& info = MainMenu::GetPage(page);

			ImGui::TextUnformatted(L(info.label).c_str());
			ImGui::TextDisabledWrapped("%s", L(info.blurb).c_str());
			ImGui::VerticalSpacing(2);
			ImGui::Separator();
			ImGui::VerticalSpacing(4);

			MainMenu::PageContext ctx = MakePageContext();
			MainMenu::DrawPage(page, ctx);
		}
	}
	ImGui::EndChild();

	DrawFooter();
}

void MainWindow::DrawSearchBox()
{
	m_featureFilter.Draw("##feature_search", -1.0f);

	if (!m_featureFilter.IsActive())
	{
		// ImGuiTextFilter has no placeholder support, so the hint is painted over the empty
		// box - it is the one line that tells a new user this box exists at all.
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 padding = ImGui::GetStyle().FramePadding;
		ImGui::GetWindowDrawList()->AddText(
			ImVec2(min.x + padding.x, min.y + padding.y),
			ImGui::GetColorU32(ImGuiCol_TextDisabled),
			L("Search every feature in the mod...").c_str());
	}
}

void MainWindow::DrawNav(float width, float height)
{
	ImGui::BeginChild("##mainmenu_nav", ImVec2(width, height), true);

	ImGui::SeparatorText(L("Pages").c_str());

	for (int i = 0; i < MainMenu::Page_Count; ++i)
	{
		const MainMenu::PageId page = static_cast<MainMenu::PageId>(i);

		// Pages are never dimmed. A page is a place, not a feature: greying one out because
		// you happen to be in the wrong game mode only hides where things live. Whatever
		// inside it cannot be used right now says so for itself.
		if (ImGui::Selectable(L(MainMenu::GetPage(page).label).c_str(), MainMenu::CurrentPage() == page))
		{
			MainMenu::GoToPage(page);
			m_featureFilter.Clear();
		}
	}

	ImGui::VerticalSpacing(6);
	DrawNavHotkeyHints();

	ImGui::EndChild();
}

void MainWindow::DrawNavHotkeyHints() const
{
	struct Hint { HotkeyManager::Action action; const char* what; };
	static const Hint kHints[] = {
		{ HotkeyManager::Hotkey_ToggleMainWindow,   "this menu" },
		{ HotkeyManager::Hotkey_ToggleOnlineWindow, "online info" },
		{ HotkeyManager::Hotkey_ToggleHud,          "hide the HUD" },
	};

	ImGui::SeparatorText(L("Keys").c_str());

	for (const Hint& hint : kHints)
	{
		ImGui::TextDisabled("%s",
			(HotkeyManager::DisplayString(HotkeyManager::GetBinding(hint.action))
				+ "  " + L(hint.what)).c_str());
	}
}

void MainWindow::DrawFooter()
{
	ImGui::Separator();

	ImGui::TextUnformatted(Messages.Current_online_players());
	ImGui::SameLine();
	const std::string playerCount = g_interfaces.pSteamApiHelper
		? g_interfaces.pSteamApiHelper->GetCurrentPlayersCountString()
		: Messages.No_data();
	ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", playerCount.c_str());

	ImGui::ButtonUrl(Messages.Discord(), MOD_LINK_DISCORD);
	ImGui::SameLine();
	ImGui::ButtonUrl(Messages.Forum(), MOD_LINK_FORUM);
	ImGui::SameLine();
	ImGui::ButtonUrl(Messages.GitHub(), MOD_LINK_GITHUB);
}

void MainWindow::DrawSearchResults()
{
	ImGui::TextUnformatted(L("Search results").c_str());
	ImGui::TextDisabledWrapped("%s", L("Pick one to jump straight to it.").c_str());
	ImGui::VerticalSpacing(2);
	ImGui::Separator();
	ImGui::VerticalSpacing(4);

	int matches = 0;

	// Whole pages match too, so searching "controller" or "replay" offers the place as well as
	// the individual controls in it.
	for (int i = 0; i < MainMenu::Page_Count; ++i)
	{
		const MainMenu::PageId page = static_cast<MainMenu::PageId>(i);
		const MainMenu::PageInfo& info = MainMenu::GetPage(page);
		const std::string label = L(info.label);

		if (!m_featureFilter.PassFilter(info.label)
			&& !m_featureFilter.PassFilter(label.c_str())
			&& !m_featureFilter.PassFilter(info.keywords))
			continue;

		++matches;
		ImGui::PushID(0x1000 + i);
		if (ImGui::Selectable((label + "  ").c_str()))
		{
			MainMenu::GoToPage(page);
			m_featureFilter.Clear();
		}
		ImGui::PopID();
	}

	for (int i = 0; i < MainMenu::Entry_Count; ++i)
	{
		const MainMenu::EntryId entry = static_cast<MainMenu::EntryId>(i);
		const MainMenu::EntryInfo& info = MainMenu::GetEntry(entry);
		const MainMenu::PageInfo& page = MainMenu::GetPage(info.page);

		// Matched against the English source text, its translation, and a keyword list that
		// deliberately includes the pre-8.5 names ("states", "framedata", "scr") so muscle
		// memory still lands somewhere useful.
		const std::string label = L(info.label);
		const bool hit = m_featureFilter.PassFilter(info.label)
			|| m_featureFilter.PassFilter(label.c_str())
			|| m_featureFilter.PassFilter(info.keywords)
			|| m_featureFilter.PassFilter(page.label);

		if (!hit)
			continue;

		++matches;
		// Labelled "Page > Entry" so the result also teaches where the feature lives - next
		// time, the user goes straight there without searching.
		const std::string text = L(page.label) + " > " + label;
		ImGui::PushID(i);
		if (ImGui::Selectable(text.c_str()))
		{
			MainMenu::GoToEntry(entry);
			m_featureFilter.Clear();
		}
		ImGui::PopID();
	}

	if (matches == 0)
	{
		ImGui::VerticalSpacing(6);
		ImGui::TextDisabledWrapped("%s", L("Nothing matched. Try a shorter word, or browse the list on the left.").c_str());
	}
}
