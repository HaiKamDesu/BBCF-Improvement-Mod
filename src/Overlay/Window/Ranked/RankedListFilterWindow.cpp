#include "RankedListFilterWindow.h"

#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Network/RankedListConnectionFilter.h"
#include "Overlay/imgui_utils.h"

#include <imgui.h>

#include <cstdio>
#include <string>
#include <vector>

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
		Open();
	}
	else if (!shouldShow && IsOpen())
	{
		Close();
	}
}

void RankedListFilterWindow::Draw()
{
	RankedListConnectionFilter& filter = RankedListConnectionFilter::GetInstance();

	bool enableFilter = Settings::settingsIni.enableRankedListConnectionFilter;
	if (ImGui::Checkbox(L("Hide unreachable players from ranked list").c_str(), &enableFilter))
	{
		Settings::settingsIni.enableRankedListConnectionFilter = enableFilter;
		Settings::changeSetting("EnableRankedListConnectionFilter", enableFilter ? "1" : "0");
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("Checks in the background whether listed players are actually reachable and hides confirmed-unreachable ones from the ranked search list. One-off connection failures only hide a player briefly; repeat offenders stay hidden for the session. Hidden players can be restored below.").c_str());

	if (!enableFilter)
	{
		ImGui::TextDisabled("%s", L("Enable the filter to sort the list and review hidden players.").c_str());
		return;
	}

	// --- Sort order ---
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
	ImGui::ShowHelpMarker(L("Reorders the ranked search list itself. Connection order uses the filter's reachability probe timing (an estimate, not real ping); level order uses each player's advertised rank.").c_str());

	// --- Status + hidden players ---
	size_t shownCount = 0;
	size_t hiddenCount = 0;
	filter.GetLastListCounts(&shownCount, &hiddenCount);

	char statusText[128];
	snprintf(statusText, sizeof(statusText), L("Last search: %d shown, %d hidden").c_str(),
		static_cast<int>(shownCount), static_cast<int>(hiddenCount));
	ImGui::Separator();
	ImGui::TextDisabled("%s", statusText);

	std::vector<RankedListConnectionFilter::HiddenPeerInfo> hiddenPeers;
	filter.GetHiddenPeers(&hiddenPeers);

	if (hiddenPeers.empty())
	{
		ImGui::TextDisabled("%s", L("No players are currently hidden.").c_str());
		return;
	}

	for (const RankedListConnectionFilter::HiddenPeerInfo& peer : hiddenPeers)
	{
		const char* reasonText;
		if (peer.sessionBlocked)
		{
			reasonText = L("blocked (repeated failures)").c_str();
		}
		else if (peer.reactiveFailCount > 0)
		{
			reasonText = L("connection failed").c_str();
		}
		else
		{
			reasonText = L("unreachable").c_str();
		}

		ImGui::PushID(static_cast<int>(peer.steamId & 0xFFFFFFFFu));
		ImGui::Text("%s - %s", peer.name.empty() ? "???" : peer.name.c_str(), reasonText);
		ImGui::SameLine();
		if (ImGui::SmallButton(L("Restore").c_str()))
		{
			filter.RestorePeer(peer.steamId);
		}
		ImGui::PopID();
	}

	if (hiddenPeers.size() > 1)
	{
		if (ImGui::SmallButton(L("Restore all").c_str()))
		{
			filter.RestoreAllPeers();
		}
	}
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
