#include "MainWindow.h"

#include "MainMenu/MainMenuPages.h"

#include "Overlay/Branding.h"
#include "Overlay/BrandingLayout.h"

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
#include "Updater/UpdateCoordinator.h"

#include <Windows.h>

#include "imgui_internal.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
	constexpr float kNavWidth = 164.0f;
}

MainWindow::MainWindow(const std::string& windowTitle, bool windowClosable, WindowContainer& windowContainer, ImGuiWindowFlags windowFlags)
	: IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer)
{
	// The title bar is drawn by hand in DrawTitleBar() so the two wordmarks can sit inside it,
	// which a plain ImGui title string cannot do. Everything before ### is what ImGui would
	// have drawn, so it stays empty; what follows is only the window's identity.
	// MOD_VERSION, not MOD_VERSION_NUM: the latter already expands to "v8.4 Oceanya Edition",
	// which is the very thing the wordmarks and the trailing "Edition" spell out.
	m_titleText = MOD_WINDOW_TITLE " ";
	m_titleText += MOD_VERSION;

#ifdef _DEBUG
	m_titleText += " (DEBUG)";
#endif

	m_windowTitle = "###MainTitle";

	// Collapsing a two-pane menu achieves nothing, and the hand-drawn title bar assumes the
	// window body is actually being laid out.
	SetWindowFlag(ImGuiWindowFlags_NoCollapse);
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

	// ImGui fixes the title bar's height inside Begin(), from FramePadding.y as it stands at
	// that moment - so the only way to make the bar taller is to push the padding here, before
	// IWindow calls Begin. Draw() pops it again straight away so the body keeps normal padding.
	const ImGuiStyle& style = ImGui::GetStyle();
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
		ImVec2(style.FramePadding.x,
			style.FramePadding.y + Branding::GetLayout().titleBarExtraPadding * 0.5f));
}

MainMenu::PageContext MainWindow::MakePageContext()
{
	MainMenu::PageContext ctx;
	ctx.container = m_pWindowContainer;
	ctx.palettesConfigWindow = &m_palettesConfigWindow;
	return ctx;
}

void MainWindow::Draw()
{
	// Balances the push in BeforeDraw(); Begin() has already used it for the title bar.
	ImGui::PopStyleVar();

	// Watermark first so the title bar's text and wordmarks stay crisp on top of it.
	DrawWatermark();
	DrawTitleBar();

	DrawSearchBox();
	ImGui::VerticalSpacing(2);

	// The player count, the settings door and the community links live outside the paging
	// entirely: they are what you might want without having gone looking for anything, so they
	// stay put no matter which page is open.
	//
	// Measured from the previous frame rather than computed: the footer row rewraps when the
	// menu is narrowed, so its height is not something this can work out up front. One frame
	// of lag while dragging the window edge is invisible.
	const float footerHeight = m_lastFooterHeight;

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

// The title bar ImGui would have drawn, plus the two wordmarks, painted directly into the
// title bar rect: "BBCF IM <version>  [OCEANYA] [Laboratories]  Edition".
void MainWindow::DrawTitleBar() const
{
	// Every measurement here comes from Branding::GetLayout() - see BrandingLayout.h.
	const Branding::Layout& layout = Branding::GetLayout();
	const ImGuiStyle& style = ImGui::GetStyle();
	ImDrawList* drawList = ImGui::GetWindowDrawList();

	const ImVec2 windowPos = ImGui::GetWindowPos();
	const float titleHeight = ImGui::GetWindowSize().y > 0.0f
		? ImGui::GetCurrentWindow()->TitleBarHeight
		: ImGui::GetFrameHeight();

	// The window's own clip rect stops at the content area, so the title bar has to be opened
	// up explicitly before anything can be painted into it.
	drawList->PushClipRect(windowPos,
		ImVec2(windowPos.x + ImGui::GetWindowWidth(), windowPos.y + titleHeight), false);

	const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
	const float fontSize = ImGui::GetFontSize();

	float x = windowPos.x + style.FramePadding.x + layout.titleTextOffsetX;
	const float textY = windowPos.y + (titleHeight - fontSize) * 0.5f + layout.titleTextOffsetY;

	drawList->AddText(ImVec2(x, textY), textColor, m_titleText.c_str());
	x += ImGui::CalcTextSize(m_titleText.c_str()).x + layout.gapAfterVersion;

	// Heights are fractions of the title bar, applied to the artwork's INKED box - the source
	// PNGs' transparent margins were measured at load and are cropped out by uv0/uv1, so a
	// fraction of 1.0 really does mean "as tall as the bar".
	//
	// The artwork is white, so the tint comes through as-is and the wordmarks pick up whatever
	// the theme's text colour is.
	struct Wordmark
	{
		const Branding::Logo* logo;
		float heightFraction;
		float offsetY;
		float gapAfter;
	};
	const Wordmark wordmarks[] = {
		{ &Branding::Oceanya(),      layout.oceanyaHeight,      layout.oceanyaOffsetY,      layout.gapAfterOceanya },
		{ &Branding::Laboratories(), layout.laboratoriesHeight, layout.laboratoriesOffsetY, layout.gapAfterLaboratories },
	};

	for (const Wordmark& wordmark : wordmarks)
	{
		const Branding::Logo* logo = wordmark.logo;
		if (!logo->IsValid())
			continue;

		const float logoHeight = titleHeight * wordmark.heightFraction;
		const float logoWidth = logoHeight * logo->Aspect();
		const float logoTop = windowPos.y + (titleHeight - logoHeight) * 0.5f + wordmark.offsetY;

		drawList->AddImage(logo->texture,
			ImVec2(x, logoTop), ImVec2(x + logoWidth, logoTop + logoHeight),
			logo->uv0, logo->uv1, textColor);
		x += logoWidth + wordmark.gapAfter;
	}

	if (layout.showEditionSuffix)
		drawList->AddText(ImVec2(x, textY), textColor, "Edition");

	drawList->PopClipRect();
}

// The "O" mark, centred in the window at low opacity. Drawn as the first thing in the body so
// it lands on top of the window background but underneath every widget; both panes below use
// transparent child backgrounds, so it shows through them.
void MainWindow::DrawWatermark() const
{
	const Branding::Logo& mark = Branding::OMark();
	if (!mark.IsValid())
		return;

	const Branding::Layout& layout = Branding::GetLayout();
	if (layout.watermarkOpacity <= 0.0f)
		return;

	ImDrawList* drawList = ImGui::GetWindowDrawList();

	const ImVec2 windowPos = ImGui::GetWindowPos();
	const ImVec2 windowSize = ImGui::GetWindowSize();

	// Measured on the inked box, which also fixes the centring: despite its name, the "O"
	// artwork is not actually centred within its own canvas.
	const float fitScale = ImMin(windowSize.x / (float)mark.contentWidth,
		windowSize.y / (float)mark.contentHeight);
	const float scale = fitScale * layout.watermarkScale;

	const ImVec2 markSize(mark.contentWidth * scale, mark.contentHeight * scale);
	const ImVec2 topLeft(
		windowPos.x + (windowSize.x - markSize.x) * 0.5f + layout.watermarkOffsetX,
		windowPos.y + (windowSize.y - markSize.y) * 0.5f + layout.watermarkOffsetY);

	ImVec4 tint = layout.watermarkTint;
	tint.w = layout.watermarkOpacity;

	drawList->AddImage(mark.texture, topLeft,
		ImVec2(topLeft.x + markSize.x, topLeft.y + markSize.y),
		mark.uv0, mark.uv1, ImGui::GetColorU32(tint));
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
		// Wrapped, not plain: the nav column is narrow and a rebound key can be "Ctrl+Shift+F1".
		ImGui::TextDisabledWrapped("%s",
			(HotkeyManager::DisplayString(HotkeyManager::GetBinding(hint.action))
				+ "  " + L(hint.what)).c_str());
	}
}

void MainWindow::DrawFooter()
{
	const float footerTop = ImGui::GetCursorPosY();

	ImGui::Separator();

	ImGui::TextUnformatted(Messages.Current_online_players());
	ImGui::SameLineOrWrap(ImGui::CalcTextSize("00000").x);
	const std::string playerCount = g_interfaces.pSteamApiHelper
		? g_interfaces.pSteamApiHelper->GetCurrentPlayersCountString()
		: Messages.No_data();
	ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", playerCount.c_str());

	// Three groups, separated by a bar: the settings door, the diagnostic windows, then the
	// community links. These sit below the paging rather than on a page because they are the
	// things you reach for without having gone looking for anything in particular.
	//
	// Every step is SameLineOrWrap, so making the menu narrow rewraps the row instead of
	// clipping whatever did not fit.
	const bool showDebugButton = ShouldShowDebugButton();

	const std::string releasesLabelStr = L("Releases");
	const char* releasesLabel = releasesLabelStr.c_str();
	const char* logLabel = Messages.Log();
	const char* discordLabel = Messages.Discord();
	const char* forumLabel = Messages.Forum();
	const char* githubLabel = Messages.GitHub();

	if (m_openSettingsRequested)
	{
		// Deferred out of the search results: OpenPopup and BeginPopupModal have to be issued
		// from the same window, and the results list lives inside the content child.
		m_settingsIniWindow.Open(m_pendingSettingsFilter.empty() ? nullptr : m_pendingSettingsFilter.c_str());
		m_openSettingsRequested = false;
		m_pendingSettingsFilter.clear();
	}

	// No (?) markers down here: the buttons say what they are, so the explanation goes on the
	// button itself and the row stays a clean line of buttons.
	m_settingsIniWindow.DrawOpenButton();
	ImGui::HoverTooltip(L("Everything the mod can be configured to do, hotkeys and language included, in one searchable list.").c_str());

	DrawFooterDivider(ImGui::ButtonWidth(releasesLabel));

	if (ImGui::Button(releasesLabel))
		m_pWindowContainer->GetWindow(WindowType_ReleaseChecker)->ToggleOpen();
	ImGui::HoverTooltip(Messages.Releases_checker_tooltip());

	ImGui::SameLineOrWrap(ImGui::ButtonWidth(logLabel));
	if (ImGui::Button(logLabel))
		m_pWindowContainer->GetWindow(WindowType_Log)->ToggleOpen();
	ImGui::HoverTooltip(Messages.Log_window_tooltip());

	if (showDebugButton)
	{
		ImGui::SameLineOrWrap(ImGui::ButtonWidth("DEBUG"));
		if (ImGui::Button("DEBUG"))
			m_pWindowContainer->GetWindow(WindowType_Debug)->ToggleOpen();
		ImGui::HoverTooltip(Messages.Debug_window_tooltip());
	}

	DrawFooterDivider(ImGui::ButtonWidth(discordLabel));

	ImGui::ButtonUrl(discordLabel, MOD_LINK_DISCORD);
	ImGui::SameLineOrWrap(ImGui::ButtonWidth(forumLabel));
	ImGui::ButtonUrl(forumLabel, MOD_LINK_FORUM);
	ImGui::SameLineOrWrap(ImGui::ButtonWidth(githubLabel));
	ImGui::ButtonUrl(githubLabel, MOD_LINK_GITHUB);

	Updater::UpdateCoordinator::GetInstance().DrawSkippedMainMenuLink();

	// Last, and in the same window the button that opens it lives in.
	m_settingsIniWindow.DrawModal();

	m_lastFooterHeight = ImMax(ImGui::GetCursorPosY() - footerTop, ImGui::GetFrameHeight());
}

// A "|" between footer groups. Kept on the same line as the group it introduces, so a wrap
// never leaves a divider stranded at the end of a row.
void MainWindow::DrawFooterDivider(float nextItemWidth)
{
	const float dividerWidth = ImGui::CalcTextSize("|").x;
	ImGui::SameLineOrWrap(dividerWidth + ImGui::GetStyle().ItemSpacing.x + nextItemWidth);
	ImGui::TextDisabled("|");
	ImGui::SameLine();
}

bool MainWindow::ShouldShowDebugButton()
{
#ifdef _DEBUG
	return true;
#else
	// Debug builds always get the DEBUG window; other builds only show it once the user opts
	// into dev/diagnostic tooling (settings.def: EnableInDevelopmentFeatures).
	return Settings::settingsIni.enableInDevelopmentFeatures;
#endif
}

void MainWindow::DrawSearchResults()
{
	ImGui::TextUnformatted(L("Search results").c_str());
	ImGui::TextDisabledWrapped("%s", L("Pick one to jump straight to it.").c_str());
	ImGui::VerticalSpacing(2);
	ImGui::Separator();
	ImGui::VerticalSpacing(4);

	int matches = 0;

	// The footer's own doors are searchable too, so a search for "language" or "hotkey" - both
	// of which are settings rather than menu entries - still lands somewhere useful instead of
	// coming back empty.
	struct GlobalAction
	{
		const char* label;
		const char* keywords;
		const char* settingsFilter; // non-null: open the settings popup pre-filtered
		WindowType_ window;
	};
	static const GlobalAction kGlobalActions[] = {
		{ "All settings", "settings ini config options preferences", "",         WindowType_Main },
		{ "Language",     "language english spanish translate idioma", "Language", WindowType_Main },
		{ "Hotkeys",      "hotkey keybind bind key shortcut rebind",  "Hotkeys",  WindowType_Main },
		{ "Releases",     "release update version changelog new",     nullptr,    WindowType_ReleaseChecker },
		{ "Log",          "log output messages troubleshoot",         nullptr,    WindowType_Log },
	};

	for (int i = 0; i < IM_ARRAYSIZE(kGlobalActions); ++i)
	{
		const GlobalAction& action = kGlobalActions[i];
		const std::string label = L(action.label);

		if (!m_featureFilter.PassFilter(action.label)
			&& !m_featureFilter.PassFilter(label.c_str())
			&& !m_featureFilter.PassFilter(action.keywords))
			continue;

		++matches;
		ImGui::PushID(0x2000 + i);
		if (ImGui::Selectable((label + "  ").c_str()))
		{
			if (action.settingsFilter)
			{
				m_openSettingsRequested = true;
				m_pendingSettingsFilter = action.settingsFilter;
			}
			else
			{
				m_pWindowContainer->GetWindow(action.window)->Open();
			}
			m_featureFilter.Clear();
		}
		ImGui::PopID();
	}

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
