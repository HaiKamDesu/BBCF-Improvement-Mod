#include "RoomWindow.h"

#include "Core/info.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/utils.h"
#include "Game/PerformanceMonitor.h"
#include "Game/gamestates.h"
#include "Game/Room/RoomMemberEntry.h"
#include "Network/RoomManager.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/WindowManager.h"
#include "Overlay/Widget/ActiveGameModeWidget.h"
#include "Overlay/Widget/GameModeSelectWidget.h"
#include "Overlay/Widget/StageSelectWidget.h"
#include "Overlay/Window/PaletteEditorWindow.h"
#include "Overlay/Window/PerfStatsWindow.h"

#include "imgui_internal.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
	using namespace PerformanceMonitor;

	// ---- remembered layout (menus.ini) ---------------------------------------------------

	enum Section
	{
		Section_Players,
		Section_Match,
		Section_Palettes,
		Section_Count
	};

	const char* const kSectionIniNames[Section_Count] = { "Players", "Match", "Palettes" };
	bool g_sectionOpen[Section_Count] = { true, true, true };

	const ImVec4 kGood(0.45f, 0.90f, 0.45f, 1.0f);
	const ImVec4 kWarn(1.00f, 0.65f, 0.20f, 1.0f);

	// HoverTooltip does not wrap, and these are sentences: same delay, wrapped like the (?) markers.
	void WrappedTip(const char* text)
	{
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
			ImGui::SetTooltipWrapped(text);
	}

	void* Layout_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
	{
		return strcmp(name, "Layout") == 0 ? (void*)1 : nullptr;
	}

	void Layout_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
	{
		// Unknown keys are ignored, which covers the 8.9.2-dev layout's "Detailed=" and
		// "Performance=" (the stats used to be a section of this window).
		int value = 0;
		for (int i = 0; i < Section_Count; ++i)
		{
			char key[32];
			sprintf_s(key, "%s=%%d", kSectionIniNames[i]);
			if (sscanf_s(line, key, &value) == 1) { g_sectionOpen[i] = value != 0; return; }
		}
	}

	void Layout_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
	{
		buf->appendf("[%s][Layout]\n", handler->TypeName);
		for (int i = 0; i < Section_Count; ++i)
			buf->appendf("%s=%d\n", kSectionIniNames[i], g_sectionOpen[i] ? 1 : 0);
		buf->append("\n");
	}

	// A collapsing header whose open state survives restarts.
	bool SectionHeader(Section section, const std::string& label)
	{
		ImGui::SetNextItemOpen(g_sectionOpen[section], ImGuiCond_Always);
		const bool open = ImGui::CollapsingHeader((label + "##online_section_" + kSectionIniNames[section]).c_str());
		if (open != g_sectionOpen[section])
		{
			g_sectionOpen[section] = open;
			ImGui::MarkIniSettingsDirty();
		}
		return open;
	}

}

void RoomWindow::RegisterLayoutSettings()
{
	// AddSettingsHandler asserts on a duplicate type, and init can run again after a device loss.
	if (ImGui::FindSettingsHandler("BBCFIMOnlineWindow"))
		return;

	ImGuiSettingsHandler handler;
	handler.TypeName = "BBCFIMOnlineWindow";
	handler.TypeHash = ImHashStr("BBCFIMOnlineWindow");
	handler.ReadOpenFn = Layout_ReadOpen;
	handler.ReadLineFn = Layout_ReadLine;
	handler.WriteAllFn = Layout_WriteAll;
	ImGui::AddSettingsHandler(&handler);
}

void RoomWindow::BeforeDraw()
{
	ImGui::SetNextWindowPos(ImVec2(200, 200), ImGuiCond_FirstUseEver);
	// Resizable, so its size is the user's and is saved with its position; it used to
	// auto-resize, which made it jump whenever a section opened or a number changed width.
	ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(320, 160), ImVec2(1600, 1400));
}

void RoomWindow::Draw()
{
	const bool roomFunctional = g_interfaces.pRoomManager && g_interfaces.pRoomManager->IsRoomFunctional();

	DrawToolbar();

	if (!roomFunctional)
	{
		m_windowTitle = m_origWindowTitle;
		ImGui::TextDisabled("%s", Messages.YOU_ARE_NOT_IN_A_ROOM_OR_ONLINE_MATCH());
	}
	else
	{
		std::string roomTypeName = g_interfaces.pRoomManager->GetRoomTypeName();
		SetWindowTitleRoomType(roomTypeName);

		if (SectionHeader(Section_Players, L("Players")))
			DrawPlayers();

		const bool matchContext = isOnCharacterSelectionScreen() || isOnVersusScreen() || isInMatch() ||
			isStageSelectorEnabledInCurrentState() || isGameModeSelectorEnabledInCurrentState();
		if (matchContext && SectionHeader(Section_Match, L("Match settings")))
			DrawMatchSettings();

		if (isInMatch() && SectionHeader(Section_Palettes, L("Palettes")))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
			WindowManager::GetInstance().GetWindowContainer()->
				GetWindow<PaletteEditorWindow>(WindowType_PaletteEditor)->ShowAllPaletteSelections("Room");
			ImGui::PopStyleVar();
		}
	}
}

void RoomWindow::DrawToolbar()
{
	PerfStatsWindow* const stats = WindowManager::GetInstance().GetWindowContainer()->
		GetWindow<PerfStatsWindow>(WindowType_PerfStats);
	if (stats)
	{
		const bool open = stats->IsOpen();
		if (ImGui::Button(((open ? L("Close PC & connection stats") : L("Open PC & connection stats")) + "##online_stats").c_str()))
		{
			if (open)
				stats->Close();
			else
				stats->OpenFocused();
		}
		WrappedTip(L("A separate window with how fast each PC runs the game, the connection between the players, and who is causing any lag. Also Ctrl+F2.").c_str());
		ImGui::SameLine();
	}
	ImGui::TextDisabled("%s", MOD_VERSION);
}

void RoomWindow::SetWindowTitleRoomType(const std::string& roomTypeName)
{
	// The ### id keeps the window's saved position and size the same whatever room type the
	// visible title shows.
	m_windowTitle = FormatText(Messages.Online_room_window_title_format(), Messages.Online(), roomTypeName.c_str()) + "###Room";
}

void RoomWindow::ShowClickableSteamUser(const char* playerName, const CSteamID& steamId) const
{
	ImGui::TextUnformatted(playerName);
	WrappedTip(Messages.Click_to_open_Steam_profile());
	if (ImGui::IsItemClicked())
	{
		g_interfaces.pSteamFriendsWrapper->ActivateGameOverlayToUser("steamid", steamId);
	}
}

void RoomWindow::DrawPlayers()
{
	RoomManager* const room = g_interfaces.pRoomManager;
	const RoomMemberEntry* const self = room->GetThisPlayerRoomMemberEntry();
	RoomType roomType = RoomType_Lobby;
	const bool ffa = room->TryGetRoomType(&roomType) && roomType == RoomType_FFA;

	ImGui::TextDisabled("%s", FormatText(Messages.Online_type_s(), room->GetRoomTypeName().c_str()).c_str());

	const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
	if (!ImGui::BeginTable("##online_players", 3, flags))
		return;
	ImGui::TableSetupColumn(L("Player").c_str(), ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn(L("Role").c_str(), ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableSetupColumn(L("Mod").c_str(), ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableHeadersRow();

	for (uint16_t i = 0; i < MAX_PLAYERS_IN_ROOM; ++i)
	{
		const RoomMemberEntry* const entry = room->GetRoomMemberEntryByIndex(i);
		if (!entry)
			continue;

		const bool isSelf = self && entry->steamId == self->steamId;
		const CSteamID steamId(entry->steamId);
		const char* name = g_interfaces.pSteamFriendsWrapper ? g_interfaces.pSteamFriendsWrapper->GetFriendPersonaName(steamId) : "";

		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::PushID(i);
		ShowClickableSteamUser(name ? name : "?", steamId);
		ImGui::PopID();

		ImGui::TableSetColumnIndex(1);
		const bool sameMatch = self && self->matchId != 0 && entry->matchId == self->matchId;
		if (entry->matchId == 0)
			ImGui::TextDisabled("%s", L("In room").c_str());
		else if (!sameMatch)
			ImGui::TextDisabled("%s", L("Another match").c_str());
		else if (ffa)
			ImGui::TextUnformatted(L("Playing").c_str()); // FFA slots do not map to P1/P2 here
		else
		{
			const uint16_t index = entry->matchPlayerIndex;
			if (index == 0)
				ImGui::TextUnformatted(Messages.Player_1());
			else if (index == 1)
				ImGui::TextUnformatted(Messages.Player_2());
			else
				ImGui::TextUnformatted(Messages.Spectator());
		}

		ImGui::TableSetColumnIndex(2);
		IMPlayer imPlayer;
		if (isSelf)
			ImGui::TextColored(kGood, "%s", MOD_VERSION);
		else if (room->FindIMPlayer(entry->steamId, &imPlayer))
		{
			if (imPlayer.versionKnown)
				ImGui::TextColored(kGood, "%s", imPlayer.modVersion.c_str());
			else
			{
				ImGui::TextColored(kWarn, "%s", L("Older mod").c_str());
				WrappedTip(L("Runs a version of the mod older than 8.9.2, which does not say which version it is.").c_str());
			}
		}
		else
		{
			ImGui::TextDisabled("%s", L("No mod").c_str());
			WrappedTip(L("No reply to the mod's room handshake: this player runs the plain game. Someone who has only just joined can show this for a moment.").c_str());
		}
	}
	ImGui::EndTable();
}

void RoomWindow::DrawMatchSettings()
{
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
	if (isStageSelectorEnabledInCurrentState())
	{
		StageSelectWidget();
		ImGui::VerticalSpacing(6);
	}

	if (isOnCharacterSelectionScreen() || isOnVersusScreen() || isInMatch())
	{
		ActiveGameModeWidget();
		ImGui::VerticalSpacing(6);
	}

	if (isGameModeSelectorEnabledInCurrentState())
	{
		const bool isThisPlayerSpectator = g_interfaces.pRoomManager->IsRoomFunctional() &&
			g_interfaces.pRoomManager->GetThisPlayerRoomMemberEntry() &&
			g_interfaces.pRoomManager->IsThisPlayerSpectator();
		if (!isThisPlayerSpectator)
			GameModeSelectWidget();
	}
	ImGui::PopStyleVar();
}
