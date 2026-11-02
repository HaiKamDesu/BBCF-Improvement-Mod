#include "DesyncDiagnostics.h"

#include "Audio/MusicManager.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Game/GhidraDefs.h"
#include "Network/RoomManager.h"

#include <Windows.h>
#include <cstdint>
#include <string>

namespace
{
	// RollbackSessionManager* (DAT_0182A920). Null outside an online battle.
	constexpr uintptr_t ROLLBACK_MGR_PTR_RVA = 0x0142A920;
	constexpr uintptr_t MGR_CHECKER = 0x08;          // desync checker, 0x220 bytes (0x783A00)
	constexpr uintptr_t MGR_DISCONNECTED = 0x112;    // disconnected / host inputs lost
	constexpr uintptr_t CHECKER_LOGGED = 0x190;      // mismatches recorded below, max 10
	constexpr uintptr_t CHECKER_LOG = 0x194;         // { frame, local, remote } x 10
	constexpr uintptr_t CHECKER_IN_A_ROW = 0x210;    // reset by a matching check (0x783AD0)
	constexpr int kMaxLogged = 10;

	struct CheckerView
	{
		uintptr_t checker;
		int logged;
		int inARow;
		bool disconnected;
		int frame[kMaxLogged];
		uint32_t local[kMaxLogged];
		uint32_t remote[kMaxLogged];
	};

	struct Session
	{
		uintptr_t checker = 0;
		int frames = 0;
		int logged = 0;
		int inARow = 0;
		int mismatches = 0;
		int worstInARow = 0;
		bool disconnected = false;
		int musicChanges = 0;
	};

	// The GGPO backend the wrapper points at (+0). StartSession (0x783750) builds a
	// Peer2PeerBackend and stores the local player's handle (1 = P1, 2 = P2) at wrapper+0x28;
	// StartSpectating (0x783940) builds a SpectatorBackend whose one endpoint is the host (P1).
	constexpr uintptr_t SPECTATOR_BACKEND_VTABLE_RVA = 0x00586098; // VA 0x00986098
	constexpr uintptr_t MGR_LOCAL_HANDLE = 0x28;
	constexpr uintptr_t SPECTATOR_HOST_ENDPOINT = 0x548;
	constexpr uintptr_t EP_PEER_STEAM_ID = 0x08;

	enum GameSeatKind { GameSeat_None, GameSeat_Player, GameSeat_Spectator, GameSeat_Unknown };

	struct GameSeatView
	{
		uintptr_t backend;
		int kind;
		int handle;      // players: 1 or 2
		uint64_t peer;   // players: the remote player; spectators: the host
	};

	struct MatchInitRole
	{
		bool valid = false;
		bool spectator = false;
		int index = -1;
	};

	Session g_session;
	MatchInitRole g_matchInitRole;
	uintptr_t g_seatCheckedBackend = 0;
	std::string g_lastMusicChange;
	int g_lastMusicChangeFrame = -1;

	// No C++ objects in here: __try cannot unwind them.
	bool ReadChecker(CheckerView* out)
	{
		out->checker = 0;
		__try
		{
			const uintptr_t base = (uintptr_t)GetBbcfBaseAdress();
			const uintptr_t mgr = *(uintptr_t*)(base + ROLLBACK_MGR_PTR_RVA);
			if (!mgr)
				return false;
			const uintptr_t checker = *(uintptr_t*)(mgr + MGR_CHECKER);
			if (!checker)
				return false;

			out->checker = checker;
			out->disconnected = *(uint8_t*)(mgr + MGR_DISCONNECTED) != 0;
			out->inARow = *(int*)(checker + CHECKER_IN_A_ROW);
			int logged = *(int*)(checker + CHECKER_LOGGED);
			if (logged < 0) logged = 0;
			if (logged > kMaxLogged) logged = kMaxLogged;
			out->logged = logged;
			for (int i = 0; i < logged; i++)
			{
				const uintptr_t entry = checker + CHECKER_LOG + (uintptr_t)i * 0xC;
				out->frame[i] = *(int*)(entry + 0);
				out->local[i] = *(uint32_t*)(entry + 4);
				out->remote[i] = *(uint32_t*)(entry + 8);
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// No C++ objects in here: __try cannot unwind them.
	bool ReadGameSeat(GameSeatView* out)
	{
		out->backend = 0;
		out->kind = GameSeat_None;
		out->handle = 0;
		out->peer = 0;
		__try
		{
			const uintptr_t base = (uintptr_t)GetBbcfBaseAdress();
			const uintptr_t mgr = *(uintptr_t*)(base + ROLLBACK_MGR_PTR_RVA);
			if (!mgr)
				return false;
			const uintptr_t backend = *(uintptr_t*)(mgr + OFF_GgpoMgrBackend);
			if (!backend)
				return false;

			out->backend = backend;
			const uintptr_t vtable = *(uintptr_t*)backend;
			if (vtable == base + ADDR_GgpoPeer2PeerBackendVtable)
			{
				out->kind = GameSeat_Player;
				out->handle = *(int*)(mgr + MGR_LOCAL_HANDLE);
				const int count = *(int*)(backend + OFF_GgpoBackendNumEndpoints);
				const uintptr_t endpoints = *(uintptr_t*)(backend + OFF_GgpoBackendEndpoints);
				if (endpoints && count > 0 && count <= 8)
				{
					// The local player's endpoint is never initialised (no udp).
					for (int i = 0; i < count; i++)
					{
						const uintptr_t ep = endpoints + (uintptr_t)i * SIZE_GgpoEndpoint;
						if (*(uintptr_t*)(ep + OFF_GgpoEpUdp) != 0)
						{
							out->peer = *(uint64_t*)(ep + EP_PEER_STEAM_ID);
							break;
						}
					}
				}
			}
			else if (vtable == base + SPECTATOR_BACKEND_VTABLE_RVA)
			{
				out->kind = GameSeat_Spectator;
				out->peer = *(uint64_t*)(backend + SPECTATOR_HOST_ENDPOINT + EP_PEER_STEAM_ID);
			}
			else
			{
				out->kind = GameSeat_Unknown;
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	const char* RoleName(bool spectator, int index)
	{
		if (spectator)
			return "spectator";
		return index == 0 ? "P1" : (index == 1 ? "P2" : "P?");
	}

	// The seated member of our match on the given side, or 0.
	uint64_t FindSeatedMember(RoomManager* room, int side)
	{
		const RoomMemberEntry* self = room->GetThisPlayerRoomMemberEntry();
		if (!self)
			return 0;
		for (uint16_t i = 0; i < MAX_PLAYERS_IN_ROOM; i++)
		{
			const RoomMemberEntry* entry = room->GetRoomMemberEntryByIndex(i);
			if (!entry || entry->steamId == 0 || entry->matchId != self->matchId)
				continue;
			if (RoomManager::IsSeatedEntry(entry) && (int)(entry->seatFlags & 1) == side)
				return entry->steamId;
		}
		return 0;
	}

	void CheckSeat(const GameSeatView& game)
	{
		RoomManager* room = g_interfaces.pRoomManager;
		if (!room || !room->IsRoomFunctional())
			return;

		const RoomMemberEntry* self = room->GetThisPlayerRoomMemberEntry();
		RoomType roomType = (RoomType)0;
		room->TryGetRoomType(&roomType);
		const bool nowSpectator = room->IsThisPlayerSpectator();
		const int nowIndex = nowSpectator ? -1 : (int)room->GetThisPlayerMatchPlayerIndex();

		if (game.kind == GameSeat_Unknown)
		{
			LOG(0, "[SeatCheck] unknown GGPO backend class, cannot compare (room type 0x%02X)\n", (unsigned)roomType);
			return;
		}

		const bool gameSpectator = game.kind == GameSeat_Spectator;
		const int gameIndex = gameSpectator ? -1 : game.handle - 1;

		// Players: the remote peer must be the seated member on the other side.
		// Spectators: the host endpoint must be the seated P1 (only P1 adds spectators).
		const uint64_t expectedPeer = gameSpectator ? FindSeatedMember(room, 0)
			: (nowSpectator ? 0 : FindSeatedMember(room, nowIndex == 0 ? 1 : 0));

		bool agree = nowSpectator == gameSpectator && nowIndex == gameIndex && expectedPeer == game.peer;
		if (g_matchInitRole.valid)
			agree = agree && g_matchInitRole.spectator == gameSpectator &&
				(gameSpectator || g_matchInitRole.index == gameIndex);

		const int level = agree ? 1 : 0;
		LOG(level, "[SeatCheck] %s: game=%s peer=%llu | ours at match init=%s | ours now=%s expected peer=%llu"
			" | room type 0x%02X seat %d flags 0x%X matchPlayerIndex %d\n",
			agree ? "AGREE" : "MISMATCH",
			RoleName(gameSpectator, gameIndex), (unsigned long long)game.peer,
			g_matchInitRole.valid ? RoleName(g_matchInitRole.spectator, g_matchInitRole.index) : "not recorded",
			RoleName(nowSpectator, nowIndex), (unsigned long long)expectedPeer,
			(unsigned)roomType, self ? (int)self->memberIndex : -1, self ? self->seatFlags : 0u,
			self ? (int)self->matchPlayerIndex : -1);
	}

	std::string DescribePeers()
	{
		RoomManager* room = g_interfaces.pRoomManager;
		if (!room || !room->IsRoomFunctional())
			return "room=none";

		std::string out = room->IsThisPlayerSpectator() ? "role=spectator" : "role=player";
		out += " matchIndex=" + std::to_string((int)room->GetThisPlayerMatchPlayerIndex());

		// Only peers on the same IM packet version announce themselves, so "IM" here means
		// "IM with a compatible build"; anyone else (vanilla, or an IM too old/new to talk to
		// us) shows as not IM.
		const std::vector<IMPlayer> peers = room->GetIMPlayersInCurrentMatchNonSpec();
		out += " opponentIM=" + std::string(peers.empty() ? "no" : "yes");
		for (const IMPlayer& p : peers)
			out += " ['" + p.steamName + "' idx=" + std::to_string(p.matchPlayerIndex) + "]";
		return out;
	}

	void LogLastMusicChange()
	{
		if (g_lastMusicChange.empty())
		{
			LOG(1, "[Desync]   no music change this session\n");
			return;
		}
		LOG(1, "[Desync]   last music change at session frame %d: %s\n",
			g_lastMusicChangeFrame, g_lastMusicChange.c_str());
	}

	void EndSession(const char* why)
	{
		LOG(1, "[Desync] GGPO session ended (%s) after %d frames: mismatches=%d worstInARow=%d "
			"disconnected=%d musicChanges=%d\n",
			why, g_session.frames, g_session.mismatches, g_session.worstInARow,
			g_session.disconnected ? 1 : 0, g_session.musicChanges);
		g_session = Session();
		g_lastMusicChange.clear();
		g_lastMusicChangeFrame = -1;
	}
}

namespace DesyncDiagnostics
{
	void NoteMatchInitRole(bool spectator, int matchPlayerIndex)
	{
		g_matchInitRole.valid = true;
		g_matchInitRole.spectator = spectator;
		g_matchInitRole.index = matchPlayerIndex;
	}

	void Tick()
	{
		// Once per GGPO session, independent of the checker (a spectator may have none).
		GameSeatView seat;
		if (!ReadGameSeat(&seat))
		{
			g_seatCheckedBackend = 0;
		}
		else if (seat.backend != g_seatCheckedBackend)
		{
			g_seatCheckedBackend = seat.backend;
			CheckSeat(seat);
			g_matchInitRole = MatchInitRole();
		}

		CheckerView view;
		if (!ReadChecker(&view))
		{
			if (g_session.checker)
				EndSession("session closed");
			return;
		}

		if (view.checker != g_session.checker)
		{
			if (g_session.checker)
				EndSession("replaced by a new session");

			g_session.checker = view.checker;
			const std::string peers = DescribePeers();
			const std::string jukebox = GetMusicManager().DescribeForDiagnostics();
			LOG(1, "[Desync] GGPO session started (checker=%08X): %s\n",
				(unsigned)view.checker, peers.c_str());
			LOG(1, "[Desync]   jukebox: %s\n", jukebox.c_str());
		}

		g_session.frames++;

		for (int i = g_session.logged; i < view.logged; i++)
		{
			LOG(1, "[Desync] checksum MISMATCH at GGPO frame %d: local=%08X remote=%08X "
				"(inARow=%d, session frame %d)\n",
				view.frame[i], view.local[i], view.remote[i], view.inARow, g_session.frames);
			LogLastMusicChange();
		}
		g_session.logged = view.logged;

		if (view.inARow != g_session.inARow)
		{
			if (view.inARow > g_session.inARow)
			{
				g_session.mismatches += view.inARow - g_session.inARow;
				// Past the 10 recorded entries the checker stops recording, so the count is
				// the only trace left.
				if (view.logged >= kMaxLogged)
				{
					LOG(1, "[Desync] checksum mismatch (unrecorded, inARow=%d, session frame %d)\n",
						view.inARow, g_session.frames);
				}
				if (g_session.inARow <= 1 && view.inARow > 1)
				{
					LOG(1, "[Desync] DESYNC DETECTED overlay is now showing (inARow=%d, session frame %d)\n",
						view.inARow, g_session.frames);
					LogLastMusicChange();
				}
				if (g_session.inARow <= 5 && view.inARow > 5)
				{
					LOG(1, "[Desync] more than 5 mismatches in a row: the game drops the match now "
						"(session frame %d)\n", g_session.frames);
					LogLastMusicChange();
				}
			}
			else if (view.inARow == 0)
			{
				LOG(1, "[Desync] checksums match again after %d mismatch(es) in a row (session frame %d)\n",
					g_session.inARow, g_session.frames);
			}
			g_session.inARow = view.inARow;
			if (view.inARow > g_session.worstInARow)
				g_session.worstInARow = view.inARow;
		}

		if (view.disconnected && !g_session.disconnected)
		{
			LOG(1, "[Desync] GGPO reports the peer disconnected (session frame %d, inARow=%d)\n",
				g_session.frames, view.inARow);
		}
		g_session.disconnected = view.disconnected;
	}

	int GetSessionFrame()
	{
		return g_session.checker ? g_session.frames : -1;
	}

	void NoteMusicChange(const char* description)
	{
		if (!g_session.checker)
			return;
		g_session.musicChanges++;
		g_lastMusicChange = description ? description : "";
		g_lastMusicChangeFrame = g_session.frames;
	}
}
