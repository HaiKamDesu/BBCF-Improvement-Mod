#include "RankedListConnectionFilter.h"

#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Game/gamestates.h"
#include "Network/RoomManager.h"
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

	// Periodic whole-list reachability recheck (2026-07-12 rework): every
	// interval, every listed candidate - hidden ones included - gets a fresh
	// P2P probe regardless of verdict freshness (session-blocked peers
	// excepted). A peer whose network recovered flips back to Reachable when
	// the probe lands, and the live pass restores their row within ~400ms;
	// one that is still dead just re-confirms. This is what makes hide AND
	// restore fully live instead of "hidden until some long TTL expires".
	constexpr unsigned long long kListRecheckIntervalMs = 15 * 1000;

	// Size of the game's ranked-list row permutation array at listStruct+0xaf4
	// (0x32 entries - both the game's own identity reset FUN_004a5430 and the
	// row renderer FUN_00661060 use exactly 50).
	constexpr int32_t kGamePermSlots = 50;

	// GAMESTEAM_SearchResultNode payload region for live hide/restore swaps:
	// the node is 0x118 bytes (operator_new size in its constructor
	// FUN_0046f680); +0 is the vtable pointer and +4/+8 are the intrusive
	// list next/prev links, all of which must stay put - everything from +0xC
	// up (the exact range the field-initializer FUN_0046dc00 zeroes, plus the
	// +0x114 identity sub-object pointer) is per-row payload and moves as a
	// unit.
	constexpr size_t kNodePayloadOffset = 0xC;
	constexpr size_t kNodePayloadSize = 0x118 - kNodePayloadOffset;

	// The ranked search-result list WIDGET - the UI object owning the
	// scrollbar, cursor and 50 row slots. Built by the game's FUN_0064bfb0
	// ("NetworkRankMatchSearchResultWindow"): a lazy-init UI context singleton
	// (FUN_00643b40, static at RVA 0xEF1ED0, guard bit RVA 0xEF4898) holds a
	// pool pointer at +0x29C4 -> 4 containers of stride 0x15D90 (in-use flag
	// +0x8C, config-string pointer +0x90); the ranked one's config pointer is
	// the static "Rank Match Search Result" string (RVA 0x566238). The widget
	// struct itself sits at container+0x68:
	//   +0x3C     scroll top (first visible row)
	//   +0x40     slot array, 50 x 0x6FC (slot+0x4 = active flag)
	//   +0x15D78  cursor (selected row)
	//   +0x15D80  bottom visible row
	//   +0x15D84  page size - 1 (builder clamps to 10 -> 11 visible rows)
	//   +0x15D88  item count (bounds cursor wrap and the scrollbar)
	// Semantics recovered from the widget's own member functions (cursor-next
	// 0x00648DF0, cursor-prev 0x00648ED0, AddItem 0x006480C0) and the
	// builder's tail (0x0064C126: 15D84/15D80 = min(count-1, 10)).
	constexpr uintptr_t kUiContextRva = 0xEF1ED0;
	constexpr uintptr_t kUiContextGuardRva = 0xEF4898;
	constexpr uintptr_t kUiWidgetPoolOffset = 0x29C4;
	constexpr uintptr_t kUiContainerStride = 0x15D90;
	constexpr uintptr_t kUiContainerInUseOffset = 0x8C;
	constexpr uintptr_t kUiContainerConfigOffset = 0x90;
	constexpr uintptr_t kRankedResultConfigStrRva = 0x566238;
	constexpr uintptr_t kUiContainerWidgetOffset = 0x68;
	constexpr uintptr_t kWidgetScrollTopOffset = 0x3C;
	constexpr uintptr_t kWidgetSlotArrayOffset = 0x40;
	constexpr uintptr_t kWidgetSlotStride = 0x6FC;
	constexpr uintptr_t kWidgetCursorOffset = 0x15D78;
	constexpr uintptr_t kWidgetBottomRowOffset = 0x15D80;
	constexpr uintptr_t kWidgetPageM1Offset = 0x15D84;
	constexpr uintptr_t kWidgetCountOffset = 0x15D88;
	constexpr int32_t kWidgetMaxPageM1 = 10;

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

	// A real connection failure hides the peer only briefly - live testing
	// showed one-off transient failures happen to otherwise-fine players, and
	// the periodic whole-list recheck (kListRecheckIntervalMs) means everyone
	// gets a clean-slate re-test regularly anyway. (An earlier design also
	// blocked repeat offenders for the whole session - removed 2026-07-12 per
	// user direction: with continuous rechecking, permanent penalties are
	// unnecessary and only create stale hides.)
	constexpr unsigned long long kReactiveFailHideMs = 2 * 60 * 1000;

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
	LOG(1, "[RankedListFilter] %s steamId=%llu name=\"%s\" - fail #%d (hidden temporarily)\n",
		reason, static_cast<unsigned long long>(steamId), name, verdict.reactiveFailCount);

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
	// Restart the periodic reachability recheck cycle relative to the new
	// search's own delivery-time probes.
	m_lastListRecheckTickMs = 0;
	m_restoreExemptions.clear();
	m_liveHiddenPeers.clear();

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

void RankedListConnectionFilter::StartProbeIfNeeded(uint64_t steamId, bool force)
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
		// force (the periodic whole-list recheck) skips the freshness gates:
		// the whole point is re-measuring peers whose verdict is still
		// within TTL, in both directions (restore recovered peers quickly,
		// re-confirm dead ones).
		if (!force)
		{
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
			// Keep the FIRST establishment time - the periodic whole-list
			// recheck re-probes sessions that are often still open, which
			// resolves in one pump (~16ms) and would otherwise flatten the
			// fallback sort metric for every reachable peer.
			if (verdict.probeElapsedMs == ~0ull)
			{
				verdict.probeElapsedMs = now - it->second;
			}
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

uint8_t* RankedListConnectionFilter::ResolveRankedResultWidget() const
{
	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	if (moduleBase == 0)
	{
		return nullptr;
	}
	// Lazy-init guard first - never touch the context before the game built it.
	const uint8_t* const guard = reinterpret_cast<const uint8_t*>(moduleBase + kUiContextGuardRva);
	if (IsBadReadPtr(guard, 1) || (*guard & 1) == 0)
	{
		return nullptr;
	}
	const uint8_t* const ctx = reinterpret_cast<const uint8_t*>(moduleBase + kUiContextRva);
	if (IsBadReadPtr(ctx + kUiWidgetPoolOffset, sizeof(void*)))
	{
		return nullptr;
	}
	uint8_t* const pool = *reinterpret_cast<uint8_t* const*>(ctx + kUiWidgetPoolOffset);
	if (pool == nullptr)
	{
		return nullptr;
	}
	const void* const rankedConfigStr =
		reinterpret_cast<const void*>(moduleBase + kRankedResultConfigStrRva);

	for (int i = 0; i < 4; ++i)
	{
		uint8_t* const container = pool + static_cast<uintptr_t>(i) * kUiContainerStride;
		if (IsBadReadPtr(container, kUiContainerConfigOffset + sizeof(void*)))
		{
			return nullptr;
		}
		if (*reinterpret_cast<const int32_t*>(container + kUiContainerInUseOffset) == 0 ||
			*reinterpret_cast<void* const*>(container + kUiContainerConfigOffset) != rankedConfigStr)
		{
			continue;
		}

		uint8_t* const widget = container + kUiContainerWidgetOffset;
		if (IsBadWritePtr(widget + kWidgetCountOffset, sizeof(int32_t)))
		{
			return nullptr;
		}
		return widget;
	}
	return nullptr;
}

void RankedListConnectionFilter::FixupRankedResultWidget(int32_t shownCount)
{
	if (shownCount < 0)
	{
		return;
	}
	uint8_t* const widget = ResolveRankedResultWidget();
	if (widget == nullptr)
	{
		return;
	}
	{
		int32_t* const count = reinterpret_cast<int32_t*>(widget + kWidgetCountOffset);
		int32_t* const pageM1 = reinterpret_cast<int32_t*>(widget + kWidgetPageM1Offset);
		int32_t* const cursor = reinterpret_cast<int32_t*>(widget + kWidgetCursorOffset);
		int32_t* const bottom = reinterpret_cast<int32_t*>(widget + kWidgetBottomRowOffset);
		int32_t* const scrollTop = reinterpret_cast<int32_t*>(widget + kWidgetScrollTopOffset);

		const int32_t cap = (shownCount < kGamePermSlots) ? shownCount : kGamePermSlots;
		if (*count == cap)
		{
			return; // already consistent - the common case, nothing to write
		}
		LOG(1, "[RankedListFilter] widget fixup: count %d -> %d (cursor=%d top=%d)\n",
			*count, cap, *cursor, *scrollTop);
		*count = cap;
		const int32_t lastIndex = cap > 0 ? cap - 1 : 0;
		const int32_t newPageM1 = (lastIndex < kWidgetMaxPageM1) ? lastIndex : kWidgetMaxPageM1;
		*pageM1 = newPageM1;
		if (*cursor > lastIndex)
		{
			*cursor = lastIndex;
		}
		if (*cursor < 0)
		{
			*cursor = 0;
		}
		// Keep the visible window valid: top in [0, count-1-pageM1], bottom =
		// top + pageM1, cursor within [top, bottom].
		int32_t maxTop = lastIndex - newPageM1;
		if (maxTop < 0)
		{
			maxTop = 0;
		}
		if (*scrollTop > maxTop)
		{
			*scrollTop = maxTop;
		}
		if (*scrollTop < 0)
		{
			*scrollTop = 0;
		}
		if (*cursor < *scrollTop)
		{
			*scrollTop = *cursor;
		}
		if (*cursor > *scrollTop + newPageM1)
		{
			*scrollTop = *cursor - newPageM1;
		}
		*bottom = *scrollTop + newPageM1;

		// Per-slot active flags: exactly the rows 0..count-1, like the
		// builder leaves them. (Re-activated slots past a game rebuild that
		// used a shrunk count get their info-string pointer refreshed by the
		// game's own next rebuild; the renderer reads row content live from
		// the entries, not from the slots.)
		for (int32_t slot = 0; slot < kGamePermSlots; ++slot)
		{
			uint8_t* const slotPtr = widget + kWidgetSlotArrayOffset +
				static_cast<uintptr_t>(slot) * kWidgetSlotStride;
			if (IsBadWritePtr(slotPtr + 4, sizeof(int32_t)))
			{
				break;
			}
			*reinterpret_cast<int32_t*>(slotPtr + 4) = (slot < cap) ? 1 : 0;
		}
		return;
	}
}

namespace
{
	// The live row permutation/payload swaps below are only safe while the user is
	// actually BROWSING the results list: state==4 && state1 in {36,38,39}
	// (RankedAutomationHarness::IsRankedSearchResultsState). Every other state1 in
	// the ranked flow is a window where the game is resolving or re-resolving a
	// clicked row through these same structures:
	//   - 40-42: click pressed, state machine still latching perm[cursor] into the
	//     connect request (FUN_004a89d0 runs from a later state-machine tick, not
	//     the input frame itself - state1=42 observed live between 39 and 43).
	//   - 43-48: the connect/confirmation resolution band (the ONLY band the
	//     previous fix froze - too narrow, reproduced wrong-player again
	//     2026-07-19 22:09, DEBUG.txt: clicked HYUCKUMEN, popup showed
	//     castroluisangel99).
	//   - 30/31/34: entry menu / post-confirm screens. The list is not visible,
	//     deliveries still arrive in the background, and the connect flow keeps
	//     re-resolving its latched row index every frame - mutating here can
	//     retarget a pending connection to a different player.
	// So: freeze in ALL ranked states except the browsable-list band. When the
	// struct is unreadable or state!=4 (not in the ranked flow at all), don't
	// freeze - preserves the pre-existing cleanup behavior outside ranked.
	bool IsRankedListSafeToMutate()
	{
		const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
		if (moduleBase == 0)
		{
			return true;
		}

		const uint8_t* const network = reinterpret_cast<const uint8_t*>(moduleBase + kRankedNetworkStructRva);
		if (IsBadReadPtr(network, 0x08))
		{
			return true;
		}

		const int32_t state = *reinterpret_cast<const int32_t*>(network + 0x00);
		const int32_t state1 = *reinterpret_cast<const int32_t*>(network + 0x04);
		if (state != 4)
		{
			return true;
		}
		return state1 == 36 || state1 == 38 || state1 == 39;
	}
}

void RankedListConnectionFilter::PollGameListAndApplyOrder()
{
	const bool safeToMutate = IsRankedListSafeToMutate();
	if (m_liveOrderFrozen == safeToMutate)
	{
		// Log freeze engage/release once per transition, not per 400ms tick -
		// and only while the pipeline is actually doing something, so menu
		// navigation with the features off doesn't spam the log.
		m_liveOrderFrozen = !safeToMutate;
		if (m_hasRemapResult && IsPipelineActive())
		{
			LOG(1, "[RankedListFilter] live mutations %s (list %s browsable)\n",
				m_liveOrderFrozen ? "FROZEN" : "resumed",
				m_liveOrderFrozen ? "not" : "is");
		}
	}
	if (!safeToMutate)
	{
		// A row click is being resolved (or a connect is pending, or the list
		// screen simply isn't the one on screen) - leave the permutation, node
		// payloads, row count and widget untouched so whatever row memory the
		// game reads back matches what the user actually clicked.
		return;
	}

	if (!m_hasRemapResult || m_candidates.empty() || !IsPipelineActive())
	{
		// Features were turned off (or nothing delivered): put everything
		// back the way the game expects it, once - identity permutation,
		// the full game-authored row count, and the widget bounds. All-live
		// filtering means turning the features off restores the full list
		// immediately, no refresh needed.
		if (m_gamePermCustomized)
		{
			WriteIdentityGamePermutation();
			if (m_gameListOrigCount > 0)
			{
				uint8_t* const listStructOff = ResolveGameRowListStruct();
				if (listStructOff != nullptr &&
					!IsBadWritePtr(listStructOff + 0xae8, sizeof(int32_t)))
				{
					*reinterpret_cast<int32_t*>(listStructOff + 0xae8) = m_gameListOrigCount;
					m_gameListLastCountWritten = m_gameListOrigCount;
					FixupRankedResultWidget(m_gameListOrigCount);
				}
			}
			m_liveHiddenPeers.clear();
			m_lastHiddenCount = 0;
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
	int32_t* const countField = reinterpret_cast<int32_t*>(listStruct + 0xae8);
	if (IsBadWritePtr(countField, sizeof(int32_t)))
	{
		return;
	}
	const int32_t countRead = *countField;

	// --- Count resync: learn the game-authored full row count. ---
	// The game's populate pass (FUN_0046d890) rewrites node payloads and the
	// count on its own tick shortly after each delivery (and again on
	// spontaneous repopulates, e.g. lobby-metadata updates re-running it from
	// the cached records). Any count value we did not write ourselves is
	// game-authored and defines the true full list size. While a resync is
	// pending right after a delivery, the count may still be a stale value
	// from before the populate pass - hands off until it is unambiguous.
	if (m_gameListResyncPending)
	{
		if (countRead != m_gameListLastCountWritten || now - m_lastDeliveryTickMs > 1500)
		{
			// Either the populate pass visibly ran (count differs from
			// whatever we last wrote), or enough time passed that it
			// certainly ran and its kept-count merely coincides with the old
			// value. Both ways the current count is game-authored.
			m_gameListOrigCount = countRead;
			m_gameListLastCountWritten = countRead;
			m_gameListResyncPending = false;
		}
		else
		{
			return;
		}
	}
	else if (countRead != m_gameListLastCountWritten)
	{
		// The game changed the count outside a delivery we know about
		// (spontaneous repopulate, or the search-start clear zeroing it).
		// Re-learn it. A spontaneous repopulate also resurrects any rows we
		// had hidden - the partition below re-hides them within this pass.
		m_gameListOrigCount = countRead;
		m_gameListLastCountWritten = countRead;
	}

	const int32_t orig = (m_gameListOrigCount < kGamePermSlots) ? m_gameListOrigCount : kGamePermSlots;
	if (orig <= 0)
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

	// Pass 1: walk ALL delivered rows (0..orig-1, including any currently
	// parked past a shrunk count - the node chain is a physical pool the
	// walker traverses without bounds checks, and tail nodes still hold the
	// payloads parked there) and harvest node pointer, Steam64 identity
	// (entry+0x114 -> +0xc/+0x10, attached synchronously at populate) and
	// Delay RTT (entry+0x78, resolves asynchronously, -1 until then).
	struct LiveRow
	{
		uint8_t* node = nullptr;
		uint64_t steamId = 0;
		int32_t rttMs = -1;        // entry+0x78, -1 until the game resolves it
		int16_t rttFilter = 0;     // entry+0x10a, RANK_RTT_FILTER (host requirement)
		int16_t areaFilter = 0;    // entry+0x5e, RANK_AREA_FILTER (secondary requirement)
		uint8_t netColor = 0xFF;   // entry+0x74, HOST_NETCOLOR - the row's native square icon
	};
	// Cursor follow, part 1: remember which PHYSICAL row (node-chain position)
	// the widget cursor currently sits on, before any payload moves. Without
	// this, the cursor keeps its raw slot INDEX across the partition/re-sort
	// below, so the highlighted player silently changes under the user right
	// as they navigate/confirm - the other half of the wrong-player-selected
	// bug (the user aims at one name, the click resolves to another).
	uint8_t* const resultWidget = ResolveRankedResultWidget();
	int32_t cursorSlotBefore = -1;
	int32_t cursorPhysBefore = -1;
	if (resultWidget != nullptr &&
		!IsBadReadPtr(resultWidget + kWidgetCursorOffset, sizeof(int32_t)))
	{
		const int32_t cur = *reinterpret_cast<const int32_t*>(resultWidget + kWidgetCursorOffset);
		if (cur >= 0 && cur < orig && perm[cur] >= 0 && perm[cur] < orig)
		{
			cursorSlotBefore = cur;
			cursorPhysBefore = perm[cur];
		}
	}

	std::vector<LiveRow> rows(static_cast<size_t>(orig));
	for (int32_t logical = 0; logical < orig; ++logical)
	{
		LiveRow& row = rows[static_cast<size_t>(logical)];
		void* const entry = walkRowList(listStruct, logical);
		if (entry == nullptr || IsBadReadPtr(entry, 0x118))
		{
			continue;
		}
		row.node = reinterpret_cast<uint8_t*>(entry);

		const void* const idSubObj = *reinterpret_cast<void* const*>(row.node + 0x114);
		if (idSubObj != nullptr &&
			!IsBadReadPtr(reinterpret_cast<const uint8_t*>(idSubObj) + 0xc, sizeof(uint32_t) * 2))
		{
			const uint32_t idLow = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(idSubObj) + 0xc);
			const uint32_t idHigh = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(idSubObj) + 0x10);
			row.steamId = (static_cast<uint64_t>(idHigh) << 32) | idLow;
		}

		row.rttMs = *reinterpret_cast<const int32_t*>(row.node + 0x78);
		row.rttFilter = *reinterpret_cast<const int16_t*>(row.node + 0x10a);
		row.areaFilter = *reinterpret_cast<const int16_t*>(row.node + 0x5e);
		row.netColor = *reinterpret_cast<const uint8_t*>(row.node + 0x74);

		if (row.steamId != 0)
		{
			const int32_t rttMs = row.rttMs;
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

	// Pass 2: capture the current visible sequence as a stability baseline
	// BEFORE any payload moves. Tracked as pre-move logical indices;
	// `newPosOf` translates them after the partition. Invalid/stale/duplicate
	// perm values fall back to appending the missed indices in ascending
	// order.
	std::vector<int32_t> baselineSeq;
	baselineSeq.reserve(static_cast<size_t>(orig));
	{
		std::vector<bool> used(static_cast<size_t>(orig), false);
		for (int32_t slot = 0; slot < orig; ++slot)
		{
			const int32_t logical = perm[slot];
			if (logical >= 0 && logical < orig && !used[static_cast<size_t>(logical)])
			{
				used[static_cast<size_t>(logical)] = true;
				baselineSeq.push_back(logical);
			}
		}
		for (int32_t logical = 0; logical < orig; ++logical)
		{
			if (!used[static_cast<size_t>(logical)])
			{
				baselineSeq.push_back(logical);
			}
		}
	}

	// Pass 3: LIVE HIDE/RESTORE. Rows whose peer should currently be hidden
	// get their node payloads swapped to the logical tail, and the game's row
	// count is shrunk so positions 0..shownCount-1 form a consistent, fully
	// game-shaped shorter list (identical to what the game's own populate
	// pass produces when a refresh returns fewer lobbies - a vanilla-
	// exercised shape). Payload swaps move bytes 0xC..0x117 of the 0x118-byte
	// GAMESTEAM_SearchResultNode and leave the vtable (+0) and the intrusive
	// list links (+4/+8) in place, so the node chain and the walker's cursor
	// cache stay untouched. Restores are the same mechanism in reverse: a
	// peer no longer hidden simply stops being partitioned out, the count
	// grows back, and their parked payload re-enters the visible region.
	const bool filterEnabled = Settings::settingsIni.enableRankedListConnectionFilter;
	// Network tier filter: 0 = All (off), 1-3 = "N and above", 4 = "4 only".
	// Hides rows whose CURRENT Delay digit is below the floor. Rows whose RTT
	// hasn't resolved yet (digit unknown) are shown - benefit of the doubt,
	// they get hidden within ~400ms of the digit resolving if it's too low.
	int networkFloor = Settings::settingsIni.rankedListNetworkFilter;
	if (networkFloor < 0 || networkFloor > 4)
	{
		networkFloor = 0;
	}
	// Requirement filter: replicate the game's own join gate
	// (FUN_004ae6d0 case 0x27) that produces "The room's connectivity
	// requirements are not met" - the row's RANK_RTT_FILTER (entry+0x10a,
	// required = 1..4, anything else nonzero = 4) or, failing that, its
	// RANK_AREA_FILTER (entry+0x5e, 1 -> requires digit 2, 2 -> requires
	// digit 3) is compared against OUR measured Delay digit to that host.
	// Rows we'd be rejected from are hidden preemptively. Unresolved RTT
	// (digit unknown) rows are shown - the game itself waits in
	// "RMSR_CheckingRTT" rather than rejecting in that state.
	const bool hideUnmet = Settings::settingsIni.hideUnmetRequirementRooms;
	std::vector<bool> hiddenAt(static_cast<size_t>(orig), false);
	m_liveHiddenPeers.clear();
	for (int32_t logical = 0; logical < orig; ++logical)
	{
		const LiveRow& row = rows[static_cast<size_t>(logical)];
		bool hide = false;
		HiddenReason reason = HiddenReason::Unreachable;
		// A user-restored peer is exempt from the rule-based filters until
		// the next periodic recheck, so the restore actually sticks for a
		// visible while instead of being undone 400ms later.
		const bool restoreExempt =
			row.steamId != 0 && m_restoreExemptions.find(row.steamId) != m_restoreExemptions.end();
		if (filterEnabled && row.steamId != 0 && ShouldHidePeer(row.steamId, now))
		{
			hide = true;
			const auto verdictIt = m_verdicts.find(row.steamId);
			reason = (verdictIt != m_verdicts.end() && verdictIt->second.reactiveFailCount > 0)
				? HiddenReason::ConnectionFailed : HiddenReason::Unreachable;
		}
		const int delayDigit = GameDelayDigitFromRtt(row.rttMs);
		if (!hide && !restoreExempt && networkFloor > 0 && delayDigit >= 0 && delayDigit < networkFloor)
		{
			hide = true;
			reason = HiddenReason::NetworkFilter;
		}
		if (!hide && !restoreExempt && hideUnmet && delayDigit >= 0)
		{
			int requiredDigit = 0;
			if (row.rttFilter != 0)
			{
				requiredDigit = (row.rttFilter >= 1 && row.rttFilter <= 4)
					? row.rttFilter : 4;
			}
			else if (row.areaFilter == 1)
			{
				requiredDigit = 2;
			}
			else if (row.areaFilter == 2)
			{
				requiredDigit = 3;
			}
			if (delayDigit < requiredDigit)
			{
				hide = true;
				reason = HiddenReason::Requirement;
			}
		}
		hiddenAt[static_cast<size_t>(logical)] = hide;
		if (hide && row.steamId != 0)
		{
			HiddenPeerInfo info;
			info.steamId = row.steamId;
			info.reason = reason;
			info.netColor = row.netColor;
			info.delayDigit = delayDigit;
			const auto verdictIt = m_verdicts.find(row.steamId);
			if (verdictIt != m_verdicts.end() && !verdictIt->second.lastKnownName.empty())
			{
				info.name = verdictIt->second.lastKnownName;
			}
			for (const LobbyCandidate& candidate : m_candidates)
			{
				if (candidate.ownerSteamId == row.steamId)
				{
					if (info.name.empty() && !candidate.ownerName.empty())
					{
						info.name = candidate.ownerName;
					}
					info.rank = candidate.internalRankLevel;
					break;
				}
			}
			m_liveHiddenPeers[row.steamId] = info;
		}
	}
	// newPosOf[preMoveLogical] = position after the partition.
	std::vector<int32_t> newPosOf(static_cast<size_t>(orig));
	std::vector<int32_t> preAt(static_cast<size_t>(orig)); // position -> pre-move logical
	for (int32_t i = 0; i < orig; ++i)
	{
		newPosOf[static_cast<size_t>(i)] = i;
		preAt[static_cast<size_t>(i)] = i;
	}

	int32_t front = 0;
	int32_t back = orig - 1;
	int32_t swapsDone = 0;
	bool partitionAborted = false;
	while (front < back)
	{
		if (!hiddenAt[static_cast<size_t>(front)])
		{
			++front;
			continue;
		}
		if (hiddenAt[static_cast<size_t>(back)])
		{
			--back;
			continue;
		}
		uint8_t* const nodeA = rows[static_cast<size_t>(front)].node;
		uint8_t* const nodeB = rows[static_cast<size_t>(back)].node;
		if (nodeA == nullptr || nodeB == nullptr ||
			IsBadWritePtr(nodeA + kNodePayloadOffset, kNodePayloadSize) ||
			IsBadWritePtr(nodeB + kNodePayloadOffset, kNodePayloadSize))
		{
			partitionAborted = true;
			break; // something is off - leave the list alone this pass
		}
		uint8_t tmp[kNodePayloadSize];
		memcpy(tmp, nodeA + kNodePayloadOffset, kNodePayloadSize);
		memcpy(nodeA + kNodePayloadOffset, nodeB + kNodePayloadOffset, kNodePayloadSize);
		memcpy(nodeB + kNodePayloadOffset, tmp, kNodePayloadSize);
		++swapsDone;

		std::swap(rows[static_cast<size_t>(front)].steamId, rows[static_cast<size_t>(back)].steamId);
		const bool hf = hiddenAt[static_cast<size_t>(front)];
		hiddenAt[static_cast<size_t>(front)] = hiddenAt[static_cast<size_t>(back)];
		hiddenAt[static_cast<size_t>(back)] = hf;
		const int32_t preF = preAt[static_cast<size_t>(front)];
		const int32_t preB = preAt[static_cast<size_t>(back)];
		preAt[static_cast<size_t>(front)] = preB;
		preAt[static_cast<size_t>(back)] = preF;
		newPosOf[static_cast<size_t>(preF)] = back;
		newPosOf[static_cast<size_t>(preB)] = front;
	}

	if (partitionAborted)
	{
		// A half-partitioned region must not be clipped by a count write -
		// some hidden rows could stay visible and shown rows get cut off.
		// Leave everything as-is; the next pass retries from scratch.
		return;
	}

	int32_t hiddenCount = 0;
	for (int32_t i = 0; i < orig; ++i)
	{
		if (hiddenAt[static_cast<size_t>(i)])
		{
			++hiddenCount;
		}
	}
	const int32_t shownCount = orig - hiddenCount;

	if (*countField != shownCount)
	{
		LOG(1, "[RankedListFilter] live row count %d -> %d (%d hidden of %d, %d payload swaps)\n",
			*countField, shownCount, hiddenCount, orig, swapsDone);
		*countField = shownCount;
	}
	m_gameListLastCountWritten = shownCount;
	m_lastShownCount = static_cast<size_t>(shownCount);
	m_lastHiddenCount = static_cast<size_t>(hiddenCount);

	// Keep the scrollbar/cursor widget consistent with the shrunk/grown list
	// (write-on-change inside) - without this, the scroll range and cursor
	// kept the delivery-time size and could select into hidden territory.
	FixupRankedResultWidget(shownCount);

	if (shownCount <= 0)
	{
		// Everything hidden: nothing to order. Keep perm identity so the
		// game sees a fully consistent (empty) list.
		for (int32_t slot = 0; slot < kGamePermSlots; ++slot)
		{
			perm[slot] = slot;
		}
		return;
	}

	// Pass 4: desired visible order over the shown region (post-partition
	// positions 0..shownCount-1), stable against the pre-partition visible
	// sequence. Rows are matched to delivered candidates by owner steamId
	// (consuming each candidate once, keeping the mapping one-to-one) and
	// sorted with the exact comparator the delivery path uses; rows with no
	// matching candidate keep their relative order after the sorted block.
	std::vector<int32_t> sortableLogicals;
	std::vector<const LobbyCandidate*> sortableCandidates;
	std::vector<int32_t> unmatchedLogicals;
	std::vector<bool> candidateConsumed(m_candidates.size(), false);
	for (const int32_t preLogical : baselineSeq)
	{
		const int32_t logical = newPosOf[static_cast<size_t>(preLogical)];
		if (logical >= shownCount)
		{
			continue; // parked hidden row - not part of the visible region
		}
		const uint64_t steamId = rows[static_cast<size_t>(logical)].steamId;
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
	// ~2.5x/sec; we log only when the on-screen order actually changes).
	std::vector<const LobbyCandidate*> sortedCandidates = sortableCandidates;
	SortShownCandidates(&sortedCandidates, false);
	std::vector<int32_t> desired;
	desired.reserve(static_cast<size_t>(shownCount));
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
	// Safety net: any shown position not covered above (shouldn't happen)
	// still needs to appear exactly once for perm to stay a permutation.
	{
		std::vector<bool> present(static_cast<size_t>(shownCount), false);
		for (const int32_t logical : desired)
		{
			if (logical >= 0 && logical < shownCount)
			{
				present[static_cast<size_t>(logical)] = true;
			}
		}
		for (int32_t logical = 0; logical < shownCount; ++logical)
		{
			if (!present[static_cast<size_t>(logical)])
			{
				desired.push_back(logical);
			}
		}
	}

	// Pass 5: write the permutation only if it actually changed. Same thread
	// as the renderer/selection logic (the Steam callback pump runs on the
	// game's main thread), so this is frame-atomic - no torn reads possible.
	bool changed = swapsDone > 0;
	for (int32_t slot = 0; slot < shownCount && !changed; ++slot)
	{
		if (perm[slot] != desired[static_cast<size_t>(slot)])
		{
			changed = true;
		}
	}
	if (!changed)
	{
		return;
	}
	for (int32_t slot = 0; slot < shownCount; ++slot)
	{
		perm[slot] = desired[static_cast<size_t>(slot)];
	}
	for (int32_t slot = shownCount; slot < kGamePermSlots; ++slot)
	{
		perm[slot] = slot;
	}
	m_gamePermCustomized = true;

	// Cursor follow, part 2: the on-screen order just changed - move the
	// widget cursor so it stays on the SAME PLAYER it was on before this
	// pass, and keep the scroll window over it. If that player's row got
	// hidden this pass, leave the cursor where FixupRankedResultWidget's
	// clamp put it (there is no right answer for a vanished row).
	if (resultWidget != nullptr && cursorSlotBefore >= 0 &&
		!IsBadWritePtr(resultWidget + kWidgetCursorOffset, sizeof(int32_t)))
	{
		const int32_t physAfter = newPosOf[static_cast<size_t>(cursorPhysBefore)];
		int32_t newSlot = -1;
		if (physAfter >= 0 && physAfter < shownCount)
		{
			for (int32_t slot = 0; slot < shownCount; ++slot)
			{
				if (perm[slot] == physAfter)
				{
					newSlot = slot;
					break;
				}
			}
		}
		int32_t* const cursor = reinterpret_cast<int32_t*>(resultWidget + kWidgetCursorOffset);
		if (newSlot >= 0 && *cursor != newSlot)
		{
			LOG(1, "[RankedListFilter] cursor follow: slot %d -> %d (tracking the same player through the reorder)\n",
				cursorSlotBefore, newSlot);
			*cursor = newSlot;
			// Keep the visible window over the cursor, same invariants the
			// widget fixup maintains: top in [0, count-1-pageM1], bottom =
			// top + pageM1, cursor within [top, bottom].
			int32_t* const scrollTop = reinterpret_cast<int32_t*>(resultWidget + kWidgetScrollTopOffset);
			int32_t* const bottom = reinterpret_cast<int32_t*>(resultWidget + kWidgetBottomRowOffset);
			const int32_t pageM1 = *reinterpret_cast<const int32_t*>(resultWidget + kWidgetPageM1Offset);
			if (pageM1 >= 0)
			{
				int32_t maxTop = (shownCount - 1) - pageM1;
				if (maxTop < 0)
				{
					maxTop = 0;
				}
				if (*scrollTop > maxTop)
				{
					*scrollTop = maxTop;
				}
				if (*scrollTop < 0)
				{
					*scrollTop = 0;
				}
				if (newSlot < *scrollTop)
				{
					*scrollTop = newSlot;
				}
				if (newSlot > *scrollTop + pageM1)
				{
					*scrollTop = newSlot - pageM1;
				}
				*bottom = *scrollTop + pageM1;
			}
		}
	}

	std::string orderDump;
	for (int32_t slot = 0; slot < shownCount; ++slot)
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
	LOG(1, "[RankedListFilter] live order applied (%d shown, %d hidden live):%s\n",
		shownCount, hiddenCount, orderDump.c_str());
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

void RankedListConnectionFilter::SortHiddenPeers(std::vector<HiddenPeerInfo>* peers) const
{
	const int mode = Settings::settingsIni.rankedListSortMode;
	if (peers == nullptr || peers->size() < 2 ||
		mode <= RankedListSortMode_Default || mode >= RankedListSortMode_COUNT)
	{
		return;
	}

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
			return;
		}
	}

	struct SortEntry
	{
		HiddenPeerInfo peer;
		bool keyKnown = false;
		long long numericKey = 0;
		std::string textKey;
	};

	std::vector<SortEntry> entries;
	entries.reserve(peers->size());
	for (const HiddenPeerInfo& peer : *peers)
	{
		SortEntry entry;
		entry.peer = peer;

		switch (mode)
		{
		case RankedListSortMode_BestConnection:
		case RankedListSortMode_WorstConnection:
			if (peer.delayDigit >= 0)
			{
				const bool worst = mode == RankedListSortMode_WorstConnection;
				entry.numericKey = worst ? -peer.delayDigit : peer.delayDigit;
				entry.keyKnown = true;
			}
			break;
		case RankedListSortMode_ClosestLevel:
		case RankedListSortMode_FurthestLevel:
			if (peer.rank >= 0)
			{
				const long long theirVisible = peer.rank + 1;
				const long long diff = theirVisible - static_cast<long long>(myVisibleRank);
				entry.numericKey = diff >= 0 ? diff : -diff;
				entry.keyKnown = true;
			}
			break;
		case RankedListSortMode_HighestLevel:
		case RankedListSortMode_LowestLevel:
			if (peer.rank >= 0)
			{
				entry.numericKey = peer.rank;
				entry.keyKnown = true;
			}
			break;
		case RankedListSortMode_NameAZ:
		case RankedListSortMode_NameZA:
			if (!peer.name.empty())
			{
				entry.textKey = peer.name;
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

	// WorstConnection excluded here for the same reason as SortShownCandidates:
	// its direction is baked directly into numericKey above.
	const bool descending =
		mode == RankedListSortMode_FurthestLevel ||
		mode == RankedListSortMode_HighestLevel ||
		mode == RankedListSortMode_NameZA;
	const bool textual = mode == RankedListSortMode_NameAZ || mode == RankedListSortMode_NameZA;

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

	peers->clear();
	for (const SortEntry& entry : entries)
	{
		peers->push_back(entry.peer);
	}
}

void RankedListConnectionFilter::BuildCompactedListAndDeliver(const char* reason)
{
	m_reachableLobbies.clear();
	std::vector<const LobbyCandidate*> shownCandidates;
	// REWORK (2026-07-12, per explicit user direction): delivery no longer
	// hides ANYONE. The full list is served instantly, and every hide/restore
	// decision (reputation, network tier filter, requirement filter) is
	// applied afterward by PollGameListAndApplyOrder()'s live row
	// manipulation - which can now both remove AND resurrect rows in place,
	// so there is nothing a pre-delivery compaction can do that the live
	// pass can't do better (and reversibly). This also removes the old
	// asymmetry where a peer hidden at delivery had no game row to restore
	// without a refresh.
	for (const LobbyCandidate& candidate : m_candidates)
	{
		shownCandidates.push_back(&candidate);
		if (candidate.ownerSteamId != 0 && !candidate.ownerName.empty())
		{
			m_verdicts[candidate.ownerSteamId].lastKnownName = candidate.ownerName;
		}
	}

	SortShownCandidates(&shownCandidates);
	for (const LobbyCandidate* const candidate : shownCandidates)
	{
		m_reachableLobbies.push_back(candidate->lobbyId);
	}

	m_lastShownCount = m_reachableLobbies.size();
	m_lastHiddenCount = 0; // live pass recomputes both within ~400ms
	LOG(1, "[RankedListFilter] delivering (%s): %zu lobbies (full list, live pass filters), sortMode=%d\n",
		reason, m_reachableLobbies.size(),
		Settings::settingsIni.rankedListSortMode);

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

	// The game's populate pass (which rewrites node payloads and the row
	// count from this delivery's records) runs on a subsequent tick, not
	// inside Run() - flag the live pass to re-learn the game-authored count
	// before it resumes any live hide/reorder work.
	m_gameListResyncPending = true;
	m_lastDeliveryTickMs = GetTickCount64();
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

	// Periodic whole-list reachability recheck - hidden entries included, so
	// recovered peers get restored live (see kListRecheckIntervalMs).
	if (m_hasRemapResult && !m_candidates.empty() && IsPipelineActive())
	{
		const unsigned long long now = GetTickCount64();
		if (m_lastListRecheckTickMs == 0)
		{
			// Delivery already probed everyone; start the cycle from there.
			m_lastListRecheckTickMs = now;
		}
		else if (now - m_lastListRecheckTickMs >= kListRecheckIntervalMs)
		{
			m_lastListRecheckTickMs = now;
			// Manual-restore exemptions last exactly one recheck cycle - from
			// here on, fresh data decides again.
			m_restoreExemptions.clear();
			int reprobed = 0;
			for (const LobbyCandidate& candidate : m_candidates)
			{
				const size_t inFlightBefore = m_probesInFlight.size();
				StartProbeIfNeeded(candidate.ownerSteamId, true);
				if (m_probesInFlight.size() != inFlightBefore)
				{
					++reprobed;
				}
			}
			LOG(1, "[RankedListFilter] periodic list recheck: re-probing %d of %zu candidates\n",
				reprobed, m_candidates.size());
		}
	}

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

	// Serve the live snapshot built by PollGameListAndApplyOrder - every row
	// currently hidden from the on-screen list, whatever the reason
	// (reputation, network tier filter, requirement filter). Peers with no
	// displayable name are skipped per user direction - an anonymous
	// "??? - unreachable" row is just noise.
	for (const auto& entry : m_liveHiddenPeers)
	{
		if (entry.second.name.empty())
		{
			continue;
		}
		outPeers->push_back(entry.second);
	}

	SortHiddenPeers(outPeers);
}

void RankedListConnectionFilter::RestorePeer(uint64_t steamId)
{
	// Clears any reputation verdict AND exempts the peer from the rule-based
	// filters (network tier / requirement) until the next periodic recheck -
	// the row returns within ~400ms either way, and may legitimately re-hide
	// once fresh data says so again.
	m_verdicts.erase(steamId);
	m_restoreExemptions.insert(steamId);
	LOG(1, "[RankedListFilter] user restored steamId=%llu - verdict cleared, filters exempted until next recheck\n",
		static_cast<unsigned long long>(steamId));
}

void RankedListConnectionFilter::RestoreAllPeers()
{
	size_t restored = 0;
	for (const auto& entry : m_liveHiddenPeers)
	{
		m_verdicts.erase(entry.first);
		m_restoreExemptions.insert(entry.first);
		++restored;
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
		Settings::settingsIni.rankedListSortMode != RankedListSortMode_Default ||
		Settings::settingsIni.rankedListNetworkFilter != 0 ||
		Settings::settingsIni.hideUnmetRequirementRooms;
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

			// The game's own row list currently holds rows. This replaces an
			// earlier "RequestLobbyList issued within the last 50s" recency
			// window, which live testing (2026-07-12 DEBUG.txt) proved wrong:
			// the game's auto-refresh cadence is NOT bounded - observed gaps
			// of 95s and 144s between requests while idling on a perfectly
			// stable list - so the window expired and the config window
			// vanished mid-list (onList=0 with state1=39 for 45-90s stretches,
			// starting exactly 50s after the last request every time). The
			// row list itself is authoritative: populated per delivery,
			// cleared at search start/back-out (its count is zeroed by the
			// same functions that reset the permutation array).
			// m_gameListOrigCount covers the corner where the live hide pass
			// has temporarily shrunk the game's count to zero (all rows
			// hidden) - the list screen is still very much on screen then.
			bool gameListHasRows = m_gameListOrigCount > 0;
			if (!gameListHasRows)
			{
				const uint8_t* const listStruct = ResolveGameRowListStruct();
				if (listStruct != nullptr)
				{
					gameListHasRows = *reinterpret_cast<const int32_t*>(listStruct + 0xae8) > 0;
				}
			}

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
				gameListHasRows && !inFunctionalRoomWithOpponent;
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
