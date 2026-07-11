#include "RankedListConnectionFilter.h"

#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Network/RoomManager.h"
#include "Overlay/NotificationBar/NotificationBar.h"
#include "SteamApiWrapper/SteamMatchmakingWrapper.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
	// Dedicated P2P channel for reachability probes, chosen to be unlikely to
	// collide with whatever channel(s) BBCF's own network protocol uses - if a
	// probe byte does reach the remote client, it should just sit unread on a
	// channel their game never pumps, rather than getting misinterpreted as a
	// real protocol packet.
	constexpr int kProbeChannel = 200;

	// How long a held lobby list waits for still-unknown peers before being
	// delivered with unknowns shown (benefit of the doubt). Cache-warm
	// refreshes deliver instantly - this only delays the first search after
	// boot or after verdicts expire.
	constexpr unsigned long long kHoldDeadlineMs = 2000;

	// Reputation lifetimes. A reachable verdict keeps refreshes instant for a
	// while; a probe-confirmed unreachable verdict eventually expires so peers
	// whose network recovered get another chance.
	constexpr unsigned long long kReachableTtlMs = 5 * 60 * 1000;
	constexpr unsigned long long kProbeUnreachableTtlMs = 10 * 60 * 1000;

	// One real connection failure hides the peer only briefly - live testing
	// showed one-off transient failures happen to otherwise-fine players.
	// Repeat offenders get blocked for the session.
	constexpr unsigned long long kReactiveFailHideMs = 2 * 60 * 1000;
	constexpr int kSessionBlockFailCount = 2;
}

RankedListConnectionFilter::RankedListConnectionFilter()
{
}

RankedListConnectionFilter& RankedListConnectionFilter::GetInstance()
{
	static RankedListConnectionFilter instance;
	return instance;
}

void RankedListConnectionFilter::LobbyListResultProxy::Run(void* pvParam)
{
	RankedListConnectionFilter& filter = RankedListConnectionFilter::GetInstance();
	filter.OnLobbyListResultDelivered(pvParam, false, filter.m_pendingApiCall);
}

void RankedListConnectionFilter::LobbyListResultProxy::Run(void* pvParam, bool bIOFailure, SteamAPICall_t hSteamAPICall)
{
	RankedListConnectionFilter::GetInstance().OnLobbyListResultDelivered(pvParam, bIOFailure, hSteamAPICall);
}

void RankedListConnectionFilter::OnP2PSessionConnectFail(P2PSessionConnectFail_t* pParam)
{
	if (!pParam)
	{
		return;
	}

	// Steam's own verdict that this peer can't be reached - may arrive well
	// after our short hold window (observed ~20s for true-dead peers). Feeds
	// the reputation cache so the peer vanishes on the next list refresh.
	const uint64_t steamId = pParam->m_steamIDRemote.ConvertToUint64();
	PeerVerdict& verdict = m_verdicts[steamId];
	verdict.kind = PeerVerdict::Kind::ProbeUnreachable;
	verdict.verdictTickMs = GetTickCount64();
	m_probesInFlight.erase(steamId);
	LOG(1, "[RankedListFilter] P2P connect fail steamId=%llu error=%u - marked probe-unreachable\n",
		static_cast<unsigned long long>(steamId), static_cast<unsigned int>(pParam->m_eP2PSessionError));

	if (g_interfaces.pSteamNetworkingWrapper != nullptr)
	{
		g_interfaces.pSteamNetworkingWrapper->CloseP2PSessionWithUser(pParam->m_steamIDRemote);
	}
}

void RankedListConnectionFilter::MarkUnreachable(uint64_t steamId, const char* reason)
{
	if (steamId == 0)
	{
		return;
	}

	PeerVerdict& verdict = m_verdicts[steamId];
	verdict.reactiveFailCount += 1;
	verdict.lastReactiveFailTickMs = GetTickCount64();
	if (verdict.reactiveFailCount >= kSessionBlockFailCount)
	{
		verdict.sessionBlocked = true;
	}

	const char* name = "<unknown>";
	if (g_interfaces.pSteamFriendsWrapper != nullptr)
	{
		const char* const resolved = g_interfaces.pSteamFriendsWrapper->GetFriendPersonaName(CSteamID(steamId));
		if (resolved != nullptr && resolved[0] != '\0')
		{
			name = resolved;
		}
	}
	if (verdict.lastKnownName.empty() && name[0] != '<')
	{
		verdict.lastKnownName = name;
	}
	LOG(1, "[RankedListFilter] %s steamId=%llu name=\"%s\" - fail #%d (%s)\n",
		reason, static_cast<unsigned long long>(steamId), name, verdict.reactiveFailCount,
		verdict.sessionBlocked ? "blocked for session" : "hidden temporarily");

	if (g_interfaces.pSteamNetworkingWrapper != nullptr)
	{
		g_interfaces.pSteamNetworkingWrapper->CloseP2PSessionWithUser(CSteamID(steamId));
	}
}

void RankedListConnectionFilter::OnLobbyListRequestIssued(uint64_t apiCallHandle)
{
	// Track list activity regardless of whether the filter is enabled - the
	// hidden-players window uses this as its "search list is on screen" signal.
	m_lastListRequestTickMs = GetTickCount64();

	// New search supersedes any previous result/remap and any held delivery.
	// The reputation cache (m_verdicts) intentionally persists.
	m_hasRemapResult = false;
	m_reachableLobbies.clear();
	m_candidates.clear();
	m_gameLobbyListHandler = nullptr;
	m_heldApiCall = 0;

	if (!Settings::settingsIni.enableRankedListConnectionFilter || apiCallHandle == 0)
	{
		m_pipelineState = PipelineState::Idle;
		m_pendingApiCall = 0;
		return;
	}

	m_pipelineState = PipelineState::Armed;
	m_pendingApiCall = apiCallHandle;
}

CCallbackBase* RankedListConnectionFilter::SubstituteRegisterCallResult(CCallbackBase* pCallback, uint64_t apiCallHandle)
{
	if (m_pipelineState != PipelineState::Armed ||
		pCallback == nullptr ||
		pCallback == &m_lobbyListProxy ||
		apiCallHandle == 0 ||
		apiCallHandle != m_pendingApiCall)
	{
		return pCallback;
	}

	m_gameLobbyListHandler = pCallback;
	LOG(2, "[RankedListFilter] proxied lobby-list call result (game handler 0x%p, call %llu)\n",
		static_cast<void*>(pCallback), static_cast<unsigned long long>(apiCallHandle));
	return &m_lobbyListProxy;
}

CCallbackBase* RankedListConnectionFilter::SubstituteUnregisterCallResult(CCallbackBase* pCallback, uint64_t apiCallHandle)
{
	if (pCallback != nullptr && pCallback == m_gameLobbyListHandler && apiCallHandle == m_pendingApiCall)
	{
		LOG(2, "[RankedListFilter] game cancelled proxied lobby-list call %llu\n",
			static_cast<unsigned long long>(apiCallHandle));
		m_gameLobbyListHandler = nullptr;
		m_pipelineState = PipelineState::Idle;
		m_pendingApiCall = 0;
		m_heldApiCall = 0;
		return &m_lobbyListProxy;
	}
	return pCallback;
}

void RankedListConnectionFilter::OnLobbyListResultDelivered(void* pvParam, bool bIOFailure, SteamAPICall_t hSteamAPICall)
{
	if (m_pipelineState != PipelineState::Armed ||
		(hSteamAPICall != 0 && m_pendingApiCall != 0 && hSteamAPICall != m_pendingApiCall))
	{
		LOG(2, "[RankedListFilter] ignoring stale lobby-list delivery (call %llu)\n",
			static_cast<unsigned long long>(hSteamAPICall));
		return;
	}

	m_heldIOFailure = bIOFailure;
	m_heldApiCall = (hSteamAPICall != 0) ? hSteamAPICall : m_pendingApiCall;
	if (pvParam != nullptr)
	{
		memcpy(&m_heldResult, pvParam, sizeof(m_heldResult));
	}
	else
	{
		m_heldResult.m_nLobbiesMatching = 0;
	}

	m_pipelineState = PipelineState::Holding;

	if (bIOFailure || pvParam == nullptr || m_heldResult.m_nLobbiesMatching == 0 ||
		g_interfaces.pSteamMatchmakingWrapper == nullptr)
	{
		// Nothing to evaluate - hand the result straight through.
		BuildCompactedListAndDeliver("nothing to evaluate");
		return;
	}

	ISteamMatchmaking* const raw = g_interfaces.pSteamMatchmakingWrapper->m_SteamMatchmaking;
	for (uint32 i = 0; i < m_heldResult.m_nLobbiesMatching; ++i)
	{
		const CSteamID lobby = raw->GetLobbyByIndex(static_cast<int>(i));
		if (!lobby.IsValid())
		{
			continue;
		}
		LobbyCandidate candidate;
		candidate.lobbyId = lobby.ConvertToUint64();
		candidate.ownerSteamId = g_interfaces.pSteamMatchmakingWrapper->ResolveLobbyOwnerSteamId(lobby);
		const char* const ownerName = raw->GetLobbyData(lobby, "ownerName");
		candidate.ownerName = (ownerName != nullptr) ? ownerName : "";
		if (candidate.ownerSteamId != 0 && !candidate.ownerName.empty())
		{
			// Keep the freshest display name on record for the hidden-players UI.
			const auto verdictIt = m_verdicts.find(candidate.ownerSteamId);
			if (verdictIt != m_verdicts.end())
			{
				verdictIt->second.lastKnownName = candidate.ownerName;
			}
		}
		m_candidates.push_back(candidate);
		StartProbeIfNeeded(candidate.ownerSteamId);
	}

	// Deliver immediately when every listed peer already has a valid verdict;
	// otherwise hold briefly to give fresh probes a chance to resolve.
	bool anyUnresolved = false;
	for (const LobbyCandidate& candidate : m_candidates)
	{
		if (IsPeerUnresolved(candidate.ownerSteamId))
		{
			anyUnresolved = true;
			break;
		}
	}

	LOG(1, "[RankedListFilter] lobby list held: %u lobbies, unresolved=%d\n",
		static_cast<unsigned int>(m_heldResult.m_nLobbiesMatching), anyUnresolved ? 1 : 0);

	if (!anyUnresolved)
	{
		BuildCompactedListAndDeliver("verdicts cached");
	}
	else
	{
		m_holdDeadlineTickMs = GetTickCount64() + kHoldDeadlineMs;
	}
}

void RankedListConnectionFilter::StartProbeIfNeeded(uint64_t steamId)
{
	if (steamId == 0 || m_probesInFlight.find(steamId) != m_probesInFlight.end())
	{
		return;
	}

	const unsigned long long now = GetTickCount64();
	const auto it = m_verdicts.find(steamId);
	if (it != m_verdicts.end())
	{
		const PeerVerdict& verdict = it->second;
		if (verdict.sessionBlocked)
		{
			return; // no point probing a session-blocked peer
		}
		if (verdict.kind == PeerVerdict::Kind::Reachable &&
			now - verdict.verdictTickMs < kReachableTtlMs)
		{
			return; // verdict still fresh
		}
		if (verdict.kind == PeerVerdict::Kind::ProbeUnreachable &&
			now - verdict.verdictTickMs < kProbeUnreachableTtlMs)
		{
			return; // verdict still fresh
		}
	}

	m_probesInFlight.insert(steamId);

	if (g_interfaces.pSteamNetworkingWrapper != nullptr)
	{
		const uint8_t probeByte = 0;
		g_interfaces.pSteamNetworkingWrapper->SendP2PPacket(
			CSteamID(steamId), &probeByte, sizeof(probeByte), k_EP2PSendReliable, kProbeChannel);
	}
}

void RankedListConnectionFilter::PollProbes()
{
	if (g_interfaces.pSteamNetworkingWrapper == nullptr || m_probesInFlight.empty())
	{
		return;
	}

	const unsigned long long now = GetTickCount64();
	for (auto it = m_probesInFlight.begin(); it != m_probesInFlight.end();)
	{
		const uint64_t steamId = *it;
		P2PSessionState_t state = {};
		if (!g_interfaces.pSteamNetworkingWrapper->GetP2PSessionState(CSteamID(steamId), &state))
		{
			++it;
			continue;
		}

		if (state.m_bConnectionActive)
		{
			PeerVerdict& verdict = m_verdicts[steamId];
			verdict.kind = PeerVerdict::Kind::Reachable;
			verdict.verdictTickMs = now;
			LOG(1, "[RankedListFilter] probe steamId=%llu reachable=1 relay=%u\n",
				static_cast<unsigned long long>(steamId),
				static_cast<unsigned int>(state.m_bUsingRelay));
			it = m_probesInFlight.erase(it);
		}
		else if (state.m_eP2PSessionError != k_EP2PSessionErrorNone)
		{
			PeerVerdict& verdict = m_verdicts[steamId];
			verdict.kind = PeerVerdict::Kind::ProbeUnreachable;
			verdict.verdictTickMs = now;
			LOG(1, "[RankedListFilter] probe steamId=%llu reachable=0 error=%u\n",
				static_cast<unsigned long long>(steamId),
				static_cast<unsigned int>(state.m_eP2PSessionError));
			g_interfaces.pSteamNetworkingWrapper->CloseP2PSessionWithUser(CSteamID(steamId));
			it = m_probesInFlight.erase(it);
		}
		else
		{
			++it; // still connecting - keep polling on future pumps
		}
	}
}

bool RankedListConnectionFilter::ShouldHidePeer(uint64_t steamId, unsigned long long nowMs) const
{
	if (steamId == 0)
	{
		return false; // can't judge - show
	}

	const auto it = m_verdicts.find(steamId);
	if (it == m_verdicts.end())
	{
		return false; // unknown - benefit of the doubt
	}

	const PeerVerdict& verdict = it->second;
	if (verdict.sessionBlocked)
	{
		return true;
	}
	if (verdict.reactiveFailCount > 0 &&
		nowMs - verdict.lastReactiveFailTickMs < kReactiveFailHideMs)
	{
		return true;
	}
	if (verdict.kind == PeerVerdict::Kind::ProbeUnreachable &&
		nowMs - verdict.verdictTickMs < kProbeUnreachableTtlMs)
	{
		return true;
	}
	return false;
}

bool RankedListConnectionFilter::IsPeerUnresolved(uint64_t steamId) const
{
	if (steamId == 0)
	{
		return false;
	}
	if (m_probesInFlight.find(steamId) == m_probesInFlight.end())
	{
		return false; // no probe running - whatever we know is what we get
	}

	const auto it = m_verdicts.find(steamId);
	if (it == m_verdicts.end())
	{
		return true; // probing, no prior verdict at all
	}

	const unsigned long long now = GetTickCount64();
	const PeerVerdict& verdict = it->second;
	if (verdict.kind == PeerVerdict::Kind::Reachable)
	{
		return now - verdict.verdictTickMs >= kReachableTtlMs;
	}
	if (verdict.kind == PeerVerdict::Kind::ProbeUnreachable)
	{
		return now - verdict.verdictTickMs >= kProbeUnreachableTtlMs;
	}
	return true;
}

void RankedListConnectionFilter::BuildCompactedListAndDeliver(const char* reason)
{
	const unsigned long long now = GetTickCount64();

	m_reachableLobbies.clear();
	std::string hiddenNames;
	size_t newlyHiddenCount = 0;
	std::unordered_set<uint64_t> hiddenThisPass;
	for (const LobbyCandidate& candidate : m_candidates)
	{
		if (!ShouldHidePeer(candidate.ownerSteamId, now))
		{
			m_reachableLobbies.push_back(candidate.lobbyId);
			continue;
		}

		hiddenThisPass.insert(candidate.ownerSteamId);
		if (!candidate.ownerName.empty())
		{
			m_verdicts[candidate.ownerSteamId].lastKnownName = candidate.ownerName;
		}
		LOG(1, "[RankedListFilter] hiding lobby %llu (owner %llu, name \"%s\")\n",
			static_cast<unsigned long long>(candidate.lobbyId),
			static_cast<unsigned long long>(candidate.ownerSteamId),
			candidate.ownerName.c_str());

		// Only announce players newly hidden since the last list, so the
		// notification bar isn't spammed with the same names every few seconds
		// as the game auto-refreshes.
		if (m_announcedHidden.find(candidate.ownerSteamId) == m_announcedHidden.end())
		{
			m_announcedHidden.insert(candidate.ownerSteamId);
			++newlyHiddenCount;
			if (!hiddenNames.empty())
			{
				hiddenNames += ", ";
			}
			hiddenNames += candidate.ownerName.empty() ? "<unknown>" : candidate.ownerName;
		}
	}

	// Forget announced players who are no longer listed/hidden, so if they
	// come back later and get hidden again, that gets announced again.
	for (auto it = m_announcedHidden.begin(); it != m_announcedHidden.end();)
	{
		if (hiddenThisPass.find(*it) == hiddenThisPass.end())
		{
			it = m_announcedHidden.erase(it);
		}
		else
		{
			++it;
		}
	}

	m_lastShownCount = m_reachableLobbies.size();
	m_lastHiddenCount = m_candidates.size() - m_reachableLobbies.size();
	LOG(1, "[RankedListFilter] delivering (%s): %zu/%zu lobbies shown\n",
		reason, m_reachableLobbies.size(), m_candidates.size());

	if (newlyHiddenCount > 0 && g_notificationBar != nullptr)
	{
		g_notificationBar->AddNotification("Ranked list filter: hiding %zu unreachable player%s (%s)",
			newlyHiddenCount, newlyHiddenCount == 1 ? "" : "s", hiddenNames.c_str());
	}

	m_hasRemapResult = true;
	m_pipelineState = PipelineState::Idle;
	m_pendingApiCall = 0;

	CCallbackBase* const handler = m_gameLobbyListHandler;
	m_gameLobbyListHandler = nullptr;
	if (handler == nullptr)
	{
		LOG(1, "[RankedListFilter] no game handler to deliver lobby list to\n");
		return;
	}

	LobbyMatchList_t patched = m_heldResult;
	patched.m_nLobbiesMatching = static_cast<uint32>(m_reachableLobbies.size());
	handler->Run(&patched, m_heldIOFailure, m_heldApiCall);
}

void RankedListConnectionFilter::PollPendingConnectionConfirmation()
{
	if (m_pendingConnectionTarget == 0 || g_interfaces.pRoomManager == nullptr ||
		!g_interfaces.pRoomManager->IsRoomFunctional())
	{
		return;
	}

	// Read the raw game room slots (same source the ranked prediction UI uses on
	// the confirmation screen) - NOT GetIMPlayersInCurrentRoom, which only lists
	// other Improvement-Mod users and misses vanilla-client opponents.
	const std::vector<const RoomMemberEntry*> roomMembers =
		g_interfaces.pRoomManager->GetOtherRoomMemberEntriesInCurrentMatch();
	for (const RoomMemberEntry* const member : roomMembers)
	{
		if (member == nullptr || member->steamId != m_pendingConnectionTarget)
		{
			continue;
		}

		// The target showed up in the game's own room struct - the connection
		// worked (this is what the confirmation screen renders from). Credit
		// them and stop tracking, so a voluntary back-out isn't punished.
		PeerVerdict& verdict = m_verdicts[m_pendingConnectionTarget];
		verdict.kind = PeerVerdict::Kind::Reachable;
		verdict.verdictTickMs = GetTickCount64();
		LOG(1, "[RankedListFilter] connection to steamId=%llu confirmed (room member) - back-out won't count as failure\n",
			static_cast<unsigned long long>(m_pendingConnectionTarget));
		m_pendingLobbyId = 0;
		m_pendingConnectionTarget = 0;
		return;
	}
}

void RankedListConnectionFilter::OnSteamCallbacksPump()
{
	// Background probe polling runs regardless of pipeline state, so verdicts
	// keep maturing after a list has been delivered (the game auto-refreshes
	// every few seconds and picks up the results).
	PollProbes();
	PollPendingConnectionConfirmation();

	if (m_pipelineState != PipelineState::Holding)
	{
		return;
	}
	if (!Settings::settingsIni.enableRankedListConnectionFilter)
	{
		// Feature toggled off mid-hold - deliver everything unfiltered.
		m_verdicts.clear();
		BuildCompactedListAndDeliver("filter disabled mid-hold");
		return;
	}

	bool anyUnresolved = false;
	for (const LobbyCandidate& candidate : m_candidates)
	{
		if (IsPeerUnresolved(candidate.ownerSteamId))
		{
			anyUnresolved = true;
			break;
		}
	}

	if (!anyUnresolved)
	{
		BuildCompactedListAndDeliver("all resolved");
	}
	else if (GetTickCount64() >= m_holdDeadlineTickMs)
	{
		BuildCompactedListAndDeliver("hold deadline, unknowns shown");
	}
}

bool RankedListConnectionFilter::TryGetRemappedLobby(int index, uint64_t* outLobbyId)
{
	if (!m_hasRemapResult || outLobbyId == nullptr ||
		!Settings::settingsIni.enableRankedListConnectionFilter)
	{
		return false;
	}

	if (index < 0 || static_cast<size_t>(index) >= m_reachableLobbies.size())
	{
		*outLobbyId = 0;
		return true; // past the end of the compacted list
	}

	*outLobbyId = m_reachableLobbies[static_cast<size_t>(index)];
	return true;
}

void RankedListConnectionFilter::OnJoinLobbyAttempt(uint64_t lobbyId, uint64_t lobbyOwnerSteamId)
{
	m_pendingLobbyId = lobbyId;
	m_pendingConnectionTarget = lobbyOwnerSteamId;
}

void RankedListConnectionFilter::OnMatchStarted()
{
	// The pending attempt succeeded - clear it so a later ordinary LeaveLobby
	// (match/set ending normally) isn't mistaken for a failed connection.
	// Also credit the opponent as reachable: a real match is the strongest
	// possible reachability signal.
	if (m_pendingConnectionTarget != 0)
	{
		PeerVerdict& verdict = m_verdicts[m_pendingConnectionTarget];
		verdict.kind = PeerVerdict::Kind::Reachable;
		verdict.verdictTickMs = GetTickCount64();
	}
	m_pendingLobbyId = 0;
	m_pendingConnectionTarget = 0;
}

void RankedListConnectionFilter::OnLobbyEnter(LobbyEnter_t* pParam)
{
	if (!pParam || pParam->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess)
	{
		return;
	}

	char reason[48];
	sprintf_s(reason, "LobbyEnter failed response=%u", static_cast<unsigned int>(pParam->m_EChatRoomEnterResponse));
	NotifyConnectionAttemptFailed(reason);
}

void RankedListConnectionFilter::OnLeaveLobby(uint64_t lobbyId)
{
	if (lobbyId != 0 && lobbyId == m_pendingLobbyId && m_pendingConnectionTarget != 0)
	{
		MarkUnreachable(m_pendingConnectionTarget, "LeaveLobby before match start");
	}
	m_pendingLobbyId = 0;
	m_pendingConnectionTarget = 0;
}

void RankedListConnectionFilter::NotifyConnectionAttemptFailed(const char* reason)
{
	if (m_pendingConnectionTarget != 0)
	{
		MarkUnreachable(m_pendingConnectionTarget, reason);
	}
	m_pendingLobbyId = 0;
	m_pendingConnectionTarget = 0;
}

bool RankedListConnectionFilter::IsSteamIdFiltered(uint64_t steamId) const
{
	if (!Settings::settingsIni.enableRankedListConnectionFilter)
	{
		return false;
	}
	return ShouldHidePeer(steamId, GetTickCount64());
}

void RankedListConnectionFilter::GetHiddenPeers(std::vector<HiddenPeerInfo>* outPeers) const
{
	if (outPeers == nullptr)
	{
		return;
	}
	outPeers->clear();

	const unsigned long long now = GetTickCount64();
	for (const auto& entry : m_verdicts)
	{
		if (!ShouldHidePeer(entry.first, now))
		{
			continue;
		}
		HiddenPeerInfo info;
		info.steamId = entry.first;
		info.name = entry.second.lastKnownName;
		info.reactiveFailCount = entry.second.reactiveFailCount;
		info.sessionBlocked = entry.second.sessionBlocked;
		info.probeUnreachable = entry.second.kind == PeerVerdict::Kind::ProbeUnreachable;
		outPeers->push_back(info);
	}
}

void RankedListConnectionFilter::RestorePeer(uint64_t steamId)
{
	if (m_verdicts.erase(steamId) > 0)
	{
		m_announcedHidden.erase(steamId);
		LOG(1, "[RankedListFilter] user restored steamId=%llu - verdict cleared\n",
			static_cast<unsigned long long>(steamId));
	}
}

void RankedListConnectionFilter::RestoreAllPeers()
{
	const unsigned long long now = GetTickCount64();
	size_t restored = 0;
	for (auto it = m_verdicts.begin(); it != m_verdicts.end();)
	{
		if (ShouldHidePeer(it->first, now))
		{
			m_announcedHidden.erase(it->first);
			it = m_verdicts.erase(it);
			++restored;
		}
		else
		{
			++it;
		}
	}
	LOG(1, "[RankedListFilter] user restored all hidden peers (%zu)\n", restored);
}

void RankedListConnectionFilter::GetLastListCounts(size_t* outShown, size_t* outHidden) const
{
	if (outShown != nullptr)
	{
		*outShown = m_lastShownCount;
	}
	if (outHidden != nullptr)
	{
		*outHidden = m_lastHiddenCount;
	}
}

bool RankedListConnectionFilter::IsLobbyListLikelyOpen() const
{
	// Longest observed gap between the game's automatic list re-requests while
	// the search list stayed on screen was ~15s.
	constexpr unsigned long long kListActivityWindowMs = 16000;
	return m_lastListRequestTickMs != 0 &&
		GetTickCount64() - m_lastListRequestTickMs < kListActivityWindowMs;
}
