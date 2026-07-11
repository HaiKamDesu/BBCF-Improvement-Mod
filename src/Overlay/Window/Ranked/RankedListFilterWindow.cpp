#include "RankedListFilterWindow.h"

#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Network/RankedListConnectionFilter.h"

#include <imgui.h>

#include <cstdio>
#include <vector>

void RankedListFilterWindow::UpdateAutoVisibility()
{
	const bool shouldShow =
		Settings::settingsIni.enableRankedListConnectionFilter &&
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

	size_t shownCount = 0;
	size_t hiddenCount = 0;
	filter.GetLastListCounts(&shownCount, &hiddenCount);

	char statusText[128];
	snprintf(statusText, sizeof(statusText), L("Last search: %d shown, %d hidden").c_str(),
		static_cast<int>(shownCount), static_cast<int>(hiddenCount));
	ImGui::TextDisabled("%s", statusText);
	ImGui::Separator();

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
		ImGui::Separator();
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
