#include "RankedListConnectionFilter.h"

#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Game/gamestates.h"
#include "Network/RoomManager.h"
#include "Overlay/NotificationBar/NotificationBar.h"
#include "Overlay/Window/Ranked/RankedProgressWindow.h"
#include "SteamApiWrapper/SteamMatchmakingWrapper.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
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

	// Connection-based sort needs actual probeElapsedMs measurements to mean
	// anything. Live testing showed most probes take 4-8s to establish, far
	// longer than kHoldDeadlineMs - delivering at 2s left 15/16 candidates
	// with no measurement yet, so "Best/Worst Connection" was sorting almost
	// nothing. Give connection-sort deliveries a longer hold so most probes
	// have a chance to land before the order is locked in.
	constexpr unsigned long long kConnectionSortHoldDeadlineMs = 6000;

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

	// Raw ranked network state struct (same one RankedProgressWindow.cpp reads
	// unconditionally every frame as "networkState.state/state1"). state==4 is
	// "engaged in ranked networking flow"; state1 discriminates the specific
	// screen. gstate stays GameState_MainMenu (27) throughout. state1's actual
	// meaning per value is validated by RankedAutomationHarness.cpp's
	// IsRankedSearchEntryMenuState/IsRankedSearchResultsState/
	// IsRankedPostSearchBackState - see IsLobbyListLikelyOpen().
	constexpr uintptr_t kRankedNetworkStructRva = 0x008F7958;

	// Game's ranked-search row array layout (RVA/offsets confirmed via
	// FUN_004AAAD0 / FUN_004A8AB0 / FUN_004A6620). See RebuildGameMeasuredRttMap.
	constexpr uintptr_t kRowContainerRva = 0x0065D270;
	constexpr uintptr_t kRowArrayOffset = 0x1510;
	constexpr uintptr_t kRowStride = 0x68;
	constexpr int kMaxRows = 64;
	constexpr uintptr_t kRowOccupied = 0x08;   // != 0 when populated
	constexpr uintptr_t kRowFlags = 0x30;      // bit 0x100 set once session index valid
	constexpr uintptr_t kRowSessionIndex = 0x4C;
	constexpr uintptr_t kRowSideFlag = 0x50;
	constexpr uintptr_t kRowRtt = 0x5C;
	constexpr uintptr_t kSessionSteamIdLo = 0x28;
	constexpr uintptr_t kSessionSteamIdHi = 0x2C;

	// Lazy-construction guard for the row container (bit 0 set once
	// FUN_00469E20 has placement-constructed DAT_00A5D270). If this is unset,
	// the container is all-zero and every row reads as unoccupied regardless
	// of address correctness.
	constexpr uintptr_t kRowContainerInitGuardRva = 0x002224E0;

	// NOTE: a second candidate live connection-quality array (AASTEAM_CNetworker,
	// RVA 0x625788+0x129C) was investigated and diagnostically logged here -
	// confirmed DEAD (128 samples across a full session, always flat zero,
	// never changed). See docs/Research/RankedListConnectionFilter_Progress.md
	// for the full writeup; do not re-add this diagnostic without new evidence.

	// Third candidate, found by working backward from the ranked list's own
	// delay-dot icon-draw call (FUN_00661060, confirmed ranked-specific via
	// the "NTER_RankMatch_NotMatching" string) rather than guessing a data
	// container forward. This is a singleton POINTER slot (not a struct
	// base directly) - base+kRankedListMgrSlotRva holds a pointer to a
	// polymorphic "ranked room-list manager" object; see the progress doc's
	// "found the ACTUAL ranked-list delay-dot render call chain" section for
	// the full vtable-slot-7 call chain. CONFIRMED LIVE via
	// DiagnosticLogRankedListMgrSlot: non-null/stable during real play, and
	// its row count (+0xae8 off the struct returned by vtable slot 7) has
	// matched the real on-screen lobby count exactly across two sessions.
	constexpr uintptr_t kRankedListMgrSlotRva = 0x00897E3C;

	// FUN_004a5450 (Ghidra decompile: EntryPtrWrapperValidator) - a cached
	// intrusive-doubly-linked-list walk. __thiscall: this = the list struct
	// returned by the mgr's vtable slot 7 (NOT the mgr itself), explicit arg
	// = the underlying/logical row index read from that struct's +0xaf4
	// permutation array. Mutates the struct's own traversal cache
	// (+0xaec/+0xaf0) as a side effect - harmless to call ourselves since
	// it's the exact same call the game makes every frame per visible row,
	// using the same real index values, so it only ever reflects a state
	// the game itself would already produce.
	constexpr uintptr_t kWalkRowListRva = 0x000A5450;

	// Cap on how many rows the diagnostic below will read per tick - purely
	// to bound log spam/repeated virtual-call cost, not a real limit on the
	// game's own row count.
	constexpr int kMaxDiagnosticRows = 32;
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

	if (!IsPipelineActive() || apiCallHandle == 0)
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
	m_lastGameLobbyListHandler = pCallback;
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
		const char* const hostLevel = raw->GetLobbyData(lobby, "RANK_HOST_LEVEL");
		if (hostLevel != nullptr && hostLevel[0] != '\0')
		{
			char* end = nullptr;
			const long parsedLevel = std::strtol(hostLevel, &end, 10);
			if (end != hostLevel && parsedLevel >= 0 && parsedLevel <= 63)
			{
				candidate.internalRankLevel = static_cast<int>(parsedLevel);
			}
		}
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
		const int sortMode = Settings::settingsIni.rankedListSortMode;
		const bool connectionSort = sortMode == RankedListSortMode_BestConnection ||
			sortMode == RankedListSortMode_WorstConnection;
		m_holdDeadlineTickMs = GetTickCount64() +
			(connectionSort ? kConnectionSortHoldDeadlineMs : kHoldDeadlineMs);
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

	m_probesInFlight[steamId] = now;

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
		const uint64_t steamId = it->first;
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
			verdict.probeElapsedMs = now - it->second;
			verdict.usedRelay = state.m_bUsingRelay != 0;
			LOG(1, "[RankedListFilter] probe steamId=%llu reachable=1 relay=%u elapsedMs=%llu\n",
				static_cast<unsigned long long>(steamId),
				static_cast<unsigned int>(state.m_bUsingRelay),
				verdict.probeElapsedMs);
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

void RankedListConnectionFilter::PollGameTiers()
{
	if (m_reachableLobbies.empty())
	{
		return; // nothing delivered yet this session
	}

	// Throttled like the diagnostic this reuses the chain from - reading is
	// cheap, but no need to do it more than a few times/sec.
	static unsigned long long s_lastPollTickMs = 0;
	const unsigned long long now = GetTickCount64();
	if (now - s_lastPollTickMs < 500)
	{
		return;
	}
	s_lastPollTickMs = now;

	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	if (moduleBase == 0)
	{
		return;
	}

	const void* const* const slot =
		reinterpret_cast<const void* const*>(moduleBase + kRankedListMgrSlotRva);
	if (IsBadReadPtr(slot, sizeof(void*)))
	{
		return;
	}
	const void* const mgr = *slot;
	if (mgr == nullptr || IsBadReadPtr(mgr, sizeof(void*)))
	{
		return;
	}
	const void* const vtable = *reinterpret_cast<const void* const*>(mgr);
	if (vtable == nullptr || IsBadReadPtr(reinterpret_cast<const uint8_t*>(vtable) + 0x1c, sizeof(void*)))
	{
		return;
	}

	typedef void*(__thiscall * GetListStructFn)(void*);
	const GetListStructFn getListStruct =
		*reinterpret_cast<const GetListStructFn*>(reinterpret_cast<uintptr_t>(vtable) + 0x1c);
	if (getListStruct == nullptr)
	{
		return;
	}
	void* const listStruct = getListStruct(const_cast<void*>(mgr));
	if (listStruct == nullptr ||
		IsBadReadPtr(reinterpret_cast<const uint8_t*>(listStruct) + 0xae8, sizeof(int32_t)))
	{
		return;
	}

	const int32_t rowCount = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(listStruct) + 0xae8);
	// Only trust this read when the game's own row count exactly matches our
	// last delivery - live testing showed a brief lag (a tick or two) right
	// after each delivery where the game's count reads 0/stale before
	// catching up. Skipping the mismatched window avoids pairing a row's
	// tier with the wrong peer.
	if (rowCount <= 0 || static_cast<size_t>(rowCount) != m_reachableLobbies.size())
	{
		return;
	}

	const uint8_t* const permutationArray = reinterpret_cast<const uint8_t*>(listStruct) + 0xaf4;
	typedef void* (__thiscall * WalkRowListFn)(void*, int32_t);
	const WalkRowListFn walkRowList = reinterpret_cast<WalkRowListFn>(moduleBase + kWalkRowListRva);
	typedef uint8_t(__thiscall * GetTierFn)(void*);

	for (int32_t row = 0; row < rowCount; ++row)
	{
		const int32_t* const permutationSlot = reinterpret_cast<const int32_t*>(permutationArray + row * 4);
		if (IsBadReadPtr(permutationSlot, sizeof(int32_t)))
		{
			break;
		}
		const int32_t underlyingIndex = *permutationSlot;

		void* const entry = walkRowList(listStruct, underlyingIndex);
		if (entry == nullptr || IsBadReadPtr(entry, sizeof(void*)))
		{
			continue;
		}
		const void* const entryVtable = *reinterpret_cast<void* const*>(entry);
		if (entryVtable == nullptr ||
			IsBadReadPtr(reinterpret_cast<const uint8_t*>(entryVtable) + 0x1c, sizeof(void*)))
		{
			continue;
		}
		const GetTierFn getTier =
			*reinterpret_cast<const GetTierFn*>(reinterpret_cast<uintptr_t>(entryVtable) + 0x1c);
		if (getTier == nullptr)
		{
			continue;
		}
		const uint8_t tier = getTier(entry);

		// Row order matches our own last-delivered order exactly (rowCount
		// was just confirmed equal to m_reachableLobbies.size() above) - map
		// position -> lobbyId -> ownerSteamId to know which peer this is.
		const uint64_t lobbyId = m_reachableLobbies[static_cast<size_t>(row)];
		for (const LobbyCandidate& candidate : m_candidates)
		{
			if (candidate.lobbyId == lobbyId)
			{
				PeerVerdict& verdict = m_verdicts[candidate.ownerSteamId];
				verdict.gameTier = tier;
				// Cumulative average across every observation this session -
				// smooths out any single noisy/mismapped reading rather than
				// letting the sort key jump around on one bad sample.
				if (verdict.gameTierSampleCount <= 0)
				{
					verdict.gameTierAverage = static_cast<double>(tier);
				}
				else
				{
					verdict.gameTierAverage =
						(verdict.gameTierAverage * verdict.gameTierSampleCount + tier) /
						(verdict.gameTierSampleCount + 1);
				}
				++verdict.gameTierSampleCount;
				break;
			}
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

// NOTE: an earlier attempt read the game's own delay-column row array here
// (container/row/session offsets that RE identified from FUN_004AAAD0). Live
// testing disproved it: the container is confirmed constructed
// (kRowContainerInitGuardRva set) but every row reads as entirely zero bytes
// even with a full, visibly-populated 32-lobby list on screen - that function
// is not what drives this UI. Connection sort uses our own reachability probe
// timing instead (see SortShownCandidates); CountPopulatedGameRows below is
// kept only as a diagnostic cross-check, not a real signal.

void RankedListConnectionFilter::SortShownCandidates(std::vector<const LobbyCandidate*>* shown) const
{
	const int mode = Settings::settingsIni.rankedListSortMode;
	if (shown == nullptr || shown->size() < 2 ||
		mode <= RankedListSortMode_Default || mode >= RankedListSortMode_COUNT)
	{
		return;
	}

	// "My level" for the closest/furthest modes, from the ranked progress
	// machinery. If it can't be captured (e.g. unranked), those modes keep the
	// default order.
	uint32_t myVisibleRank = 0;
	bool haveMyRank = false;
	if (mode == RankedListSortMode_ClosestLevel || mode == RankedListSortMode_FurthestLevel)
	{
		RankedProgressOverlaySnapshot snapshot;
		if (CaptureRankedProgressOverlaySnapshot(&snapshot) && snapshot.active && !snapshot.isUnranked)
		{
			myVisibleRank = snapshot.currentRank;
			haveMyRank = true;
		}
		if (!haveMyRank)
		{
			LOG(1, "[RankedListFilter] sort: own rank unavailable, keeping default order\n");
			return;
		}
	}

	struct SortEntry
	{
		const LobbyCandidate* candidate = nullptr;
		bool keyKnown = false;
		long long numericKey = 0;
		std::string textKey;
	};

	std::vector<SortEntry> entries;
	entries.reserve(shown->size());
	for (const LobbyCandidate* const candidate : *shown)
	{
		SortEntry entry;
		entry.candidate = candidate;

		switch (mode)
		{
		case RankedListSortMode_BestConnection:
		case RankedListSortMode_WorstConnection:
		{
			// Prefer the game's own real connection-quality tier
			// (0-7, higher = better - see docs/Research/RankedListConnectionFilter_Progress.md
			// for the full RE trace and live confirmation) whenever it's been
			// observed at least once for this peer, read live by
			// PollGameTiers(). Uses the cumulative average across every
			// observation this session (gameTierAverage), not just the
			// latest single reading, per the reasoning that a one-off noisy
			// or mismapped sample shouldn't move a peer's sort position by
			// itself. Fixed-point (x100) so the average survives as an
			// integer sort key; negated so ascending numericKey still means
			// "best first" for RankedListSortMode_BestConnection, matching
			// the convention the probeElapsedMs fallback below already uses
			// (smaller = better = sorts first ascending).
			const auto it = m_verdicts.find(candidate->ownerSteamId);
			if (it != m_verdicts.end() && it->second.gameTierSampleCount > 0)
			{
				entry.numericKey = -static_cast<long long>(it->second.gameTierAverage * 100.0);
				entry.keyKnown = true;
			}
			// Fallback: the mod's own reachability-probe establishment time,
			// only used until the real tier above has been observed - it's a
			// real measurement, just not guaranteed to match the game's own
			// column, and unlike the tier it never updates after first probe.
			else if (it != m_verdicts.end() && it->second.probeElapsedMs != ~0ull)
			{
				entry.numericKey = static_cast<long long>(it->second.probeElapsedMs) +
					(it->second.usedRelay ? 2000 : 0);
				entry.keyKnown = true;
			}
			break;
		}
		case RankedListSortMode_ClosestLevel:
		case RankedListSortMode_FurthestLevel:
			if (candidate->internalRankLevel >= 0)
			{
				const long long theirVisible = candidate->internalRankLevel + 1;
				const long long diff = theirVisible - static_cast<long long>(myVisibleRank);
				entry.numericKey = diff >= 0 ? diff : -diff;
				entry.keyKnown = true;
			}
			break;
		case RankedListSortMode_HighestLevel:
		case RankedListSortMode_LowestLevel:
			if (candidate->internalRankLevel >= 0)
			{
				entry.numericKey = candidate->internalRankLevel;
				entry.keyKnown = true;
			}
			break;
		case RankedListSortMode_NameAZ:
		case RankedListSortMode_NameZA:
			if (!candidate->ownerName.empty())
			{
				entry.textKey = candidate->ownerName;
				std::transform(entry.textKey.begin(), entry.textKey.end(), entry.textKey.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				entry.keyKnown = true;
			}
			break;
		default:
			break;
		}

		entries.push_back(entry);
	}

	const bool descending =
		mode == RankedListSortMode_WorstConnection ||
		mode == RankedListSortMode_FurthestLevel ||
		mode == RankedListSortMode_HighestLevel ||
		mode == RankedListSortMode_NameZA;
	const bool textual = mode == RankedListSortMode_NameAZ || mode == RankedListSortMode_NameZA;

	// Unknown-key entries always sink to the end in their original relative
	// order, regardless of direction - "worst connection first" should not
	// reward players we simply haven't measured yet.
	std::stable_sort(entries.begin(), entries.end(),
		[descending, textual](const SortEntry& a, const SortEntry& b)
		{
			if (a.keyKnown != b.keyKnown)
			{
				return a.keyKnown;
			}
			if (!a.keyKnown)
			{
				return false;
			}
			if (textual)
			{
				return descending ? (b.textKey < a.textKey) : (a.textKey < b.textKey);
			}
			return descending ? (b.numericKey < a.numericKey) : (a.numericKey < b.numericKey);
		});

	shown->clear();
	for (const SortEntry& entry : entries)
	{
		shown->push_back(entry.candidate);
	}

	// DIAGNOSTIC: log the final order with each entry's key, so a live test
	// can distinguish "the comparator did something wrong" from "the
	// underlying metric was noisy/unresolved for most entries" - user report
	// (2026-07-11): "still not properly sorted" even after extending the
	// initial hold. See docs/Research/RankedListConnectionFilter_Progress.md.
	std::string orderDump;
	for (size_t i = 0; i < entries.size(); ++i)
	{
		char part[160];
		// Includes ownerName (not just steamId) so a screenshot of the
		// on-screen list can be directly, visually cross-referenced against
		// this log line without needing a steamId lookup - added after the
		// user provided screenshots showing an apparently-unsorted delay
		// column and steamId-only correlation proved ambiguous.
		const char* const name = entries[i].candidate->ownerName.empty() ? "???" : entries[i].candidate->ownerName.c_str();
		if (textual)
		{
			snprintf(part, sizeof(part), " #%zu owner=%llu name=\"%s\" known=%d text=\"%s\"", i,
				static_cast<unsigned long long>(entries[i].candidate->ownerSteamId), name,
				entries[i].keyKnown ? 1 : 0, entries[i].textKey.c_str());
		}
		else
		{
			snprintf(part, sizeof(part), " #%zu owner=%llu name=\"%s\" known=%d key=%lld", i,
				static_cast<unsigned long long>(entries[i].candidate->ownerSteamId), name,
				entries[i].keyKnown ? 1 : 0, entries[i].numericKey);
		}
		orderDump += part;
	}
	LOG(1, "[RankedListFilter] sort order (mode=%d):%s\n", mode, orderDump.c_str());
}

void RankedListConnectionFilter::BuildCompactedListAndDeliver(const char* reason, bool forceDeliver)
{
	const unsigned long long now = GetTickCount64();

	const std::vector<uint64_t> previouslyDelivered = m_reachableLobbies;

	m_reachableLobbies.clear();
	std::vector<const LobbyCandidate*> shownCandidates;
	std::string hiddenNames;
	size_t newlyHiddenCount = 0;
	std::unordered_set<uint64_t> hiddenThisPass;
	// The pipeline (this function) also runs purely for sorting, with the
	// hide-filter checkbox off (see IsPipelineActive) - ShouldHidePeer must
	// never be consulted in that case, or reputation-based hiding silently
	// applies regardless of the checkbox state. Confirmed live: this was
	// previously unconditional, so toggling the checkbox off while a non-
	// default sort mode was active did not actually stop hiding.
	const bool filterEnabled = Settings::settingsIni.enableRankedListConnectionFilter;
	for (const LobbyCandidate& candidate : m_candidates)
	{
		if (!filterEnabled || !ShouldHidePeer(candidate.ownerSteamId, now))
		{
			shownCandidates.push_back(&candidate);
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

	SortShownCandidates(&shownCandidates);
	for (const LobbyCandidate* const candidate : shownCandidates)
	{
		m_reachableLobbies.push_back(candidate->lobbyId);
	}

	// Live-resort calls (forceDeliver=false) only need to actually push a new
	// delivery to the game when the order/membership genuinely changed -
	// otherwise every probe-poll tick would needlessly re-invoke the game's
	// list-rebuild handler.
	const bool orderChanged = m_reachableLobbies != previouslyDelivered;

	m_lastShownCount = m_reachableLobbies.size();
	m_lastHiddenCount = m_candidates.size() - m_reachableLobbies.size();
	LOG(1, "[RankedListFilter] delivering (%s): %zu/%zu lobbies shown, sortMode=%d%s\n",
		reason, m_reachableLobbies.size(), m_candidates.size(),
		Settings::settingsIni.rankedListSortMode,
		(!forceDeliver && !orderChanged) ? " (unchanged, skipping re-delivery)" : "");

	if (newlyHiddenCount > 0 && g_notificationBar != nullptr)
	{
		g_notificationBar->AddNotification("Ranked list filter: hiding %zu unreachable player%s (%s)",
			newlyHiddenCount, newlyHiddenCount == 1 ? "" : "s", hiddenNames.c_str());
	}

	m_hasRemapResult = true;
	m_pipelineState = PipelineState::Idle;
	m_pendingApiCall = 0;

	if (!forceDeliver && !orderChanged)
	{
		return;
	}

	CCallbackBase* const handler = (m_gameLobbyListHandler != nullptr) ? m_gameLobbyListHandler : m_lastGameLobbyListHandler;
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
	// TEMPORARILY DISABLED (2026-07-11) for an isolation test: name-tagged
	// logs showed the row/entry association for this chain rotating on its
	// own roughly once per second with zero real delivery in between - see
	// docs/Research/RankedListConnectionFilter_Progress.md "MAJOR NEW
	// FINDING". Not yet known whether this rotation is a game-internal
	// behavior or caused/worsened by the mod's own repeated calls into this
	// chain. Disabling this call (and the diagnostic below) for one test
	// session to see whether the on-screen list stays visually stable with
	// zero mod interference - re-enable once that's answered either way.
	// PollGameTiers();

	if (m_pipelineState == PipelineState::Idle)
	{
		// A list is already delivered and on screen (or nothing has ever been
		// requested yet, in which case this is a cheap no-op) - see whether
		// newly-resolved probes should reorder/re-hide it live.
		TryLiveResort();
		return;
	}
	if (m_pipelineState != PipelineState::Holding)
	{
		return;
	}
	if (!IsPipelineActive())
	{
		// Both features toggled off mid-hold - deliver everything as-is.
		BuildCompactedListAndDeliver("pipeline disabled mid-hold");
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

void RankedListConnectionFilter::TryLiveResort()
{
	// Nothing delivered yet this session, or both features are off - nothing
	// to keep live.
	if (!m_hasRemapResult || m_candidates.empty() || !IsPipelineActive())
	{
		return;
	}

	// Recomputing shown/hidden/order is cheap (list size is a few dozen at
	// most), but re-invoking the game's own list-rebuild handler is not
	// something to do every single pump - throttle it.
	const unsigned long long now = GetTickCount64();
	constexpr unsigned long long kLiveResortIntervalMs = 750;
	if (now - m_lastLiveResortTickMs < kLiveResortIntervalMs)
	{
		return;
	}
	m_lastLiveResortTickMs = now;

	if (m_lastGameLobbyListHandler == nullptr)
	{
		return; // nothing to re-deliver to yet
	}

	// forceDeliver=false: BuildCompactedListAndDeliver only actually pushes a
	// new delivery when the recomputed shown/hidden/order genuinely changed
	// since last time - this is what makes the list reorder itself live as
	// probes resolve, without waiting for the game's own next auto-refresh.
	BuildCompactedListAndDeliver("live resort", false);
}

bool RankedListConnectionFilter::TryGetRemappedLobby(int index, uint64_t* outLobbyId)
{
	// Serve the remap whenever the pipeline produced one - this drives both
	// hiding (filter) and reordering (sort), which are independent features.
	if (!m_hasRemapResult || outLobbyId == nullptr)
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
		if (verdict.probeElapsedMs == ~0ull)
		{
			verdict.probeElapsedMs = 0; // proven by a real match - best possible standing
		}
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

bool RankedListConnectionFilter::IsPipelineActive() const
{
	return Settings::settingsIni.enableRankedListConnectionFilter ||
		Settings::settingsIni.rankedListSortMode != RankedListSortMode_Default;
}

int RankedListConnectionFilter::CountPopulatedGameRows() const
{
	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	if (moduleBase == 0)
	{
		return 0;
	}

	// DIAGNOSTIC: confirms whether the row container has actually been
	// placement-constructed yet (bit 0 of this flag). If it's unset, the
	// container is all-zero regardless of address correctness.
	const int32_t* const initGuard =
		reinterpret_cast<const int32_t*>(moduleBase + kRowContainerInitGuardRva);
	const bool initGuardReadable = !IsBadReadPtr(initGuard, sizeof(int32_t));
	const bool initDone = initGuardReadable && (*initGuard & 1) != 0;

	const uintptr_t container = moduleBase + kRowContainerRva;

	int count = 0;
	int firstNonZeroRawByte = -1;
	for (int i = 0; i < kMaxRows; ++i)
	{
		const uint8_t* const row = reinterpret_cast<const uint8_t*>(container + kRowArrayOffset + i * kRowStride);
		if (IsBadReadPtr(row, kRowStride))
		{
			continue;
		}
		if (*reinterpret_cast<const int32_t*>(row + kRowOccupied) != 0)
		{
			++count;
		}
		if (firstNonZeroRawByte < 0)
		{
			for (uintptr_t b = 0; b < kRowStride; ++b)
			{
				if (row[b] != 0)
				{
					firstNonZeroRawByte = i;
					break;
				}
			}
		}
	}

	static unsigned long long s_lastRowDiagLogTickMs = 0;
	static int s_lastRowDiagCount = -1;
	const unsigned long long now = GetTickCount64();
	if (count != s_lastRowDiagCount || now - s_lastRowDiagLogTickMs > 2000)
	{
		LOG(1, "[RankedListFilter] rowDiag: initGuardReadable=%d initDone=%d populatedRows=%d firstNonZeroRow=%d\n",
			initGuardReadable ? 1 : 0, initDone ? 1 : 0, count, firstNonZeroRawByte);
		s_lastRowDiagLogTickMs = now;
		s_lastRowDiagCount = count;
	}

	return count;
}

void RankedListConnectionFilter::DiagnosticLogRankedListMgrSlot() const
{
	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	if (moduleBase == 0)
	{
		return;
	}

	static unsigned long long s_lastLogTickMs = 0;
	const unsigned long long now = GetTickCount64();
	if (now - s_lastLogTickMs < 1000)
	{
		return;
	}
	s_lastLogTickMs = now;

	const void* const* const slot =
		reinterpret_cast<const void* const*>(moduleBase + kRankedListMgrSlotRva);
	if (IsBadReadPtr(slot, sizeof(void*)))
	{
		LOG(1, "[RankedListFilter] rankedListMgrDiag: slot unreadable\n");
		return;
	}

	const void* const mgr = *slot;
	if (mgr == nullptr)
	{
		LOG(1, "[RankedListFilter] rankedListMgrDiag: mgr=null\n");
		return;
	}

	// Non-null alone is the key confirmation this pass needs - see progress
	// doc step 1. Also peek the vtable pointer (first field) so a future
	// pass can sanity-check it's a real object, not garbage.
	const void* vtable = nullptr;
	if (!IsBadReadPtr(mgr, sizeof(void*)))
	{
		vtable = *reinterpret_cast<const void* const*>(mgr);
	}

	// Step 2 (progress doc): call the mgr's own vtable slot 7 (+0x1c) the
	// same way the game's FUN_004a7b40 does - thiscall, this=mgr, no
	// explicit args, per the decompile (`(**(code**)(*mgr+0x1c))()`) - to
	// get the row-list struct, then read its +0xae8 count field. This is a
	// real virtual call into game code, but it's the exact same call the
	// game itself performs every frame while the list is drawn, so it's
	// safe to replicate. Deliberately NOT calling the per-row entry's own
	// slot 7 (the actual tier getter) yet - that call takes an unconfirmed
	// explicit argument in the game's code and guessing it risks a crash;
	// see the progress doc for why this step stops here.
	int rowCount = -1;
	std::string tierDump;
	if (vtable != nullptr && !IsBadReadPtr(reinterpret_cast<const uint8_t*>(vtable) + 0x1c, sizeof(void*)))
	{
		typedef void*(__thiscall * GetListStructFn)(void*);
		const GetListStructFn getListStruct =
			*reinterpret_cast<const GetListStructFn*>(reinterpret_cast<uintptr_t>(vtable) + 0x1c);
		if (getListStruct != nullptr)
		{
			void* const listStruct = getListStruct(const_cast<void*>(mgr));
			if (listStruct != nullptr &&
				!IsBadReadPtr(reinterpret_cast<const uint8_t*>(listStruct) + 0xae8, sizeof(int32_t)))
			{
				rowCount = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(listStruct) + 0xae8);

				// TEMPORARILY DISABLED (2026-07-11), per-row loop only - kept
				// the mgr/rowCount read above alive for basic liveness
				// confirmation. Name-tagged logs showed the tier-to-name
				// association from this exact per-row walk (FUN_004a5450 +
				// entry vtable slot 7) rotating on its own roughly once per
				// second with zero real delivery in between - see progress
				// doc "MAJOR NEW FINDING". Disabling this specific loop
				// (the only place besides PollGameTiers, also disabled, that
				// calls into this chain) isolates whether the mod's own
				// repeated calls are causing/worsening the rotation, or
				// whether it's purely game-internal. Re-enable once
				// answered either way.
				const uint8_t* const permutationArray =
					reinterpret_cast<const uint8_t*>(listStruct) + 0xaf4;
				typedef void* (__thiscall * WalkRowListFn)(void*, int32_t);
				const WalkRowListFn walkRowList =
					reinterpret_cast<WalkRowListFn>(moduleBase + kWalkRowListRva);
				typedef uint8_t(__thiscall * GetTierFn)(void*);

				const int rowsToRead = 0; // was: (rowCount <= 0) ? 0 : (rowCount < kMaxDiagnosticRows ? rowCount : kMaxDiagnosticRows);
				for (int row = 0; row < rowsToRead; ++row)
				{
					const int32_t* const permutationSlot =
						reinterpret_cast<const int32_t*>(permutationArray + row * 4);
					if (IsBadReadPtr(permutationSlot, sizeof(int32_t)))
					{
						break;
					}
					const int32_t underlyingIndex = *permutationSlot;

					void* const entry = walkRowList(listStruct, underlyingIndex);
					if (entry == nullptr || IsBadReadPtr(entry, sizeof(void*)))
					{
						continue;
					}
					const void* const entryVtable = *reinterpret_cast<void* const*>(entry);
					if (entryVtable == nullptr ||
						IsBadReadPtr(reinterpret_cast<const uint8_t*>(entryVtable) + 0x1c, sizeof(void*)))
					{
						continue;
					}
					const GetTierFn getTier =
						*reinterpret_cast<const GetTierFn*>(reinterpret_cast<uintptr_t>(entryVtable) + 0x1c);
					if (getTier == nullptr)
					{
						continue;
					}
					const uint8_t tier = getTier(entry);

					// Include the peer's name (not just row index) so a
					// screenshot of the actual on-screen list can be
					// directly, visually cross-referenced line-by-line
					// against this log - added after steamId-only
					// correlation of a user-provided screenshot proved
					// ambiguous. Row-to-name mapping only valid if
					// rowCount==m_reachableLobbies.size(), already required
					// above to even reach this loop.
					const char* name = "???";
					if (static_cast<size_t>(row) < m_reachableLobbies.size())
					{
						const uint64_t lobbyId = m_reachableLobbies[static_cast<size_t>(row)];
						for (const LobbyCandidate& candidate : m_candidates)
						{
							if (candidate.lobbyId == lobbyId)
							{
								if (!candidate.ownerName.empty())
								{
									name = candidate.ownerName.c_str();
								}
								break;
							}
						}
					}

					char part[64];
					snprintf(part, sizeof(part), " [%d]=%u(%s)", row, static_cast<unsigned int>(tier), name);
					tierDump += part;
				}
			}
		}
	}

	LOG(1, "[RankedListFilter] rankedListMgrDiag: mgr=0x%p vtable=0x%p rowCount=%d shownCount=%zu tiers:%s\n",
		mgr, vtable, rowCount, m_lastShownCount, tierDump.c_str());
}

bool RankedListConnectionFilter::IsLobbyListLikelyOpen()
{
	const unsigned long long now = GetTickCount64();

	// DIAGNOSTIC ONLY (does not affect onList below): cross-checks the
	// row-array signal (currently unused for the real decision) against the
	// gstate/state1 signal that IS driving visibility right now.
	CountPopulatedGameRows();
	// DIAGNOSTIC ONLY: logs the candidate ranked-list-manager singleton slot
	// found by tracing the delay-dot render code backward (see progress doc).
	// A non-null read here while a populated list is on screen would confirm
	// the whole chain is live; a persistent null kills this candidate.
	DiagnosticLogRankedListMgrSlot();

	bool onList = false;
	int gstate = -1;
	int state = -1;
	int state1 = -1;

	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	if (moduleBase != 0 && g_gameVals.pGameState != nullptr)
	{
		gstate = *g_gameVals.pGameState;

		const uint8_t* const network = reinterpret_cast<const uint8_t*>(moduleBase + kRankedNetworkStructRva);
		if (!IsBadReadPtr(network, 0x08))
		{
			state = *reinterpret_cast<const int32_t*>(network + 0x00);
			state1 = *reinterpret_cast<const int32_t*>(network + 0x04);

			// state==4's full non-confirmation range also covers screens that
			// are NOT the results list - state1==30 is the pre-search "press
			// search" entry screen (no results on screen at all), reached
			// every time the user backs out of results. Treating all of
			// state==4 as "on list" (the original approach) meant the config
			// window stayed open through that screen too - live testing
			// showed onList=1 continuously for 46s while the user backed out
			// and re-searched repeatedly, only closing once state left 4
			// entirely. RankedAutomationHarness.cpp already reverse-engineered
			// and validated the precise classification for these values
			// (IsRankedSearchEntryMenuState/IsRankedSearchResultsState/
			// IsRankedPostSearchBackState) - state1 in {36,38,39} is
			// specifically "results are rendered on screen", which is the
			// actual signal we want here.
			const bool isSearchResultsState = state1 == 36 || state1 == 38 || state1 == 39;

			// Real list-request activity only happens while actually browsing -
			// once a lobby is joined, no further RequestLobbyList calls fire, so
			// recency of the last one helps exclude later screens too.
			constexpr unsigned long long kListActivityWindowMs = 50000;
			const bool recentListActivity =
				m_lastListRequestTickMs != 0 && now - m_lastListRequestTickMs < kListActivityWindowMs;

			// Additional exclusion: once a real room is joined with an actual
			// opponent (character select onward), IsRoomFunctional() goes true
			// and stays true - live testing showed state1 can stay at 39 (a
			// "results" value) the whole time even after joining, since the
			// automation harness's own state1 classification was designed for
			// progressing FROM results INTO character select, not for telling
			// them apart. IsRoomFunctional() alone isn't enough though: the
			// game also spins up a "functional" room in the background just
			// from sitting on the list (RoomOne/RoomTwo housekeeping, no
			// opponent yet) - confirmed live when the config window closed
			// itself ~8s into a fresh list with zero user input. Requiring an
			// actual other-member entry (the same source the confirmation
			// screen reads) filters that phantom room out.
			const bool inFunctionalRoomWithOpponent =
				g_interfaces.pRoomManager != nullptr && g_interfaces.pRoomManager->IsRoomFunctional() &&
				!g_interfaces.pRoomManager->GetOtherRoomMemberEntriesInCurrentMatch().empty();

			onList = gstate == GameState_MainMenu && state == 4 && isSearchResultsState &&
				recentListActivity && !inFunctionalRoomWithOpponent;
		}
	}

	// DIAGNOSTIC (throttled ~once/sec, or on any change): confirms the state
	// values seen while testing visibility - this let us pin down the
	// confirmation-vs-list discriminator in the first place.
	static unsigned long long s_lastDiagLogTickMs = 0;
	static int s_lastGstate = -2;
	static int s_lastState = -2;
	static int s_lastState1 = -2;
	if (gstate != s_lastGstate || state != s_lastState || state1 != s_lastState1 ||
		now - s_lastDiagLogTickMs > 1000)
	{
		LOG(1, "[RankedListFilter] visibility check: gstate=%d state=%d state1=%d onList=%d\n",
			gstate, state, state1, onList ? 1 : 0);
		s_lastDiagLogTickMs = now;
		s_lastGstate = gstate;
		s_lastState = state;
		s_lastState1 = state1;
	}

	if (onList)
	{
		m_lastRowsPopulatedTickMs = now;
		return true;
	}
	// Short grace period to bridge single-frame reads mid-transition.
	constexpr unsigned long long kGraceMs = 750;
	return m_lastRowsPopulatedTickMs != 0 && now - m_lastRowsPopulatedTickMs < kGraceMs;
}
