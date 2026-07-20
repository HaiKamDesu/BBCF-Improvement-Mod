#pragma once

#include <isteamclient.h>
#include <isteamnetworking.h>
#include <steam_api.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Keeps unreachable players out of the ranked search list, with no user
// interaction and no UI freeze, using a per-player reputation model rather
// than per-refresh snap judgments (single-shot probing proved far too noisy:
// slow-but-fine peers got hidden, and verdicts flapped between refreshes).
//
// Delivery pipeline (see docs/Research/DCodeNetworkStallBug.md for history):
//
// 1. When the game requests the lobby list, the wrapper reports the async call
//    handle; a detour on SteamAPI_RegisterCallResult substitutes our proxy for
//    the game's handler so Steam delivers the LobbyMatchList_t payload to US.
//    (Reading the payload manually consumes it - the proxy is the only way to
//    inspect-and-forward.)
// 2. On delivery we immediately invoke the game's handler with the FULL
//    (sorted, but unfiltered) list - nobody is hidden at delivery time.
//    There is deliberately NO hold/delay - responsiveness is prioritized,
//    and every hide/restore decision is applied live afterward (step 3).
// 3. PollGameListAndApplyOrder() (~2.5x/sec) reads each game row's live
//    Delay data, then manipulates the game's own row structures in place:
//    reorders via the permutation array, and hides/restores rows via node
//    payload partition + row-count control. Hide criteria: reputation
//    (unreachable/failed peers, when the hide checkbox is on), the Network
//    Filter Delay-tier floor, and the unmet-connection-requirement filter.
//    A periodic whole-list re-probe (kListRecheckIntervalMs, hidden entries
//    included) keeps verdicts current in both directions, so recovered
//    peers pop back into the list live and newly-dead ones drop out - no
//    refresh needed for any of it.
//
// Reputation rules:
// - Probe confirmed unreachable (Steam P2P error): hidden for
//   kProbeUnreachableTtlMs, then eligible for re-probe.
// - One real connection failure (LeaveLobby before match start, failed
//   LobbyEnter, RankMatchLeaveMyself): hidden for kReactiveFailHideMs -
//   transient one-off failures don't earn permanent blocks.
// - Two+ real connection failures in a session: blocked for the session.
//
// Sort order applied to the delivered (compacted) list - the game renders
// rows in exactly the order we serve them, so this reorders the actual UI.
// Values persist in settings.ini as RankedListSortMode.
enum RankedListSortMode_
{
	RankedListSortMode_Default = 0,
	RankedListSortMode_BestConnection,   // probe establishment time asc, relay penalized
	RankedListSortMode_WorstConnection,
	RankedListSortMode_ClosestLevel,     // |their level - my level| asc
	RankedListSortMode_FurthestLevel,
	RankedListSortMode_HighestLevel,
	RankedListSortMode_LowestLevel,
	RankedListSortMode_NameAZ,
	RankedListSortMode_NameZA,
	RankedListSortMode_COUNT,
};

// Gated behind Settings::settingsIni.enableRankedListConnectionFilter.
class RankedListConnectionFilter
{
public:
	static RankedListConnectionFilter& GetInstance();

	void MarkUnreachable(uint64_t steamId, const char* reason);

	// Called from SteamMatchmakingWrapper::RequestLobbyList with the async call
	// handle - arms the proxy for the game's imminent RegisterCallResult.
	void OnLobbyListRequestIssued(uint64_t apiCallHandle);

	// Called from the SteamAPI_RegisterCallResult detour. Returns the callback
	// object that should actually be registered with Steam: our proxy when this
	// is the armed lobby-list call, the caller's own callback otherwise.
	class CCallbackBase* SubstituteRegisterCallResult(class CCallbackBase* pCallback, uint64_t apiCallHandle);

	// Called from the SteamAPI_UnregisterCallResult detour. Returns the callback
	// object that should actually be unregistered (the proxy when the game is
	// cancelling the call we proxied).
	class CCallbackBase* SubstituteUnregisterCallResult(class CCallbackBase* pCallback, uint64_t apiCallHandle);

	// Called from the SteamAPI_RunCallbacks detour after each pump. Polls probe
	// progress, updates the reputation cache, and delivers any held lobby list
	// once its unknowns resolve or the short hold deadline passes. Never blocks.
	void OnSteamCallbacksPump();

	// Called from SteamMatchmakingWrapper::GetLobbyByIndex. Returns true and
	// writes the remapped (compacted) lobby id when a probed result set is
	// active; returns false to fall through to the raw API.
	bool TryGetRemappedLobby(int index, uint64_t* outLobbyId);

	// Called from SteamMatchmakingWrapper::JoinLobby() with the lobby ID and its
	// owner's SteamID - remembers who/what we're currently attempting to connect
	// to, so that if the attempt fails (see NotifyConnectionAttemptFailed /
	// OnLeaveLobby) we know who to penalize.
	void OnJoinLobbyAttempt(uint64_t lobbyId, uint64_t lobbyOwnerSteamId);

	// Called from MatchState::OnMatchInit() - a real match starting means the
	// pending join attempt succeeded, so a later ordinary LeaveLobby (match/set
	// ending normally) must not be treated as a failed connection.
	void OnMatchStarted();

	// Called from SteamMatchmakingWrapper::LeaveLobby() - catch-all for any
	// connection failure mode that ends with the game giving up on the lobby
	// before a match ever started (observed live: 0.3s to ~34s after JoinLobby
	// depending on where the handshake fails). Only treated as a failure if
	// lobbyId matches the still-pending JoinLobby target.
	void OnLeaveLobby(uint64_t lobbyId);

	// Called from NetworkStallDiagnostics when the "RankMatchLeaveMyself" popup
	// fires. Penalizes whichever SteamID was last passed to OnJoinLobbyAttempt.
	void NotifyConnectionAttemptFailed(const char* reason);

	bool IsSteamIdFiltered(uint64_t steamId) const;

	// Called from the JMP hook on the game's ranked-row click latch
	// (FUN_004a89d0) with the screen controller ('this' of the state machine,
	// which holds the game's own selection index at +0x1EC). The game tracks
	// that index separately from the list widget's cursor (widget+0x15D78);
	// the mod's widget-cursor writes (live-shrink clamp, cursor-follow) made
	// the two drift, so the click resolved a different row than the
	// highlighted one. This rewrites +0x1EC from the widget cursor right
	// before the game reads it. Safe no-op when the widget is unresolvable.
	void OnRankedRowClickLatch(void* screenCtrl);

	// Called from the JMP hook on the ranked click's REAL latch path (the
	// `push [ctrl+0x1EC]` at VA 0x4AEE64 feeding FUN_004A4110, request type
	// 0). That path pushes the controller index RAW as a LOGICAL row index -
	// no perm[] lookup - which is only correct while the permutation is
	// identity (always true in vanilla, never true once the mod reorders).
	// Returns the logical row the click must resolve to: perm[widget cursor].
	// Falls back to the game's own ctrl+0x1EC value when anything is
	// unresolvable, so vanilla behavior is preserved exactly.
	int32_t ResolveClickedLogicalRow(void* screenCtrl);

	// UI support: snapshot of players currently hidden from the LIVE list
	// (all hide reasons included), and manual restore. Restoring clears the
	// peer's bad verdict and exempts them from the rule-based filters until
	// the next periodic recheck - the row returns within ~400ms, and may get
	// re-hidden once fresh data says so again.
	enum class HiddenReason
	{
		Unreachable,      // probe confirmed unreachable
		ConnectionFailed, // a real join attempt failed recently
		NetworkFilter,    // Delay rating below the Network Filter floor
		Requirement,      // room's connection requirement not met by us
	};
	struct HiddenPeerInfo
	{
		uint64_t steamId = 0;
		std::string name;
		HiddenReason reason = HiddenReason::Unreachable;
		uint8_t netColor = 0xFF;    // entry+0x74, the row's native square icon color; 0xFF = unknown
		int rank = -1;              // internalRankLevel (visible level - 1); -1 = unknown
		int delayDigit = -1;        // 0-4 Delay/connection tag; -1 = unresolved
	};
	void GetHiddenPeers(std::vector<HiddenPeerInfo>* outPeers) const;
	void RestorePeer(uint64_t steamId);
	void RestoreAllPeers();
	void GetLastListCounts(size_t* outShown, size_t* outHidden) const;

	// True while the ranked search list screen is on screen. Detected directly
	// from the game's own row array (populated only while that screen exists),
	// which is reliable regardless of the game's irregular Steam re-request
	// cadence. A short grace period debounces the brief moment the game rebuilds
	// the array between refreshes.
	bool IsLobbyListLikelyOpen();

private:
	RankedListConnectionFilter();

	// Receives the LobbyMatchList_t delivery from Steam's dispatch in place of
	// the game's own CCallResult handler.
	class LobbyListResultProxy : public CCallbackBase
	{
	public:
		LobbyListResultProxy() { m_iCallback = LobbyMatchList_t::k_iCallback; }
		void Run(void* pvParam) override;
		void Run(void* pvParam, bool bIOFailure, SteamAPICall_t hSteamAPICall) override;
		int GetCallbackSizeBytes() override { return sizeof(LobbyMatchList_t); }
	};

	enum class PipelineState
	{
		Idle,
		Armed,   // RequestLobbyList issued; waiting for proxy delivery
	};

	struct PeerVerdict
	{
		enum class Kind
		{
			Unknown,
			Reachable,
			ProbeUnreachable,
		};
		Kind kind = Kind::Unknown;
		unsigned long long verdictTickMs = 0;
		unsigned long long lastReactiveFailTickMs = 0;
		int reactiveFailCount = 0;
		std::string lastKnownName; // best-effort, for the hidden-players UI
		// Connection-quality estimate from the reachability probe: how long the
		// P2P session took to establish, and whether it went through a relay.
		// ~0 = never measured. Used as a fallback sort key only until
		// gameTier below has been observed at least once for this peer.
		unsigned long long probeElapsedMs = ~0ull;
		bool usedRelay = false;
		// The game's OWN per-viewer measured average RTT to this host, in ms,
		// read from the ranked-list row entry's +0x78 field via PollGameListAndApplyOrder()
		// (see docs/Research/RankedListConnectionFilter_Progress.md, 2026-07-12
		// "Delay column source" section: the on-screen Delay digit is exactly
		// this value bucketed by FUN_004a6620's thresholds). -1 = never
		// observed/unresolved. This is what connection sorting keys on.
		int gameRttMs = -1;
		// The 0-4 Delay digit the game renders for that RTT ('0'+digit glyph,
		// FUN_004a6620: <60ms=4, <100=3, <200=2, <300=1, else 0). Derived from
		// gameRttMs at read time; kept for diagnostics/screenshot cross-checks.
		int gameTier = -1;
		// Cumulative average of every gameRttMs observation for this peer
		// across the whole session (not just the current list) - smooths out
		// any single noisy reading. Not used for sorting (user chose latest).
		double gameTierAverage = -1.0;
		int gameTierSampleCount = 0;
		// GetTickCount64() at the last successful gameTier read. Used to
		// bound how long a cached tier is trusted across a brand-new search
		// (see kGameTierTtlMs) - the game recreates its row/entry objects
		// from scratch on every new search, so a value from a much earlier
		// point shouldn't be trusted indefinitely, but discarding it
		// entirely on every new search left the sort with no real data at
		// all for the one delivery that actually reaches the screen (see
		// progress doc "regressed to fallback-only"). A bounded TTL is the
		// middle ground: survives the gap before the new search's own
		// entries resolve, without trusting truly stale data forever.
		unsigned long long gameTierTickMs = 0;
	};

	struct LobbyCandidate
	{
		uint64_t lobbyId = 0;
		uint64_t ownerSteamId = 0;
		std::string ownerName;     // from "ownerName" lobby metadata, for user-facing notifications
		int internalRankLevel = -1; // from "RANK_HOST_LEVEL" lobby metadata (visible level - 1); -1 = unknown
	};

	// The delivery pipeline (proxy + reorder/compaction) runs when EITHER the
	// hide-unreachable filter or a non-default sort order is active - the two
	// features are independent.
	bool IsPipelineActive() const;
	// Counts currently-populated rows in the game's ranked search row array.
	int CountPopulatedGameRows() const;
	// DIAGNOSTIC ONLY: logs the raw pointer value at the candidate ranked-list
	// manager singleton slot found by tracing the delay-dot render call
	// backward (see docs/Research/RankedListConnectionFilter_Progress.md).
	// Does not feed any real decision yet - purely for live correlation.
	void DiagnosticLogRankedListMgrSlot() const;

	void OnLobbyListResultDelivered(void* pvParam, bool bIOFailure, SteamAPICall_t hSteamAPICall);
	// force=true (the periodic whole-list recheck) bypasses verdict-freshness
	// gating so even peers with a live verdict get re-measured; session-
	// blocked peers are never probed either way.
	void StartProbeIfNeeded(uint64_t steamId, bool force = false);
	void PollProbes();
	// The live in-place list manipulator (see progress doc 2026-07-12 "Live
	// permutation" section). Every ~400ms while a list is on screen: walks
	// the game's own row list by logical index, harvests each row's Steam64
	// ID (entry+0x114 -> +0xc/+0x10) and measured RTT (entry+0x78, the value
	// behind the on-screen 0-4 Delay digit) into m_verdicts, computes the
	// desired visible order (sort mode + hidden-peers-to-tail), and rewrites
	// the game's own row permutation array (listStruct+0xaf4) in place. The
	// renderer, row selection and the auto-connect state machines all resolve
	// visible rows through that same array every frame, so this reorders the
	// on-screen list instantly - no re-delivery, no refresh, no Run() replay
	// (which was proven to be a no-op). Same-thread with the renderer (the
	// Steam callback pump runs on the game's main thread), so writes are
	// frame-atomic.
	void PollGameListAndApplyOrder();
	// Resets the game's row permutation array to the identity mapping (what
	// the game itself writes at search start) - called right after every real
	// delivery so a customized order from the PREVIOUS list can never scramble
	// a freshly rebuilt one, and when the pipeline features get turned off.
	void WriteIdentityGamePermutation();
	// Resolves the game's live ranked row-list struct via the confirmed mgr
	// singleton chain (base+0x897E3C -> mgr vtable slot 7). Returns nullptr
	// when unavailable. All guards IsBadReadPtr-based, no allocation.
	uint8_t* ResolveGameRowListStruct() const;
	// Resolves the ranked search-result list WIDGET (the UI object that owns
	// the scrollbar, cursor and row slots - built by the game's FUN_0064bfb0,
	// identified by its "Rank Match Search Result" config pointer). Returns
	// nullptr when the UI context isn't built or the ranked container isn't
	// in use. All guards IsBadReadPtr-based, no allocation.
	uint8_t* ResolveRankedResultWidget() const;
	// Rewrites the ranked result widget's item count / page bounds / cursor /
	// per-slot active flags to match the given row count, so scrolling and
	// selection can never reach past the live-shrunk list. Write-on-change only.
	void FixupRankedResultWidget(int32_t shownCount);
	// Diagnostic dump at freeze-engage (i.e. the moment a row click leaves the
	// browsable-list band): widget cursor + its row's identity, the connect
	// request's latched row index/type (listStruct+0x745/+0x74C) + its
	// identity, and how long ago the last mutation/delivery happened. Lets a
	// wrong-player report be pinned to a specific mechanism from DEBUG.txt.
	void LogClickResolutionSnapshot();
	// Reorders the shown candidates per Settings::settingsIni.rankedListSortMode.
	// Candidates whose sort key is unknown keep their relative order at the end.
	// logOrder=false suppresses the per-call "sort order" log line (used by the
	// high-frequency live path, which logs only when the order actually changes).
	void SortShownCandidates(std::vector<const LobbyCandidate*>* shown, bool logOrder = true) const;
	// Orders the hidden-players list (served to the UI) using the same
	// Settings::settingsIni.rankedListSortMode the visible list uses, so the
	// two stay consistent - "whatever order you pick" applies everywhere.
	void SortHiddenPeers(std::vector<HiddenPeerInfo>* peers) const;
	// While a join attempt is pending, watches for the target appearing as a
	// member of the game's own room struct (which happens on the confirmation
	// screen). That proves the connection worked, so a subsequent LeaveLobby -
	// e.g. the user backing out of the confirmation popup - must not be counted
	// as a connection failure.
	void PollPendingConnectionConfirmation();
	// True when this peer should be hidden based on current reputation.
	bool ShouldHidePeer(uint64_t steamId, unsigned long long nowMs) const;
	void BuildCompactedListAndDeliver(const char* reason);

	STEAM_CALLBACK(RankedListConnectionFilter, OnP2PSessionConnectFail, P2PSessionConnectFail_t);
	// Direct result of JoinLobby(): fires even when the join itself is rejected
	// outright, a distinct failure mode from the RankMatchLeaveMyself timeout.
	STEAM_CALLBACK(RankedListConnectionFilter, OnLobbyEnter, LobbyEnter_t);

	std::unordered_map<uint64_t, PeerVerdict> m_verdicts;
	// steamId -> probe start tick, for measuring establishment time.
	std::unordered_map<uint64_t, unsigned long long> m_probesInFlight;
	std::vector<LobbyCandidate> m_candidates;
	std::vector<uint64_t> m_reachableLobbies;
	bool m_hasRemapResult = false;
	PipelineState m_pipelineState = PipelineState::Idle;
	uint64_t m_pendingApiCall = 0;
	size_t m_lastShownCount = 0;
	size_t m_lastHiddenCount = 0;
	unsigned long long m_lastListRequestTickMs = 0;
	unsigned long long m_lastRowsPopulatedTickMs = 0;

	LobbyListResultProxy m_lobbyListProxy;
	CCallbackBase* m_gameLobbyListHandler = nullptr;
	LobbyMatchList_t m_heldResult = {};
	bool m_heldIOFailure = false;
	SteamAPICall_t m_heldApiCall = 0;
	// Throttle for PollGameListAndApplyOrder().
	unsigned long long m_lastLiveOrderTickMs = 0;
	// True while the game's permutation array holds a mod-written (non-
	// identity) order - lets the pipeline restore identity exactly once when
	// the features get turned off mid-session.
	bool m_gamePermCustomized = false;
	// True while live list mutations are frozen because the ranked flow is
	// outside the browsable-list band (a click being resolved, the connect/
	// confirmation flow, the entry menu). Tracked only to log the freeze
	// engage/release transitions once instead of every 400ms tick.
	bool m_liveOrderFrozen = false;
	// GetTickCount64() of the last actual in-place mutation (count write or
	// permutation write). Logged by the click snapshot so a wrong-player
	// report can be correlated with how fresh the last mutation was.
	unsigned long long m_lastMutationTickMs = 0;
	// Click cache consumed by ResolveClickedLogicalRow: the logical (node-
	// chain) index of the row under the widget cursor, refreshed every live
	// pass tick while the list is browsable and one final time at freeze-
	// engage (the click itself). Needed because the game frees the list
	// widget within ~30ms of a click, before the latch push runs, so live
	// resolution at the push site fails exactly when it matters.
	int32_t m_pendingClickLogicalRow = -1;
	int32_t m_pendingClickCursorSlot = -1;
	unsigned long long m_pendingClickCacheTickMs = 0;
	// Live hide/restore bookkeeping (see PollGameListAndApplyOrder). The
	// game's row count (+0xae8) is shrunk to hide rows live: hidden rows'
	// node payloads are swapped to the logical tail first, so positions
	// 0..count-1 stay a consistent, fully game-owned shorter list - the same
	// shape the game's own populate function produces, exercised by vanilla
	// gameplay whenever a refresh returns fewer lobbies.
	// The full (game-authored) row count of the current list, before any
	// mod-side shrink - the region 0..orig-1 always physically holds all
	// delivered rows, hidden ones parked at the tail.
	int32_t m_gameListOrigCount = 0;
	// The count value the mod last wrote to +0xae8 (or synced from the game);
	// a live count read differing from this means the game repopulated the
	// list on its own and m_gameListOrigCount must be re-learned.
	int32_t m_gameListLastCountWritten = -1;
	// Set right after each delivery's handler->Run(): the game's populate
	// pass (which rewrites contents and count) runs on a subsequent tick, so
	// the next live pass must re-learn the game-authored count instead of
	// trusting a possibly-stale read.
	bool m_gameListResyncPending = false;
	unsigned long long m_lastDeliveryTickMs = 0;
	// Periodic whole-list reachability recheck timer (kListRecheckIntervalMs).
	unsigned long long m_lastListRecheckTickMs = 0;
	// Snapshot of currently live-hidden rows (steamId -> reason), rebuilt by
	// every PollGameListAndApplyOrder pass - what GetHiddenPeers serves.
	std::unordered_map<uint64_t, HiddenPeerInfo> m_liveHiddenPeers;
	// Peers the user manually restored: exempt from the rule-based filters
	// (network tier / requirement) until the next periodic recheck, so the
	// restore is actually visible instead of being undone 400ms later.
	std::unordered_set<uint64_t> m_restoreExemptions;

	uint64_t m_pendingConnectionTarget = 0;
	uint64_t m_pendingLobbyId = 0;
};
