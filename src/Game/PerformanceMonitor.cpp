#include "PerformanceMonitor.h"

#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Game/GhidraDefs.h"
#include "Game/NetLinkType.h"
#include "Game/gamestates.h"
#include "Game/Room/RoomMemberEntry.h"
#include "Network/NetworkManager.h"
#include "Network/Packet.h"
#include "Network/RoomManager.h"

#include <Windows.h>

#include <steam_api.h>

#include <algorithm>
#include <cstring>

namespace
{
	using namespace PerformanceMonitor;

	// ---- relay baseline: ping to this PC's nearest Valve relay ---------------------------

	// The first twelve virtuals of ISteamNetworkingUtils, in the order of
	// "SteamNetworkingUtils004" (GameNetworkingSockets include/steam/isteamnetworkingutils.h;
	// InitRelayNetworkAccess is an inline wrapper for CheckPingDataUpToDate(1e10)). Only
	// CheckPingDataUpToDate, GetDirectPingToPOP, GetPOPCount and GetPOPList are called, and the
	// POP list is sanity-checked before any ping is trusted, so a layout mismatch turns the
	// feature off instead of reading garbage.
	class ISteamNetworkingUtils004Head
	{
	public:
		virtual void* AllocateMessage(int cbAllocateBuffer) = 0;
		virtual int GetRelayNetworkStatus(void* pDetails) = 0;
		virtual float GetLocalPingLocation(void* result) = 0;
		virtual int EstimatePingTimeBetweenTwoLocations(const void* l1, const void* l2) = 0;
		virtual int EstimatePingTimeFromLocalHost(const void* remote) = 0;
		virtual void ConvertPingLocationToString(const void* location, char* buf, int cch) = 0;
		virtual bool ParsePingLocationString(const char* str, void* result) = 0;
		virtual bool CheckPingDataUpToDate(float maxAgeSeconds) = 0;
		virtual int GetPingToDataCenter(uint32_t popId, uint32_t* viaRelayPop) = 0;
		virtual int GetDirectPingToPOP(uint32_t popId) = 0;
		virtual int GetPOPCount() = 0;
		virtual int GetPOPList(uint32_t* list, int listSize) = 0;
	};

	ISteamNetworkingUtils004Head* g_netUtils = nullptr;
	bool g_relayInitTried = false;
	bool g_relayDisabled = false;
	int g_relayPingMs = 0;
	uint32_t g_relayBestPop = 0; // nearest relay's code: never logged, only sent under the direct-only rule

	// ---- route check: ping locations (see PerformanceMonitor.h "ROUTE CHECK") -------------
	// Nothing in this block is ever logged, displayed or written to disk.

	constexpr int kMaxLocationChars = 1023;  // k_cchMaxSteamNetworkingPingLocationString - 1
	struct PingLocationBlob { unsigned char data[512]; }; // SteamNetworkPingLocation_t

	PingLocationBlob g_peerLocation = {};
	bool g_peerLocationValid = false;
	uint64_t g_peerLocationSteamId = 0;
	uint32_t g_peerPop = 0;
	int g_expectedPingMs = 0;  // EstimatePingTimeFromLocalHost(peer location)
	int g_crossPingMs = 0;     // GetPingToDataCenter(peer's nearest relay)
	uint64_t g_locationSentTo = 0;
	long long g_locationSentMatchStart = -1;
	uint32_t g_locationSentHash = 0;
	long long g_locationResendAt = 0;
	bool g_loggedLocationResendThisMatch = false;
	bool g_loggedLocationRejectThisMatch = false;

	// Raw buffers only: __try cannot unwind C++ objects.
	bool LocalLocationString(ISteamNetworkingUtils004Head* utils, char* buf, int cch)
	{
		PingLocationBlob loc;
		__try
		{
			if (utils->GetLocalPingLocation(&loc) < 0.0f)
				return false; // no ping data yet
			buf[0] = '\0';
			utils->ConvertPingLocationToString(&loc, buf, cch);
			buf[cch - 1] = '\0';
			return buf[0] != '\0';
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool ParseLocation(ISteamNetworkingUtils004Head* utils, const char* text, PingLocationBlob* out)
	{
		__try
		{
			return utils->ParsePingLocationString(text, out);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	int EstimateFromHere(ISteamNetworkingUtils004Head* utils, const PingLocationBlob* remote)
	{
		__try
		{
			return utils->EstimatePingTimeFromLocalHost(remote);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	int PingToDataCenter(ISteamNetworkingUtils004Head* utils, uint32_t pop)
	{
		uint32_t via = 0;
		__try
		{
			return utils->GetPingToDataCenter(pop, &via);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	void ForgetPeerLocation()
	{
		SecureZeroMemory(&g_peerLocation, sizeof(g_peerLocation));
		g_peerLocationValid = false;
		g_peerLocationSteamId = 0;
		g_peerPop = 0;
		g_expectedPingMs = 0;
		g_crossPingMs = 0;
	}

	// The opponent's location, parsed, gives two numbers; recomputed while the window is open
	// because the local ping data keeps improving for a while.
	void RecomputeRouteNumbers()
	{
		if (!g_peerLocationValid || !g_netUtils)
			return;
		const int estimate = EstimateFromHere(g_netUtils, &g_peerLocation);
		if (estimate > 0 && estimate < 5000)
			g_expectedPingMs = estimate;
		if (g_peerPop != 0)
		{
			const int cross = PingToDataCenter(g_netUtils, g_peerPop);
			if (cross > 0 && cross < 5000)
				g_crossPingMs = cross;
		}
	}

	// ---- link type: Wi-Fi or wired -----------------------------------------------------
	// Only the result ("wifi"/"wired"/"unknown") leaves this function; adapter names, MACs,
	// GUIDs, SSIDs and addresses are never stored, logged or sent.

	volatile LONG g_linkType = Link_Unknown;
	volatile LONG g_linkCheckRunning = 0;

	DWORD WINAPI DetectLinkTypeWorker(LPVOID param)
	{
		const int result = NetLinkType::Detect((unsigned long)(uintptr_t)param);
		InterlockedExchange(&g_linkType, result);
		InterlockedExchange(&g_linkCheckRunning, 0);
		LOG(1, "[PerfMonitor] network link: %s\n", result == Link_WiFi ? "wifi" : (result == Link_Wired ? "wired" : "unknown"));
		return 0;
	}

	void StartLinkTypeCheck(ULONG remoteIpHostOrder)
	{
		if (InterlockedCompareExchange(&g_linkCheckRunning, 1, 0) != 0)
			return;
		if (!QueueUserWorkItem(DetectLinkTypeWorker, (LPVOID)(uintptr_t)remoteIpHostOrder, WT_EXECUTEDEFAULT))
			InterlockedExchange(&g_linkCheckRunning, 0);
	}
	bool g_relayLogged = false;

	bool IsPlausiblePopId(uint32_t id)
	{
		if (id == 0)
			return false;
		for (int shift = 0; shift < 32; shift += 8)
		{
			const unsigned char c = (unsigned char)(id >> shift);
			if (c != 0 && !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
				return false;
		}
		return true;
	}

	// Raw pointers only: __try cannot unwind C++ objects.
	ISteamNetworkingUtils004Head* AcquireNetUtils()
	{
		HMODULE steamApi = GetModuleHandleA("steam_api.dll");
		if (!steamApi)
			return nullptr;
		typedef ISteamClient* (__cdecl* SteamClientFn)();
		typedef HSteamUser (__cdecl* GetUserFn)();
		typedef HSteamPipe (__cdecl* GetPipeFn)();
		SteamClientFn getClient = (SteamClientFn)GetProcAddress(steamApi, "SteamClient");
		GetUserFn getUser = (GetUserFn)GetProcAddress(steamApi, "SteamAPI_GetHSteamUser");
		GetPipeFn getPipe = (GetPipeFn)GetProcAddress(steamApi, "SteamAPI_GetHSteamPipe");
		if (!getClient || !getUser || !getPipe)
			return nullptr;
		__try
		{
			ISteamClient* client = getClient();
			if (!client)
				return nullptr;
			return (ISteamNetworkingUtils004Head*)client->GetISteamGenericInterface(getUser(), getPipe(), "SteamNetworkingUtils004");
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	// Returns the smallest direct ping to any relay, -1 if none measured yet, -2 on a bad layout.
	int ReadNearestRelayPing(ISteamNetworkingUtils004Head* utils, int* outPopCount, uint32_t* outBestPop)
	{
		uint32_t pops[128];
		__try
		{
			const int count = utils->GetPOPCount();
			*outPopCount = count;
			if (count == 0)
				return -1; // Steam has not fetched the relay list yet
			if (count < 0 || count > 512)
				return -2;
			const int n = utils->GetPOPList(pops, 128);
			if (n == 0)
				return -1;
			if (n < 0 || n > 128)
				return -2;
			int best = -1;
			for (int i = 0; i < n; ++i)
			{
				if (!IsPlausiblePopId(pops[i]))
					return -2;
				const int ping = utils->GetDirectPingToPOP(pops[i]);
				if (ping > 0 && ping < 2000 && (best < 0 || ping < best))
				{
					best = ping;
					*outBestPop = pops[i];
				}
			}
			return best;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -2;
		}
	}

	bool StartRelayPings(ISteamNetworkingUtils004Head* utils)
	{
		__try
		{
			utils->CheckPingDataUpToDate(1e10f); // = InitRelayNetworkAccess()
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Gets the SteamNetworkingUtils interface and asks Steam to start measuring its relay
	// pings (InitRelayNetworkAccess). Called as soon as the player is in online mode (lobby,
	// room, ranked menus), so the numbers are ready by the first match: this only makes Steam
	// ping Valve's relays - Valve's infrastructure, not ASW's - and sends nothing to any player.
	bool EnsureRelayAccess()
	{
		if (g_relayDisabled)
			return false;
		if (!g_relayInitTried)
		{
			g_relayInitTried = true;
			g_netUtils = AcquireNetUtils();
			if (!g_netUtils || !StartRelayPings(g_netUtils))
			{
				g_relayDisabled = true;
				g_netUtils = nullptr;
				LOG(1, "[PerfMonitor] relay baseline unavailable: SteamNetworkingUtils004 not provided by this Steam client\n");
				return false;
			}
			LOG(1, "[PerfMonitor] relay access started (online mode)\n");
		}
		return g_netUtils != nullptr;
	}

	// Every 2 s in online mode or while the stats window is open. Steam measures the relay
	// pings itself (a few seconds after the first request); this only reads the smallest one.
	void PollRelayBaseline()
	{
		if (!EnsureRelayAccess())
			return;
		int popCount = 0;
		uint32_t bestPop = 0;
		const int ping = ReadNearestRelayPing(g_netUtils, &popCount, &bestPop);
		if (ping == -2)
		{
			g_relayDisabled = true;
			g_relayPingMs = 0;
			LOG(1, "[PerfMonitor] relay baseline disabled: relay list failed its sanity check (count %d)\n", popCount);
			return;
		}
		if (ping > 0)
		{
			g_relayPingMs = ping;
			g_relayBestPop = bestPop;
			if (!g_relayLogged)
			{
				g_relayLogged = true;
				LOG(1, "[PerfMonitor] relay baseline: nearest Valve relay %d ms (%d relays known)\n", ping, popCount);
			}
		}
		RecomputeRouteNumbers();
	}

	// ---- clock --------------------------------------------------------------------------

	long long g_qpcFreq = 0;

	inline long long Now()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	inline uint32_t TicksToUs(long long ticks)
	{
		if (ticks <= 0 || g_qpcFreq <= 0)
			return 0;
		const long long us = ticks * 1000000LL / g_qpcFreq;
		return us > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)us;
	}

	// ---- rollback timing: call-site redirect of Sync::AdjustSimulation -----------------

	// RVAs (VA - 0x400000), the GhidraDefs.h convention. See PerformanceMonitor.h.
	constexpr uintptr_t kAdjustSimulationRva = ADDR_GgpoSyncAdjustSimulation; // FUN_007821C0
	constexpr uintptr_t kSyncFrameCountOffset = 0xF4;      // Sync::_framecount

	struct CallSite
	{
		uintptr_t callRva;            // the E8 byte
		const unsigned char* guard;   // bytes immediately before the E8
		size_t guardLength;
		const char* name;
	};

	// 0x782374: ff 75 fc  push [ebp-4] (seek_to) / 8b ce  mov ecx,esi (this)
	const unsigned char kGuardCheckSimulation[] = { 0xFF, 0x75, 0xFC, 0x8B, 0xCE };
	// 0x77D22C: 8d 8b 3c 05 00 00  lea ecx,[ebx+53Ch] (this) / 57  push edi (seek_to)
	const unsigned char kGuardDisconnectAdjust[] = { 0x8D, 0x8B, 0x3C, 0x05, 0x00, 0x00, 0x57 };

	const CallSite kCallSites[] = {
		{ ADDR_GgpoAdjustSimCall_CheckSim, kGuardCheckSimulation, sizeof(kGuardCheckSimulation), "CheckSimulation" },
		{ ADDR_GgpoAdjustSimCall_Disconnect, kGuardDisconnectAdjust, sizeof(kGuardDisconnectAdjust), "DisconnectAdjust" },
	};

	typedef void(__thiscall* AdjustSimulationFn)(void* sync, int seekTo);
	AdjustSimulationFn g_adjustSimulation = nullptr;

	bool g_installAttempted = false;
	int g_sitesPatched = 0;

	// Rollback ring: the resim cost is averaged over the last kRollbackRing rollbacks, so a
	// value survives quiet stretches with no rollback at all (the machine did not get faster).
	constexpr int kRollbackRing = 64;
	struct RollbackSample
	{
		long long at;
		uint32_t us;
		uint8_t frames;
	};
	RollbackSample g_rollbacks[kRollbackRing] = {};
	int g_rollbackCount = 0; // valid entries, up to kRollbackRing
	int g_rollbackNext = 0;
	uint32_t g_totalRollbacks = 0;
	uint32_t g_totalResimFrames = 0;

	// Rollback time spent since the last Present, and its value at the last EndScene. The
	// difference between them is limiter-time rollback, which is outside the window anyway.
	long long g_resimTicksSincePresent = 0;
	long long g_resimTicksAtEndScene = 0;

	void NoteRollbackInBucket(uint8_t frames); // the 2 s window, defined below

	void __fastcall AdjustSimulationTimed(void* sync, void* /*edx*/, int seekTo)
	{
		const int frames = *(int*)((char*)sync + kSyncFrameCountOffset) - seekTo;
		const long long t0 = Now();
		g_adjustSimulation(sync, seekTo);
		const long long ticks = Now() - t0;

		g_resimTicksSincePresent += ticks;

		if (frames > 0 && frames < 256)
		{
			RollbackSample& s = g_rollbacks[g_rollbackNext];
			s.at = t0;
			s.us = TicksToUs(ticks);
			s.frames = (uint8_t)frames;
			g_rollbackNext = (g_rollbackNext + 1) % kRollbackRing;
			if (g_rollbackCount < kRollbackRing)
				++g_rollbackCount;
			++g_totalRollbacks;
			g_totalResimFrames += (uint32_t)frames;
			NoteRollbackInBucket((uint8_t)frames);
		}
	}

	bool PatchCallSite(char* base, const CallSite& site)
	{
		unsigned char* const call = (unsigned char*)(base + site.callRva);
		unsigned char* const guard = call - site.guardLength;
		if (IsBadReadPtr(guard, site.guardLength + 5))
		{
			LOG(1, "[PerfMonitor] %s call site unreadable; not timing it\n", site.name);
			return false;
		}
		if (memcmp(guard, site.guard, site.guardLength) != 0 || call[0] != 0xE8)
		{
			LOG(1, "[PerfMonitor] %s call site does not match this build; not timing it\n", site.name);
			return false;
		}
		int32_t rel = 0;
		memcpy(&rel, call + 1, 4);
		const uintptr_t target = (uintptr_t)call + 5 + rel;
		if (target != (uintptr_t)g_adjustSimulation)
		{
			LOG(1, "[PerfMonitor] %s call goes to 0x%08X, not AdjustSimulation; not timing it\n",
				site.name, (unsigned)target);
			return false;
		}

		const int32_t newRel = (int32_t)((uintptr_t)&AdjustSimulationTimed - ((uintptr_t)call + 5));
		DWORD oldProtect = 0;
		if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			LOG(1, "[PerfMonitor] VirtualProtect failed (%lu) at %s\n", GetLastError(), site.name);
			return false;
		}
		memcpy(call + 1, &newRel, 4);
		VirtualProtect(call + 1, 4, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), call, 5);
		return true;
	}

	// Called from Tick, i.e. on the game thread between frames, which is also the only thread
	// that runs GGPO - nothing can be executing those call instructions while they change.
	// Installed the first time an online match runs, never removed (removing it would just be the
	// same write again; the timer is two QPC reads around the original call).
	void EnsureRollbackTimingInstalled()
	{
		if (g_installAttempted)
			return;
		g_installAttempted = true;

		char* const base = GetBbcfBaseAdress();
		if (!base)
		{
			LOG(1, "[PerfMonitor] no module base; resim timing disabled\n");
			return;
		}
		g_adjustSimulation = (AdjustSimulationFn)(base + kAdjustSimulationRva);

		// The function itself must look like what the call sites were matched against:
		// push ebp / mov ebp,esp / sub esp,420h.
		const unsigned char kPrologue[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x20, 0x04, 0x00, 0x00 };
		if (IsBadReadPtr((void*)g_adjustSimulation, sizeof(kPrologue)) ||
			memcmp((void*)g_adjustSimulation, kPrologue, sizeof(kPrologue)) != 0)
		{
			LOG(1, "[PerfMonitor] AdjustSimulation does not match this build; resim timing disabled\n");
			return;
		}

		for (const CallSite& site : kCallSites)
		{
			if (PatchCallSite(base, site))
				++g_sitesPatched;
		}
		LOG(1, "[PerfMonitor] resim timing installed on %d of %d AdjustSimulation call sites\n",
			g_sitesPatched, (int)(sizeof(kCallSites) / sizeof(kCallSites[0])));
	}

	// ---- frame timing -------------------------------------------------------------------

	// ~2 s at 60 FPS.
	constexpr int kFrameRing = 120;
	uint32_t g_frameWorkUs[kFrameRing] = {};
	uint32_t g_frameIntervalUs[kFrameRing] = {};
	uint32_t g_frameModUs[kFrameRing] = {};
	int g_frameCount = 0;
	int g_frameNext = 0;

	long long g_lastPresent = 0;
	long long g_lastEndScene = 0;
	long long g_endSceneBegin = 0;
	long long g_modTicksSincePresent = 0; // the mod's own EndScene work (overlay, managers)
	bool g_measuring = false; // set by Tick: in a match

	// ---- network sampling ---------------------------------------------------------------

	struct NetSample
	{
		bool valid;
		int rttMs;
		DWORD lastRecvMs;
		int sendQueue;
		int localAdvantage;
		int remoteAdvantage;
		int prediction;
	};

	// Which step of the walk failed, for the once-per-match log line. Order = walk order.
	enum NetReadStep
	{
		NetRead_Ok = 0,
		NetRead_NoWrapperPtr,   // [base+0x142A920] is null
		NetRead_NoSession,      // wrapper+0 (GGPOSession*) is null: no GGPO session
		NetRead_NotPeer2Peer,   // the session is not a Peer2PeerBackend (spectator backend)
		NetRead_BadEndpoints,   // endpoint count/array out of range
		NetRead_NoRunningPeer,  // no endpoint with a live socket in the Running state
		NetRead_Fault,          // an access violation during the walk
	};

	const char* NetReadStepName(int step)
	{
		switch (step)
		{
		case NetRead_Ok:            return "ok";
		case NetRead_NoWrapperPtr:  return "netplay wrapper pointer [base+0x142A920] is null";
		case NetRead_NoSession:     return "no GGPO session at wrapper+0";
		case NetRead_NotPeer2Peer:  return "session vtable is not Peer2PeerBackend (0x985B7C)";
		case NetRead_BadEndpoints:  return "endpoint count/array out of range";
		case NetRead_NoRunningPeer: return "no endpoint with a socket in state Running(2)";
		default:                    return "access violation while walking the session";
		}
	}

	// No C++ objects in here: __try cannot unwind them. Reads only.
	//
	// [base+0x142A920] (DAT_0182A920) points at the netplay wrapper, a static object at VA
	// 0xA5BD08. Its layout, confirmed in four crash dumps (Toby, 2026-10-21/22): +0 GGPOSession*
	// (the Peer2PeerBackend - ggpo_idle FUN_00783430 passes *this to FUN_0077C700), +4 the game's
	// state-save interface (load_game_state calls its vtable), +8 the desync checker, +0xC the
	// GGPOSessionCallbacks block (begin_game 0x668190, save 0x7842B0, load 0x783F20, ...).
	// The first build read the backend from +4, failed the vtable check every frame, and showed
	// "-" for every network number on both sides.
	int ReadGgpoNet(char* base, NetSample* out)
	{
		out->valid = false;
		__try
		{
			const uintptr_t wrapper = *(uintptr_t*)(base + ADDR_RollbackSessionManagerPtr);
			if (!wrapper)
				return NetRead_NoWrapperPtr;
			const uintptr_t backend = *(uintptr_t*)(wrapper + OFF_GgpoMgrBackend);
			if (!backend)
				return NetRead_NoSession;
			// Players only: a spectator's backend is another class with another layout.
			if (*(uintptr_t*)backend != (uintptr_t)(base + ADDR_GgpoPeer2PeerBackendVtable))
				return NetRead_NotPeer2Peer;
			const int count = *(int*)(backend + OFF_GgpoBackendNumEndpoints);
			const uintptr_t endpoints = *(uintptr_t*)(backend + OFF_GgpoBackendEndpoints);
			if (count < 1 || count > 8 || !endpoints)
				return NetRead_BadEndpoints;
			for (int i = 0; i < count; ++i)
			{
				const uintptr_t ep = endpoints + (uintptr_t)i * SIZE_GgpoEndpoint;
				if (*(uintptr_t*)(ep + OFF_GgpoEpUdp) == 0 || *(int*)(ep + OFF_GgpoEpState) != 2)
					continue;
				out->rttMs = *(int*)(ep + OFF_GgpoEpRoundTripMs);
				out->lastRecvMs = *(DWORD*)(ep + OFF_GgpoEpLastRecvTimeMs);
				out->sendQueue = *(int*)(ep + OFF_GgpoEpPendingOutput);
				out->localAdvantage = *(int*)(ep + OFF_GgpoEpLocalFrameAdvantage);
				out->remoteAdvantage = *(int*)(ep + OFF_GgpoEpRemoteFrameAdvantage);
				out->prediction = *(int*)(backend + OFF_GgpoBackendSyncFrameCount) -
					*(int*)(backend + OFF_GgpoBackendSyncLastConfirmed);
				out->valid = true;
				return NetRead_Ok;
			}
			return NetRead_NoRunningPeer;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out->valid = false;
			return NetRead_Fault;
		}
	}

	// 2 s window as four 500 ms buckets, so a worst value ages out without storing frames.
	constexpr int kBuckets = 4;
	struct NetBucket
	{
		uint32_t maxGapMs;
		int maxPrediction;
		uint32_t rollbacks;
		uint32_t rollbackFrames;
		uint8_t maxDepth;
	};
	NetBucket g_buckets[kBuckets] = {};
	int g_bucket = 0;
	NetSample g_lastNet = {};
	bool g_netSeenThisMatch = false;
	bool g_loggedNetReadThisMatch = false;
	bool g_rttSeenThisMatch = false;
	double SecondsSinceMatchStart();

	constexpr int kRttRing = 8;
	int g_rtt[kRttRing] = {};
	int g_rttCount = 0;
	int g_rttNext = 0;

	int StablePingMs()
	{
		if (g_rttCount < 3)
			return g_lastNet.rttMs;
		int sorted[kRttRing];
		for (int i = 0; i < g_rttCount; ++i)
			sorted[i] = g_rtt[(g_rttNext - g_rttCount + i + kRttRing) % kRttRing];
		std::sort(sorted, sorted + g_rttCount);
		return sorted[g_rttCount / 2];
	}

	char* g_base = nullptr;

	void NoteRollbackInBucket(uint8_t frames)
	{
		NetBucket& b = g_buckets[g_bucket];
		++b.rollbacks;
		b.rollbackFrames += frames;
		if (frames > b.maxDepth)
			b.maxDepth = frames;
	}

	void SampleNetworkPerFrame()
	{
		if (!g_base)
			return;
		NetSample s;
		const int step = ReadGgpoNet(g_base, &s);
		if (step != NetRead_Ok)
		{
			// Once per match, so a wrong walk never again shows "-" silently. The early frames of
			// a match can legitimately have no session yet; log only after 3 s of failing.
			// After a session that worked, a missing session means the match ended normally.
			if (!g_loggedNetReadThisMatch && !g_netSeenThisMatch && SecondsSinceMatchStart() > 3.0)
			{
				g_loggedNetReadThisMatch = true;
				LOG(1, "[PerfMonitor] network stats unavailable: %s (step %d)\n", NetReadStepName(step), step);
			}
			return;
		}
		if (!g_netSeenThisMatch)
		{
			LOG(1, "[PerfMonitor] network stats OK: rtt %d ms, unacked %d, frames behind %d/%d, unconfirmed %d\n",
				s.rttMs, s.sendQueue, s.localAdvantage, s.remoteAdvantage, s.prediction);
		}
		g_lastNet = s;
		g_netSeenThisMatch = true;
		// GGPO's RTT reads 0 until the first quality reply (about a second in): unknown, not 0.
		if (s.rttMs > 0)
			g_rttSeenThisMatch = true;
		NetBucket& b = g_buckets[g_bucket];
		const DWORD now = timeGetTime();
		if (s.lastRecvMs != 0 && now >= s.lastRecvMs)
		{
			const uint32_t gap = now - s.lastRecvMs;
			if (gap < 60000 && gap > b.maxGapMs)
				b.maxGapMs = gap;
		}
		if (s.prediction > b.maxPrediction && s.prediction < 1000)
			b.maxPrediction = s.prediction;
	}

	// ---- state --------------------------------------------------------------------------

	Report g_local = {};
	long long g_nextRefresh = 0;
	int g_refreshCounter = 0;

	bool g_inMatch = false;
	long long g_matchStartAt = 0;
	bool g_onlinePlayer = false;
	bool g_spectating = false;
	bool g_spectatorTest = false; // dev-only offline injection, see SetSpectatorTestMode
	long long g_spectatorTestStart = 0;
	int g_spectatorTestScenario = 0;
	int g_spectatorTestLastScenario = -1;
	constexpr int kSpectatorTestScenarios = 10;
	constexpr int kSpectatorTestNoModScenario = 9; // P2 shows as a player without the mod

	long long g_sectionVisibleAt = 0; // last frame the stats window was open
	long long g_widgetsVisibleAt = 0; // last frame the connection icons were drawn
	bool g_sessionDirect = false;     // the game's Steam P2P session with the opponent is direct
	long long g_linkCheckedMatchStart = -1;
	long long g_wantingSince = 0;     // when we started asking, for "not sharing"
	bool g_relay = false;

	// Everyone we exchange with this refresh. For a player: the opponent and the spectators
	// of this match. For a spectator: the two players. Rebuilt every 500 ms from the room.
	struct Peer
	{
		uint64_t steamId;
		uint16_t memberIndex;
		uint8_t side;       // 0 = P1, 1 = P2, 2 = spectator
		bool hasMod;
		bool capable;       // advertised kModCap_PerformanceReport in its acknowledge
		bool routeFlagAware; // advertised kModCap_RouteRequestFlag: Flag_WantsRoute is meaningful
	};
	constexpr int kMaxPeers = MAX_PLAYERS_IN_ROOM;
	Peer g_peers[kMaxPeers] = {};
	int g_peerCount = 0;
	int g_opponentPeer = -1;    // player only: index into g_peers
	int g_playerPeer[2] = { -1, -1 }; // spectator only: P1 / P2

	// What arrived, by Steam ID. Survives the 500 ms peer rebuilds.
	struct RemoteState
	{
		uint64_t steamId;
		Report report;
		long long receivedAt; // last full report
		uint32_t received;
		long long wantsAt;    // last packet from it with Flag_WantsReports
		long long wantsRouteAt; // last packet from it asking for the route check (its window is open)
	};
	RemoteState g_remotes[kMaxPeers] = {};
	bool g_loggedFirstReportThisMatch = false;
	uint32_t g_sentThisMatch = 0;

	double SecondsSince(long long t, long long now)
	{
		return g_qpcFreq > 0 ? (double)(now - t) / (double)g_qpcFreq : 0.0;
	}

	double SecondsSinceMatchStart()
	{
		return SecondsSince(g_matchStartAt, Now());
	}

	RemoteState* FindRemote(uint64_t steamId)
	{
		for (RemoteState& r : g_remotes)
			if (r.steamId == steamId && steamId != 0)
				return &r;
		return nullptr;
	}

	RemoteState* FindOrAddRemote(uint64_t steamId)
	{
		if (RemoteState* r = FindRemote(steamId))
			return r;
		RemoteState* oldest = &g_remotes[0];
		for (RemoteState& r : g_remotes)
		{
			if (r.steamId == 0) { oldest = &r; break; }
			const long long last = (std::max)(r.receivedAt, r.wantsAt);
			if (last < (std::max)(oldest->receivedAt, oldest->wantsAt))
				oldest = &r;
		}
		memset(oldest, 0, sizeof(*oldest));
		oldest->steamId = steamId;
		return oldest;
	}

	// The route check (ping-location exchange) needs the window itself; reports also flow for
	// the connection icons.
	bool LocalWantsRoute(long long now)
	{
		return g_sectionVisibleAt != 0 && SecondsSince(g_sectionVisibleAt, now) < 1.5;
	}

	bool LocalWantsReports(long long now)
	{
		return LocalWantsRoute(now) ||
			(g_widgetsVisibleAt != 0 && SecondsSince(g_widgetsVisibleAt, now) < 1.5);
	}

	bool PeerWantsRoute(uint64_t steamId, long long now)
	{
		const RemoteState* r = FindRemote(steamId);
		return r && r->wantsRouteAt != 0 && SecondsSince(r->wantsRouteAt, now) < 2.5;
	}

	// Whether this peer's build sets Flag_WantsRoute; an older one sets Flag_WantsReports only
	// from its stats window, so that flag alone already means "window open".
	bool PeerKnowsRouteFlag(uint64_t steamId)
	{
		for (int i = 0; i < g_peerCount; ++i)
			if (g_peers[i].steamId == steamId)
				return g_peers[i].routeFlagAware;
		return false;
	}

	// A peer that wants our reports re-asks every second while its section is open, so 2.5 s
	// without a request means it closed it (1.5 s of "still open" + one missed request).
	bool PeerWantsReports(uint64_t steamId, long long now)
	{
		const RemoteState* r = FindRemote(steamId);
		return r && r->wantsAt != 0 && SecondsSince(r->wantsAt, now) < 2.5;
	}

	// The one place a received report is stored, used by the network path (after the room
	// checks in OnReportPacket) and by the dev-only offline injection.
	void IngestReport(uint64_t steamId, const unsigned char* data, uint32_t size, bool fullReportAllowed)
	{
		if (size < 2 || size > MAX_DATA_SIZE)
			return;
		Report report = {};
		memcpy(&report, data, (std::min)((size_t)size, sizeof(report)));
		if (report.wireVersion < 1)
			return;

		RemoteState* const r = FindOrAddRemote(steamId);
		const long long now = Now();
		if (report.flags & Flag_WantsReports)
			r->wantsAt = now;
		if ((report.flags & Flag_WantsRoute) ||
			((report.flags & Flag_WantsReports) && !PeerKnowsRouteFlag(steamId)))
			r->wantsRouteAt = now;
		// A request-only packet (a spectator asking, or a truncated header) carries no numbers.
		if (!fullReportAllowed || (report.flags & Flag_HasFrames) == 0)
			return;
		r->report = report;
		r->receivedAt = now;
		++r->received;

		if (!g_loggedFirstReportThisMatch)
		{
			g_loggedFirstReportThisMatch = true;
			LOG(1, "[PerfMonitor] first report this match from %llu: p90 %u us, resim %u us/frame, ping %u ms (v%u, %u bytes)\n",
				(unsigned long long)steamId, report.gameDrawP90Us, report.resimFrameAvgUs, (unsigned)report.pingMs,
				(unsigned)report.wireVersion, (unsigned)size);
		}
	}

	void OnMatchStarted()
	{
		g_matchStartAt = Now();
		g_frameCount = 0;
		g_frameNext = 0;
		g_lastPresent = 0;
		g_lastEndScene = 0;
		g_totalRollbacks = 0;
		g_totalResimFrames = 0;
		memset(g_buckets, 0, sizeof(g_buckets));
		g_rttCount = 0;
		g_rttNext = 0;
		g_netSeenThisMatch = false;
		g_loggedNetReadThisMatch = false;
		g_rttSeenThisMatch = false;
		g_loggedLocationRejectThisMatch = false;
		g_loggedLocationResendThisMatch = false;
		g_loggedFirstReportThisMatch = false;
		if (g_sentThisMatch > 0)
			LOG(1, "[PerfMonitor] previous match: %u packets sent\n", g_sentThisMatch);
		g_sentThisMatch = 0;
		memset(&g_local, 0, sizeof(g_local));
	}

	void BuildLocalReport(long long now)
	{
		Report r = {};
		r.wireVersion = 1;

		if (g_frameCount > 0)
		{
			uint32_t work[kFrameRing];
			uint64_t workSum = 0, intervalSum = 0, modSum = 0;
			uint32_t workMax = 0, intervalMax = 0;
			for (int i = 0; i < g_frameCount; ++i)
			{
				work[i] = g_frameWorkUs[i];
				workSum += g_frameWorkUs[i];
				intervalSum += g_frameIntervalUs[i];
				modSum += g_frameModUs[i];
				workMax = (std::max)(workMax, g_frameWorkUs[i]);
				intervalMax = (std::max)(intervalMax, g_frameIntervalUs[i]);
			}
			int p90Index = (g_frameCount * 9) / 10;
			if (p90Index >= g_frameCount)
				p90Index = g_frameCount - 1;
			std::nth_element(work, work + p90Index, work + g_frameCount);

			r.flags |= Flag_HasFrames;
			r.frameSamples = (uint16_t)g_frameCount;
			r.gameDrawP90Us = work[p90Index];
			r.gameDrawAvgUs = (uint32_t)(workSum / (uint64_t)g_frameCount);
			r.gameDrawMaxUs = workMax;
			r.intervalAvgUs = (uint32_t)(intervalSum / (uint64_t)g_frameCount);
			r.intervalMaxUs = intervalMax;
			r.modAvgUs = (uint32_t)(modSum / (uint64_t)g_frameCount);
		}

		if (g_rollbackCount > 0)
		{
			// A rollback costs one state load (fixed) plus one sim+save per frame:
			// T = L + N*F. Dividing T by N (the first build) charged the whole load to a
			// 1-frame rollback and called a 231 FPS machine "may lag". With rollbacks of at
			// least two different depths, fit L and F by least squares; otherwise keep the
			// average per frame and leave L at 0 (an overestimate of F, never an underestimate
			// of the total for the depths actually seen).
			double sumN = 0, sumT = 0, sumNN = 0, sumNT = 0;
			uint32_t worstPerFrame = 0;
			uint8_t minDepth = 255, maxDepth = 0;
			for (int i = 0; i < g_rollbackCount; ++i)
			{
				const RollbackSample& s = g_rollbacks[i];
				sumN += s.frames;
				sumT += s.us;
				sumNN += (double)s.frames * s.frames;
				sumNT += (double)s.frames * s.us;
				worstPerFrame = (std::max)(worstPerFrame, s.us / (uint32_t)s.frames);
				minDepth = (std::min)(minDepth, s.frames);
				maxDepth = (std::max)(maxDepth, s.frames);
			}
			double perFrame = sumN > 0 ? sumT / sumN : 0.0;
			double load = 0.0;
			const double n = (double)g_rollbackCount;
			const double varN = sumNN - sumN * sumN / n;
			if (maxDepth > minDepth && varN > 0.0)
			{
				const double slope = (sumNT - sumN * sumT / n) / varN;
				const double intercept = (sumT - slope * sumN) / n;
				// A few noisy rollbacks can fit a near-flat line: 26 us/frame ("resim 64103%")
				// from 10 rollbacks on 10-24. A frame can't resimulate in a quarter of the
				// average, so keep the average until the fit is plausible.
				if (slope >= perFrame / 4.0 && intercept >= 0.0)
				{
					perFrame = slope;
					load = intercept;
				}
			}
			if (perFrame > 0.0)
			{
				r.flags |= Flag_HasResim;
				r.rollbackSamples = (uint16_t)g_rollbackCount;
				r.resimFrameAvgUs = (uint32_t)perFrame;
				r.resimLoadUs = (uint32_t)load;
				r.resimFrameMaxUs = worstPerFrame;
			}
		}
		r.relayPingMs = (uint16_t)(std::max)(0, (std::min)(RelayPingMs(), 65535));
		r.linkType = (uint8_t)g_linkType;
		// Route-check numbers: only for the current opponent, and only numbers (see header).
		if (g_peerLocationValid && g_opponentPeer >= 0 && g_peers[g_opponentPeer].steamId == g_peerLocationSteamId)
		{
			r.flags |= Flag_HaveYourLocation;
			r.expectedPingMs = (uint16_t)(std::min)(g_expectedPingMs, 65535);
			r.crossPingMs = (uint16_t)(std::min)(g_crossPingMs, 65535);
		}

		// Recent rollback counts and the network worsts come from the 2 s buckets.
		uint32_t recent = 0, recentFrames = 0, maxGap = 0;
		int maxPred = 0;
		uint8_t deepest = 0;
		for (const NetBucket& b : g_buckets)
		{
			recent += b.rollbacks;
			recentFrames += b.rollbackFrames;
			maxGap = (std::max)(maxGap, b.maxGapMs);
			maxPred = (std::max)(maxPred, b.maxPrediction);
			deepest = (std::max)(deepest, b.maxDepth);
		}
		r.rollbacksRecent = (uint16_t)(std::min)(recent, 65535u);
		r.rollbackFramesRecent = (uint16_t)(std::min)(recentFrames, 65535u);
		r.maxDepthRecent = deepest;
		r.totalRollbacks = g_totalRollbacks;
		r.totalResimFrames = g_totalResimFrames;

		if (g_netSeenThisMatch && g_rttSeenThisMatch)
		{
			r.flags |= Flag_HasNet;
			// The median of the last 8 one-second samples once there are 3: the verdicts and the route
			// check read this, and a single spike must not flip them to "use a VPN".
			r.pingMs = (uint16_t)(std::max)(0, (std::min)(StablePingMs(), 65535));
			r.maxRecvGapMs = (uint16_t)(std::min)(maxGap, 65535u);
			r.sendQueue = (uint16_t)(std::max)(0, (std::min)(g_lastNet.sendQueue, 65535));
			r.localFramesBehind = (int8_t)(std::max)(-127, (std::min)(g_lastNet.localAdvantage, 127));
			r.remoteFramesBehind = (int8_t)(std::max)(-127, (std::min)(g_lastNet.remoteAdvantage, 127));
			r.maxPrediction = (uint8_t)(std::max)(0, (std::min)(maxPred, 255));
			if (g_rttCount >= 2)
			{
				// Mean change between consecutive 1 s RTT samples, oldest to newest.
				int sum = 0, n = 0;
				for (int i = 1; i < g_rttCount; ++i)
				{
					const int a = g_rtt[(g_rttNext - g_rttCount + i - 1 + kRttRing) % kRttRing];
					const int b = g_rtt[(g_rttNext - g_rttCount + i + kRttRing) % kRttRing];
					sum += a > b ? a - b : b - a;
					++n;
				}
				r.jitterMs = (uint16_t)(n ? sum / n : 0);
			}
			if (g_relay)
				r.flags |= Flag_UsingRelay;
		}
		if (LocalWantsReports(now))
			r.flags |= Flag_WantsReports;
		if (LocalWantsRoute(now))
			r.flags |= Flag_WantsRoute;

		g_local = r;
	}

	// Who we exchange with, from the room. Allocation-free except FindIMPlayer's copy; only
	// from the 500 ms refresh.
	void RefreshOnlineContext()
	{
		g_onlinePlayer = false;
		g_spectating = false;
		g_peerCount = 0;
		g_opponentPeer = -1;
		g_playerPeer[0] = g_playerPeer[1] = -1;

		if (g_spectatorTest)
		{
			// Dev-only: two fake players fed by SpectatorTestInject. No room involved.
			g_spectating = true;
			for (int side = 0; side < 2; ++side)
			{
				Peer& p = g_peers[g_peerCount];
				p.steamId = 0x0110000100000001ULL + (uint64_t)side;
				p.memberIndex = (uint16_t)side;
				p.side = (uint8_t)side;
				p.hasMod = !(side == 1 && g_spectatorTestScenario == kSpectatorTestNoModScenario);
				p.capable = p.hasMod;
				p.routeFlagAware = p.hasMod;
				g_playerPeer[side] = g_peerCount++;
			}
			return;
		}

		if (!g_gameVals.pGameMode || !g_gameVals.pGameState)
			return;
		if (*g_gameVals.pGameMode != GameMode_Online || *g_gameVals.pGameState != GameState_InMatch)
			return;

		RoomManager* const room = g_interfaces.pRoomManager;
		if (!room || !room->IsRoomFunctional())
			return;
		const RoomMemberEntry* const self = room->GetThisPlayerRoomMemberEntry();
		if (!self || self->matchId == 0)
			return;

		// Seated or watching, and which side: the game's own seat flags (RoomMemberEntry), which
		// also hold in lobby (FFA) rooms, where matchPlayerIndex is just the lobby seat.
		const bool selfSpectating = !RoomManager::IsSeatedEntry(self);
		g_spectating = selfSpectating;
		g_onlinePlayer = !selfSpectating;

		for (uint16_t i = 0; i < MAX_PLAYERS_IN_ROOM && g_peerCount < kMaxPeers; ++i)
		{
			const RoomMemberEntry* const entry = room->GetRoomMemberEntryByIndex(i);
			if (!entry || entry == self || entry->steamId == self->steamId)
				continue;
			if (entry->matchId != self->matchId)
				continue;
			const bool entryIsPlayer = RoomManager::IsSeatedEntry(entry);
			// A spectator only exchanges with the two players.
			if (selfSpectating && !entryIsPlayer)
				continue;

			Peer& p = g_peers[g_peerCount];
			p.steamId = entry->steamId;
			p.memberIndex = entry->memberIndex;
			p.side = entryIsPlayer ? (uint8_t)(entry->seatFlags & 1) : 2;
			IMPlayer imPlayer;
			p.hasMod = room->FindIMPlayer(entry->steamId, &imPlayer);
			p.capable = p.hasMod && imPlayer.versionKnown && (imPlayer.modCaps & kModCap_PerformanceReport) != 0;
			p.routeFlagAware = p.capable && (imPlayer.modCaps & kModCap_RouteRequestFlag) != 0;

			if (!selfSpectating && entryIsPlayer && g_opponentPeer < 0)
				g_opponentPeer = g_peerCount;
			if (selfSpectating && p.side <= 1 && g_playerPeer[p.side] < 0)
				g_playerPeer[p.side] = g_peerCount;
			++g_peerCount;
		}

		// Local Steam query: is this P2P session going through Valve's relay?
		g_sessionDirect = false;
		ULONG remoteIp = 0;
		if (g_opponentPeer >= 0 && g_interfaces.pSteamNetworkingWrapper)
		{
			P2PSessionState_t state = {};
			if (g_interfaces.pSteamNetworkingWrapper->GetP2PSessionState(CSteamID(g_peers[g_opponentPeer].steamId), &state))
			{
				g_relay = state.m_bUsingRelay != 0;
				g_sessionDirect = state.m_bConnectionActive != 0 && state.m_bUsingRelay == 0;
				// Used only for a local routing-table lookup (which adapter reaches them);
				// never stored past this call, logged or sent.
				remoteIp = g_sessionDirect ? (ULONG)state.m_nRemoteIP : 0;
			}
		}
		if (g_peerLocationValid && (g_opponentPeer < 0 || g_peers[g_opponentPeer].steamId != g_peerLocationSteamId))
			ForgetPeerLocation();
		// Wi-Fi or wired: once per match, on a thread-pool thread.
		if (!g_onlinePlayer)
			return;
		if (g_linkCheckedMatchStart != g_matchStartAt)
		{
			g_linkCheckedMatchStart = g_matchStartAt;
			StartLinkTypeCheck(remoteIp);
		}
	}

	void SendTo(const Peer& peer, const void* data, uint16_t size)
	{
		RoomManager* const room = g_interfaces.pRoomManager;
		NetworkManager* const net = g_interfaces.pNetworkManager;
		if (!room || !net)
			return;
		const RoomMemberEntry* const self = room->GetThisPlayerRoomMemberEntry();
		if (!self)
			return;

		Packet packet(const_cast<void*>(data), size, PacketType_PerformanceReport, self->memberIndex);
		CSteamID to(peer.steamId);
		// Unreliable: a status superseded a second later has no business being retransmitted.
		net->SendPacketQuiet(&to, &packet, false);
		++g_sentThisMatch;
	}

	// The first 8 bytes of a Report with only Flag_WantsReports: "send me yours". Receivers
	// zero-fill the rest, see that HasFrames is clear and store nothing but the request.
	struct WantRequest
	{
		uint8_t wireVersion;
		uint8_t flags;
		uint16_t frameSamples;
		uint32_t gameDrawP90Us;
	};
	static_assert(sizeof(WantRequest) == 8, "WantRequest is the head of Report");

	uint32_t Fnv1a(const char* text, uint32_t seed)
	{
		uint32_t h = 2166136261u ^ seed;
		for (; *text; ++text)
			h = (h ^ (unsigned char)*text) * 16777619u;
		return h;
	}

	// PacketType_PingLocation payload: header, then the location text (no terminator).
#pragma pack(push, 1)
	struct PingLocationHeader
	{
		uint8_t version;    // 1
		uint8_t reserved;
		uint16_t length;    // characters that follow, 1..kMaxLocationChars
		uint32_t nearestPop;
	};
#pragma pack(pop)

	// DIRECT-ONLY RULE (see PerformanceMonitor.h "ROUTE CHECK"): this is the one place the
	// ping location and nearest-relay code leave this PC, and only to the opponent, only while
	// the game's P2P session with them is direct, only while one of the two has the stats
	// window open. Sent once per match, again if the text changes, and re-sent every 8 s until
	// the opponent's report carries Flag_HaveYourLocation (the first build sent it once, and it
	// was lost when it reached a peer whose Steam relay access had not started yet).
	void MaybeSendPingLocation(long long now, bool localWants)
	{
		if (g_opponentPeer < 0 || !g_netUtils || g_relayBestPop == 0)
			return;
		const Peer& opp = g_peers[g_opponentPeer];
		if (!opp.capable)
			return;
		if (!localWants && !PeerWantsRoute(opp.steamId, now))
			return;
		// Re-check right before sending rather than trusting the 500 ms cache.
		P2PSessionState_t state = {};
		if (!g_interfaces.pSteamNetworkingWrapper ||
			!g_interfaces.pSteamNetworkingWrapper->GetP2PSessionState(CSteamID(opp.steamId), &state) ||
			!state.m_bConnectionActive || state.m_bUsingRelay)
			return;

		unsigned char buffer[sizeof(PingLocationHeader) + kMaxLocationChars + 1];
		char* const text = (char*)(buffer + sizeof(PingLocationHeader));
		if (!LocalLocationString(g_netUtils, text, kMaxLocationChars + 1))
			return;
		const size_t length = strnlen(text, kMaxLocationChars + 1);
		const uint32_t hash = Fnv1a(text, g_relayBestPop);
		const bool sameAsSent = g_locationSentTo == opp.steamId && g_locationSentMatchStart == g_matchStartAt &&
			g_locationSentHash == hash;
		const RemoteState* const remote = FindRemote(opp.steamId);
		const bool peerHasIt = remote && remote->received > 0 && (remote->report.flags & Flag_HaveYourLocation) != 0 &&
			SecondsSince(remote->receivedAt, now) < 5.0;
		const bool resendDue = sameAsSent && !peerHasIt && now >= g_locationResendAt;
		if ((!sameAsSent || resendDue) && length > 0 && length <= (size_t)kMaxLocationChars)
		{
			PingLocationHeader header = { 1, 0, (uint16_t)length, g_relayBestPop };
			memcpy(buffer, &header, sizeof(header));
			RoomManager* const room = g_interfaces.pRoomManager;
			NetworkManager* const net = g_interfaces.pNetworkManager;
			const RoomMemberEntry* const self = room ? room->GetThisPlayerRoomMemberEntry() : nullptr;
			if (net && self)
			{
				Packet packet(buffer, (uint16_t)(sizeof(header) + length), PacketType_PingLocation, self->memberIndex);
				CSteamID to(opp.steamId);
				net->SendPacketQuiet(&to, &packet, true);
				SecureZeroMemory(packet.data, sizeof(packet.data));
				g_locationSentTo = opp.steamId;
				g_locationSentMatchStart = g_matchStartAt;
				g_locationSentHash = hash;
				g_locationResendAt = now + 8 * g_qpcFreq;
				++g_sentThisMatch;
				if (!resendDue)
				{
					LOG(1, "[PerfMonitor] route check: sent our location to the opponent (direct session)\n");
				}
				else if (!g_loggedLocationResendThisMatch)
				{
					g_loggedLocationResendThisMatch = true;
					LOG(1, "[PerfMonitor] route check: opponent has not confirmed our location yet; re-sending every 8 s\n");
				}
			}
		}
		SecureZeroMemory(buffer, sizeof(buffer));
	}

	void TalkToPeers(long long now)
	{
		const bool localWants = LocalWantsReports(now);
		if (localWants && g_wantingSince == 0)
			g_wantingSince = now;
		if (!localWants)
			g_wantingSince = 0;

		if (g_spectatorTest)
			return;

		if (g_spectating)
		{
			// Ask each capable player for its numbers while our section is open.
			if (!localWants)
				return;
			WantRequest want = { 1, Flag_WantsReports, 0, 0 };
			for (int side = 0; side < 2; ++side)
			{
				const int i = g_playerPeer[side];
				if (i >= 0 && g_peers[i].capable)
					SendTo(g_peers[i], &want, (uint16_t)sizeof(want));
			}
			return;
		}

		if (!g_onlinePlayer || (g_local.flags & Flag_HasFrames) == 0)
			return;
		MaybeSendPingLocation(now, LocalWantsRoute(now));
		for (int i = 0; i < g_peerCount; ++i)
		{
			const Peer& peer = g_peers[i];
			if (!peer.capable)
				continue;
			const bool isOpponent = i == g_opponentPeer;
			// The opponent gets it while either of us looks; a spectator only while it does.
			const bool send = isOpponent ? (localWants || PeerWantsReports(peer.steamId, now))
				: PeerWantsReports(peer.steamId, now);
			if (!send)
				continue;
			// A build without kModCap_RouteRequestFlag reads Flag_WantsReports as "stats window
			// open" and starts the route check on it, so only the window may set it for that
			// peer, never the connection icons alone.
			if (peer.routeFlagAware || (g_local.flags & Flag_WantsRoute))
				SendTo(peer, &g_local, (uint16_t)sizeof(g_local));
			else
			{
				Report legacy = g_local;
				legacy.flags = (uint8_t)(legacy.flags & ~Flag_WantsReports);
				SendTo(peer, &legacy, (uint16_t)sizeof(legacy));
			}
		}
	}

	OpponentView ViewForPeer(int peerIndex, long long now)
	{
		OpponentView view;
		if (peerIndex < 0)
		{
			view.state = Opp_NoOpponent;
			return view;
		}
		const Peer& peer = g_peers[peerIndex];
		view.steamId = peer.steamId;
		if (!peer.hasMod)
		{
			view.state = Opp_NoMod;
			return view;
		}
		if (!peer.capable)
		{
			view.state = Opp_OldMod;
			return view;
		}

		// A report from before this match (the previous game of a set) only counts once a new
		// one has had time to arrive; until then it is "waiting", not "stale".
		const RemoteState* const r = FindRemote(peer.steamId);
		const bool fromThisMatch = r && (r->receivedAt >= g_matchStartAt || SecondsSince(g_matchStartAt, now) > 5.0);
		if (r && r->received > 0 && fromThisMatch)
		{
			view.report = r->report;
			view.received = r->received;
			view.ageSeconds = SecondsSince(r->receivedAt, now);
			view.state = view.ageSeconds > 5.0 ? Opp_Stale : Opp_Live;
			return view;
		}

		view.state = (g_wantingSince != 0 && SecondsSince(g_wantingSince, now) > 5.0) ? Opp_NotSharing : Opp_Waiting;
		return view;
	}

	// Dev-only offline test of the spectator path: two fake players' reports, derived from
	// this PC's own, pushed through IngestReport exactly as received bytes would be (P2's
	// with trailing junk, to exercise the size-tolerant copy).
	// Cycles every 6 s through made-up situations so each wording can be checked:
	// 0 smooth (P1 wired, P2 Wi-Fi), 1 high ping with P2's line slow, 2 P2 unstable on Wi-Fi,
	// 3 route long - P2 should use a VPN, 4 route long - either, start with P2,
	// 5 Steam relays the session (route check cannot run),
	// 6 no rollbacks yet + only P2 has Steam's expected ping (shown in both columns),
	// 7 a location received before this PC's relay data is ready: this PC's own location is
	//   pushed through IngestPingLocation (the real receive/parse path) with the relay number
	//   hidden, and P1's expected/region ping come from the real Steam calls on it,
	// 8 P2's PC cannot hold 60 FPS, 9 P2 does not run the mod (its peer entry says so; see
	//   RefreshOnlineContext), for the connection icons' grey question mark.

	void IngestPingLocation(uint64_t steamId, const char* src, uint32_t length, uint32_t pop); // below

	void SpectatorTestFeedOwnLocation()
	{
		if (!g_netUtils || g_relayBestPop == 0)
			return;
		char text[kMaxLocationChars + 1];
		if (!LocalLocationString(g_netUtils, text, sizeof(text)))
			return;
		const int savedRelay = g_relayPingMs;
		g_relayPingMs = 0; // "not ready yet": the location must be kept anyway
		ForgetPeerLocation();
		IngestPingLocation(g_peers[g_playerPeer[1]].steamId, text, (uint32_t)strnlen(text, sizeof(text)), g_relayBestPop);
		SecureZeroMemory(text, sizeof(text));
		g_relayPingMs = savedRelay;
		RecomputeRouteNumbers(); // what the next 2 s poll would do once the relay data is in
	}

	void SpectatorTestInject()
	{
		const long long now = Now();
		if (g_spectatorTestStart == 0)
			g_spectatorTestStart = now;
		g_spectatorTestScenario = (int)(SecondsSince(g_spectatorTestStart, now) / 6.0) % kSpectatorTestScenarios;
		const bool scenarioChanged = g_spectatorTestScenario != g_spectatorTestLastScenario;
		g_spectatorTestLastScenario = g_spectatorTestScenario;

		Report p1 = g_local;
		p1.flags |= Flag_HasNet | Flag_HasResim;
		p1.frameSamples = 120;
		p1.pingMs = 38; p1.jitterMs = 3; p1.maxRecvGapMs = 25;
		p1.rollbacksRecent = 1; p1.maxDepthRecent = 1; p1.sendQueue = 2; p1.maxPrediction = 2;
		p1.localFramesBehind = 0; p1.remoteFramesBehind = 0;
		p1.rollbackSamples = 12; p1.resimLoadUs = 900; p1.resimFrameAvgUs = 1100; p1.resimFrameMaxUs = 2000;
		p1.relayPingMs = 15; p1.crossPingMs = 30; p1.expectedPingMs = 35; p1.linkType = Link_Wired;
		Report p2 = p1;
		p2.relayPingMs = 19; p2.crossPingMs = 34; p2.linkType = Link_WiFi;
		switch (g_spectatorTestScenario)
		{
		case 1:
			p1.pingMs = p2.pingMs = 142;
			p1.rollbacksRecent = p2.rollbacksRecent = 5;
			p1.maxDepthRecent = p2.maxDepthRecent = 3;
			p2.relayPingMs = 78;
			p1.crossPingMs = 70; p2.crossPingMs = 120; p1.expectedPingMs = p2.expectedPingMs = 130;
			break;
		case 2:
			p1.maxRecvGapMs = 260; // P2's packets reach P1 late
			p1.rollbacksRecent = 6; p1.maxDepthRecent = 4;
			break;
		case 3:
			p1.pingMs = p2.pingMs = 160;
			p1.crossPingMs = 45; p2.crossPingMs = 110; // P2's ISP takes the long way
			p1.expectedPingMs = p2.expectedPingMs = 60;
			p1.rollbacksRecent = p2.rollbacksRecent = 5; p1.maxDepthRecent = p2.maxDepthRecent = 4;
			break;
		case 4:
			p1.pingMs = p2.pingMs = 160;
			p1.crossPingMs = 55; p2.crossPingMs = 64; // both alike, the path between them is long
			p1.expectedPingMs = p2.expectedPingMs = 70;
			p1.rollbacksRecent = p2.rollbacksRecent = 5; p1.maxDepthRecent = p2.maxDepthRecent = 4;
			break;
		case 5:
			p1.flags |= Flag_UsingRelay; p2.flags |= Flag_UsingRelay;
			p1.pingMs = p2.pingMs = 120;
			p1.expectedPingMs = p2.expectedPingMs = 0; p1.crossPingMs = p2.crossPingMs = 0;
			break;
		case 6:
			p1.flags &= ~Flag_HasResim; p2.flags &= ~Flag_HasResim;
			p1.rollbackSamples = p2.rollbackSamples = 0;
			p1.totalRollbacks = p2.totalRollbacks = 0; p1.rollbacksRecent = p2.rollbacksRecent = 0;
			p1.maxDepthRecent = p2.maxDepthRecent = 0;
			p1.resimLoadUs = p2.resimLoadUs = p1.resimFrameAvgUs = p2.resimFrameAvgUs = 0;
			p1.expectedPingMs = 0; // only P2 has it: the window shares it
			break;
		case 7:
			if (scenarioChanged)
				SpectatorTestFeedOwnLocation();
			p1.expectedPingMs = (uint16_t)(g_peerLocationValid ? g_expectedPingMs : 0);
			p1.crossPingMs = (uint16_t)(g_peerLocationValid ? g_crossPingMs : 0);
			p1.relayPingMs = (uint16_t)g_relayPingMs;
			p2.expectedPingMs = 0;
			break;
		case 8:
			p2.gameDrawP90Us = 21000; p2.gameDrawAvgUs = 19000; p2.gameDrawMaxUs = 30000;
			p2.intervalAvgUs = 20000; // ~50 FPS
			break;
		default:
			break;
		}
		IngestReport(g_peers[g_playerPeer[0]].steamId, (const unsigned char*)&p1, (uint32_t)sizeof(p1), true);

		// P2's arrives with trailing junk, to exercise the size-tolerant copy.
		unsigned char bytes[sizeof(Report) + 16];
		memset(bytes, 0xCC, sizeof(bytes));
		memcpy(bytes, &p2, sizeof(p2));
		IngestReport(g_peers[g_playerPeer[1]].steamId, bytes, (uint32_t)sizeof(bytes), true);
	}
}

double PerformanceMonitor::CapacityPercent(uint32_t frameUs)
{
	return frameUs > 0 ? kBudgetUs / (double)frameUs * 100.0 : 0.0;
}

int PerformanceMonitor::RollbackHeadroomFrames(const Report& report)
{
	if ((report.flags & Flag_HasResim) == 0 || report.resimFrameAvgUs == 0 || (report.flags & Flag_HasFrames) == 0)
		return -1;
	const double spare = kBudgetUs - (double)report.gameDrawP90Us - (double)report.resimLoadUs;
	if (spare <= 0.0)
		return 0;
	const double fit = spare / (double)report.resimFrameAvgUs;
	return fit > 99.0 ? 99 : (int)fit;
}

int PerformanceMonitor::RealisticRollbackDepth(const Report& report)
{
	// What this connection actually produces, with a 2-frame margin for spikes; 3 when the
	// ping is unknown. Not a flat 8: a 30 ms connection never rolls back 8 frames.
	const int expected = ExpectedRollbackDepth(report);
	int depth = expected < 0 ? 3 : expected + 2;
	if (depth < 2) depth = 2;
	if (depth > 8) depth = 8;
	return depth;
}

int PerformanceMonitor::RollbackCapacityOk(const Report& report)
{
	const int headroom = RollbackHeadroomFrames(report);
	if (headroom < 0)
		return 0;
	if (headroom >= RealisticRollbackDepth(report))
		return 1;
	// Never warn from a handful of rollbacks: the fit (or the average) is not trustworthy yet.
	return report.rollbackSamples >= 8 ? -1 : 0;
}

bool PerformanceMonitor::IsStruggling(const Report& report)
{
	if ((report.flags & Flag_HasFrames) == 0 || report.frameSamples < 60)
		return false;
	if (report.gameDrawP90Us > (uint32_t)kBudgetUs)
		return true;
	return report.intervalAvgUs > 1000000u / 55u;
}

int PerformanceMonitor::ExpectedRollbackDepth(const Report& report)
{
	if ((report.flags & Flag_HasNet) == 0)
		return -1;
	// The opponent's inputs reach us ~ping/2 late; GGPO's 2-frame input delay hides 2 of
	// those frames (the game's own value - a raised OnlineInputDelay only hides more).
	const double oneWayFrames = (double)report.pingMs / 2.0 / (1000.0 / 60.0);
	const int depth = (int)(oneWayFrames + 0.999) - 2;
	return depth < 0 ? 0 : depth;
}

PerformanceMonitor::PcRating PerformanceMonitor::RatePc(const Report& r)
{
	if ((r.flags & Flag_HasFrames) == 0 || r.frameSamples < 60)
		return Pc_Unknown;
	return IsStruggling(r) ? Pc_Struggling : Pc_Good;
}

PerformanceMonitor::ConnRating PerformanceMonitor::RateConnection(const Report* own, const Report* measuredByOther)
{
	// Unstable: this player's packets reach the other end with long silences. GGPO sends every
	// frame, so a normal gap is one or two frames; 120 ms is seven frames of nothing.
	if (measuredByOther && (measuredByOther->flags & Flag_HasNet) != 0 && measuredByOther->maxRecvGapMs >= 120)
		return Conn_Unstable;
	const int ownRelay = own ? own->relayPingMs : 0;
	const int otherRelay = measuredByOther ? measuredByOther->relayPingMs : 0;
	if (ownRelay > 0)
	{
		if (ownRelay >= kSlowLineRelayMs)
			return Conn_SlowLine;
		if (otherRelay > 0 && ownRelay >= 40 && ownRelay >= 2 * otherRelay + 10)
			return Conn_SlowLine;
	}
	const bool anyEvidence = ownRelay > 0 || (measuredByOther && (measuredByOther->flags & Flag_HasNet) != 0);
	return anyEvidence ? Conn_Good : Conn_Unknown;
}

PerformanceMonitor::LagVerdict PerformanceMonitor::JudgeLag(const Report& a, const OpponentView& bView)
{
	LagVerdict v;
	// "Checking" only for the first ~2 s of frames; after that there is always an answer.
	if ((a.flags & Flag_HasFrames) == 0 || a.frameSamples < 120)
		return v;

	const bool bLive = (bView.state == Opp_Live || bView.state == Opp_Stale) && (bView.report.flags & Flag_HasFrames) != 0;
	const Report* b = bLive ? &bView.report : nullptr;

	if (IsStruggling(a)) { v.kind = Lag_Pc; v.side = 0; return v; }
	if (b && IsStruggling(*b)) { v.kind = Lag_Pc; v.side = 1; return v; }

	const bool aNet = (a.flags & Flag_HasNet) != 0;
	const bool bNet = b && (b->flags & Flag_HasNet) != 0;
	if (!aNet && !bNet) { v.kind = Lag_None; return v; } // PCs fine; no connection numbers

	// Unsteady connection, per direction: a's gap is b's packets arriving late, and vice versa.
	const uint32_t gapOfB = aNet ? a.maxRecvGapMs : 0;
	const uint32_t gapOfA = bNet ? b->maxRecvGapMs : 0;
	if (gapOfB >= 120 || gapOfA >= 120)
	{
		v.kind = Lag_Unstable;
		if (gapOfB >= 120 && bNet && gapOfA < 80) v.side = 1;
		else if (gapOfA >= 120 && gapOfB < 80) v.side = 0;
		else v.kind = Lag_UnstableUnknown;
		return v;
	}
	const int jitter = (std::max)(aNet ? (int)a.jitterMs : 0, bNet ? (int)b->jitterMs : 0);
	const Report& net = aNet ? a : *b;
	const int expected = ExpectedRollbackDepth(net);
	const bool lateInputs = net.rollbacksRecent >= 3 && expected >= 0 && net.maxDepthRecent > expected + 2;
	if (jitter >= 20 || lateInputs) { v.kind = Lag_UnstableUnknown; return v; }

	// The route check: a ping well above what the two locations allow is a route problem, and
	// worth saying even below 100 ms.
	const RouteCheck route = CheckRoute(a, b);
	if (route.state == Route_VpnSide || route.state == Route_VpnEither || route.state == Route_LineSide)
	{
		v.kind = Lag_LongRoute;
		v.side = route.side;
		return v;
	}

	if ((int)net.pingMs < kHighPingMs) { v.kind = Lag_None; return v; }

	// High ping: whose line adds it?
	if ((aNet && (a.flags & Flag_UsingRelay)) || (bNet && (b->flags & Flag_UsingRelay)))
	{
		v.kind = Lag_Relayed;
		return v;
	}
	const ConnRating ra = RateConnection(&a, b);
	const ConnRating rb = b ? RateConnection(b, &a) : Conn_Unknown;
	if (ra == Conn_SlowLine && rb != Conn_SlowLine) { v.kind = Lag_SlowLine; v.side = 0; return v; }
	if (rb == Conn_SlowLine && ra != Conn_SlowLine) { v.kind = Lag_SlowLine; v.side = 1; return v; }
	v.kind = Lag_Distance;
	return v;
}

PerformanceMonitor::RouteCheck PerformanceMonitor::CheckRoute(const Report& a, const Report* b)
{
	RouteCheck rc;
	if ((a.flags & Flag_HasNet) == 0)
		return rc;
	rc.actualMs = a.pingMs;
	if ((a.flags & Flag_UsingRelay) || (b && (b->flags & Flag_UsingRelay)))
	{
		rc.state = Route_CantCheckRelayed;
		return rc;
	}
	if (!b)
		return rc;

	rc.lastMile[0] = a.relayPingMs;
	rc.lastMile[1] = b->relayPingMs;
	rc.steamEstimateMs = a.expectedPingMs ? a.expectedPingMs : b->expectedPingMs;
	int model = 0;
	if (a.relayPingMs && b->relayPingMs && a.crossPingMs && b->crossPingMs)
	{
		// What each side's ISP route costs to reach the other side's region.
		rc.excess[0] = (std::max)(0, (int)a.crossPingMs - (int)a.relayPingMs);
		rc.excess[1] = (std::max)(0, (int)b->crossPingMs - (int)b->relayPingMs);
		model = rc.lastMile[0] + rc.lastMile[1] + (std::min)(rc.excess[0], rc.excess[1]);
	}
	// The larger of the two expectations, so a disagreement never invents a route problem.
	rc.expectedMs = (std::max)(model, rc.steamEstimateMs);
	if (rc.expectedMs <= 0 || a.frameSamples < 120)
		return rc;

	const bool longRoute = rc.actualMs > rc.expectedMs * 3 / 2 && rc.actualMs > rc.expectedMs + 30;
	if (!longRoute)
	{
		rc.state = Route_Fine;
		return rc;
	}

	if (rc.excess[0] >= 0 && rc.excess[1] >= 0)
	{
		const int e0 = rc.excess[0], e1 = rc.excess[1];
		if (e0 - e1 >= 25 && e0 * 10 >= e1 * 13) { rc.state = Route_VpnSide; rc.side = 0; return rc; }
		if (e1 - e0 >= 25 && e1 * 10 >= e0 * 13) { rc.state = Route_VpnSide; rc.side = 1; return rc; }
	}
	// No clear excess: a slow last mile on one side is that side's line (cable / ISP).
	const bool slow0 = rc.lastMile[0] >= kSlowLineRelayMs, slow1 = rc.lastMile[1] >= kSlowLineRelayMs;
	if (slow0 != slow1)
	{
		rc.state = Route_LineSide;
		rc.side = slow0 ? 0 : 1;
		return rc;
	}
	// Either side could try; start with the higher excess, then the higher last mile.
	rc.state = Route_VpnEither;
	if (rc.excess[0] >= 0 && rc.excess[1] >= 0 && rc.excess[0] != rc.excess[1])
		rc.side = rc.excess[0] > rc.excess[1] ? 0 : 1;
	else if (rc.lastMile[0] != rc.lastMile[1])
		rc.side = rc.lastMile[0] > rc.lastMile[1] ? 0 : 1;
	return rc;
}

namespace
{
	void RejectLocation(const char* why)
	{
		if (!g_loggedLocationRejectThisMatch)
		{
			g_loggedLocationRejectThisMatch = true;
			LOG(1, "[PerfMonitor] route check: opponent's location not used: %s\n", why);
		}
	}

	// The one place a received location becomes a parsed location; the network path (after the
	// room checks) and the dev-only offline test both come through here. text need not be
	// terminated; it is copied, validated, parsed, and zeroed.
	void IngestPingLocation(uint64_t steamId, const char* src, uint32_t length, uint32_t pop)
	{
		if (length == 0 || length > (uint32_t)kMaxLocationChars) { RejectLocation("bad length"); return; }
		if (!IsPlausiblePopId(pop)) { RejectLocation("bad relay code"); return; }
		// Parsing needs only the interface, not this side's ping data: start Steam's relay access
		// now if it has not been (this is what dropped the first location in the 10-24 set).
		if (!EnsureRelayAccess()) { RejectLocation("SteamNetworkingUtils unavailable on this PC"); return; }

		char text[kMaxLocationChars + 1];
		for (uint32_t i = 0; i < length; ++i)
		{
			const char c = src[i];
			if (c < 0x20 || c > 0x7E)
			{
				SecureZeroMemory(text, sizeof(text));
				RejectLocation("not printable text");
				return;
			}
			text[i] = c;
		}
		text[length] = '\0';

		PingLocationBlob parsed = {};
		const bool ok = ParseLocation(g_netUtils, text, &parsed);
		SecureZeroMemory(text, sizeof(text));
		if (!ok)
		{
			RejectLocation("Steam could not parse it");
			return;
		}

		g_peerLocation = parsed;
		SecureZeroMemory(&parsed, sizeof(parsed));
		g_peerLocationValid = true;
		g_peerLocationSteamId = steamId;
		g_peerPop = pop;
		// Kept even when this side's relay data is not ready: the numbers fill in on a later
		// poll (RecomputeRouteNumbers runs every 2 s in online mode).
		RecomputeRouteNumbers();
		LOG(1, "[PerfMonitor] route check: opponent's location received; Steam estimate %d ms, our ping to their region %d ms%s\n",
			g_expectedPingMs, g_crossPingMs, g_relayPingMs > 0 ? "" : " (our relay data not ready yet; will fill in)");
	}
}

void PerformanceMonitor::OnPingLocationPacket(Packet* packet)
{
	// Only from the opponent player of this match, to a player (spectators never get it).
	RoomManager* const room = g_interfaces.pRoomManager;
	if (!room || !room->IsRoomFunctional())
		return;
	const RoomMemberEntry* const self = room->GetThisPlayerRoomMemberEntry();
	if (!self || self->matchId == 0)
		return;
	const RoomMemberEntry* const sender = room->GetRoomMemberEntryByIndex(packet->roomMemberIndex);
	if (!sender || sender == self || sender->steamId != packet->steamID || sender->matchId != self->matchId)
		return;
	if (!RoomManager::IsSeatedEntry(self) || !RoomManager::IsSeatedEntry(sender))
		return;

	if (packet->dataSize < sizeof(PingLocationHeader) + 1 || packet->dataSize > MAX_DATA_SIZE)
	{
		RejectLocation("bad packet size");
		return;
	}
	PingLocationHeader header;
	memcpy(&header, packet->data, sizeof(header));
	if (header.version < 1 || sizeof(header) + header.length > packet->dataSize)
	{
		RejectLocation("bad header");
		return;
	}
	IngestPingLocation(packet->steamID, (const char*)(packet->data + sizeof(header)), header.length, header.nearestPop);
	SecureZeroMemory(packet->data, packet->dataSize);
}

void PerformanceMonitor::OnEndSceneBegin()
{
	if (g_measuring)
		g_endSceneBegin = Now();
}

void PerformanceMonitor::OnEndSceneDone()
{
	if (!g_measuring)
		return;
	g_lastEndScene = Now();
	g_resimTicksAtEndScene = g_resimTicksSincePresent;
	if (g_endSceneBegin != 0)
	{
		g_modTicksSincePresent += g_lastEndScene - g_endSceneBegin;
		g_endSceneBegin = 0;
	}
}

void PerformanceMonitor::OnPresentDone()
{
	if (!g_measuring)
	{
		g_lastPresent = 0;
		return;
	}

	const long long now = Now();
	if (g_lastPresent != 0 && g_lastEndScene > g_lastPresent)
	{
		const long long work = g_lastEndScene - g_lastPresent - g_resimTicksAtEndScene;
		const long long interval = now - g_lastPresent;
		// A frame longer than a second is a load or an alt-tab, not a performance sample.
		if (work > 0 && interval > 0 && interval < g_qpcFreq)
		{
			g_frameWorkUs[g_frameNext] = TicksToUs(work);
			g_frameIntervalUs[g_frameNext] = TicksToUs(interval);
			g_frameModUs[g_frameNext] = TicksToUs(g_modTicksSincePresent);
			g_frameNext = (g_frameNext + 1) % kFrameRing;
			if (g_frameCount < kFrameRing)
				++g_frameCount;
		}
	}
	g_lastPresent = now;
	g_resimTicksSincePresent = 0;
	g_resimTicksAtEndScene = 0;
	g_modTicksSincePresent = 0;
}

void PerformanceMonitor::Tick()
{
	if (g_qpcFreq == 0)
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		g_qpcFreq = f.QuadPart > 0 ? f.QuadPart : 1;
		g_base = GetBbcfBaseAdress();
	}

	const bool inMatch = g_gameVals.pGameState && *g_gameVals.pGameState == GameState_InMatch;
	if (inMatch && !g_inMatch)
		OnMatchStarted();
	g_inMatch = inMatch;
	g_measuring = inMatch;
	if (!inMatch)
	{
		g_onlinePlayer = false;
		g_spectating = false;
		g_spectatorTest = false;
		// The relay baseline takes a few seconds to measure; start it as soon as the window
		// is opened, in a lobby too, so the number is there when the match begins.
		const long long nowIdle = Now();
		const bool onlineMode = g_gameVals.pGameMode && *g_gameVals.pGameMode == GameMode_Online;
		if ((LocalWantsReports(nowIdle) || onlineMode) && nowIdle >= g_nextRefresh)
		{
			g_nextRefresh = nowIdle + 2 * g_qpcFreq;
			PollRelayBaseline();
		}
		return;
	}

	const bool online = g_gameVals.pGameMode && *g_gameVals.pGameMode == GameMode_Online;
	if (online)
	{
		// The call-site patch only goes in once an online match is actually running.
		EnsureRollbackTimingInstalled();
		SampleNetworkPerFrame();
	}

	const long long now = Now();
	if (now < g_nextRefresh)
		return;
	g_nextRefresh = now + g_qpcFreq / 2;
	++g_refreshCounter;

	const bool everySecond = (g_refreshCounter & 1) == 0;
	if (everySecond && g_netSeenThisMatch)
	{
		g_rtt[g_rttNext] = g_lastNet.rttMs;
		g_rttNext = (g_rttNext + 1) % kRttRing;
		if (g_rttCount < kRttRing)
			++g_rttCount;
	}

	RefreshOnlineContext();
	// Offline (or before the room context is known) the link type is still worth knowing for
	// the window's own column: same once-per-match check, default-route adapter.
	if (LocalWantsReports(now) && g_linkCheckedMatchStart != g_matchStartAt)
	{
		g_linkCheckedMatchStart = g_matchStartAt;
		StartLinkTypeCheck(0);
	}
	BuildLocalReport(now);
	if (g_spectatorTest)
		SpectatorTestInject();
	if (everySecond)
		TalkToPeers(now);
	if ((LocalWantsReports(now) || online) && (g_refreshCounter % 4) == 0)
		PollRelayBaseline(); // every 2 s while the window is open

	// Advance the 2 s window.
	g_bucket = (g_bucket + 1) % kBuckets;
	memset(&g_buckets[g_bucket], 0, sizeof(NetBucket));
}

void PerformanceMonitor::OnReportPacket(Packet* packet)
{
	// Room checks first: the sender must be a member of the match this client is in (playing
	// or watching), identified by its room slot AND Steam ID (RecvPacket has already checked
	// they agree), and the role pair must be one we talk to:
	//   we play,  they play      -> their full report (and maybe a request)
	//   we play,  they spectate  -> a request only; any numbers in it are ignored
	//   we watch, they play      -> their full report
	//   we watch, they spectate  -> dropped
	RoomManager* const room = g_interfaces.pRoomManager;
	if (!room || !room->IsRoomFunctional())
		return;
	const RoomMemberEntry* const self = room->GetThisPlayerRoomMemberEntry();
	if (!self || self->matchId == 0)
		return;
	const RoomMemberEntry* const sender = room->GetRoomMemberEntryByIndex(packet->roomMemberIndex);
	if (!sender || sender == self || sender->steamId != packet->steamID || sender->matchId != self->matchId)
		return;

	const bool selfPlays = RoomManager::IsSeatedEntry(self);
	const bool senderPlays = RoomManager::IsSeatedEntry(sender);
	if (!selfPlays && !senderPlays)
		return;

	IngestReport(packet->steamID, packet->data, packet->dataSize, senderPlays);
}

void PerformanceMonitor::NoteStatsWindowOpen()
{
	if (g_qpcFreq != 0)
		g_sectionVisibleAt = Now();
}

void PerformanceMonitor::NoteWidgetsVisible()
{
	if (g_qpcFreq != 0)
		g_widgetsVisibleAt = Now();
}

const PerformanceMonitor::Report& PerformanceMonitor::LocalReport()
{
	return g_local;
}

PerformanceMonitor::OpponentView PerformanceMonitor::GetOpponentView()
{
	if (!g_onlinePlayer)
		return OpponentView();
	return ViewForPeer(g_opponentPeer, Now());
}

PerformanceMonitor::OpponentView PerformanceMonitor::GetPlayerView(int side)
{
	if (!g_spectating || side < 0 || side > 1)
		return OpponentView();
	return ViewForPeer(g_playerPeer[side], Now());
}

void PerformanceMonitor::SetSpectatorTestMode(bool on)
{
	// Dev-only and offline only: never while an online match is running.
	const bool online = g_gameVals.pGameMode && *g_gameVals.pGameMode == GameMode_Online;
	g_spectatorTest = on && Settings::settingsIni.enableInDevelopmentFeatures && g_inMatch && !online;
	g_spectatorTestStart = 0;
	g_nextRefresh = 0;
}

bool PerformanceMonitor::IsSpectatorTestMode() { return g_spectatorTest; }

bool PerformanceMonitor::IsOnlineMatchAsPlayer() { return g_onlinePlayer; }
bool PerformanceMonitor::IsSpectating() { return g_spectating; }
bool PerformanceMonitor::IsInAnyMatch() { return g_inMatch; }
bool PerformanceMonitor::IsRollbackTimingAvailable() { return g_sitesPatched > 0; }
int PerformanceMonitor::RelayPingMs() { return g_relayPingMs; }
bool PerformanceMonitor::IsNetworkStatsAvailable() { return g_netSeenThisMatch; }
uint64_t PerformanceMonitor::OpponentSteamId() { return g_opponentPeer >= 0 ? g_peers[g_opponentPeer].steamId : 0; }
