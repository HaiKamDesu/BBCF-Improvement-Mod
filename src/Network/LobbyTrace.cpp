#include "LobbyTrace.h"

#include "LobbyFixes.h"

#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Game/Room/Room.h"
#include "Hooks/HookManager.h"
#include "SteamApiWrapper/SteamMatchmakingWrapper.h"
#include "SteamApiWrapper/SteamNetworkingWrapper.h"
#include "SteamApiWrapper/SteamUserWrapper.h"

#include <isteammatchmaking.h>
#include <isteamnetworking.h>

#include <Windows.h>
#include <detours.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace LobbyTrace
{
	namespace
	{
		// ------------------------------------------------------------------ game addresses
		// RVAs (VA - 0x400000). See docs/Decompilation/Lobby_B_NetworkTask.md and _C_.
		constexpr uintptr_t kNetTaskRva = 0x008F7958;       // DAT_00CF7958: +0 task, +4 state, +0xAC dialog, +0xB4 toast
		constexpr uintptr_t kAutomatchRva = 0x008F7758;     // DAT_00CF7758: +0 entry, +8 step
		constexpr uintptr_t kNetServerRva = 0x00662730;     // GAMESTEAM_CNetworkServer static
		constexpr uintptr_t kGgpoAssertRva = 0x00381890;    // cdecl(const char* message): MessageBox, caller then _exit(0)

		const char* const kTaskNames[] = {
			"idle", "network top menu", "network init + login", "profile sync", "ranked menu",
			"ranked match", "player match menu", "in player room", "enter 2D lobby", "in 2D lobby",
			"?0xA", "?0xB", "?0xC", "forced exit", "exit",
		};

		const char* DialogName(uint32_t id)
		{
			static const char* const kNames[] = {
				nullptr, "close", nullptr, "NTER_Unconnected", "NTER_CommonError", "NTER_InitError",
				"NTER_RatingAge", "NTER_VerifyError", "RankMatch_NotMatching", "RankMatch_CreateError",
				"RankMatch_SearchError", "RankMatch_SearchSelectError", "RankMatch_ConnectError",
				"RankMatch_DeleteError", "RankMatch_MemberFull", "RankMatch_Started", "RankMatch_LostBattler",
				"RankMatch_LeaveBattler", "NEER_RankMatch_SettingDesable", "PlayerMatch_SessionNameError",
				"PlayerMatch_CreateError", "PlayerMatch_SearchError", "PlayerMatch_SearchSelectError",
				"PlayerMatch_ConnectError", "PlayerMatch_MemberFull", "PlayerMatch_Started",
				"PlayerMatch_BreakUp", "PlayerMatch_HostLost", "PlayerMatch_Kicked",
				"PlayerMatch_LeaveBattler", "PlayerMatch_SpectateOver", "PlayerMatch_FinishedBattle",
				"PlayerMatch_AppVerDisagrError", "PlayerMatch_CatalogVerUnError", "TeamBtlContinueError",
				"ReplayLoadError", "RPT2_ReplayVersionError", "LeaveReplayOwner", "ReturnBattler",
				"ReturnReplayOwner", "ReturnHost", "LobbyMatch_SlotJoinError", "ForceSlotOut",
				"LobbyMatch_LostBattler", "LobbyMatch_LobbyDestroy", "LobbyMatch_ForceReturn",
				"NEER_P2PBattle_Desable", "NTER_Signout", "UpdateDCodeAsk", "UpdateDCodeAsk",
				"UpdateDCodeAsk", "UpdateDCodeAsk", "UpdateDCodeAsk", "PMRI_RoomExitYesNo",
				"PMRI_RoomReleaseYesNo", "PMRI_AskKick", "RankAskEntry", "RankAskEntryCancel",
				"RankAskMatchingCancel", "RankAskReturnMode", "RankAskReturnWarning", "LobbyAsk",
				"LobbyAsk", "LobbyAsk", "MainAsk_Entry", "MainAsk_Entry", "MainAsk_Entry", "MainAsk_Entry",
				"(toast) Initializing", "(toast) UpdateDCodeDown", "(toast) UpdateDCodeUp",
				"(toast) Synchronizing", "(toast) Creating", "(toast) Searching", "(toast) Connecting",
				"(toast) Disconnecting", "(toast) Checking", "(toast) ReplayLoading", "(toast) LobbySlotJoin",
				"(toast) NETWORK_APPEND", "(toast) NETWORK_APPEND", "(toast) NETWORK_APPEND",
				"(toast) LOBBY_LobbyLogin", "(toast) LOBBY_Register", "(toast) LOBBY_LobbyGet",
				"(toast) LOBBY_CannotGetLobby",
			};
			if (id < sizeof(kNames) / sizeof(kNames[0]) && kNames[id])
				return kNames[id];
			return "?";
		}

		const char* GameMessageName(uint16_t type)
		{
			switch (type)
			{
			case 0x08: return "Ready";
			case 0x2F: return "RematchCursor";
			case 0x30: return "RematchDecision";
			case 0x31: return "RematchSync";
			case 0x33: return "JoinRequest";
			case 0x34: return "JoinReject";
			case 0x35: return "RoomInfo";
			case 0x36: return "MemberLeave";
			case 0x37: return "RoomClosed";
			case 0x38: return "Ping";
			case 0x39: return "Pong";
			case 0x3A: return "Chat";
			default: return nullptr;
			}
		}

		const char* EnterResponseName(uint32_t r)
		{
			switch (r)
			{
			case 1: return "success";
			case 2: return "doesn't exist";
			case 3: return "not allowed";
			case 4: return "full";
			case 5: return "error";
			case 6: return "banned";
			case 7: return "limited account";
			case 8: return "clan disabled";
			case 9: return "community ban";
			case 10: return "a member blocked you";
			case 11: return "you blocked a member";
			case 12: return "rate limited";
			default: return "?";
			}
		}

		const char* P2PErrorName(int e)
		{
			switch (e)
			{
			case 0: return "none";
			case 1: return "not running app";
			case 2: return "no rights to app";
			case 3: return "destination not logged in";
			case 4: return "timeout";
			default: return "?";
			}
		}

		const char* LobbyTypeName(int t)
		{
			switch (t)
			{
			case 0: return "private";
			case 1: return "friends only";
			case 2: return "public";
			case 3: return "invisible";
			default: return "?";
			}
		}

		// Lobby membership is tracked whenever the trace or the fixes are on (LobbyFixes needs
		// it); everything that only logs runs with the trace alone.
		bool Tracking()
		{
			return Settings::settingsIni.lobbyTrace || Settings::settingsIni.lobbyFixes;
		}

		void Trace(const char* fmt, ...)
		{
			if (!Settings::settingsIni.lobbyTrace)
				return;
			char text[1024];
			va_list args;
			va_start(args, fmt);
			vsnprintf(text, sizeof(text), fmt, args);
			va_end(args);
			LOG(1, "[LobbyTrace] %s\n", text);
		}

		uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetBbcfBaseAdress()); }

		template <typename T>
		bool ReadGame(uintptr_t address, T& out)
		{
			if (IsBadReadPtr(reinterpret_cast<const void*>(address), sizeof(T)))
				return false;
			out = *reinterpret_cast<const T*>(address);
			return true;
		}

		ISteamMatchmaking* RealMatchmaking()
		{
			return g_interfaces.pSteamMatchmakingWrapper ? g_interfaces.pSteamMatchmakingWrapper->m_SteamMatchmaking : nullptr;
		}

		ISteamNetworking* RealNetworking()
		{
			return g_interfaces.pSteamNetworkingWrapper ? g_interfaces.pSteamNetworkingWrapper->m_SteamNetworking : nullptr;
		}

		uint64_t SelfId()
		{
			return g_interfaces.pSteamUserWrapper ? g_interfaces.pSteamUserWrapper->GetSteamID().ConvertToUint64() : 0;
		}

		std::string CallerText(void* caller)
		{
			char text[32];
			const uintptr_t address = reinterpret_cast<uintptr_t>(caller);
			const uintptr_t base = Base();
			if (base && address >= base && address < base + 0x2000000)
				snprintf(text, sizeof(text), "game 0x%08X", static_cast<unsigned>(address - base + 0x400000));
			else
				snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(address));
			return text;
		}

		// ------------------------------------------------------------------ who is who

		std::set<uint64_t> g_myLobbies;         // Steam lobbies we are in
		std::map<uint64_t, uint64_t> g_owners;  // their owner as of the last frame
		std::set<uint64_t> g_lastRoomMembers;   // SteamIDs in the game's room table

		bool IsKnownPeer(uint64_t id)
		{
			if (g_lastRoomMembers.count(id))
				return true;
			ISteamMatchmaking* mm = RealMatchmaking();
			if (!mm)
				return false;
			for (uint64_t lobby : g_myLobbies)
			{
				const int n = mm->GetNumLobbyMembers(CSteamID(lobby));
				for (int i = 0; i < n; ++i)
				{
					if (mm->GetLobbyMemberByIndex(CSteamID(lobby), i).ConvertToUint64() == id)
						return true;
				}
			}
			return false;
		}

		std::string PeerText(uint64_t id)
		{
			char text[64];
			snprintf(text, sizeof(text), "%llu%s", static_cast<unsigned long long>(id),
				IsKnownPeer(id) ? "" : " (NOT in our lobby/room)");
			return text;
		}

		// ------------------------------------------------------------------ pending requests

		struct Pending
		{
			std::string what;
			ULONGLONG startMs = 0;
			int warned = 0;
		};
		std::map<uint64_t, Pending> g_pending;   // call handle -> request
		std::string g_listFilters;

		void StartPending(uint64_t call, const std::string& what)
		{
			if (call == 0)
			{
				Trace("%s: Steam returned an INVALID call handle - no result will ever come", what.c_str());
				return;
			}
			Pending p;
			p.what = what;
			p.startMs = GetTickCount64();
			g_pending[call] = p;
		}

		// ------------------------------------------------------------------ call result proxies

		struct CallbackPeek : CCallbackBase
		{
			static int Id(CCallbackBase* b) { return static_cast<CallbackPeek*>(b)->m_iCallback; }
		};

		void OnResult(int id, void* param, bool ioFailure, uint64_t call);

		class TraceProxy : public CCallbackBase
		{
		public:
			TraceProxy(CCallbackBase* inner, uint64_t call) : m_inner(inner), m_call(call)
			{
				m_iCallback = CallbackPeek::Id(inner);
			}
			void Run(void* param) override
			{
				OnResult(m_iCallback, param, false, m_call);
				m_inner->Run(param);
			}
			void Run(void* param, bool ioFailure, SteamAPICall_t call) override
			{
				OnResult(m_iCallback, param, ioFailure, call);
				m_done = GetTickCount64();
				m_inner->Run(param, ioFailure, call);
			}
			int GetCallbackSizeBytes() override { return m_inner->GetCallbackSizeBytes(); }

			CCallbackBase* m_inner;
			uint64_t m_call;
			ULONGLONG m_done = 0;
		};

		std::vector<TraceProxy*> g_proxies;

		void ListLobbies(int count)
		{
			ISteamMatchmaking* mm = RealMatchmaking();
			if (!mm)
				return;
			int full = 0;
			const int shown = count < 25 ? count : 25;
			for (int i = 0; i < count; ++i)
			{
				const CSteamID lobby = mm->GetLobbyByIndex(i);
				const int members = mm->GetNumLobbyMembers(lobby);
				const int limit = mm->GetLobbyMemberLimit(lobby);
				if (limit > 0 && members >= limit)
					++full;
				if (i >= shown)
					continue;
				std::string keys;
				const int keyCount = mm->GetLobbyDataCount(lobby);
				for (int k = 0; k < keyCount && k < 12; ++k)
				{
					char key[64] = {};
					char value[128] = {};
					if (mm->GetLobbyDataByIndex(lobby, k, key, sizeof(key), value, sizeof(value)))
						keys += std::string(keys.empty() ? "" : " ") + key + "=" + value;
				}
				Trace("    #%d lobby=%llu members=%d/%d%s %s", i, static_cast<unsigned long long>(lobby.ConvertToUint64()),
					members, limit, (limit > 0 && members >= limit) ? " FULL" : "", keys.c_str());
			}
			if (count > shown)
				Trace("    ... %d more", count - shown);
			if (full > 0)
				Trace("    %d of %d listed lobbies are FULL (no slots-available filter)", full, count);
		}

		void OnResult(int id, void* param, bool ioFailure, uint64_t call)
		{
			std::string what = "call";
			ULONGLONG waited = 0;
			auto it = g_pending.find(call);
			if (it != g_pending.end())
			{
				what = it->second.what;
				waited = GetTickCount64() - it->second.startMs;
				g_pending.erase(it);
			}
			if (!param)
			{
				Trace("%s: result with no data (io failure %d)", what.c_str(), ioFailure ? 1 : 0);
				return;
			}
			switch (id)
			{
			case LobbyCreated_t::k_iCallback:
			{
				const LobbyCreated_t* r = static_cast<const LobbyCreated_t*>(param);
				Trace("%s -> %s (EResult %d) lobby=%llu after %llu ms%s", what.c_str(), r->m_eResult == k_EResultOK ? "created" : "FAILED",
					static_cast<int>(r->m_eResult), static_cast<unsigned long long>(r->m_ulSteamIDLobby),
					static_cast<unsigned long long>(waited), ioFailure ? " [IO FAILURE]" : "");
				if (r->m_eResult == k_EResultOK && r->m_ulSteamIDLobby)
				{
					g_myLobbies.insert(r->m_ulSteamIDLobby);
					g_owners[r->m_ulSteamIDLobby] = SelfId();
				}
				break;
			}
			case LobbyEnter_t::k_iCallback:
			{
				const LobbyEnter_t* r = static_cast<const LobbyEnter_t*>(param);
				Trace("%s -> %s (response %u) lobby=%llu locked=%d after %llu ms%s", what.c_str(),
					EnterResponseName(r->m_EChatRoomEnterResponse), r->m_EChatRoomEnterResponse,
					static_cast<unsigned long long>(r->m_ulSteamIDLobby), r->m_bLocked ? 1 : 0,
					static_cast<unsigned long long>(waited), ioFailure ? " [IO FAILURE]" : "");
				if (r->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess)
					LobbyFixes::OnJoinFailed(r->m_EChatRoomEnterResponse);
				if (r->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess && r->m_ulSteamIDLobby)
				{
					g_myLobbies.insert(r->m_ulSteamIDLobby);
					ISteamMatchmaking* mm = RealMatchmaking();
					if (mm)
						g_owners[r->m_ulSteamIDLobby] = mm->GetLobbyOwner(CSteamID(r->m_ulSteamIDLobby)).ConvertToUint64();
					if (mm && Enabled())
					{
						const CSteamID lobby(r->m_ulSteamIDLobby);
						Trace("    owner=%llu members=%d/%d", static_cast<unsigned long long>(mm->GetLobbyOwner(lobby).ConvertToUint64()),
							mm->GetNumLobbyMembers(lobby), mm->GetLobbyMemberLimit(lobby));
					}
				}
				break;
			}
			case LobbyMatchList_t::k_iCallback:
			{
				const LobbyMatchList_t* r = static_cast<const LobbyMatchList_t*>(param);
				Trace("%s -> %u lobbies after %llu ms%s", what.c_str(), r->m_nLobbiesMatching,
					static_cast<unsigned long long>(waited), ioFailure ? " [IO FAILURE]" : "");
				if (Enabled())
					ListLobbies(static_cast<int>(r->m_nLobbiesMatching));
				break;
			}
			default:
				Trace("%s -> callback %d after %llu ms", what.c_str(), id, static_cast<unsigned long long>(waited));
				break;
			}
		}

		// ------------------------------------------------------------------ Steam broadcasts

		class Listener
		{
		public:
			STEAM_CALLBACK(Listener, OnLobbyChatUpdate, LobbyChatUpdate_t);
			STEAM_CALLBACK(Listener, OnLobbyKicked, LobbyKicked_t);
			STEAM_CALLBACK(Listener, OnJoinRequested, GameLobbyJoinRequested_t);
			STEAM_CALLBACK(Listener, OnP2PRequest, P2PSessionRequest_t);
			STEAM_CALLBACK(Listener, OnP2PConnectFail, P2PSessionConnectFail_t);
			STEAM_CALLBACK(Listener, OnLobbyDataUpdate, LobbyDataUpdate_t);
			int m_dataUpdates = 0;
		};

		void Listener::OnLobbyChatUpdate(LobbyChatUpdate_t* p)
		{
			const uint32 left = k_EChatMemberStateChangeLeft | k_EChatMemberStateChangeDisconnected |
				k_EChatMemberStateChangeKicked | k_EChatMemberStateChangeBanned;
			if ((p->m_rgfChatMemberStateChange & left) && g_myLobbies.count(p->m_ulSteamIDLobby))
			{
				auto owner = g_owners.find(p->m_ulSteamIDLobby);
				LobbyFixes::OnSteamMemberLeft(p->m_ulSteamIDLobby, p->m_ulSteamIDUserChanged, owner != g_owners.end() ? owner->second : 0,
					(p->m_rgfChatMemberStateChange & k_EChatMemberStateChangeLeft) ? "left" :
					(p->m_rgfChatMemberStateChange & k_EChatMemberStateChangeDisconnected) ? "disconnected" :
					(p->m_rgfChatMemberStateChange & k_EChatMemberStateChangeKicked) ? "kicked" : "banned");
			}
			if (!Enabled())
				return;
			const char* change = "changed";
			if (p->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered) change = "ENTERED";
			else if (p->m_rgfChatMemberStateChange & k_EChatMemberStateChangeLeft) change = "LEFT";
			else if (p->m_rgfChatMemberStateChange & k_EChatMemberStateChangeDisconnected) change = "DISCONNECTED";
			else if (p->m_rgfChatMemberStateChange & k_EChatMemberStateChangeKicked) change = "KICKED";
			else if (p->m_rgfChatMemberStateChange & k_EChatMemberStateChangeBanned) change = "BANNED";
			const bool inRoom = g_lastRoomMembers.count(p->m_ulSteamIDUserChanged) != 0;
			Trace("Steam: lobby %llu member %llu %s (by %llu)%s", static_cast<unsigned long long>(p->m_ulSteamIDLobby),
				static_cast<unsigned long long>(p->m_ulSteamIDUserChanged), change,
				static_cast<unsigned long long>(p->m_ulSteamIDMakingChange),
				(change[0] != 'E' && inRoom) ? " - still in the game's room table" : "");
		}

		void Listener::OnLobbyKicked(LobbyKicked_t* p)
		{
			Trace("Steam: kicked from lobby %llu by %llu (disconnect %d)", static_cast<unsigned long long>(p->m_ulSteamIDLobby),
				static_cast<unsigned long long>(p->m_ulSteamIDAdmin), p->m_bKickedDueToDisconnect);
			g_myLobbies.erase(p->m_ulSteamIDLobby);
			g_owners.erase(p->m_ulSteamIDLobby);
		}

		void Listener::OnJoinRequested(GameLobbyJoinRequested_t* p)
		{
			if (!Enabled())
				return;
			Trace("Steam: join requested through the overlay/invite: lobby %llu from friend %llu",
				static_cast<unsigned long long>(p->m_steamIDLobby.ConvertToUint64()),
				static_cast<unsigned long long>(p->m_steamIDFriend.ConvertToUint64()));
		}

		void Listener::OnP2PRequest(P2PSessionRequest_t* p)
		{
			if (!Enabled())
				return;
			Trace("Steam: P2P session request from %s", PeerText(p->m_steamIDRemote.ConvertToUint64()).c_str());
		}

		void Listener::OnP2PConnectFail(P2PSessionConnectFail_t* p)
		{
			if (!Enabled())
				return;
			Trace("Steam: P2P connection FAILED with %s: %s (%d) - the game ignores this",
				PeerText(p->m_steamIDRemote.ConvertToUint64()).c_str(), P2PErrorName(p->m_eP2PSessionError), p->m_eP2PSessionError);
		}

		void Listener::OnLobbyDataUpdate(LobbyDataUpdate_t* p)
		{
			if (!Enabled())
				return;
			static std::set<uint64_t> s_failedLobbies;
			if (!p->m_bSuccess)
			{
				if (s_failedLobbies.insert(p->m_ulSteamIDLobby).second)
					Trace("Steam: lobby data update FAILED for lobby %llu (member %llu) - the lobby may be gone (logged once per lobby)",
						static_cast<unsigned long long>(p->m_ulSteamIDLobby), static_cast<unsigned long long>(p->m_ulSteamIDMember));
			}
			else
				++m_dataUpdates;
		}

		Listener* g_listener = nullptr;

		// ------------------------------------------------------------------ traffic

		struct ChannelStats
		{
			uint32_t sent = 0, sentBytes = 0, sendFails = 0, recv = 0, recvBytes = 0;
		};
		struct PeerStats
		{
			std::map<int, ChannelStats> channels;
			ULONGLONG lastRecvMs = 0;
			ULONGLONG firstMs = 0;
			bool silentReported = false;
			bool known = false;
		};
		std::map<uint64_t, PeerStats> g_peers;
		ULONGLONG g_lastSummaryMs = 0;

		struct MessageKey
		{
			uint64_t peer; bool out; uint16_t type;
			bool operator<(const MessageKey& o) const
			{
				if (peer != o.peer) return peer < o.peer;
				if (out != o.out) return out < o.out;
				return type < o.type;
			}
		};
		struct MessageLog { ULONGLONG lastMs = 0; uint32_t suppressed = 0; };
		std::map<MessageKey, MessageLog> g_messageLog;

		// Game lobby messages on channel 0: word +0 length, word +2 type. The mod's own packets
		// repeat the size at +2 and carry version 0x95 at +4.
		void TraceGameMessage(uint64_t peer, bool out, const void* data, uint32_t size)
		{
			if (!data || size < 4)
				return;
			// On the wire (CNetworker_FlushSendQueue 0x0041D260): +0 u16 data length, then the
			// message itself: +2 u16 length, +4 u16 type. The mod's own packets are type 0x95.
			if (size < 6)
				return;
			const uint8_t* b = static_cast<const uint8_t*>(data);
			const uint16_t type = *reinterpret_cast<const uint16_t*>(b + 4);
			if (type == 0x95)
				return;
			const char* name = GameMessageName(type);
			if (type == 0x38 || type == 0x39)
				return;   // ping/pong: counted in the channel stats only
			MessageLog& log = g_messageLog[MessageKey{ peer, out, type }];
			const ULONGLONG now = GetTickCount64();
			if (log.lastMs != 0 && now - log.lastMs < 3000)
			{
				++log.suppressed;
				return;
			}
			char extra[64] = "";
			if (type == 0x34 && size >= 8)
				snprintf(extra, sizeof(extra), " reason %u", *reinterpret_cast<const uint16_t*>(b + 6));
			char repeats[48] = "";
			if (log.suppressed)
				snprintf(repeats, sizeof(repeats), " (+%u more in the last 3 s)", log.suppressed);
			Trace("msg %s %s %s: 0x%02X %s len %u%s%s", out ? "->" : "<-", out ? "to" : "from", PeerText(peer).c_str(),
				type, name ? name : "", size, extra, repeats);
			log.lastMs = now;
			log.suppressed = 0;
		}

		// ------------------------------------------------------------------ P2P state probes

		struct ProbeState { bool active = false, connecting = false, relay = false; int error = 0; bool valid = false; };
		std::map<uint64_t, ProbeState> g_probes;

		void ProbePeers(const char* why, bool force)
		{
			ISteamNetworking* net = RealNetworking();
			if (!net)
				return;
			const uint64_t self = SelfId();
			for (uint64_t id : g_lastRoomMembers)
			{
				if (id == 0 || id == self)
					continue;
				P2PSessionState_t s = {};
				const bool have = net->GetP2PSessionState(CSteamID(id), &s);
				ProbeState now;
				now.valid = have;
				now.active = have && s.m_bConnectionActive;
				now.connecting = have && s.m_bConnecting;
				now.relay = have && s.m_bUsingRelay;
				now.error = have ? s.m_eP2PSessionError : -1;
				ProbeState& before = g_probes[id];
				if (!force && before.valid == now.valid && before.active == now.active && before.connecting == now.connecting &&
					before.relay == now.relay && before.error == now.error)
					continue;
				before = now;
				if (!have)
					Trace("P2P %llu [%s]: no session", static_cast<unsigned long long>(id), why);
				else
					Trace("P2P %llu [%s]: active=%d connecting=%d relay=%d error=%s queued=%d bytes", static_cast<unsigned long long>(id),
						why, now.active, now.connecting, now.relay, P2PErrorName(now.error), s.m_nBytesQueuedForSend);
			}
		}

		// ------------------------------------------------------------------ game state

		struct GameState
		{
			uint32_t task = 0xFFFFFFFF, state = 0xFFFFFFFF, dialog = 0, toast = 0;
			uint32_t autoEntry = 0xFFFFFFFF, autoStep = 0xFFFFFFFF;
			uint8_t server[16] = {};
			bool serverValid = false;
			uint32_t serverError = 0;
			uint8_t inSession = 0;
		};
		GameState g_state;
		std::string g_roomSignature;
		ULONGLONG g_lastProbeMs = 0;

		const char* TaskName(uint32_t t) { return t < sizeof(kTaskNames) / sizeof(kTaskNames[0]) ? kTaskNames[t] : "?"; }

		void PollGameState()
		{
			const uintptr_t base = Base();
			if (!base)
				return;
			bool changed = false;

			uint32_t task = 0, state = 0, dialog = 0, toast = 0;
			const uintptr_t taskAddr = base + kNetTaskRva;
			if (ReadGame(taskAddr, task) && ReadGame(taskAddr + 4, state) && ReadGame(taskAddr + 0xAC, dialog) && ReadGame(taskAddr + 0xB4, toast))
			{
				if (task != g_state.task)
				{
					Trace("network task: %s (%u) -> %s (%u), state 0x%X", TaskName(g_state.task), g_state.task, TaskName(task), task, state);
					changed = true;
				}
				else if (state != g_state.state)
				{
					Trace("network task %s: state 0x%X -> 0x%X", TaskName(task), g_state.state, state);
					changed = true;
				}
				if (dialog != g_state.dialog)
				{
					if (dialog)
						Trace("DIALOG shown: 0x%X %s (task %s, state 0x%X)", dialog, DialogName(dialog), TaskName(task), state);
					else
						Trace("dialog closed (was 0x%X %s)", g_state.dialog, DialogName(g_state.dialog));
					changed = true;
				}
				if (toast != g_state.toast && toast)
					Trace("notice: 0x%X %s", toast, DialogName(toast));
				g_state.task = task; g_state.state = state; g_state.dialog = dialog; g_state.toast = toast;
			}

			uint32_t entry = 0, step = 0;
			if (ReadGame(base + kAutomatchRva, entry) && ReadGame(base + kAutomatchRva + 8, step))
			{
				if (entry != g_state.autoEntry || step != g_state.autoStep)
				{
					if (g_state.autoEntry != 0xFFFFFFFF)
						Trace("ranked automatch: entry %u -> %u, step %u -> %u", g_state.autoEntry, entry, g_state.autoStep, step);
					g_state.autoEntry = entry; g_state.autoStep = step;
				}
			}

			// Network server: +0x25DC login, +0x25DD host, +0x25DF search, +0x25E0 join,
			// +0x25E3..+0x25EA room slots; +0xAB0 last reason; +0xAAC in session.
			const uintptr_t server = base + kNetServerRva;
			uint8_t bytes[16] = {};
			uint32_t error = 0;
			uint8_t inSession = 0;
			if (!IsBadReadPtr(reinterpret_cast<const void*>(server + 0x25DC), 15) && ReadGame(server + 0xAB0, error) && ReadGame(server + 0xAAC, inSession))
			{
				std::memcpy(bytes, reinterpret_cast<const void*>(server + 0x25DC), 15);
				if (!g_state.serverValid || std::memcmp(bytes, g_state.server, 15) != 0 || error != g_state.serverError || inSession != g_state.inSession)
				{
					Trace("net server: login %u host %u search %u join %u slots %u %u %u %u %u %u %u %u | inSession %u reason %u",
						bytes[0], bytes[1], bytes[3], bytes[4], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
						inSession, error);
					std::memcpy(g_state.server, bytes, sizeof(bytes));
					g_state.serverValid = true;
					g_state.serverError = error;
					g_state.inSession = inSession;
				}
			}

			// The room member table.
			Room* room = g_gameVals.pRoom;
			std::set<uint64_t> members;
			std::string signature;
			if (room && !IsBadReadPtr(room, sizeof(Room)))
			{
				const RoomMemberEntry* entries = &room->member1;
				char head[96];
				snprintf(head, sizeof(head), "type 0x%02X status %d count %u cap %u |", room->roomType, room->roomStatus, room->memberCount, room->capacity);
				signature = head;
				for (int i = 0; i < 8; ++i)
				{
					const RoomMemberEntry& m = entries[i];
					if (m.steamId == 0)
						continue;
					const uint32_t flags = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(&m) + 0x2C);
					char one[96];
					snprintf(one, sizeof(one), " [%d] %llu flags 0x%X match %u", i, static_cast<unsigned long long>(m.steamId), flags, m.matchPlayerIndex);
					signature += one;
					members.insert(m.steamId);
				}
			}
			if (signature != g_roomSignature)
			{
				if (!signature.empty())
					Trace("room: %s", signature.c_str());
				else if (!g_roomSignature.empty())
					Trace("room: none");
				g_roomSignature = signature;
				g_lastRoomMembers = members;
				changed = true;
			}

			const ULONGLONG now = GetTickCount64();
			if (changed)
			{
				char why[64];
				snprintf(why, sizeof(why), "%s 0x%X", TaskName(task), state);
				ProbePeers(why, true);
				g_lastProbeMs = now;
			}
			else if (!g_lastRoomMembers.empty() && now - g_lastProbeMs > 5000)
			{
				ProbePeers("periodic", false);
				g_lastProbeMs = now;
			}
		}

		// ------------------------------------------------------------------ Arc's web API
		// HttpPerformPost (0x00438BB0), stdcall(desc), ret 4, runs one request on its own
		// thread: desc +0x00 URL (std::wstring), +0xE3C 1 ok / 2 any curl error (connect
		// failure, 10 s timeout, HTTP >= 400), +0xE40 HTTP status. Timing it here shows whether
		// Arc's server answered, how fast, and with what - even when the game stopped waiting
		// (login gives up after ~3 s while curl waits up to 10 s).
		constexpr uintptr_t kHttpPostRva = 0x00038BB0;
		using HttpPost_t = int(__stdcall*)(void* desc);
		HttpPost_t g_origHttpPost = nullptr;

		std::string DescEndpoint(const uint8_t* desc)
		{
			if (!desc || IsBadReadPtr(desc, 0x18))
				return "?";
			const uint32_t size = *reinterpret_cast<const uint32_t*>(desc + 0x10);
			const uint32_t capacity = *reinterpret_cast<const uint32_t*>(desc + 0x14);
			const wchar_t* text = capacity >= 8 ? *reinterpret_cast<wchar_t* const*>(desc) : reinterpret_cast<const wchar_t*>(desc);
			if (!text || size == 0 || size > 512 || IsBadReadPtr(text, size * sizeof(wchar_t)))
				return "?";
			std::string url;
			for (uint32_t i = 0; i < size; ++i)
				url += static_cast<char>(text[i] < 128 ? text[i] : '?');
			const size_t api = url.find("/api/");
			return api == std::string::npos ? url : url.substr(api + 5);
		}

		int __stdcall HttpPostHook(void* desc)
		{
			const uint8_t* d = static_cast<const uint8_t*>(desc);
			const std::string endpoint = DescEndpoint(d);
			const ULONGLONG start = GetTickCount64();
			const int result = g_origHttpPost(desc);
			const ULONGLONG took = GetTickCount64() - start;
			uint32_t outcome = 0, http = 0;
			if (!IsBadReadPtr(d + 0xE3C, 8))
			{
				outcome = *reinterpret_cast<const uint32_t*>(d + 0xE3C);
				http = *reinterpret_cast<const uint32_t*>(d + 0xE40);
			}
			if (Enabled())
				Trace("Arc server %s -> %s, HTTP %u, %llu ms%s", endpoint.c_str(),
					outcome == 1 ? "answered" : outcome == 2 ? "FAILED (no connection, timeout or HTTP error)" : "?",
					http, static_cast<unsigned long long>(took), took > 3000 ? " (slower than the game's 3 s login wait)" : "");
			return result;
		}

		// ------------------------------------------------------------------ GGPO assertion

		DWORD g_assertBack = 0;

		void __cdecl OnGgpoAssert(const char* message, uintptr_t returnAddress)
		{
			char text[512] = "?";
			if (message && !IsBadReadPtr(message, 1))
				strncpy_s(text, message, _TRUNCATE);
			Trace("GGPO ASSERTION (the game closes right after): \"%s\" from %s", text,
				CallerText(reinterpret_cast<void*>(returnAddress)).c_str());
		}

		void __declspec(naked) GgpoAssertHook()
		{
			__asm
			{
				pushad
				mov eax, [esp + 24h]   // message
				mov ecx, [esp + 20h]   // return address
				push ecx
				push eax
				call OnGgpoAssert
				add esp, 8
				popad
				push ebp
				mov ebp, esp
				push 30h
				jmp[g_assertBack]
			}
		}
	}

	// ====================================================================== public

	bool Enabled()
	{
		return Settings::settingsIni.lobbyTrace;
	}

	void OnCreateLobby(int lobbyType, int maxMembers, uint64_t call, void* caller)
	{
		if (!Enabled()) return;
		char what[128];
		snprintf(what, sizeof(what), "CreateLobby(%s, max %d)", LobbyTypeName(lobbyType), maxMembers);
		Trace("%s call %llu from %s", what, static_cast<unsigned long long>(call), CallerText(caller).c_str());
		StartPending(call, what);
	}

	void OnJoinLobby(uint64_t lobby, uint64_t owner, uint64_t call, void* caller)
	{
		if (!Enabled()) return;
		char what[128];
		snprintf(what, sizeof(what), "JoinLobby(%llu, owner %llu)", static_cast<unsigned long long>(lobby), static_cast<unsigned long long>(owner));
		std::string inOthers;
		for (uint64_t mine : g_myLobbies)
		{
			if (mine != lobby)
			{
				char t[64];
				snprintf(t, sizeof(t), " %llu", static_cast<unsigned long long>(mine));
				inOthers += t;
			}
		}
		Trace("%s call %llu from %s%s%s", what, static_cast<unsigned long long>(call), CallerText(caller).c_str(),
			inOthers.empty() ? "" : " - while still in lobby", inOthers.c_str());
		StartPending(call, what);
	}

	void OnLeaveLobby(uint64_t lobby, void* caller)
	{
		if (!Tracking()) return;
		Trace("LeaveLobby(%llu) from %s%s", static_cast<unsigned long long>(lobby), CallerText(caller).c_str(),
			g_myLobbies.count(lobby) ? "" : " (not a lobby we knew we were in)");
		g_myLobbies.erase(lobby);
		g_owners.erase(lobby);
	}

	bool IsOwnerOfJoinedLobby(uint64_t id)
	{
		ISteamMatchmaking* mm = RealMatchmaking();
		if (!mm || !id)
			return false;
		for (uint64_t lobby : g_myLobbies)
		{
			if (mm->GetLobbyOwner(CSteamID(lobby)).ConvertToUint64() == id)
				return true;
		}
		return false;
	}

	void OnListFilter(const char* description)
	{
		if (!Enabled() || !description) return;
		g_listFilters += std::string(g_listFilters.empty() ? "" : ", ") + description;
	}

	void OnRequestLobbyList(uint64_t call, void* caller)
	{
		if (!Enabled()) return;
		const std::string what = "RequestLobbyList(" + (g_listFilters.empty() ? std::string("no filters") : g_listFilters) + ")";
		g_listFilters.clear();
		Trace("%s call %llu from %s", what.c_str(), static_cast<unsigned long long>(call), CallerText(caller).c_str());
		StartPending(call, what);
	}

	void OnSetLobbyData(uint64_t lobby, const char* key, const char* value)
	{
		if (!Enabled() || !key) return;
		static std::map<std::string, std::string> s_last;
		char id[32];
		snprintf(id, sizeof(id), "%llu:", static_cast<unsigned long long>(lobby));
		const std::string k = std::string(id) + key;
		const std::string v = value ? value : "";
		auto it = s_last.find(k);
		if (it != s_last.end() && it->second == v)
			return;
		s_last[k] = v;
		Trace("SetLobbyData(%llu) %s=%s", static_cast<unsigned long long>(lobby), key, v.c_str());
	}

	void OnLobbySetting(const char* what, uint64_t lobby, int value)
	{
		if (!Enabled()) return;
		Trace("%s(%llu, %d)", what, static_cast<unsigned long long>(lobby), value);
	}

	CCallbackBase* WrapRegisterCallResult(CCallbackBase* callback, uint64_t call)
	{
		if (!Tracking() || !callback)
			return callback;
		const int id = CallbackPeek::Id(callback);
		if (id != LobbyCreated_t::k_iCallback && id != LobbyEnter_t::k_iCallback && id != LobbyMatchList_t::k_iCallback)
			return callback;
		TraceProxy* proxy = new TraceProxy(callback, call);
		g_proxies.push_back(proxy);
		return proxy;
	}

	CCallbackBase* MapUnregisterCallResult(CCallbackBase* callback, uint64_t call)
	{
		for (TraceProxy* proxy : g_proxies)
		{
			if (proxy->m_inner == callback && proxy->m_call == call && !proxy->m_done)
			{
				auto it = g_pending.find(call);
				if (it != g_pending.end())
				{
					Trace("%s: the game abandoned it after %llu ms, before Steam answered", it->second.what.c_str(),
						static_cast<unsigned long long>(GetTickCount64() - it->second.startMs));
					g_pending.erase(it);
				}
				proxy->m_done = GetTickCount64();
				return proxy;
			}
		}
		return callback;
	}

	void OnSendP2P(uint64_t remote, const void* data, uint32_t size, int sendType, int channel, bool ok)
	{
		if (!Enabled()) return;
		PeerStats& peer = g_peers[remote];
		if (!peer.firstMs)
		{
			peer.firstMs = GetTickCount64();
			peer.known = IsKnownPeer(remote);
			Trace("first P2P send to %s on channel %d (%s)", PeerText(remote).c_str(), channel,
				sendType == 0 ? "unreliable" : sendType == 2 ? "reliable" : "other");
		}
		ChannelStats& c = peer.channels[channel];
		++c.sent;
		c.sentBytes += size;
		if (!ok)
		{
			if (c.sendFails++ == 0)
				Trace("P2P send to %llu on channel %d FAILED", static_cast<unsigned long long>(remote), channel);
		}
		if (channel == 0)
			TraceGameMessage(remote, true, data, size);
	}

	void OnReadP2P(uint64_t remote, const void* data, uint32_t size, int channel)
	{
		if (!Enabled()) return;
		PeerStats& peer = g_peers[remote];
		const ULONGLONG now = GetTickCount64();
		if (!peer.firstMs)
		{
			peer.firstMs = now;
			peer.known = IsKnownPeer(remote);
			Trace("first P2P packet from %s on channel %d", PeerText(remote).c_str(), channel);
		}
		if (peer.silentReported)
			Trace("packets from %llu again after %.1f s of silence", static_cast<unsigned long long>(remote), (now - peer.lastRecvMs) / 1000.0);
		peer.silentReported = false;
		peer.lastRecvMs = now;
		ChannelStats& c = peer.channels[channel];
		++c.recv;
		c.recvBytes += size;
		if (channel == 0)
			TraceGameMessage(remote, false, data, size);
	}

	void OnAcceptP2P(uint64_t remote, bool ok, void* caller)
	{
		if (!Enabled()) return;
		Trace("game ACCEPTED P2P session with %s from %s%s", PeerText(remote).c_str(), CallerText(caller).c_str(), ok ? "" : " (Steam said no)");
	}

	void OnCloseP2P(uint64_t remote, int channel, void* caller)
	{
		if (!Enabled()) return;
		Trace("close P2P %s with %llu from %s", channel < 0 ? "session" : "channel", static_cast<unsigned long long>(remote), CallerText(caller).c_str());
	}

	void OnAllowRelay(bool allow)
	{
		if (!Enabled()) return;
		Trace("AllowP2PPacketRelay(%d)", allow ? 1 : 0);
	}

	void InstallGameHooks()
	{
		if (!Enabled())
			return;
		const uintptr_t site = Base() + kGgpoAssertRva;
		static const uint8_t kEntry[] = { 0x55, 0x8B, 0xEC, 0x6A, 0x30 };
		if (IsBadReadPtr(reinterpret_cast<const void*>(site), sizeof(kEntry)) ||
			std::memcmp(reinterpret_cast<const void*>(site), kEntry, sizeof(kEntry)) != 0)
		{
			LOG(1, "[LobbyTrace] GGPO assertion function did not match; not hooked\n");
			return;
		}
		g_assertBack = HookManager::SetHook("LobbyTraceGgpoAssert", static_cast<DWORD>(site), sizeof(kEntry),
			reinterpret_cast<void*>(GgpoAssertHook));
		LOG(1, "[LobbyTrace] %s\n", g_assertBack ? "GGPO assertions are logged" : "could not hook the GGPO assertion function");

		static const uint8_t kHttpEntry[] = { 0x55, 0x8B, 0xEC, 0x56, 0xE8 };
		const uintptr_t http = Base() + kHttpPostRva;
		if (!IsBadReadPtr(reinterpret_cast<const void*>(http), sizeof(kHttpEntry)) &&
			std::memcmp(reinterpret_cast<const void*>(http), kHttpEntry, sizeof(kHttpEntry)) == 0)
		{
			g_origHttpPost = reinterpret_cast<HttpPost_t>(DetourFunction(reinterpret_cast<PBYTE>(http), reinterpret_cast<PBYTE>(HttpPostHook)));
			LOG(1, "[LobbyTrace] %s\n", g_origHttpPost ? "Arc server requests are timed" : "could not hook the HTTP request function");
		}
		else
		{
			LOG(1, "[LobbyTrace] HTTP request function did not match; not hooked\n");
		}
	}

	void Tick()
	{
		if (!Tracking())
			return;
		if (!g_listener && g_interfaces.pSteamMatchmakingWrapper)
		{
			g_listener = new Listener();
			Trace("listening to Steam lobby and P2P events");
		}

		const ULONGLONG now = GetTickCount64();

		// Owners as of this frame, so a leave in next frame's callbacks knows who owned it before.
		if (ISteamMatchmaking* mm = RealMatchmaking())
		{
			for (uint64_t lobby : g_myLobbies)
			{
				const uint64_t owner = mm->GetLobbyOwner(CSteamID(lobby)).ConvertToUint64();
				if (owner)
					g_owners[lobby] = owner;
			}
		}

		// Finished proxies are freed a while after Steam is done with them.
		for (size_t i = 0; i < g_proxies.size();)
		{
			if (g_proxies[i]->m_done && now - g_proxies[i]->m_done > 5000)
			{
				delete g_proxies[i];
				g_proxies.erase(g_proxies.begin() + i);
			}
			else
			{
				++i;
			}
		}

		if (!Enabled())
			return;

		PollGameState();

		// Requests Steam has not answered.
		for (auto& entry : g_pending)
		{
			Pending& p = entry.second;
			const ULONGLONG age = now - p.startMs;
			const int level = age > 120000 ? 3 : age > 30000 ? 2 : age > 10000 ? 1 : 0;
			if (level > p.warned)
			{
				Trace("%s: STILL no answer from Steam after %llu s", p.what.c_str(), static_cast<unsigned long long>(age / 1000));
				p.warned = level;
			}
		}

		// Peers that went quiet, and a traffic summary every 10 s while anything flows.
		for (auto& entry : g_peers)
		{
			PeerStats& p = entry.second;
			if (p.lastRecvMs && !p.silentReported && now - p.lastRecvMs > 2000 && g_lastRoomMembers.count(entry.first))
			{
				Trace("no packets from %llu for %.1f s (still in the room table)", static_cast<unsigned long long>(entry.first), (now - p.lastRecvMs) / 1000.0);
				p.silentReported = true;
			}
		}
		if (now - g_lastSummaryMs > 10000)
		{
			for (auto& entry : g_peers)
			{
				std::string line;
				for (auto& ch : entry.second.channels)
				{
					char t[96];
					snprintf(t, sizeof(t), " ch%d out %u/%uB%s in %u/%uB", ch.first, ch.second.sent, ch.second.sentBytes,
						ch.second.sendFails ? " FAILS" : "", ch.second.recv, ch.second.recvBytes);
					line += t;
				}
				if (!line.empty())
					Trace("traffic %llu:%s", static_cast<unsigned long long>(entry.first), line.c_str());
				entry.second.channels.clear();
			}
			g_lastSummaryMs = now;
		}
	}
}
