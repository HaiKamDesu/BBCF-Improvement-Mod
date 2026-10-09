#include "DesyncDiagnostics.h"

#include "Audio/MusicManager.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"
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

	Session g_session;
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
	void Tick()
	{
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
