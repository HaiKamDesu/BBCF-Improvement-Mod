#include "RankedListFilterWindow.h"

#include "Core/Localization.h"
#include "Core/logger.h"
#include "Core/Settings.h"
#include "Network/RankedListConnectionFilter.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Window/NetworkSquareColorWindow.h"
#include "Overlay/Window/Ranked/RankedProgressWindow.h"

#include <imgui.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
	// 0-4 Delay/connection tag color, as specified for this list: worst (0) to
	// best (4). Distinct from the native netcolor square (0-8), which already
	// has its own palette in NetworkSquareColorWindow.
	ImVec4 GetConnectionTagColor(int delayDigit)
	{
		switch (delayDigit)
		{
		case 0: return ImVec4(0.90f, 0.20f, 0.20f, 1.0f); // red
		case 1: return ImVec4(0.62f, 0.32f, 0.90f, 1.0f); // purple
		case 2: return ImVec4(0.95f, 0.55f, 0.10f, 1.0f); // orange
		case 3: return ImVec4(0.30f, 0.85f, 0.30f, 1.0f); // green
		case 4: return ImVec4(0.92f, 0.92f, 0.92f, 1.0f); // white
		default: return ImVec4(0.55f, 0.55f, 0.55f, 1.0f); // unresolved
		}
	}

	void DrawSquare(ImDrawList* drawList, const ImVec2& p0, float size, const ImVec4& fill)
	{
		const ImVec2 p1 = ImVec2(p0.x + size, p0.y + size);
		drawList->AddRectFilled(p0, p1, ImGui::GetColorU32(fill), 3.0f);
		drawList->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(0.08f, 0.08f, 0.08f, 1.0f)), 3.0f);
	}
}

void RankedListFilterWindow::UpdateAutoVisibility()
{
	// The window hosts the filter's own enable toggle, so it must be available
	// whenever the ranked list is open regardless of whether the filter is
	// currently on - otherwise a user who disabled the filter inside it could
	// never bring it back from here.
	const bool shouldShow =
		Settings::settingsIni.showRankedListFilterWindow &&
		RankedListConnectionFilter::GetInstance().IsLobbyListLikelyOpen();

	if (shouldShow && !IsOpen())
	{
		LOG(1, "[RankedListFilter] config window opening (showSetting=%d)\n",
			Settings::settingsIni.showRankedListFilterWindow ? 1 : 0);
		Open();
	}
	else if (!shouldShow && IsOpen())
	{
		LOG(1, "[RankedListFilter] config window closing (showSetting=%d)\n",
			Settings::settingsIni.showRankedListFilterWindow ? 1 : 0);
		Close();
	}
}

void RankedListFilterWindow::Draw()
{
	RankedListConnectionFilter& filter = RankedListConnectionFilter::GetInstance();

	// --- Sorting (independent of the filters) ---
	const std::string sortModeLabels[RankedListSortMode_COUNT] = {
		L("Default"),
		L("Best to Worst Connection"),
		L("Worst to Best Connection"),
		L("Closest to Furthest from my Level"),
		L("Furthest to Closest from my Level"),
		L("Highest to Lowest Level"),
		L("Lowest to Highest Level"),
		L("Names in A-Z Order"),
		L("Names in Z-A Order"),
	};
	const char* sortModeItems[RankedListSortMode_COUNT];
	for (int i = 0; i < RankedListSortMode_COUNT; ++i)
	{
		sortModeItems[i] = sortModeLabels[i].c_str();
	}

	int sortMode = Settings::settingsIni.rankedListSortMode;
	if (sortMode < 0 || sortMode >= RankedListSortMode_COUNT)
	{
		sortMode = RankedListSortMode_Default;
	}
	ImGui::PushItemWidth(260.0f);
	if (ImGui::Combo(L("Sort ranked list").c_str(), &sortMode, sortModeItems, RankedListSortMode_COUNT))
	{
		Settings::settingsIni.rankedListSortMode = sortMode;
		char sortModeValue[8];
		snprintf(sortModeValue, sizeof(sortModeValue), "%d", sortMode);
		Settings::changeSetting("RankedListSortMode", sortModeValue);
	}
	ImGui::PopItemWidth();
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("Reorders the ranked search list itself. Connection order matches the list's own Delay rating (0-4) once the game has measured it for a player, updating as it changes; falls back to the mod's own reachability check timing as a rough estimate until then. Level order uses each player's advertised rank.").c_str());

	ImGui::Separator();

	// --- Filtering section ---

	// Network filter: minimum Delay rating a row must show to stay listed.
	const std::string networkFilterLabels[5] = {
		L("All"),
		L("1 and above"),
		L("2 and above"),
		L("3 and above"),
		L("4 only"),
	};
	const char* networkFilterItems[5];
	for (int i = 0; i < 5; ++i)
	{
		networkFilterItems[i] = networkFilterLabels[i].c_str();
	}
	int networkFilter = Settings::settingsIni.rankedListNetworkFilter;
	if (networkFilter < 0 || networkFilter > 4)
	{
		networkFilter = 0;
	}
	ImGui::PushItemWidth(260.0f);
	if (ImGui::Combo(L("Network Filter").c_str(), &networkFilter, networkFilterItems, 5))
	{
		Settings::settingsIni.rankedListNetworkFilter = networkFilter;
		char networkFilterValue[8];
		snprintf(networkFilterValue, sizeof(networkFilterValue), "%d", networkFilter);
		Settings::changeSetting("RankedListNetworkFilter", networkFilterValue);
	}
	ImGui::PopItemWidth();
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("Hides players whose Delay rating (0-4) is below the selected level. Players whose rating hasn't been measured yet stay visible until it resolves; rows are hidden and restored live as ratings change.").c_str());

	bool enableFilter = Settings::settingsIni.enableRankedListConnectionFilter;
	if (ImGui::Checkbox(L("Hide unreachable players from ranked list").c_str(), &enableFilter))
	{
		Settings::settingsIni.enableRankedListConnectionFilter = enableFilter;
		Settings::changeSetting("EnableRankedListConnectionFilter", enableFilter ? "1" : "0");
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("Checks in the background whether listed players are actually reachable and hides confirmed-unreachable ones from the ranked search list. Everyone (hidden players included) is re-checked every 15 seconds, so players whose connection recovers reappear on their own. Click the hidden counter below to see and restore hidden players manually.").c_str());

	bool hideUnmet = Settings::settingsIni.hideUnmetRequirementRooms;
	if (ImGui::Checkbox(L("Hide rooms with unmet network requirements").c_str(), &hideUnmet))
	{
		Settings::settingsIni.hideUnmetRequirementRooms = hideUnmet;
		Settings::changeSetting("HideUnmetRequirementRooms", hideUnmet ? "1" : "0");
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("Some rooms require a minimum connection quality and show \"The room's connectivity requirements are not met\" when you try to enter. This hides those rooms once your Delay rating to them is measured, so everything left in the list is actually joinable.").c_str());

	// --- Status line (any filtering feature can hide rows) ---
	if (!enableFilter && networkFilter == 0 && !hideUnmet)
	{
		if (m_showHiddenPopup)
		{
			m_showHiddenPopup = false;
		}
		return;
	}

	size_t shownCount = 0;
	size_t hiddenCount = 0;
	filter.GetLastListCounts(&shownCount, &hiddenCount);

	char shownText[96];
	snprintf(shownText, sizeof(shownText), L("Last search: %d shown,").c_str(),
		static_cast<int>(shownCount));
	ImGui::TextDisabled("%s", shownText);
	ImGui::SameLine();

	// "N hidden" is a link: underlined on hover, click opens the hidden-
	// players window.
	char hiddenText[64];
	snprintf(hiddenText, sizeof(hiddenText), L("%d hidden").c_str(),
		static_cast<int>(hiddenCount));
	ImGui::TextDisabled("%s", hiddenText);
	if (ImGui::IsItemHovered())
	{
		// (This ImGui version has no hand cursor - the underline is the
		// clickability affordance.)
		const ImVec2 rectMin = ImGui::GetItemRectMin();
		const ImVec2 rectMax = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddLine(
			ImVec2(rectMin.x, rectMax.y),
			ImVec2(rectMax.x, rectMax.y),
			ImGui::GetColorU32(ImGuiCol_TextDisabled));
	}
	if (ImGui::IsItemClicked())
	{
		m_showHiddenPopup = !m_showHiddenPopup;
		Settings::settingsIni.showRankedListHiddenPopup = m_showHiddenPopup;
		Settings::changeSetting("ShowRankedListHiddenPopup", m_showHiddenPopup ? "1" : "0");
	}
	else if (!m_showHiddenPopup && Settings::settingsIni.showRankedListHiddenPopup)
	{
		// Reopen automatically - the setting says the user wants this
		// visible whenever there are hidden players to show.
		m_showHiddenPopup = true;
	}

	if (m_showHiddenPopup)
	{
		DrawHiddenPlayersPopup();
		if (!m_showHiddenPopup && Settings::settingsIni.showRankedListHiddenPopup)
		{
			// Closed via the popup's own X - persist so it stays closed
			// until manually reopened.
			Settings::settingsIni.showRankedListHiddenPopup = false;
			Settings::changeSetting("ShowRankedListHiddenPopup", "0");
		}
	}
}

void RankedListFilterWindow::DrawHiddenPlayersPopup()
{
	RankedListConnectionFilter& filter = RankedListConnectionFilter::GetInstance();

	ImGui::SetNextWindowSize(ImVec2(380.0f, 260.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(L("Hidden players").c_str(), &m_showHiddenPopup))
	{
		ImGui::End();
		return;
	}

	std::vector<RankedListConnectionFilter::HiddenPeerInfo> hiddenPeers;
	filter.GetHiddenPeers(&hiddenPeers);

	if (hiddenPeers.empty())
	{
		ImGui::TextDisabled("%s", L("No players are currently hidden.").c_str());
		ImGui::End();
		return;
	}

	if (hiddenPeers.size() > 1)
	{
		if (ImGui::SmallButton(L("Restore all").c_str()))
		{
			filter.RestoreAllPeers();
		}
		ImGui::Separator();
	}

	// Scrollable region so a long list never grows the window off screen.
	if (ImGui::BeginChild("hidden_players_scroll", ImVec2(0.0f, 0.0f), false))
	{
		const float squareSize = 16.0f;
		for (const RankedListConnectionFilter::HiddenPeerInfo& peer : hiddenPeers)
		{
			std::string reasonText;
			switch (peer.reason)
			{
			case RankedListConnectionFilter::HiddenReason::ConnectionFailed:
				reasonText = L("connection failed");
				break;
			case RankedListConnectionFilter::HiddenReason::NetworkFilter:
				reasonText = L("below network filter");
				break;
			case RankedListConnectionFilter::HiddenReason::Requirement:
				reasonText = L("network requirements not met");
				break;
			default:
				reasonText = L("unreachable");
				break;
			}

			ImGui::PushID(static_cast<int>(peer.steamId & 0xFFFFFFFFu));

			// Square color (native row icon).
			ImDrawList* const drawList = ImGui::GetWindowDrawList();
			ImVec2 p0 = ImGui::GetCursorScreenPos();
			p0.y += (ImGui::GetTextLineHeight() - squareSize) * 0.5f;
			DrawSquare(drawList, p0, squareSize, GetNetColorVec4(peer.netColor));
			ImGui::Dummy(ImVec2(squareSize, squareSize));
			ImGui::SameLine();

			// Name.
			ImGui::TextUnformatted(peer.name.empty() ? "???" : peer.name.c_str());
			ImGui::SameLine();

			// Rank, colored the same way as the ranked progress overlay.
			if (peer.rank >= 0)
			{
				const uint32_t visibleRank = InternalRankToVisibleRank(static_cast<uint32_t>(peer.rank), false);
				const std::string rankLabel = FormatVisibleRankLabel(visibleRank, false);
				ImGui::TextColored(GetVisibleRankColor(visibleRank, false), "%s", rankLabel.c_str());
				ImGui::SameLine();
			}

			// Connection tag (0-4 Delay digit), colored per spec.
			if (peer.delayDigit >= 0)
			{
				char connLabel[4];
				snprintf(connLabel, sizeof(connLabel), "%d", peer.delayDigit);
				ImGui::TextColored(GetConnectionTagColor(peer.delayDigit), "%s", connLabel);
				ImGui::SameLine();
			}

			// Reason + restore, on their own trailing line so the header row
			// above stays uncluttered.
			ImGui::TextDisabled("- %s", reasonText.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton(L("Restore").c_str()))
			{
				filter.RestorePeer(peer.steamId);
			}

			ImGui::PopID();
			ImGui::Separator();
		}
	}
	ImGui::EndChild();

	ImGui::End();
}

void RankedListFilterWindow::AfterDraw()
{
	// AfterDraw only runs on frames where the window was open going into
	// Update(); m_windowOpen turning false here means the user clicked the X.
	// Per design, that is equivalent to unchecking the "show window" setting.
	if (!m_windowOpen && Settings::settingsIni.showRankedListFilterWindow)
	{
		Settings::settingsIni.showRankedListFilterWindow = false;
		Settings::changeSetting("ShowRankedListFilterWindow", "0");
	}
}
