#include "LobbyFixes.h"

#include "LobbyTrace.h"
#include "RankedListConnectionFilter.h"

#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Game/Room/Room.h"
#include "Game/gamestates.h"
#include "Overlay/NotificationBar/NotificationBar.h"
#include "SteamApiWrapper/SteamMatchmakingWrapper.h"
#include "SteamApiWrapper/SteamUserWrapper.h"

#include <isteammatchmaking.h>

#include <Windows.h>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace LobbyFixes
{
	namespace
	{
		// Ranked state 0x37 (pre-match profile upload) arms a 3000-frame budget in +0x14 and
		// decrements +0x14, but its give-up test reads +0x20 (`cmp dword ptr [edi+20h],0`),
		// a field of other tasks, so the budget never runs out and a stuck upload hangs the
		// match. Pointing the test at +0x14 makes the intended timeout real.
		constexpr uintptr_t kRankedUploadTestRva = 0xAF78F;
		const uint8_t kRankedUploadTest[] = { 0x83, 0x7F, 0x20, 0x00, 0x0F, 0x8D };

		// CNetworker_Update (0x0041D0E0) drains P2P channel 0 into the game's incoming queue;
		// when PacketMgr_PushIncoming (0x0040A770) reports the queue full, it falls into
		// `jmp $` (0x0041D1E0): the main thread spins forever and the game freezes in the room.
		// Jumping to the loop exit instead loses that one packet and leaves the rest in
		// Steam's queue for the next frame, once the game has worked through its own.
		constexpr uintptr_t kQueueFullSpinRva = 0x1D1DD;
		const uint8_t kQueueFullSpin[] = { 0x8D, 0x49, 0x00, 0xEB, 0xFE };   // lea ecx,[ecx]; jmp $

		// The profile workers (download 0x00422B00, upload 0x00423A50) wait 300 x Sleep(10),
		// about 3 s, for a tus/read or tus/write, then submit it again with the same session
		// token - while the first request is still running (curl allows it 10 s). Arc's server
		// mints a new token with every answer and refuses the old one, so when the first
		// request lands, the re-send is refused with param.status 3: a failed upload or an
		// empty D-code card, and a forced re-login ("dropped your sign-in"). Every slow
		// answer (> 3 s) in a 2026-10-09 session was followed by exactly that. Waiting 1100
		// ticks (> curl's 10 s) means a re-send only ever follows a request that has ended.
		constexpr uintptr_t kTusDownloadWaitRva = 0x22BC2;
		constexpr uintptr_t kTusUploadWaitRva = 0x23B10;
		const uint8_t kTusWaitCmp[] = { 0x81, 0xFF, 0x2C, 0x01, 0x00, 0x00, 0x7D };   // cmp edi,300; jge
		constexpr uint32_t kTusWaitTicks = 1100;

		// The game's join handshake on P2P channel 0. On the wire every message is preceded by a
		// u16 data length (CNetworker_FlushSendQueue 0x0041D260), so: +0 wire length, +2 message
		// length, +4 type. These only
		// ever come from the host of the room being joined; the game accepts them from anyone
		// (Server_OnPacket 0x0046EC00), so a late one from a previous host kicks you out of
		// the room you are in now, or overwrites it.
		//
		// The room snapshot (msg 2) is the worst of them. The host broadcasts it about once a
		// second to every member it still lists, and Msg2_RoomSnapshot (0x0049C000) applies it
		// with no sender or role check; a new room's revision is 0, so it always passes. A
		// player who left a room before its host dropped them, then hosts ranked, gets their
		// new room replaced by the old one: count 2, the old host as host, themselves a
		// plain member. Their Steam lobby stays listed with only them in it, every join is
		// refused with "room full", and nothing heals it because they no longer hold the host
		// bit (2026-10-09: two such hosts in one ranked list).
		struct HostOnlyMessage
		{
			uint16_t type;
			uint16_t length;
			const char* name;
		};
		const HostOnlyMessage kHostOnly[] = {
			{ 0x34, 0x0006, "join rejected" },
			{ 0x35, 0x0388, "room info" },
			{ 0x37, 0x0004, "room closed" },
			{ 0x02, 0x03A8, "room snapshot" },
		};

		ISteamMatchmaking* RealMatchmaking()
		{
			return g_interfaces.pSteamMatchmakingWrapper ? g_interfaces.pSteamMatchmakingWrapper->m_SteamMatchmaking : nullptr;
		}

		uint64_t SelfId()
		{
			return g_interfaces.pSteamUserWrapper ? g_interfaces.pSteamUserWrapper->GetSteamID().ConvertToUint64() : 0;
		}

		// GAMESTEAM_CNetworkServer (0x00A62730): +0x25DD host state (3 = hosting), +0x25E0 join
		// state (7 = joined), +0xAAC in a room session. Lobby_C_NetworkServer.md.
		constexpr uintptr_t kNetServerRva = 0x00662730;
		constexpr uint8_t kHostStateHosting = 3;
		constexpr uint8_t kJoinStateJoined = 7;

		template <typename T>
		bool ReadServer(uintptr_t offset, T& out)
		{
			const uintptr_t address = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress()) + kNetServerRva + offset;
			if (IsBadReadPtr(reinterpret_cast<const void*>(address), sizeof(T)))
				return false;
			out = *reinterpret_cast<const T*>(address);
			return true;
		}

		bool InRoomTable(uint64_t id)
		{
			Room* room = g_gameVals.pRoom;
			if (!room || !id || IsBadReadPtr(room, sizeof(Room)))
				return false;
			const RoomMemberEntry* entries = &room->member1;
			for (int i = 0; i < 8; ++i)
			{
				if (entries[i].steamId == id)
					return true;
			}
			return false;
		}

		// Only on the menus (the room and ranked screens live in the main menu scene). In a
		// match GGPO's own disconnect handling runs, and a removal mid-match would race it; a
		// leave seen then waits here until the players are back on the menu.
		bool OnMenus()
		{
			return g_gameVals.pGameState && *g_gameVals.pGameState == GameState_MainMenu;
		}

		// ------------------------------------------------------------------ missed leaves

		// The peer's own leave message (0x36 member leaving, 0x37 room closed) goes out as 3
		// unreliable copies and never at all when its game crashes or loses the connection.
		// Steam's lobby still reports the departure; the game does not listen to Steam's. So
		// a couple of seconds after Steam says so, if the game still has that peer, the mod
		// delivers the message the peer would have sent and the game runs its own handling.
		constexpr ULONGLONG kLeaveGraceMs = 2000;
		constexpr ULONGLONG kLeaveGiveUpMs = 10 * 60 * 1000;

		struct MissedLeave
		{
			uint64_t peer;
			bool peerWasHost;   // true: we are a guest and the host left (0x37); false: 0x36
			ULONGLONG seenMs;
			std::string how;
		};
		std::vector<MissedLeave> g_missedLeaves;

		struct Injected
		{
			uint64_t from;
			uint8_t bytes[6];   // wire length, message length, type
		};
		std::deque<Injected> g_injected;

		void Inject(uint64_t from, uint16_t type)
		{
			Injected m;
			m.from = from;
			const uint16_t length = 4;
			std::memcpy(m.bytes, &length, 2);
			std::memcpy(m.bytes + 2, &length, 2);
			std::memcpy(m.bytes + 4, &type, 2);
			g_injected.push_back(m);
		}

		// true once the case is settled (delivered, or no longer needed).
		bool ResolveMissedLeave(const MissedLeave& leave, ULONGLONG now)
		{
			if (!InRoomTable(leave.peer))
				return true;   // the game already has it (its message arrived, or it noticed)
			if (now - leave.seenMs > kLeaveGiveUpMs)
			{
				LOG(1, "[LobbyFixes] gave up delivering the leave of %llu: still on a match/loading screen after 10 min\n",
					static_cast<unsigned long long>(leave.peer));
				return true;
			}
			if (now - leave.seenMs < kLeaveGraceMs || !OnMenus())
				return false;

			uint8_t hostState = 0, joinState = 0, inSession = 0;
			ReadServer(0x25DD, hostState);
			ReadServer(0x25E0, joinState);
			ReadServer(0xAAC, inSession);
			if (leave.peerWasHost)
			{
				if (joinState != kJoinStateJoined || !inSession)
					return true;   // not in that room as a guest any more
				Inject(leave.peer, 0x37);
				LOG(1, "[LobbyFixes] the host %llu left the Steam lobby (%s) %.1f s ago without closing the room: delivering \"room closed\"\n",
					static_cast<unsigned long long>(leave.peer), leave.how.c_str(), (now - leave.seenMs) / 1000.0);
			}
			else
			{
				if (hostState != kHostStateHosting)
					return true;
				Inject(leave.peer, 0x36);
				LOG(1, "[LobbyFixes] %llu left the Steam lobby (%s) %.1f s ago but is still in our room: delivering their leave\n",
					static_cast<unsigned long long>(leave.peer), leave.how.c_str(), (now - leave.seenMs) / 1000.0);
			}
			return true;
		}

		// ------------------------------------------------------------------ connection ratings

		// The ranked and room lists rate each result with GAMESTEAM_QoSListner (netServer
		// +0x1178): one row per result, 5 ping/pong samples, RTT = mean of the 4 lowest. The pump
		// (QoS_PumpPings 0x004702E0) sends ping k only while ts[k] == 0, and ping k+1 only after
		// pong k came back. Nothing ever re-sends, so one lost ping or pong - likely on the very
		// first one, sent while Steam is still opening the P2P session - leaves that row, and the
		// lobby's rating, blank until the list is refreshed. Re-arming a ping that got no answer
		// makes the pump send it again with a fresh timestamp; a late pong for the old one
		// matches nothing and is ignored (QoS_OnPong 0x0046F7A0 matches on the timestamp).
		//
		// The listener also has only 20 rows, and QoS_AddRow (0x0046F720) silently drops the
		// rest, so results past the 20th are never rated. Finished rows have already been copied
		// into their result (Server_PollResultRtts 0x0046DB40), so they are handed to the
		// results still waiting.
		constexpr uintptr_t kQoSOffset = 0x1178;
		constexpr uintptr_t kQoSRows = 0x1180;
		constexpr uintptr_t kQoSRowSize = 0x68;    // +0 id64, +8 ts_us[5], +0x30 rtt_ms[5], +0x58 count, +0x5C active, +0x60 done
		constexpr int kQoSRowCount = 20;
		constexpr int kQoSSamples = 5;
		constexpr int64_t kPingRetryUs = 1000 * 1000;
		constexpr int kPingRetryLimit = 15;        // per row; a host that never answers stays blank

		constexpr uintptr_t kNetTaskRva = 0x008F7958;      // +0 outer task: 4 ranked menu, 6 player match menu
		constexpr uintptr_t kGetResultNodeRva = 0x000A5450; // thiscall(server, int index) ret 4 -> node
		constexpr uintptr_t kQoSAddRowRva = 0x0006F720;     // thiscall(qos, idLo, idHi) ret 8
		const uint8_t kGetResultNodeEntry[] = { 0x55, 0x8B, 0xEC, 0x53, 0x8B, 0xD9, 0x56 };
		const uint8_t kQoSAddRowEntry[] = { 0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08 };

		typedef void* (__thiscall* GetResultNode_t)(void* server, int index);
		typedef void (__thiscall* QoSAddRow_t)(void* qos, uint32_t idLo, uint32_t idHi);
		GetResultNode_t g_getResultNode = nullptr;
		QoSAddRow_t g_qosAddRow = nullptr;

		struct RowRetries
		{
			uint64_t id = 0;
			int count = 0;
			int retries = 0;
		};
		RowRetries g_rowRetries[kQoSRowCount];

		// Timer_NowMicros (0x0044F600): QPC * 1e6 / frequency, with the same 64-bit wrap.
		int64_t GameNowMicros()
		{
			LARGE_INTEGER counter, frequency;
			QueryPerformanceCounter(&counter);
			QueryPerformanceFrequency(&frequency);
			const int64_t scaled = static_cast<int64_t>(static_cast<uint64_t>(counter.QuadPart) * 1000000ULL);
			return scaled / frequency.QuadPart;
		}

		uint8_t* ServerBase()
		{
			return reinterpret_cast<uint8_t*>(GetBbcfBaseAdress()) + kNetServerRva;
		}

		void RetryLostPings()
		{
			uint8_t* rows = ServerBase() + kQoSRows;
			if (IsBadWritePtr(rows, kQoSRowSize * kQoSRowCount))
				return;
			const int64_t now = GameNowMicros();
			for (int i = 0; i < kQoSRowCount; ++i)
			{
				uint8_t* row = rows + i * kQoSRowSize;
				const uint64_t id = *reinterpret_cast<uint64_t*>(row);
				const int count = *reinterpret_cast<int*>(row + 0x58);
				const int active = *reinterpret_cast<int*>(row + 0x5C);
				RowRetries& r = g_rowRetries[i];
				if (r.id != id || count < r.count)   // another peer, or the rows were cleared for a new list
				{
					r.id = id;
					r.retries = 0;
				}
				r.count = count;
				if (!active || count < 0 || count >= kQoSSamples)
				{
					if (!active && r.retries > 0 && *reinterpret_cast<int*>(row + 0x60))
					{
						LOG(1, "[LobbyFixes] connection to %llu rated after %d re-sent ping(s)\n", static_cast<unsigned long long>(id), r.retries);
						r.retries = 0;
					}
					continue;
				}
				int64_t& sentAt = *reinterpret_cast<int64_t*>(row + 8 + count * 8);
				if (sentAt == 0 || now - sentAt < kPingRetryUs || r.retries >= kPingRetryLimit)
					continue;
				sentAt = 0;   // the pump sends this ping again on its next pass
				if (++r.retries == 1 || r.retries == kPingRetryLimit)
					LOG(1, "[LobbyFixes] no answer to ping %d of 5 from %llu after 1 s: re-sending%s\n", count + 1,
						static_cast<unsigned long long>(id), r.retries == kPingRetryLimit ? " (last try)" : "");
			}
		}

		bool RowHolds(uint8_t* rows, uint32_t lo, uint32_t hi)
		{
			for (int i = 0; i < kQoSRowCount; ++i)
			{
				uint8_t* row = rows + i * kQoSRowSize;
				const bool used = *reinterpret_cast<int*>(row + 0x5C) || *reinterpret_cast<int*>(row + 0x60);
				if (used && *reinterpret_cast<uint32_t*>(row) == lo && *reinterpret_cast<uint32_t*>(row + 4) == hi)
					return true;
			}
			return false;
		}

		void RateResultsPastTwenty()
		{
			if (!g_getResultNode || !g_qosAddRow)
				return;
			const uintptr_t base = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
			uint32_t task = 0;
			if (IsBadReadPtr(reinterpret_cast<const void*>(base + kNetTaskRva), 4))
				return;
			task = *reinterpret_cast<const uint32_t*>(base + kNetTaskRva);
			if (task != 4 && task != 6)
				return;   // only on the ranked and player match lists; rooms need their host-RTT row
			uint8_t* server = ServerBase();
			uint8_t searching = 0;
			int results = 0;
			ReadServer(0x25DF, searching);
			ReadServer(0xAE8, results);
			if (searching || results <= kQoSRowCount || results > 50)
				return;
			uint8_t* rows = server + kQoSRows;
			int added = 0;
			for (int i = 0; i < results; ++i)
			{
				uint8_t* node = static_cast<uint8_t*>(g_getResultNode(server, i));
				if (!node || IsBadReadPtr(node, 0x118) || !*reinterpret_cast<int*>(node + 0x110))
					continue;
				const uint8_t* entry = *reinterpret_cast<uint8_t**>(node + 0x114);
				if (!entry || IsBadReadPtr(entry, 0x14))
					continue;
				const uint32_t lo = *reinterpret_cast<const uint32_t*>(entry + 0xC);
				const uint32_t hi = *reinterpret_cast<const uint32_t*>(entry + 0x10);
				if (RowHolds(rows, lo, hi))
					continue;
				g_qosAddRow(server + kQoSOffset, lo, hi);   // no-op when every row is busy
				if (!RowHolds(rows, lo, hi))
					break;
				++added;
			}
			if (added)
				LOG(1, "[LobbyFixes] %d more of %d results queued for a connection rating (the game rates only 20 at a time)\n", added, results);
		}

		const char* JoinFailureText(uint32_t response)
		{
			switch (response)
			{
			case k_EChatRoomEnterResponseDoesntExist: return "that room no longer exists";
			case k_EChatRoomEnterResponseNotAllowed: return "that room is not accepting players";
			case k_EChatRoomEnterResponseFull: return "that room is full";
			case k_EChatRoomEnterResponseError: return "Steam returned an error";
			case k_EChatRoomEnterResponseBanned: return "you are banned from that room";
			case k_EChatRoomEnterResponseLimited: return "your Steam account is limited and cannot join rooms";
			case k_EChatRoomEnterResponseMemberBlockedYou: return "a player in that room has blocked you on Steam";
			case k_EChatRoomEnterResponseYouBlockedMember: return "you have blocked a player in that room on Steam";
			default: return nullptr;
			}
		}
	}

	bool Enabled()
	{
		return Settings::settingsIni.lobbyFixes;
	}

	void InstallGamePatches()
	{
		if (!Enabled())
		{
			LOG(1, "[LobbyFixes] off (LobbyFixes=0): the lobby system runs as vanilla\n");
			return;
		}

		const uintptr_t site = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress()) + kRankedUploadTestRva;
		if (!IsBadReadPtr(reinterpret_cast<const void*>(site), sizeof(kRankedUploadTest)) &&
			std::memcmp(reinterpret_cast<const void*>(site), kRankedUploadTest, sizeof(kRankedUploadTest)) == 0)
		{
			char disp = 0x14;
			WriteToProtectedMemory(site + 2, &disp, 1);
			LOG(1, "[LobbyFixes] ranked pre-match upload timeout fixed (3000 frames)\n");
		}
		else
		{
			LOG(1, "[LobbyFixes] ranked pre-match upload test did not match; not patched\n");
		}

		const uintptr_t gameBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
		if (std::memcmp(reinterpret_cast<const void*>(gameBase + kGetResultNodeRva), kGetResultNodeEntry, sizeof(kGetResultNodeEntry)) == 0 &&
			std::memcmp(reinterpret_cast<const void*>(gameBase + kQoSAddRowRva), kQoSAddRowEntry, sizeof(kQoSAddRowEntry)) == 0)
		{
			g_getResultNode = reinterpret_cast<GetResultNode_t>(gameBase + kGetResultNodeRva);
			g_qosAddRow = reinterpret_cast<QoSAddRow_t>(gameBase + kQoSAddRowRva);
		}
		else
		{
			LOG(1, "[LobbyFixes] QoS functions did not match; results past the 20th stay unrated\n");
		}

		const uintptr_t spin = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress()) + kQueueFullSpinRva;
		if (!IsBadReadPtr(reinterpret_cast<const void*>(spin), sizeof(kQueueFullSpin)) &&
			std::memcmp(reinterpret_cast<const void*>(spin), kQueueFullSpin, sizeof(kQueueFullSpin)) == 0)
		{
			char rel = 0x00;   // jmp +0: falls through to the function exit at 0x0041D1E2
			WriteToProtectedMemory(spin + 4, &rel, 1);
			LOG(1, "[LobbyFixes] full incoming-message queue no longer freezes the game\n");
		}
		else
		{
			LOG(1, "[LobbyFixes] incoming-queue spin did not match; not patched\n");
		}

		int tusPatched = 0;
		for (const uintptr_t rva : { kTusDownloadWaitRva, kTusUploadWaitRva })
		{
			const uintptr_t cmp = gameBase + rva;
			if (!IsBadReadPtr(reinterpret_cast<const void*>(cmp), sizeof(kTusWaitCmp)) &&
				std::memcmp(reinterpret_cast<const void*>(cmp), kTusWaitCmp, sizeof(kTusWaitCmp)) == 0)
			{
				uint32_t ticks = kTusWaitTicks;
				WriteToProtectedMemory(cmp + 2, reinterpret_cast<char*>(&ticks), sizeof(ticks));
				++tusPatched;
			}
		}
		if (tusPatched == 2)
		{
			LOG(1, "[LobbyFixes] profile requests no longer re-sent while the first is running (11 s wait)\n");
		}
		else
		{
			LOG(1, "[LobbyFixes] profile request waits did not match (%d/2 patched)\n", tusPatched);
		}
	}

	void BeforeRequestLobbyList()
	{
		if (!Enabled())
			return;
		// The game lists full lobbies (no slots filter, Lobby_D bug 6): a ranked host already
		// in a match, a full room. Clicking one can only end in "failed to connect".
		ISteamMatchmaking* mm = RealMatchmaking();
		if (!mm)
			return;
		mm->AddRequestLobbyListFilterSlotsAvailable(1);
		LobbyTrace::OnListFilter("slots available >= 1 [LobbyFixes]");
	}

	bool DropIncoming(uint64_t remote, const void* data, uint32_t size, int channel)
	{
		if (channel != 0 || !data || size < 6)
			return false;
		const uint8_t* b = static_cast<const uint8_t*>(data);
		const uint16_t length = *reinterpret_cast<const uint16_t*>(b + 2);
		const uint16_t type = *reinterpret_cast<const uint16_t*>(b + 4);

		const HostOnlyMessage* message = nullptr;
		for (const HostOnlyMessage& m : kHostOnly)
		{
			if (m.type == type && m.length == length)
				message = &m;
		}
		if (!message)
			return false;
		const bool fromHost = LobbyTrace::IsOwnerOfJoinedLobby(remote);
		// The ranked list filter hides a host whose room is stuck (refused as full while its
		// lobby had a free slot); it needs the refusal whether or not the fixes are on.
		if (fromHost && type == 0x34 && size >= 8)
			RankedListConnectionFilter::GetInstance().OnHostRefusedJoin(remote, *reinterpret_cast<const uint16_t*>(b + 6));
		if (!Enabled())
			return false;
		if (fromHost)
		{
			// Ranked shows every refusal as "failed to connect". Reason 2/3/4 is the host's game
			// saying its room is full: in 2026-10-09 ranked, two hosts whose Steam lobby held
			// only themselves refused that way, again and again - their game still counted a
			// player who had left. Say so, so it does not read as a connection problem.
			if (type == 0x34 && size >= 8)
			{
				const uint16_t reason = *reinterpret_cast<const uint16_t*>(b + 6);
				const char* name = g_interfaces.pSteamFriendsWrapper
					? g_interfaces.pSteamFriendsWrapper->GetFriendPersonaName(CSteamID(remote)) : nullptr;
				if (!name || !*name)
					name = "The host";
				if (g_notificationBar && reason >= 2 && reason <= 4)
				{
					g_notificationBar->AddNotification("Could not join: %s's game says its room is full. If their room "
						"showed a free slot, their game is stuck holding a player who left; try someone else.", name);
				}
				else if (g_notificationBar && reason == 1)
				{
					g_notificationBar->AddNotification("Could not join: %s is no longer hosting.", name);
				}
			}
			return false;
		}

		static std::map<uint64_t, ULONGLONG> s_lastLog;
		const ULONGLONG now = GetTickCount64();
		ULONGLONG& last = s_lastLog[(remote << 8) ^ type];
		if (!last || now - last > 3000)
		{
			LOG(1, "[LobbyFixes] ignored \"%s\" (0x%X) from %llu: not the host of any lobby we are in\n",
				message->name, type, static_cast<unsigned long long>(remote));
			last = now;
		}
		return true;
	}

	bool PeekInjected(uint32_t* size, int channel)
	{
		if (channel != 0 || g_injected.empty())
			return false;
		if (size)
			*size = sizeof(g_injected.front().bytes);
		return true;
	}

	bool TakeInjected(void* dest, uint32_t capacity, uint32_t* size, uint64_t* remote, int channel)
	{
		if (channel != 0 || g_injected.empty() || !dest || capacity < sizeof(g_injected.front().bytes))
			return false;
		const Injected m = g_injected.front();
		g_injected.pop_front();
		std::memcpy(dest, m.bytes, sizeof(m.bytes));
		if (size)
			*size = sizeof(m.bytes);
		if (remote)
			*remote = m.from;
		return true;
	}

	void OnSteamMemberLeft(uint64_t lobby, uint64_t user, uint64_t owner, const char* how)
	{
		if (!Enabled() || !user)
			return;
		const uint64_t self = SelfId();
		if (!self || user == self)
			return;
		bool peerWasHost;
		if (owner == self)
			peerWasHost = false;
		else if (owner == user)
			peerWasHost = true;
		else
			return;   // another guest leaving a room we are a guest in: the host handles it
		for (const MissedLeave& m : g_missedLeaves)
		{
			if (m.peer == user)
				return;
		}
		MissedLeave m;
		m.peer = user;
		m.peerWasHost = peerWasHost;
		m.seenMs = GetTickCount64();
		m.how = how ? how : "left";
		g_missedLeaves.push_back(m);
		(void)lobby;
	}

	void OnJoinFailed(uint32_t enterResponse)
	{
		if (!Enabled())
			return;
		const char* why = JoinFailureText(enterResponse);
		if (why && g_notificationBar)
			g_notificationBar->AddNotification("Could not join: %s.", why);
	}

	void Tick()
	{
		if (!Enabled())
			return;
		RetryLostPings();
		RateResultsPastTwenty();
		if (g_missedLeaves.empty())
			return;
		const ULONGLONG now = GetTickCount64();
		for (size_t i = 0; i < g_missedLeaves.size();)
		{
			if (ResolveMissedLeave(g_missedLeaves[i], now))
				g_missedLeaves.erase(g_missedLeaves.begin() + i);
			else
				++i;
		}
	}
}
