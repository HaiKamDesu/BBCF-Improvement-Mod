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
// 2. On delivery we hold the payload briefly (kHoldDeadlineMs, only while
//    listed players have no cached verdict yet), then invoke the game's
//    handler with a count-patched copy: confirmed-bad players are removed and
//    GetLobbyByIndex serves the compacted list. Unknown players are SHOWN
//    (benefit of the doubt) - hiding is reserved for confirmed evidence.
// 3. Probes keep running in the background after delivery. Verdicts land in a
//    session-wide reputation cache; since the game auto-refreshes the list
//    every few seconds, a player confirmed dead simply vanishes on the next
//    refresh. Reachable verdicts are cached (kReachableTtlMs) so warm
//    refreshes deliver instantly with no hold.
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

	// UI support (RankedMainMenuSection): snapshot of currently hidden players
	// and manual restore. Restoring erases the peer's bad verdict, so they
	// reappear on the next list refresh; if they fail again they get re-hidden.
	struct HiddenPeerInfo
	{
		uint64_t steamId = 0;
		std::string name;
		int reactiveFailCount = 0;
		bool sessionBlocked = false;
		bool probeUnreachable = false;
	};
	void GetHiddenPeers(std::vector<HiddenPeerInfo>* outPeers) const;
	void RestorePeer(uint64_t steamId);
	void RestoreAllPeers();
	void GetLastListCounts(size_t* outShown, size_t* outHidden) const;

	// True while the game appears to have the lobby search list on screen. The
	// game calls GetLobbyByIndex continuously (every render frame) while the
	// list is visible, so recent index access is a tight, prompt open-signal;
	// recent RequestLobbyList is kept as a coarse fallback.
	bool IsLobbyListLikelyOpen() const;

	// Called from SteamMatchmakingWrapper::GetLobbyByIndex (filter on or off) to
	// timestamp list-render activity for IsLobbyListLikelyOpen.
	void NoteLobbyIndexAccessed();

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
		Holding, // result held; waiting for unknowns to resolve (short deadline)
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
		bool sessionBlocked = false;
		std::string lastKnownName; // best-effort, for the hidden-players UI
		// Connection-quality estimate from the reachability probe: how long the
		// P2P session took to establish, and whether it went through a relay.
		// ~0 = never measured.
		unsigned long long probeElapsedMs = ~0ull;
		bool usedRelay = false;
	};

	struct LobbyCandidate
	{
		uint64_t lobbyId = 0;
		uint64_t ownerSteamId = 0;
		std::string ownerName;     // from "ownerName" lobby metadata, for user-facing notifications
		int internalRankLevel = -1; // from "RANK_HOST_LEVEL" lobby metadata (visible level - 1); -1 = unknown
	};

	void OnLobbyListResultDelivered(void* pvParam, bool bIOFailure, SteamAPICall_t hSteamAPICall);
	void StartProbeIfNeeded(uint64_t steamId);
	void PollProbes();
	// Reorders the shown candidates per Settings::settingsIni.rankedListSortMode.
	// Candidates whose sort key is unknown keep their relative order at the end.
	void SortShownCandidates(std::vector<const LobbyCandidate*>* shown) const;
	// While a join attempt is pending, watches for the target appearing as a
	// member of the game's own room struct (which happens on the confirmation
	// screen). That proves the connection worked, so a subsequent LeaveLobby -
	// e.g. the user backing out of the confirmation popup - must not be counted
	// as a connection failure.
	void PollPendingConnectionConfirmation();
	// True when this peer should be hidden based on current reputation.
	bool ShouldHidePeer(uint64_t steamId, unsigned long long nowMs) const;
	// True when this peer has no valid verdict and a probe is still in flight.
	bool IsPeerUnresolved(uint64_t steamId) const;
	void BuildCompactedListAndDeliver(const char* reason);

	STEAM_CALLBACK(RankedListConnectionFilter, OnP2PSessionConnectFail, P2PSessionConnectFail_t);
	// Direct result of JoinLobby(): fires even when the join itself is rejected
	// outright, a distinct failure mode from the RankMatchLeaveMyself timeout.
	STEAM_CALLBACK(RankedListConnectionFilter, OnLobbyEnter, LobbyEnter_t);

	std::unordered_map<uint64_t, PeerVerdict> m_verdicts;
	// steamId -> probe start tick, for measuring establishment time.
	std::unordered_map<uint64_t, unsigned long long> m_probesInFlight;
	std::unordered_set<uint64_t> m_announcedHidden;
	std::vector<LobbyCandidate> m_candidates;
	std::vector<uint64_t> m_reachableLobbies;
	bool m_hasRemapResult = false;
	PipelineState m_pipelineState = PipelineState::Idle;
	uint64_t m_pendingApiCall = 0;
	unsigned long long m_holdDeadlineTickMs = 0;
	size_t m_lastShownCount = 0;
	size_t m_lastHiddenCount = 0;
	unsigned long long m_lastListRequestTickMs = 0;
	unsigned long long m_lastLobbyIndexAccessTickMs = 0;

	LobbyListResultProxy m_lobbyListProxy;
	CCallbackBase* m_gameLobbyListHandler = nullptr;
	LobbyMatchList_t m_heldResult = {};
	bool m_heldIOFailure = false;
	SteamAPICall_t m_heldApiCall = 0;

	uint64_t m_pendingConnectionTarget = 0;
	uint64_t m_pendingLobbyId = 0;
};
