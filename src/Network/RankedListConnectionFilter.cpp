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

	// NOTE: earlier versions held the payload before delivering (2s, then 6s
	// for connection sort) to let probes mature. Removed entirely (2026-07-12,
	// per explicit user request for responsiveness): lists now deliver the
	// instant they arrive, and ordering corrections happen live afterward via
	// PollGameListAndApplyOrder()'s in-place permutation rewrites.

	// Reputation lifetimes. A reachable verdict keeps refreshes instant for a
	// while; a probe-confirmed unreachable verdict eventually expires so peers
	// whose network recovered get another chance.
	constexpr unsigned long long kReachableTtlMs = 5 * 60 * 1000;
	constexpr unsigned long long kProbeUnreachableTtlMs = 10 * 60 * 1000;

	// How long a cached real gameTier is trusted for sorting before it's
	// treated as unknown again. Long enough to survive the gap between a
	// brand-new search's delivery and its own row/entry objects resolving
	// (the game only creates those at delivery time, so there's necessarily
	// a lag), short enough that a truly stale reading from long ago (e.g. a
	// player who left and came back much later) doesn't keep winning the
	// sort. See "regressed to fallback-only" section in the progress doc for
	// why an unconditional per-search wipe was tried and reverted instead.
	constexpr unsigned long long kGameTierTtlMs = 45 * 1000;

	// Size of the game's ranked-list row permutation array at listStruct+0xaf4
	// (0x32 entries - both the game's own identity reset FUN_004a5430 and the
	// row renderer FUN_00661060 use exactly 50).
	constexpr int32_t kGamePermSlots = 50;

	// Exact reproduction of the game's own RTT -> Delay-digit bucketing
	// (FUN_004a6620, used by the ranked list row renderer FUN_00661060 to draw
	// the '0'+digit glyph): negative = unresolved (nothing drawn), <60ms = 4,
	// <100ms = 3, <200ms = 2, <300ms = 1, otherwise 0. Higher = better.
	int GameDelayDigitFromRtt(int rttMs)
	{
		if (rttMs < 0) return -1;
		if (rttMs < 0x3C) return 4;
		if (rttMs < 0x64) return 3;
		if (rttMs < 0xC8) return 2;
		if (rttMs < 0x12C) return 1;
		return 0;
	}

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
	// The game resets its own permutation array (identity) as part of search
	// start, so whatever custom order we wrote is gone with it.
	m_gamePermCustomized = false;

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

	// NOTE: an earlier version of this function unconditionally wiped every
	// cached gameTier here on every new list. That fixed one real bug (a
	// stale tier from an EARLIER search winning the sort for a player whose
	// brand-new row hadn't resolved yet) but caused a WORSE regression:
	// the game only creates its row/entry objects at the moment we actually
	// deliver a list (that's what triggers its own GetLobbyByIndex calls),
	// so during the hold period before delivery there is nothing fresh to
	// poll for THIS new list yet - and since only the first delivery of a
	// search ever reaches the screen (later in-place recomputations don't -
	// see "MAJOR FINDING" section in the progress doc), wiping the cache
	// here meant NO delivery ever had real tier data again. Live-confirmed:
	// 0 of 3 real deliveries in one full session used real tier data after
	// this wipe was added. Reverted - staleness is now bounded by
	// PeerVerdict::gameTierTickMs + kGameTierTtlMs in SortShownCandidates
	// instead of an unconditional wipe here.

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
		// NOTE (2026-07-12): an earlier version read the "HOST_NETCOLOR" lobby
		// metadata key here and used it as the connection sort key. That key
		// IS what fills the row entry's +0x74 byte (instruction-level verified
		// against the FUN_0046fcc0 switch jump table) - but +0x74 only drives
		// the row's small colored ICON, not the numeric 0-4 Delay column the
		// user actually compares against. The Delay digit is rendered from
		// entry+0x78 (viewer-measured average RTT, bucketed by FUN_004a6620) -
		// see the progress doc's 2026-07-12 "Delay column source" section.
		// PollGameListAndApplyOrder() now reads that RTT field; no metadata tier read here.
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

	// Deliver immediately, always - no hold. Hiding uses whatever the
	// reputation cache already knows; anything that resolves in the next few
	// seconds (probes, the game's own Delay measurements) reaches the screen
	// through PollGameListAndApplyOrder()'s live permutation rewrites instead
	// of a pre-delivery wait. This removes the 2s/6s search delay entirely,
	// per explicit user request (2026-07-12).
	LOG(1, "[RankedListFilter] lobby list received: %u lobbies, delivering immediately\n",
		static_cast<unsigned int>(m_heldResult.m_nLobbiesMatching));
	BuildCompactedListAndDeliver("immediate");
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

uint8_t* RankedListConnectionFilter::ResolveGameRowListStruct() const
{
	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	if (moduleBase == 0)
	{
		return nullptr;
	}

	const void* const* const slot =
		reinterpret_cast<const void* const*>(moduleBase + kRankedListMgrSlotRva);
	if (IsBadReadPtr(slot, sizeof(void*)))
	{
		return nullptr;
	}
	const void* const mgr = *slot;
	if (mgr == nullptr || IsBadReadPtr(mgr, sizeof(void*)))
	{
		return nullptr;
	}
	const void* const vtable = *reinterpret_cast<const void* const*>(mgr);
	if (vtable == nullptr || IsBadReadPtr(reinterpret_cast<const uint8_t*>(vtable) + 0x1c, sizeof(void*)))
	{
		return nullptr;
	}

	typedef void*(__thiscall * GetListStructFn)(void*);
	const GetListStructFn getListStruct =
		*reinterpret_cast<const GetListStructFn*>(reinterpret_cast<uintptr_t>(vtable) + 0x1c);
	if (getListStruct == nullptr)
	{
		return nullptr;
	}
	void* const listStruct = getListStruct(const_cast<void*>(mgr));
	if (listStruct == nullptr ||
		IsBadReadPtr(reinterpret_cast<const uint8_t*>(listStruct) + 0xae8, sizeof(int32_t)))
	{
		return nullptr;
	}
	return reinterpret_cast<uint8_t*>(listStruct);
}

void RankedListConnectionFilter::WriteIdentityGamePermutation()
{
	uint8_t* const listStruct = ResolveGameRowListStruct();
	if (listStruct == nullptr)
	{
		return;
	}
	int32_t* const perm = reinterpret_cast<int32_t*>(listStruct + 0xaf4);
	if (IsBadWritePtr(perm, sizeof(int32_t) * kGamePermSlots))
	{
		return;
	}
	// Same write the game's own search-start reset (FUN_004a5430) performs:
	// perm[i] = i for all 50 slots.
	for (int32_t i = 0; i < kGamePermSlots; ++i)
	{
		perm[i] = i;
	}
	m_gamePermCustomized = false;
}

void RankedListConnectionFilter::PollGameListAndApplyOrder()
{
	if (!m_hasRemapResult || m_candidates.empty() || !IsPipelineActive())
	{
		// Features were turned off (or nothing delivered): if we left a custom
		// order in the game's permutation array, put it back the way the game
		// expects it, once.
		if (m_gamePermCustomized)
		{
			WriteIdentityGamePermutation();
		}
		return;
	}

	const unsigned long long now = GetTickCount64();
	if (now - m_lastLiveOrderTickMs < 400)
	{
		return;
	}
	m_lastLiveOrderTickMs = now;

	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	uint8_t* const listStruct = ResolveGameRowListStruct();
	if (moduleBase == 0 || listStruct == nullptr)
	{
		return;
	}
	const int32_t rowCount = *reinterpret_cast<const int32_t*>(listStruct + 0xae8);
	const int32_t cap = (rowCount < kGamePermSlots) ? rowCount : kGamePermSlots;
	if (cap <= 0)
	{
		return;
	}
	int32_t* const perm = reinterpret_cast<int32_t*>(listStruct + 0xaf4);
	if (IsBadWritePtr(perm, sizeof(int32_t) * kGamePermSlots))
	{
		return;
	}

	typedef void* (__thiscall * WalkRowListFn)(void*, int32_t);
	const WalkRowListFn walkRowList = reinterpret_cast<WalkRowListFn>(moduleBase + kWalkRowListRva);

	// Pass 1: walk the game's rows by LOGICAL list position (0..count-1, the
	// index space the permutation array maps into) and harvest each row's
	// identity + measured RTT. The identity sub-object pointer (entry+0x114)
	// is set synchronously when the game populates the row (FUN_0046fcc0's
	// first store), so it is available immediately after a delivery; the RTT
	// (entry+0x78, the value behind the on-screen 0-4 Delay digit) resolves
	// asynchronously and reads -1 until then. Both are plain field reads -
	// no virtual calls.
	struct LiveRow
	{
		int32_t logical = 0;
		uint64_t steamId = 0;
	};
	std::vector<LiveRow> rows;
	rows.reserve(static_cast<size_t>(cap));
	for (int32_t logical = 0; logical < cap; ++logical)
	{
		LiveRow row;
		row.logical = logical;

		void* const entry = walkRowList(listStruct, logical);
		if (entry != nullptr && !IsBadReadPtr(entry, sizeof(void*)))
		{
			const uint8_t* const idSubObjPtr = reinterpret_cast<const uint8_t*>(entry) + 0x114;
			if (!IsBadReadPtr(idSubObjPtr, sizeof(void*)))
			{
				const void* const idSubObj = *reinterpret_cast<void* const*>(idSubObjPtr);
				if (idSubObj != nullptr &&
					!IsBadReadPtr(reinterpret_cast<const uint8_t*>(idSubObj) + 0xc, sizeof(uint32_t) * 2))
				{
					const uint32_t idLow = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(idSubObj) + 0xc);
					const uint32_t idHigh = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(idSubObj) + 0x10);
					row.steamId = (static_cast<uint64_t>(idHigh) << 32) | idLow;
				}
			}

			const uint8_t* const rttFieldPtr = reinterpret_cast<const uint8_t*>(entry) + 0x78;
			if (row.steamId != 0 && !IsBadReadPtr(rttFieldPtr, sizeof(int32_t)))
			{
				const int32_t rttMs = *reinterpret_cast<const int32_t*>(rttFieldPtr);
				if (rttMs >= 0)
				{
					PeerVerdict& verdict = m_verdicts[row.steamId];
					verdict.gameRttMs = rttMs;
					verdict.gameTier = GameDelayDigitFromRtt(rttMs);
					verdict.gameTierTickMs = now;
					// Cumulative average kept for diagnostics only (user chose
					// latest-reading sorting).
					if (verdict.gameTierSampleCount <= 0)
					{
						verdict.gameTierAverage = static_cast<double>(rttMs);
					}
					else
					{
						verdict.gameTierAverage =
							(verdict.gameTierAverage * verdict.gameTierSampleCount + rttMs) /
							(verdict.gameTierSampleCount + 1);
					}
					++verdict.gameTierSampleCount;
				}
			}
		}
		rows.push_back(row);
	}

	// Pass 2: current visible order as the stability baseline, from the
	// permutation array as it stands. Invalid/stale/duplicate perm values
	// (possible for one pass right after the game rebuilt the list) fall back
	// to appending the missed logical indices in ascending order.
	std::vector<int32_t> visibleSeq;
	visibleSeq.reserve(static_cast<size_t>(cap));
	std::vector<bool> logicalUsed(static_cast<size_t>(cap), false);
	for (int32_t slot = 0; slot < cap; ++slot)
	{
		const int32_t logical = perm[slot];
		if (logical >= 0 && logical < cap && !logicalUsed[static_cast<size_t>(logical)])
		{
			logicalUsed[static_cast<size_t>(logical)] = true;
			visibleSeq.push_back(logical);
		}
	}
	for (int32_t logical = 0; logical < cap; ++logical)
	{
		if (!logicalUsed[static_cast<size_t>(logical)])
		{
			visibleSeq.push_back(logical);
		}
	}

	// Pass 3: desired visible order. Split the current sequence into
	// [sortable rows] / [rows with no matching candidate] / [hidden-pending
	// rows], sort the first group with the exact same comparator the
	// delivery path uses, and reassemble. Hidden-pending peers (confirmed
	// bad AFTER this list was delivered) sink to the tail instantly - their
	// full removal still happens on the next real delivery.
	const bool filterEnabled = Settings::settingsIni.enableRankedListConnectionFilter;
	std::vector<int32_t> sortableLogicals;
	std::vector<const LobbyCandidate*> sortableCandidates;
	std::vector<int32_t> unmatchedLogicals;
	std::vector<int32_t> hiddenLogicals;
	std::vector<bool> candidateConsumed(m_candidates.size(), false);
	for (const int32_t logical : visibleSeq)
	{
		const uint64_t steamId = rows[static_cast<size_t>(logical)].steamId;
		if (filterEnabled && steamId != 0 && ShouldHidePeer(steamId, now))
		{
			hiddenLogicals.push_back(logical);
			continue;
		}
		// Match this row to one of our delivered candidates by owner steamId,
		// consuming each candidate at most once (two lobbies can share an
		// owner in weird cases; consumption keeps the mapping one-to-one).
		const LobbyCandidate* matched = nullptr;
		if (steamId != 0)
		{
			for (size_t i = 0; i < m_candidates.size(); ++i)
			{
				if (!candidateConsumed[i] && m_candidates[i].ownerSteamId == steamId)
				{
					candidateConsumed[i] = true;
					matched = &m_candidates[i];
					break;
				}
			}
		}
		if (matched != nullptr)
		{
			sortableLogicals.push_back(logical);
			sortableCandidates.push_back(matched);
		}
		else
		{
			unmatchedLogicals.push_back(logical);
		}
	}

	// Sort the matched rows with the shared comparator (quietly - this runs
	// ~2x/sec; we log only when the on-screen order actually changes).
	std::vector<const LobbyCandidate*> sortedCandidates = sortableCandidates;
	SortShownCandidates(&sortedCandidates, false);
	// Map the sorted candidate sequence back to logical indices.
	std::vector<int32_t> desired;
	desired.reserve(static_cast<size_t>(cap));
	{
		std::vector<bool> rowTaken(sortableLogicals.size(), false);
		for (const LobbyCandidate* const candidate : sortedCandidates)
		{
			for (size_t i = 0; i < sortableCandidates.size(); ++i)
			{
				if (!rowTaken[i] && sortableCandidates[i] == candidate)
				{
					rowTaken[i] = true;
					desired.push_back(sortableLogicals[i]);
					break;
				}
			}
		}
	}
	desired.insert(desired.end(), unmatchedLogicals.begin(), unmatchedLogicals.end());
	desired.insert(desired.end(), hiddenLogicals.begin(), hiddenLogicals.end());

	// Pass 4: write the permutation only if it actually changed. Same thread
	// as the renderer/selection logic (the Steam callback pump runs on the
	// game's main thread), so this is frame-atomic - no torn reads possible.
	bool changed = false;
	for (int32_t slot = 0; slot < cap; ++slot)
	{
		if (perm[slot] != desired[static_cast<size_t>(slot)])
		{
			changed = true;
			break;
		}
	}
	if (!changed)
	{
		return;
	}
	for (int32_t slot = 0; slot < cap; ++slot)
	{
		perm[slot] = desired[static_cast<size_t>(slot)];
	}
	for (int32_t slot = cap; slot < kGamePermSlots; ++slot)
	{
		perm[slot] = slot;
	}
	m_gamePermCustomized = true;

	std::string orderDump;
	for (int32_t slot = 0; slot < cap; ++slot)
	{
		const uint64_t steamId = rows[static_cast<size_t>(desired[static_cast<size_t>(slot)])].steamId;
		const char* name = "???";
		int delay = -1;
		for (const LobbyCandidate& candidate : m_candidates)
		{
			if (candidate.ownerSteamId == steamId && !candidate.ownerName.empty())
			{
				name = candidate.ownerName.c_str();
				break;
			}
		}
		const auto it = m_verdicts.find(steamId);
		if (it != m_verdicts.end())
		{
			delay = it->second.gameTier;
		}
		char part[96];
		snprintf(part, sizeof(part), " #%d %s(delay=%d)", slot, name, delay);
		orderDump += part;
	}
	LOG(1, "[RankedListFilter] live order applied (%d rows, %zu hidden-pending):%s\n",
		cap, hiddenLogicals.size(), orderDump.c_str());
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

// NOTE: an earlier attempt read the game's own delay-column row array here
// (container/row/session offsets that RE identified from FUN_004AAAD0). Live
// testing disproved it: the container is confirmed constructed
// (kRowContainerInitGuardRva set) but every row reads as entirely zero bytes
// even with a full, visibly-populated 32-lobby list on screen - that function
// is not what drives this UI. Connection sort uses our own reachability probe
// timing instead (see SortShownCandidates); CountPopulatedGameRows below is
// kept only as a diagnostic cross-check, not a real signal.

void RankedListConnectionFilter::SortShownCandidates(std::vector<const LobbyCandidate*>* shown, bool logOrder) const
{
	const int mode = Settings::settingsIni.rankedListSortMode;
	if (shown == nullptr || shown->size() < 2 ||
		mode <= RankedListSortMode_Default || mode >= RankedListSortMode_COUNT)
	{
		return;
	}

	const unsigned long long now = GetTickCount64();

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
		// Connection modes only: the peer's current on-screen Delay digit
		// (0-4) and raw RTT when the real reading was used; -1 otherwise.
		// Logged so a screenshot's visible Delay column can be compared
		// against the delivered order digit-for-digit.
		int delayDigit = -1;
		int rttMs = -1;
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
			// Prefer the game's own measured per-viewer RTT (the exact value
			// behind the on-screen 0-4 Delay digit - lower = better; see
			// docs/Research/RankedListConnectionFilter_Progress.md 2026-07-12
			// "Delay column source" section) whenever it's been observed
			// recently for this peer, read live by PollGameListAndApplyOrder(). Sorting by
			// the raw RTT rather than the bucketed digit gives a finer order
			// that is still digit-monotonic, so the visible Delay column reads
			// sorted. Uses the LATEST single reading, not the cumulative
			// average - user explicitly chose "match the currently-displayed
			// number" over a smoother but sometimes-stale ranking.
			//
			// Real RTT and the probeElapsedMs fallback are fundamentally
			// different metrics and must never be compared directly. Live
			// testing found exactly this bug class before: values on
			// incompatible scales let a confirmed-worst peer beat every
			// not-yet-measured one. Real-RTT keys therefore live in their own
			// band (around kRealTierKeyBase, always far more negative than any
			// plausible probeElapsedMs value) so ANY resolved real reading
			// sorts as a block ahead of ANY fallback-only peer, in EITHER Best
			// or Worst mode - direction is baked in directly here (via the
			// `worst` bool) rather than via the shared `descending` flip
			// below, because that flip alone would invert the
			// real-vs-fallback bucket priority for Worst mode.
			constexpr long long kRealTierKeyBase = -1000000000LL;
			const bool worst = mode == RankedListSortMode_WorstConnection;
			const auto it = m_verdicts.find(candidate->ownerSteamId);
			if (it != m_verdicts.end() && it->second.gameRttMs >= 0 &&
				it->second.gameTierSampleCount > 0 &&
				now - it->second.gameTierTickMs < kGameTierTtlMs)
			{
				// rttMs is realistically < ~100000, so the key stays deep
				// inside the negative band in both directions.
				const long long rtt = static_cast<long long>(it->second.gameRttMs);
				entry.numericKey = kRealTierKeyBase + (worst ? -rtt : rtt);
				entry.keyKnown = true;
				entry.delayDigit = it->second.gameTier;
				entry.rttMs = it->second.gameRttMs;
			}
			// Fallback: the mod's own reachability-probe establishment time,
			// only used until the real tier above has been observed - it's a
			// real measurement, just not guaranteed to match the game's own
			// column, and unlike the tier it never updates after first probe.
			else if (it != m_verdicts.end() && it->second.probeElapsedMs != ~0ull)
			{
				const long long probeKey = static_cast<long long>(it->second.probeElapsedMs) +
					(it->second.usedRelay ? 2000 : 0);
				entry.numericKey = worst ? -probeKey : probeKey;
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

	// RankedListSortMode_WorstConnection deliberately excluded here - its
	// direction is baked directly into numericKey above (see the `worst`
	// bool in the connection-mode case), because a simple ascending/
	// descending flip alone would invert the real-tier-vs-fallback bucket
	// priority (see the comment there for why).
	const bool descending =
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
	// Suppressed for the high-frequency live-permutation path (logOrder=false),
	// which logs only when the on-screen order actually changes.
	if (!logOrder)
	{
		return;
	}
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
		else if (entries[i].delayDigit >= 0)
		{
			// Connection mode with a real game reading: include the on-screen
			// Delay digit and raw RTT so screenshots can be cross-checked
			// digit-for-digit against this delivered order.
			snprintf(part, sizeof(part), " #%zu owner=%llu name=\"%s\" known=%d key=%lld delay=%d rtt=%d", i,
				static_cast<unsigned long long>(entries[i].candidate->ownerSteamId), name,
				entries[i].keyKnown ? 1 : 0, entries[i].numericKey,
				entries[i].delayDigit, entries[i].rttMs);
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

void RankedListConnectionFilter::BuildCompactedListAndDeliver(const char* reason)
{
	const unsigned long long now = GetTickCount64();

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

	m_lastShownCount = m_reachableLobbies.size();
	m_lastHiddenCount = m_candidates.size() - m_reachableLobbies.size();
	LOG(1, "[RankedListFilter] delivering (%s): %zu/%zu lobbies shown, sortMode=%d\n",
		reason, m_reachableLobbies.size(), m_candidates.size(),
		Settings::settingsIni.rankedListSortMode);

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

	// The game's own permutation array is reset only at SEARCH START (its
	// FUN_004a5430 identity reset), never per delivery - so a custom order we
	// wrote for the PREVIOUS list would still be in place when the game
	// rebuilds its rows from this delivery, scrambling it. Restore identity
	// right here, same call stack, before any render/selection code can run
	// (everything is on the game's main thread), then let the live pass
	// re-apply the proper order within ~400ms.
	WriteIdentityGamePermutation();
	m_lastLiveOrderTickMs = 0; // let the live pass run on the very next pump
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

	if (m_pipelineState == PipelineState::Idle)
	{
		// A list is already delivered and on screen (or nothing has ever been
		// requested yet, in which case this is a cheap no-op). Harvest each
		// row's Delay RTT and rewrite the game's own row permutation array in
		// place if the desired order changed - this is what makes the visible
		// list reorder itself live as measurements resolve, with no
		// re-delivery. (An earlier mechanism re-invoked the game's CCallResult
		// handler Run() here - proven to be a complete no-op, see progress doc
		// "MAJOR FINDING": the game reads the list exactly once per genuine
		// delivery. The permutation rewrite is the mechanism that actually
		// reaches the screen.)
		PollGameListAndApplyOrder();
	}
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

				// Per-row tier-vs-name correlation loop stays DISABLED (see
				// "MAJOR NEW FINDING" in the progress doc - that correlation
				// is proven unreliable). Re-enabled just enough rows here
				// (kIdentityProbeRowCount) for a NARROWER, SAFER purpose:
				// this exact walk (FUN_004a5450 + entry vtable slot 7) has
				// been called from the mod's own code across many prior
				// sessions with zero crashes - only a live CDB breakpoint on
				// the underlying game function ever crashed the game (see
				// progress doc "live CDB debugging attempts" - two crashes,
				// both from setting an actual breakpoint, never from the
				// mod's own calls). So it's safe to also peek the entry's own
				// vtable pointer here (already read as entryVtable below,
				// previously discarded after use) and log its RVA, to
				// identify the entry's concrete class statically via Ghidra
				// - entirely without a live debugger.
				const uint8_t* const permutationArray =
					reinterpret_cast<const uint8_t*>(listStruct) + 0xaf4;
				typedef void* (__thiscall * WalkRowListFn)(void*, int32_t);
				const WalkRowListFn walkRowList =
					reinterpret_cast<WalkRowListFn>(moduleBase + kWalkRowListRva);
				typedef uint8_t(__thiscall * GetTierFn)(void*);

				constexpr int kIdentityProbeRowCount = 10;
				const int rowsToRead = (rowCount <= 0) ? 0 : (rowCount < kIdentityProbeRowCount ? rowCount : kIdentityProbeRowCount);
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

					// Entry vtable RVA (address - moduleBase), computed and
					// logged entirely from within the mod's own already-safe
					// call chain - no live debugger needed. This is what
					// lets the entry's concrete class be identified
					// statically in Ghidra (0x00400000 + this RVA), to
					// check its fields/other vtable slots for an identity
					// value (steamId or similar) instead of relying on the
					// now-disproven positional row/name mapping.
					const uintptr_t entryVtableRva =
						reinterpret_cast<uintptr_t>(entryVtable) - moduleBase;

					// Candidate identity key (progress doc: "ENTRY vtable
					// resolved via static Ghidra analysis" section) - a
					// plain pointer chase, no virtual call, no calling-
					// convention risk. entry+0x114 is a sub-object pointer,
					// null until the game's own connection-quality resolver
					// (FUN_0046db40) runs for this row at least once; its
					// +0xc/+0x10 dwords are the same 2-dword key shape the
					// game itself uses elsewhere to correlate this row with
					// an established P2P connection (leading hypothesis:
					// the peer's Steam64 ID, not yet proven bit-for-bit).
					// Logged for correlation only - not used for any real
					// decision yet.
					uint32_t idLow = 0;
					uint32_t idHigh = 0;
					bool idKnown = false;
					const uint8_t* const idSubObjPtr = reinterpret_cast<const uint8_t*>(entry) + 0x114;
					if (!IsBadReadPtr(idSubObjPtr, sizeof(void*)))
					{
						const void* const idSubObj = *reinterpret_cast<void* const*>(idSubObjPtr);
						if (idSubObj != nullptr &&
							!IsBadReadPtr(reinterpret_cast<const uint8_t*>(idSubObj) + 0xc, sizeof(uint32_t) * 2))
						{
							idLow = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(idSubObj) + 0xc);
							idHigh = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(idSubObj) + 0x10);
							idKnown = true;
						}
					}

					// The +0x78 measured-RTT field that actually drives the
					// on-screen 0-4 Delay digit (see progress doc 2026-07-12
					// "Delay column source") - logged alongside the +0x74
					// icon tier so future sessions can cross-check both.
					int32_t rttMs = -1;
					const uint8_t* const rttPtr = reinterpret_cast<const uint8_t*>(entry) + 0x78;
					if (!IsBadReadPtr(rttPtr, sizeof(int32_t)))
					{
						rttMs = *reinterpret_cast<const int32_t*>(rttPtr);
					}
					const int delayDigit = GameDelayDigitFromRtt(rttMs);

					char part[200];
					if (idKnown)
					{
						snprintf(part, sizeof(part), " [%d]=icon:%u,delay:%d,rtt:%d(%s,vtableRva=0x%zx,id=%08x%08x)", row,
							static_cast<unsigned int>(tier), delayDigit, rttMs, name,
							static_cast<size_t>(entryVtableRva), idHigh, idLow);
					}
					else
					{
						snprintf(part, sizeof(part), " [%d]=icon:%u,delay:%d,rtt:%d(%s,vtableRva=0x%zx,id=unresolved)", row,
							static_cast<unsigned int>(tier), delayDigit, rttMs, name,
							static_cast<size_t>(entryVtableRva));
					}
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
