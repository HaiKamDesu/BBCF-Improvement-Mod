#pragma once

#include <cstdint>

struct Packet;

/*
	PC and connection performance for online matches, shown in the online window (F2, or
	Ctrl+F2 to jump straight to it). Answers "can both PCs hold 60 FPS?" (modelled on UMVC3
	rollback's PC PERFORMANCE panel, requested by tournament organizers) and "if it lags,
	whose fault is it?".

	HARDWARE (game thread, three QPC reads per frame, no allocation)
	-----------------------------------------------------------------
	Game+Draw - the CPU time of one rendered frame, without the frame limiter and without
	Present. BBCF's main loop step (FUN_0043B780) ends with

	    Clear, StretchRect, EndScene          <- the frame's work is done here
	    FUN_0044F790                          <- frame limiter: busy-waits / Sleep(1) until the
	                                             next 1/60 s tick, and calls ggpo idle once
	    PresentEx                             <- vsync / driver queue wait

	so "previous PresentEx returned -> last EndScene returned" is the game update + render
	submission (plus the mod's own EndScene work, reported separately as modAvgUs), with
	neither the limiter's sleep nor the vsync wait in it. Capacity = 16.67 ms / that time.

	Resim - Sync::AdjustSimulation is FUN_007821C0 (__thiscall, this = Sync, arg = seek_to;
	asserts name ggpo\lib\sync.cpp). It loads seek_to's state and runs advance_frame (0x783EB0)
	once per frame back to [Sync+0xF4]; depth <= 8. Its two callers (0x782379 in
	CheckSimulation, 0x77D233 in the disconnect adjust) are redirected - rel32 only - to a timer
	that calls the original. Resim per frame = AdjustSimulation time / depth. Rollback time that
	lands inside the Game+Draw window is subtracted from it.

	NETWORK (read-only GGPO memory, see GhidraDefs.h "GGPO network stats")
	---------------------------------------------------------------------
	The same fields ggpo_get_network_stats returns, read straight from the live
	SteamUdpProtocol endpoint: round-trip time, unacked local inputs, both frame advantages;
	plus the time since the last packet from the peer (sampled per frame, worst gap over 2 s),
	prediction depth (Sync framecount - last confirmed frame), jitter (mean change of the RTT
	between 1 s samples) and whether Steam relays the P2P session (GetP2PSessionState, a local
	query).

	Nothing here writes game memory except the two call operands, and the redirected call runs
	the original with the original arguments. Netplay determinism is untouched.

	EXCHANGE (Steam P2P only, see NetworkManager::SendPacketQuiet)
	-------------------------------------------------------------
	PacketType_PerformanceReport, only to members of this match whose ModVersionInfo (in the
	acknowledge) advertised kModCap_PerformanceReport - so never to a vanilla client and never
	to an older mod build. Always on (there is no opt-out setting since 2026-10-24; the payload
	is audited to carry only frame-time/connection numbers, see the field list below):
	  - player -> opponent: the 72-byte report, 1 Hz, while either of the two has the
	    PC & connection stats window open;
	  - spectator -> each player: an 8-byte request (Flag_WantsReports, no numbers), 1 Hz,
	    while the spectator has the stats window open;
	  - player -> spectator: the 72-byte report, 1 Hz, only while that spectator's requests
	    keep arriving (within 2.5 s).
	All unreliable. The report already carries each player's own view of the connection
	(ping, jitter, longest silence, frame advantage, unacked inputs, route), which is what a
	spectator - whose GGPO backend has no peer stats for the players - needs.
	Nothing goes to ASW's servers or to any web service.

	RELAY BASELINE (who has the slow line)
	--------------------------------------
	The ping between the two players is one shared number. To say whose side adds the delay,
	each side also measures its own ping to the nearest Valve relay (ISteamNetworkingUtils,
	"SteamNetworkingUtils004", through SteamClient017::GetISteamGenericInterface): Steam pings
	Valve's relay points of presence itself; we read the smallest direct ping. Only that one
	number is sent (relayPingMs) - never the ping location string, never the list of relays,
	which together would locate the player. Limits: it measures the player's line up to
	Valve's nearest relay, not the route between the two players, so it can point at a slow
	or wireless line but cannot see a bad ISP route between two good lines.

	ROUTE CHECK ("who should turn on a VPN")
	----------------------------------------
	To tell a bad route from plain distance, the two players exchange their Steam ping
	location (ISteamNetworkingUtils::GetLocalPingLocation -> ConvertPingLocationToString) and
	the code of their nearest Valve relay, in PacketType_PingLocation. Privacy rule, decided
	with the project owner and kept here because the code is public:
	  - DIRECT SESSIONS ONLY. It is sent only while the game's own Steam P2P session with the
	    opponent is direct (GetP2PSessionState: active and m_bUsingRelay == 0). In that state
	    the old ISteamNetworking API already gives every program in the other player's game
	    their real IP (P2PSessionState_t::m_nRemoteIP), and it is on the wire anyway, so a
	    coarse location adds no exposure. When Steam relays the session, vanilla hides the IP,
	    so nothing is sent and the check says it cannot run.
	  - Opponent only, never spectators (they get the computed numbers in the 1 Hz report).
	  - Only while the stats window is open on either side; once per match and on change.
	  - Never logged, displayed or saved. The receiver validates it (printable ASCII, max
	    length), parses it with ParsePingLocationString, keeps only the parsed binary form in
	    memory for the match, and zeroes the text.
	From those, each side computes Steam's estimate of the ping between them
	(EstimatePingTimeFromLocalHost) and its own ping to the other side's nearest relay
	(GetPingToDataCenter). Only those NUMBERS go into the report:
	  excess = ping to the other's region - own last mile = what this side's ISP route costs;
	  expected RTT ~= lastMileA + lastMileB + min(excessA, excessB), cross-checked with Steam's
	  estimate. Actual RTT well above that (>1.5x and >30 ms) = the route is the problem; the
	  side with the clearly higher excess should use a VPN (e.g. Cloudflare WARP, which enters
	  Cloudflare's network at that player's nearest PoP); similar excesses = either side.
	Limits: it needs both players on a current build and a direct session (a VPN that makes
	the session relayed turns the check off), and Steam's relay map is an estimate of what a
	good route looks like, not a guarantee.
*/
namespace PerformanceMonitor
{
#pragma pack(push, 1)
	// Wire format of the report, and the local snapshot. Fixed size, little endian. Only ever
	// append fields: a receiver copies min(dataSize, sizeof) into a zeroed struct.
	struct Report
	{
		uint8_t wireVersion;       // 1
		uint8_t flags;             // Flag_*
		uint16_t frameSamples;     // frames in the Game+Draw window
		uint32_t gameDrawP90Us;    // 90th percentile Game+Draw - the headline number
		uint32_t gameDrawAvgUs;
		uint32_t gameDrawMaxUs;
		uint32_t intervalAvgUs;    // real frame-to-frame time (what an FPS counter shows)
		uint32_t intervalMaxUs;
		uint16_t rollbackSamples;  // rollbacks the resim numbers are averaged over
		uint16_t rollbacksRecent;  // rollbacks in the last 2 s
		uint32_t resimFrameAvgUs;  // per re-simulated frame
		uint32_t resimFrameMaxUs;
		uint8_t maxDepthRecent;    // deepest rollback in the last 2 s
		int8_t localFramesBehind;  // GGPO local_frames_behind (+ = this side is behind)
		int8_t remoteFramesBehind; // GGPO remote_frames_behind
		uint8_t maxPrediction;     // most unconfirmed frames in the last 2 s
		uint32_t totalRollbacks;   // this match
		uint32_t totalResimFrames;
		uint32_t modAvgUs;         // of gameDrawAvgUs, the mod's own EndScene work
		uint16_t pingMs;           // GGPO round trip
		uint16_t jitterMs;
		uint16_t maxRecvGapMs;     // longest silence from the peer in the last 2 s
		uint16_t sendQueue;        // local inputs the peer has not acknowledged
		uint16_t rollbackFramesRecent; // sum of rollback depths in the last 2 s
		uint8_t linkType;          // LinkType of the adapter carrying the match: 0 unknown, 1 wired, 2 Wi-Fi
		uint8_t reserved;
		uint32_t resimLoadUs;      // fixed cost of one rollback (state load), fitted; resimFrameAvgUs is then the per-frame part
		uint16_t relayPingMs;      // last mile: this side's ping to its nearest Valve relay, 0 = unknown
		uint16_t expectedPingMs;   // Steam's estimate of the RTT to the opponent from both ping locations, 0 = unknown
		uint16_t crossPingMs;      // this side's ping to the OPPONENT's nearest relay, 0 = unknown
		uint16_t reserved2;
		uint32_t reserved3;
	};
#pragma pack(pop)
	static_assert(sizeof(Report) == 80, "PerformanceMonitor::Report is a wire format");

	enum LinkType : uint8_t { Link_Unknown = 0, Link_Wired = 1, Link_WiFi = 2 };

	// The route check from both sides' numbers. a and b are the two players' reports.
	enum RouteState
	{
		Route_CantCheckRelayed, // Steam relays the session: no location exchange, no check
		Route_CantCheckYet,     // waiting for numbers (or the other side has no current mod)
		Route_Fine,             // actual ping close to what their locations allow
		Route_VpnSide,          // the route is long, and side's ISP route is the bad one
		Route_VpnEither,        // the route is long, both ISP routes look alike: try side first
		Route_LineSide,         // the route is long and side's own line (last mile) is slow
	};

	struct RouteCheck
	{
		RouteState state = Route_CantCheckYet;
		int side = -1;          // 0 = a, 1 = b
		int actualMs = 0;
		int expectedMs = 0;     // the model's expectation
		int steamEstimateMs = 0;
		int lastMile[2] = { 0, 0 };
		int excess[2] = { -1, -1 };
	};

	enum ReportFlags : uint8_t
	{
		Flag_HasFrames = 1 << 0,
		Flag_HasResim = 1 << 1,
		Flag_HasNet = 1 << 2,
		Flag_UsingRelay = 1 << 3,
		Flag_WantsReports = 1 << 4, // sender has the performance section on screen
	};

	enum OpponentState
	{
		Opp_NotOnline,   // not in an online match as a player
		Opp_NoOpponent,  // no opponent found in the room yet
		Opp_NoMod,       // opponent never answered the mod's announce
		Opp_OldMod,      // runs a mod build older than 8.9.2 (no version, no reports)
		Opp_Waiting,     // capable, nothing received yet
		Opp_NotSharing,  // capable, asked for a while, nothing came (an older test build)
		Opp_Live,
		Opp_Stale,       // reports stopped arriving
	};

	struct OpponentView
	{
		OpponentState state = Opp_NotOnline;
		uint64_t steamId = 0;
		Report report = {};
		double ageSeconds = 0.0;
		uint32_t received = 0;
	};

	// "If it lags, whose side is it on?" - judged from one side's report (a) and the other's
	// view (b). side: 0 = a's side, 1 = b's side, -1 = nobody / cannot tell.
	enum LagKind
	{
		Lag_Checking,        // first seconds of a match
		Lag_None,            // both PCs keep up and the connection is steady
		Lag_Pc,              // side's PC cannot hold 60 FPS
		Lag_Unstable,        // side's packets arrive with long gaps (Wi-Fi, busy line)
		Lag_UnstableUnknown, // the connection is unsteady, but not pinned on one side
		Lag_SlowLine,        // high ping, and side's line to Valve's nearest relay is slow
		Lag_Relayed,         // high ping and Steam relays the traffic (no direct connection)
		Lag_Distance,        // high ping, both lines look fine: they are far apart
		Lag_LongRoute,       // the ping is well above what their locations allow: a route problem
	};

	struct LagVerdict
	{
		LagKind kind = Lag_Checking;
		int side = -1;
	};

	enum PcRating { Pc_Unknown, Pc_Good, Pc_Struggling };
	enum ConnRating { Conn_Unknown, Conn_Good, Conn_Unstable, Conn_SlowLine };

	// Ping thresholds used by the verdict and the plain view.
	constexpr int kHighPingMs = 100;
	constexpr int kSlowLineRelayMs = 60;

	// D3D wrapper hooks.
	void OnEndSceneBegin();
	void OnEndSceneDone();
	void OnPresentDone();

	// Once per rendered frame, from EndScene. Cheap except every 500 ms.
	void Tick();

	// From NetworkManager::RecvPacket.
	void OnReportPacket(Packet* packet);
	void OnPingLocationPacket(Packet* packet);

	// The PC & connection stats window calls this every frame it is open; that is what turns
	// the exchange on (for the opponent, and for a spectator asking the players) and off again
	// ~1.5 s after it closes.
	void NoteStatsWindowOpen();

	const Report& LocalReport();
	OpponentView GetOpponentView();     // players
	OpponentView GetPlayerView(int side); // spectators: 0 = P1, 1 = P2

	// Dev-only (EnableInDevelopmentFeatures, offline match only): show the spectator layout
	// fed by two fake player reports pushed through the same receive/parse path.
	void SetSpectatorTestMode(bool on);
	bool IsSpectatorTestMode();
	bool IsOnlineMatchAsPlayer(); // cached, refreshed every 500 ms
	bool IsSpectating();
	bool IsInAnyMatch();
	bool IsRollbackTimingAvailable();
	bool IsNetworkStatsAvailable();
	// Ping to this PC's nearest Valve relay, ms; 0 = not measured (yet, or unavailable).
	int RelayPingMs();
	uint64_t OpponentSteamId();

	constexpr double kBudgetUs = 1000000.0 / 60.0;
	double CapacityPercent(uint32_t frameUs);
	// Rollback frames that fit in one 60 FPS frame on top of a normal frame and the fixed
	// state-load cost; -1 = unknown.
	int RollbackHeadroomFrames(const Report& report);
	// The rollback depth this connection realistically produces (ping, plus margin), 2..8.
	int RealisticRollbackDepth(const Report& report);
	// 1 = fine, 0 = not enough samples to say, -1 = deep rollbacks would slow this PC down.
	int RollbackCapacityOk(const Report& report);
	bool IsStruggling(const Report& report);
	// Rollback depth the ping alone explains (one-way latency in frames minus input delay).
	int ExpectedRollbackDepth(const Report& report);
	LagVerdict JudgeLag(const Report& a, const OpponentView& b);
	RouteCheck CheckRoute(const Report& a, const Report* b);
	PcRating RatePc(const Report& r);
	// own = that player's report (its relay ping); measuredByOther = the report from the other
	// end, whose maxRecvGapMs is how late this player's packets arrive. Either may be null.
	ConnRating RateConnection(const Report* own, const Report* measuredByOther);
}
