#include "steamApiWrappers.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Hooks/hooks_bbcf.h"

#include <detours.h>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace
{
	constexpr SteamLeaderboard_t kRankAllLeaderboardHandle = static_cast<SteamLeaderboard_t>(1759932);

	std::mutex g_steamApiTraceMutex;
	std::unordered_map<uint64, std::string> g_apiCallLabels;
	std::unordered_map<uint64, std::string> g_leaderboardHandleNames;
	std::unordered_map<uint64, std::string> g_leaderboardEntriesNames;

	using RawStoreStatsFn = bool(__thiscall*)(ISteamUserStats*);
	using RawSteamClientGetISteamGenericInterfaceFn = void*(__thiscall*)(ISteamClient*, HSteamUser, HSteamPipe, const char*);
	using RawFindOrCreateLeaderboardFn = SteamAPICall_t(__thiscall*)(ISteamUserStats*, const char*, ELeaderboardSortMethod, ELeaderboardDisplayType);
	using RawFindLeaderboardFn = SteamAPICall_t(__thiscall*)(ISteamUserStats*, const char*);
	using RawUploadLeaderboardScoreFn = SteamAPICall_t(__thiscall*)(ISteamUserStats*, SteamLeaderboard_t, ELeaderboardUploadScoreMethod, int32, const int32*, int);
	using RawSteamClientGetISteamUserStatsFn = ISteamUserStats*(__thiscall*)(ISteamClient*, HSteamUser, HSteamPipe, const char*);

	RawStoreStatsFn g_rawStoreStatsOrig = nullptr;
	RawSteamClientGetISteamGenericInterfaceFn g_rawSteamClientGetISteamGenericInterfaceOrig = nullptr;
	RawFindOrCreateLeaderboardFn g_rawFindOrCreateLeaderboardOrig = nullptr;
	RawFindLeaderboardFn g_rawFindLeaderboardOrig = nullptr;
	RawUploadLeaderboardScoreFn g_rawUploadLeaderboardScoreOrig = nullptr;
	RawSteamClientGetISteamUserStatsFn g_rawSteamClientGetISteamUserStatsOrig = nullptr;

	std::string FormatDetails(const int32* details, int count)
	{
		std::ostringstream out;
		out << "[";
		for (int i = 0; i < 16; ++i)
		{
			if (i != 0)
			{
				out << ",";
			}

			if (details && i < count)
			{
				out << details[i];
			}
			else
			{
				out << "-";
			}
		}
		out << "]";
		return out.str();
	}

	std::string GetLeaderboardLabel(SteamLeaderboard_t handle)
	{
		const std::string knownName = GetLeaderboardHandleName(handle);
		std::ostringstream out;
		out << "handle=" << static_cast<unsigned long long>(handle);
		if (!knownName.empty())
		{
			out << " name='" << knownName << "'";
		}
		else if (handle == kRankAllLeaderboardHandle)
		{
			out << " name='RANK_ALL'";
		}
		return out.str();
	}

	bool IsRankLikeLeaderboardHandle(SteamLeaderboard_t handle)
	{
		if (handle == kRankAllLeaderboardHandle)
		{
			return true;
		}

		const std::string name = GetLeaderboardHandleName(handle);
		return !name.empty() && name.find("RANK") != std::string::npos;
	}

	bool __fastcall HookedRawStoreStats(ISteamUserStats* self)
	{
		LOG(2, "[STEAM][RawUserStats] StoreStats self=%p\n", self);
		return g_rawStoreStatsOrig ? g_rawStoreStatsOrig(self) : false;
	}

	SteamAPICall_t __fastcall HookedRawFindOrCreateLeaderboard(ISteamUserStats* self, void*, const char* name, ELeaderboardSortMethod sortMethod, ELeaderboardDisplayType displayType)
	{
		const SteamAPICall_t call = g_rawFindOrCreateLeaderboardOrig ? g_rawFindOrCreateLeaderboardOrig(self, name, sortMethod, displayType) : 0;
		RegisterSteamApiCallLabel(call, std::string("RawFindOrCreateLeaderboard:") + (name ? name : "<null>"));
		LOG(2, "[STEAM][RawUserStats] FindOrCreateLeaderboard self=%p name='%s' sort=%d display=%d call=%llu\n",
			self,
			name ? name : "<null>",
			static_cast<int>(sortMethod),
			static_cast<int>(displayType),
			static_cast<unsigned long long>(call));
		return call;
	}

	SteamAPICall_t __fastcall HookedRawFindLeaderboard(ISteamUserStats* self, void*, const char* name)
	{
		const SteamAPICall_t call = g_rawFindLeaderboardOrig ? g_rawFindLeaderboardOrig(self, name) : 0;
		RegisterSteamApiCallLabel(call, std::string("RawFindLeaderboard:") + (name ? name : "<null>"));
		LOG(2, "[STEAM][RawUserStats] FindLeaderboard self=%p name='%s' call=%llu\n",
			self,
			name ? name : "<null>",
			static_cast<unsigned long long>(call));
		return call;
	}

	SteamAPICall_t __fastcall HookedRawUploadLeaderboardScore(ISteamUserStats* self, void*, SteamLeaderboard_t handle, ELeaderboardUploadScoreMethod method, int32 score, const int32* details, int detailCount)
	{
		if (IsRankLikeLeaderboardHandle(handle))
		{
			LOG(2, "[RANK][UploadObserved] reason='RawUploadLeaderboardScore' self=%p %s method=%d score=%d detailsCount=%d details=%s\n",
				self,
				GetLeaderboardLabel(handle).c_str(),
				static_cast<int>(method),
				score,
				detailCount,
				FormatDetails(details, detailCount).c_str());
		}

		if (handle == kRankAllLeaderboardHandle)
		{
			RankedProbeNoteUpload();
			RankedProbeDumpSummary("RawUploadLeaderboardScore:RANK_ALL");
		}

		const SteamAPICall_t call = g_rawUploadLeaderboardScoreOrig ? g_rawUploadLeaderboardScoreOrig(self, handle, method, score, details, detailCount) : 0;
		RegisterSteamApiCallLabel(call, std::string("RawUploadLeaderboardScore:") + GetLeaderboardLabel(handle));
		LOG(2, "[STEAM][RawUserStats] UploadLeaderboardScore self=%p %s method=%d score=%d detailsCount=%d details=%s call=%llu\n",
			self,
			GetLeaderboardLabel(handle).c_str(),
			static_cast<int>(method),
			score,
			detailCount,
			FormatDetails(details, detailCount).c_str(),
			static_cast<unsigned long long>(call));
		return call;
	}

	void InstallSteamUserStatsRawHooks(ISteamUserStats* observed)
	{
		if (!observed || IsBadReadPtr(observed, sizeof(void*)))
		{
			return;
		}

		void** const vtable = *reinterpret_cast<void***>(observed);
		if (!vtable || IsBadReadPtr(vtable, 32 * sizeof(void*)))
		{
			LOG(2, "[STEAM][RawUserStats] vtable unreadable self=%p vtable=%p\n", observed, vtable);
			return;
		}

		if (!g_rawStoreStatsOrig && vtable[10])
		{
			g_rawStoreStatsOrig = reinterpret_cast<RawStoreStatsFn>(DetourFunction(reinterpret_cast<PBYTE>(vtable[10]), reinterpret_cast<PBYTE>(HookedRawStoreStats)));
			LOG(2, "[STEAM][RawUserStats] Hook StoreStats target=%p orig=%p\n", vtable[10], g_rawStoreStatsOrig);
		}

		if (!g_rawFindOrCreateLeaderboardOrig && vtable[22])
		{
			g_rawFindOrCreateLeaderboardOrig = reinterpret_cast<RawFindOrCreateLeaderboardFn>(DetourFunction(reinterpret_cast<PBYTE>(vtable[22]), reinterpret_cast<PBYTE>(HookedRawFindOrCreateLeaderboard)));
			LOG(2, "[STEAM][RawUserStats] Hook FindOrCreateLeaderboard target=%p orig=%p\n", vtable[22], g_rawFindOrCreateLeaderboardOrig);
		}

		if (!g_rawFindLeaderboardOrig && vtable[23])
		{
			g_rawFindLeaderboardOrig = reinterpret_cast<RawFindLeaderboardFn>(DetourFunction(reinterpret_cast<PBYTE>(vtable[23]), reinterpret_cast<PBYTE>(HookedRawFindLeaderboard)));
			LOG(2, "[STEAM][RawUserStats] Hook FindLeaderboard target=%p orig=%p\n", vtable[23], g_rawFindLeaderboardOrig);
		}

		if (!g_rawUploadLeaderboardScoreOrig && vtable[31])
		{
			g_rawUploadLeaderboardScoreOrig = reinterpret_cast<RawUploadLeaderboardScoreFn>(DetourFunction(reinterpret_cast<PBYTE>(vtable[31]), reinterpret_cast<PBYTE>(HookedRawUploadLeaderboardScore)));
			LOG(2, "[STEAM][RawUserStats] Hook UploadLeaderboardScore target=%p orig=%p\n", vtable[31], g_rawUploadLeaderboardScoreOrig);
		}
	}

	ISteamUserStats* __fastcall HookedRawSteamClientGetISteamUserStats(ISteamClient* self, void*, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion)
	{
		ISteamUserStats* const result = g_rawSteamClientGetISteamUserStatsOrig ?
			g_rawSteamClientGetISteamUserStatsOrig(self, hSteamUser, hSteamPipe, pchVersion) : nullptr;
		LOG(2, "[STEAM][RawSteamClient] GetISteamUserStats self=%p user=%d pipe=%d version='%s' result=%p\n",
			self,
			static_cast<int>(hSteamUser),
			static_cast<int>(hSteamPipe),
			pchVersion ? pchVersion : "<null>",
			result);
		ObserveSteamUserStatsInterface(result, "RawSteamClient::GetISteamUserStats");
		return result;
	}

	void* __fastcall HookedRawSteamClientGetISteamGenericInterface(ISteamClient* self, void*, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion)
	{
		void* const result = g_rawSteamClientGetISteamGenericInterfaceOrig ?
			g_rawSteamClientGetISteamGenericInterfaceOrig(self, hSteamUser, hSteamPipe, pchVersion) : nullptr;
		LOG(2, "[STEAM][RawSteamClient] GetISteamGenericInterface self=%p user=%d pipe=%d version='%s' result=%p\n",
			self,
			static_cast<int>(hSteamUser),
			static_cast<int>(hSteamPipe),
			pchVersion ? pchVersion : "<null>",
			result);
		if (pchVersion && strstr(pchVersion, "USERSTATS"))
		{
			ObserveSteamUserStatsInterface(reinterpret_cast<ISteamUserStats*>(result), "RawSteamClient::GetISteamGenericInterface");
		}
		return result;
	}

	void InstallSteamClientRawHooks(ISteamClient* steamClient)
	{
		if (!steamClient || IsBadReadPtr(steamClient, sizeof(void*)))
		{
			return;
		}

		void** const vtable = *reinterpret_cast<void***>(steamClient);
		if (!vtable || IsBadReadPtr(vtable, 16 * sizeof(void*)))
		{
			LOG(2, "[STEAM][RawSteamClient] vtable unreadable self=%p vtable=%p\n", steamClient, vtable);
			return;
		}

		if (!g_rawSteamClientGetISteamUserStatsOrig && vtable[13])
		{
			g_rawSteamClientGetISteamUserStatsOrig =
				reinterpret_cast<RawSteamClientGetISteamUserStatsFn>(
					DetourFunction(reinterpret_cast<PBYTE>(vtable[13]), reinterpret_cast<PBYTE>(HookedRawSteamClientGetISteamUserStats)));
			LOG(2, "[STEAM][RawSteamClient] Hook GetISteamUserStats target=%p orig=%p\n",
				vtable[13],
				g_rawSteamClientGetISteamUserStatsOrig);
		}

		if (!g_rawSteamClientGetISteamGenericInterfaceOrig && vtable[12])
		{
			g_rawSteamClientGetISteamGenericInterfaceOrig =
				reinterpret_cast<RawSteamClientGetISteamGenericInterfaceFn>(
					DetourFunction(reinterpret_cast<PBYTE>(vtable[12]), reinterpret_cast<PBYTE>(HookedRawSteamClientGetISteamGenericInterface)));
			LOG(2, "[STEAM][RawSteamClient] Hook GetISteamGenericInterface target=%p orig=%p\n",
				vtable[12],
				g_rawSteamClientGetISteamGenericInterfaceOrig);
		}
	}
}

void ObserveSteamUserStatsInterface(ISteamUserStats* pSteamUserStats, const char* sourceTag)
{
	if (!pSteamUserStats)
	{
		LOG(2, "[STEAM][ObserveUserStats] source='%s' ptr=<null>\n", sourceTag ? sourceTag : "<null>");
		return;
	}

	LOG(2, "[STEAM][ObserveUserStats] source='%s' ptr=%p\n",
		sourceTag ? sourceTag : "<null>",
		pSteamUserStats);
	InstallSteamUserStatsRawHooks(pSteamUserStats);
}

void ObserveSteamClientInterface(ISteamClient* pSteamClient, const char* sourceTag)
{
	if (!pSteamClient)
	{
		LOG(2, "[STEAM][ObserveSteamClient] source='%s' ptr=<null>\n", sourceTag ? sourceTag : "<null>");
		return;
	}

	LOG(2, "[STEAM][ObserveSteamClient] source='%s' ptr=%p\n",
		sourceTag ? sourceTag : "<null>",
		pSteamClient);
	InstallSteamClientRawHooks(pSteamClient);
}

void RegisterSteamApiCallLabel(SteamAPICall_t call, const std::string& label)
{
	if (!call || label.empty())
	{
		return;
	}

	std::lock_guard<std::mutex> lock(g_steamApiTraceMutex);
	g_apiCallLabels[static_cast<uint64>(call)] = label;
}

std::string GetSteamApiCallLabel(SteamAPICall_t call)
{
	std::lock_guard<std::mutex> lock(g_steamApiTraceMutex);
	const auto it = g_apiCallLabels.find(static_cast<uint64>(call));
	return it != g_apiCallLabels.end() ? it->second : std::string();
}

void RegisterLeaderboardHandleName(SteamLeaderboard_t handle, const std::string& name)
{
	if (!handle || name.empty())
	{
		return;
	}

	std::lock_guard<std::mutex> lock(g_steamApiTraceMutex);
	g_leaderboardHandleNames[static_cast<uint64>(handle)] = name;
}

std::string GetLeaderboardHandleName(SteamLeaderboard_t handle)
{
	std::lock_guard<std::mutex> lock(g_steamApiTraceMutex);
	const auto it = g_leaderboardHandleNames.find(static_cast<uint64>(handle));
	return it != g_leaderboardHandleNames.end() ? it->second : std::string();
}

void RegisterLeaderboardEntriesName(SteamLeaderboardEntries_t entries, const std::string& name)
{
	if (!entries || name.empty())
	{
		return;
	}

	std::lock_guard<std::mutex> lock(g_steamApiTraceMutex);
	g_leaderboardEntriesNames[static_cast<uint64>(entries)] = name;
}

std::string GetLeaderboardEntriesName(SteamLeaderboardEntries_t entries)
{
	std::lock_guard<std::mutex> lock(g_steamApiTraceMutex);
	const auto it = g_leaderboardEntriesNames.find(static_cast<uint64>(entries));
	return it != g_leaderboardEntriesNames.end() ? it->second : std::string();
}

void RefreshSteamUserStatsWrapperSlot(ISteamUserStats** ppSteamUserStats)
{
	if (!ppSteamUserStats)
	{
		return;
	}

	ISteamUserStats* const observed = *ppSteamUserStats;
	if (!observed)
	{
		return;
	}

	if (!g_interfaces.pSteamUserStatsWrapper)
	{
		LOG(2, "[STEAM][UserStats] Create wrapper slot=%p raw=%p\n", ppSteamUserStats, observed);
		InstallSteamUserStatsRawHooks(observed);
		g_interfaces.pSteamUserStatsWrapper = new SteamUserStatsWrapper(ppSteamUserStats);
		return;
	}

	SteamUserStatsWrapper* const wrapper = g_interfaces.pSteamUserStatsWrapper;
	if (observed == reinterpret_cast<ISteamUserStats*>(wrapper))
	{
		return;
	}

	ISteamUserStats* const previousRaw = wrapper->m_SteamUserStats;
	wrapper->m_SteamUserStats = observed;
	InstallSteamUserStatsRawHooks(observed);

	void* wrapperAddress = wrapper;
	WriteToProtectedMemory(reinterpret_cast<uintptr_t>(ppSteamUserStats), reinterpret_cast<char*>(&wrapperAddress), 4);

	LOG(2, "[STEAM][UserStats] Refresh wrapper slot=%p raw_old=%p raw_new=%p wrapper=%p\n",
		ppSteamUserStats,
		previousRaw,
		observed,
		wrapper);
}

namespace
{
	// BBCF's own copy of steam_api.h's s_CallbackCounterAndContext: { init callback, counter,
	// CSteamAPIContext }. The accessors the game calls (SteamFriends() and the rest, 116 of
	// them) all push its address to SteamInternal_ContextInit, and CSteamAPIContext::Init
	// (0x407A70) fills the context - those are the stores hook_SteamAPI_Init patches to learn
	// where each interface pointer lives. RVAs, since the image is relocatable.
	const uintptr_t kSteamContextBlockRva = 0x5D3210;
	const uintptr_t kSteamContextInitCallbackRva = 0x7E60; // what the block's first slot holds

	// The hook_SteamAPI_Init path lost the startup race: the game called SteamAPI_Init before
	// our detour was in, so the store-site patches were never placed and no interface slot is
	// known. The slots are not a secret, though - they are fixed offsets into the block above,
	// and by the title screen the game has long since filled them. Recover them from there.
	void RecoverMissedSteamContextSlots()
	{
		const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
		void** const block = reinterpret_cast<void**>(base + kSteamContextBlockRva);

		// Only trust the address if it still holds what BBCF puts there: the build it was
		// read from is the only one there is, but a mismatch must mean "leave it alone".
		if (block[0] != reinterpret_cast<void*>(base + kSteamContextInitCallbackRva))
		{
			LOG(0, "[STEAM] Missed SteamAPI_Init and the Steam context is not where it should be "
				"(slot0=%p, expected %p); the overlay cannot start this session.\n",
				block[0], reinterpret_cast<void*>(base + kSteamContextInitCallbackRva));
			return;
		}

		// block[1] is the callback counter; the context itself starts at block[2].
		void** const context = block + 2;
		if (reinterpret_cast<uintptr_t>(block[1]) == 0 || !context[0])
		{
			LOG(0, "[STEAM] Missed SteamAPI_Init and the game has not filled its Steam context yet; "
				"the overlay cannot start this session.\n");
			return;
		}

		// Same slots the store-site patches record: CSteamAPIContext's member offsets.
		auto slot = [&](int byteOffset) { return reinterpret_cast<void*>(reinterpret_cast<char*>(context) + byteOffset); };
		if (!g_tempVals.ppSteamUser)        g_tempVals.ppSteamUser = static_cast<ISteamUser**>(slot(0x04));
		if (!g_tempVals.ppSteamFriends)     g_tempVals.ppSteamFriends = static_cast<ISteamFriends**>(slot(0x08));
		if (!g_tempVals.ppSteamUtils)       g_tempVals.ppSteamUtils = static_cast<ISteamUtils**>(slot(0x0C));
		if (!g_tempVals.ppSteamMatchmaking) g_tempVals.ppSteamMatchmaking = static_cast<ISteamMatchmaking**>(slot(0x10));
		if (!g_tempVals.ppSteamUserStats)   g_tempVals.ppSteamUserStats = static_cast<ISteamUserStats**>(slot(0x14));
		if (!g_tempVals.ppSteamNetworking)  g_tempVals.ppSteamNetworking = static_cast<ISteamNetworking**>(slot(0x20));

		LOG(0, "[STEAM] Missed SteamAPI_Init; recovered the interface slots from the game's Steam "
			"context at %p (user=%p friends=%p utils=%p matchmaking=%p userstats=%p networking=%p)\n",
			context, *g_tempVals.ppSteamUser, *g_tempVals.ppSteamFriends, *g_tempVals.ppSteamUtils,
			*g_tempVals.ppSteamMatchmaking, *g_tempVals.ppSteamUserStats, *g_tempVals.ppSteamNetworking);
	}
}

bool InitSteamApiWrappers()
{
	LOG(1, "InitSteamApiWrappers\n");

	if (!g_tempVals.ppSteamUser || !g_tempVals.ppSteamFriends || !g_tempVals.ppSteamUtils ||
		!g_tempVals.ppSteamMatchmaking || !g_tempVals.ppSteamUserStats || !g_tempVals.ppSteamNetworking)
	{
		RecoverMissedSteamContextSlots();
	}
	else
	{
		// Known already - from the SteamAPI_Init hook, or an earlier call's recovery. Say
		// whether that agrees with the recovery path's address, so a normal boot keeps
		// proving the fallback is aimed at the right place.
		const uintptr_t context = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) + kSteamContextBlockRva + 8;
		LOG(1, "[STEAM] Interface slots already known; context fallback address %s (%p vs %p)\n",
			reinterpret_cast<uintptr_t>(g_tempVals.ppSteamUser) == context + 4 ? "matches" : "DOES NOT MATCH",
			g_tempVals.ppSteamUser, reinterpret_cast<void*>(context + 4));
	}

	if (g_tempVals.ppSteamMatchmaking &&
		!g_interfaces.pSteamMatchmakingWrapper)
	{
		g_interfaces.pSteamMatchmakingWrapper = new SteamMatchmakingWrapper(g_tempVals.ppSteamMatchmaking);
	}

	if (g_tempVals.ppSteamFriends &&
		!g_interfaces.pSteamFriendsWrapper)
	{
		g_interfaces.pSteamFriendsWrapper = new SteamFriendsWrapper(g_tempVals.ppSteamFriends);
	}

	if (g_tempVals.ppSteamUser &&
		!g_interfaces.pSteamUserWrapper)
	{
		g_interfaces.pSteamUserWrapper = new SteamUserWrapper(g_tempVals.ppSteamUser);
	}

	if (g_tempVals.ppSteamNetworking &&
		!g_interfaces.pSteamNetworkingWrapper)
	{
		g_interfaces.pSteamNetworkingWrapper = new SteamNetworkingWrapper(g_tempVals.ppSteamNetworking);
	}

	RefreshSteamUserStatsWrapperSlot(g_tempVals.ppSteamUserStats);

	if (g_tempVals.ppSteamUtils &&
		!g_interfaces.pSteamUtilsWrapper)
	{
		g_interfaces.pSteamUtilsWrapper = new SteamUtilsWrapper(g_tempVals.ppSteamUtils);
	}

	if (g_interfaces.pSteamUserStatsWrapper &&
		g_interfaces.pSteamFriendsWrapper &&
		!g_interfaces.pSteamApiHelper)
	{
		g_interfaces.pSteamApiHelper = new SteamApiHelper(g_interfaces.pSteamUserStatsWrapper, g_interfaces.pSteamFriendsWrapper);
	}

	return true;
}
