#include "RankHistory.h"

#include "RankedProgressWindow.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Game/CharData.h"
#include "Game/Room/RoomMemberEntry.h"
#include "Network/RoomManager.h"
#include "SteamApiWrapper/SteamFriendsWrapper.h"
#include "SteamApiWrapper/SteamMatchmakingWrapper.h"
#include "SteamApiWrapper/SteamUserWrapper.h"

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace RankHistory
{
	namespace
	{
		const char* const kHistoryFile = "BBCF_IM\\RankHistory.tsv";
		const char* const kHeader =
			"#RankHistory v1\ttime\tlocalSteamId\tcharacter\topponentSteamId\topponentName\topponentCharacter"
			"\topponentRank\tresult\tlpBefore\tlpAfter\trankBefore\trankAfter\tunrankedBefore\tunrankedAfter";
		// How long to wait after the upload for the game's ranked row to show the match before
		// recording from the LP change alone.
		const ULONGLONG kRowSettleTimeoutMs = 8000;

		std::vector<Entry> g_entries;
		bool g_loaded = false;
		unsigned g_revision = 0;

		// The match being played: what it looked like before, and who it is against.
		struct PendingMatch
		{
			bool active = false;
			Entry context;                 // character + opponent filled in, result/after not yet
			RankedCharacterRecord before;
			bool finalizeRequested = false;
			ULONGLONG finalizeRequestTickMs = 0;
		};
		PendingMatch g_pending;

		std::string Sanitize(const std::string& text)
		{
			std::string out = text;
			for (char& c : out)
			{
				if (c == '\t' || c == '\n' || c == '\r')
				{
					c = ' ';
				}
			}
			return out;
		}

		std::vector<std::string> SplitTabs(const std::string& line)
		{
			std::vector<std::string> fields;
			size_t start = 0;
			while (true)
			{
				const size_t tab = line.find('\t', start);
				fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
				if (tab == std::string::npos)
				{
					break;
				}
				start = tab + 1;
			}
			return fields;
		}

		void EnsureLoaded()
		{
			if (g_loaded)
			{
				return;
			}
			g_loaded = true;

			FILE* file = nullptr;
			if (fopen_s(&file, kHistoryFile, "rb") != 0 || file == nullptr)
			{
				return;
			}

			char buffer[2048];
			int lineNumber = 0;
			while (fgets(buffer, sizeof(buffer), file) != nullptr)
			{
				++lineNumber;
				std::string line(buffer);
				while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
				{
					line.pop_back();
				}
				if (line.empty() || line[0] == '#')
				{
					continue;
				}

				const std::vector<std::string> f = SplitTabs(line);
				if (f.size() < 14)
				{
					LOG(1, "[RankHistory] skipped malformed line %d (%zu fields)\n", lineNumber, f.size());
					continue;
				}

				Entry entry;
				entry.unixTime = _strtoi64(f[0].c_str(), nullptr, 10);
				entry.localSteamId = _strtoui64(f[1].c_str(), nullptr, 10);
				entry.characterId = static_cast<uint32_t>(std::strtoul(f[2].c_str(), nullptr, 10));
				entry.opponentSteamId = _strtoui64(f[3].c_str(), nullptr, 10);
				entry.opponentName = f[4];
				entry.opponentCharacterId = std::atoi(f[5].c_str());
				entry.opponentVisibleRank = std::atoi(f[6].c_str());
				entry.result = f[7].empty() ? '?' : f[7][0];
				entry.lpBefore = static_cast<uint32_t>(std::strtoul(f[8].c_str(), nullptr, 10));
				entry.lpAfter = static_cast<uint32_t>(std::strtoul(f[9].c_str(), nullptr, 10));
				entry.rankBefore = static_cast<uint32_t>(std::strtoul(f[10].c_str(), nullptr, 10));
				entry.rankAfter = static_cast<uint32_t>(std::strtoul(f[11].c_str(), nullptr, 10));
				entry.unrankedBefore = f[12] == "1";
				entry.unrankedAfter = f[13] == "1";
				g_entries.push_back(entry);
			}
			fclose(file);

			LOG(1, "[RankHistory] loaded %zu ranked match(es) from %s\n", g_entries.size(), kHistoryFile);
		}

		void WriteLine(FILE* file, const Entry& entry);

		void Append(const Entry& entry)
		{
			FILE* file = nullptr;
			bool writeHeader = false;
			{
				FILE* probe = nullptr;
				if (fopen_s(&probe, kHistoryFile, "rb") != 0 || probe == nullptr)
				{
					writeHeader = true;
				}
				else
				{
					fclose(probe);
				}
			}

			if (fopen_s(&file, kHistoryFile, "ab") != 0 || file == nullptr)
			{
				LOG(1, "[RankHistory] could not open %s for writing; the match is kept for this session only\n", kHistoryFile);
				return;
			}

			if (writeHeader)
			{
				fprintf(file, "%s\n", kHeader);
			}
			WriteLine(file, entry);
			fclose(file);
		}

		void WriteLine(FILE* file, const Entry& entry)
		{
			fprintf(file, "%lld\t%llu\t%u\t%llu\t%s\t%d\t%d\t%c\t%u\t%u\t%u\t%u\t%d\t%d\n",
				static_cast<long long>(entry.unixTime),
				static_cast<unsigned long long>(entry.localSteamId),
				entry.characterId,
				static_cast<unsigned long long>(entry.opponentSteamId),
				Sanitize(entry.opponentName).c_str(),
				entry.opponentCharacterId,
				entry.opponentVisibleRank,
				entry.result,
				entry.lpBefore,
				entry.lpAfter,
				entry.rankBefore,
				entry.rankAfter,
				entry.unrankedBefore ? 1 : 0,
				entry.unrankedAfter ? 1 : 0);
		}

		bool BelongsToLocalAccount(const Entry& entry, uint64_t localSteamId)
		{
			return localSteamId == 0 || entry.localSteamId == 0 || entry.localSteamId == localSteamId;
		}

		int ReadCharacterIndex(uint16_t matchPlayerIndex)
		{
			Player& player = matchPlayerIndex == 0 ? g_interfaces.player1 : g_interfaces.player2;
			if (player.IsCharDataNullPtr())
			{
				return -1;
			}
			const CharData* const data = player.GetData();
			if (data == nullptr || IsBadReadPtr(data, sizeof(CharData)) || data->charIndex < 0 || data->charIndex >= 64)
			{
				return -1;
			}
			return data->charIndex;
		}

		std::string ResolveName(uint64_t steamId)
		{
			if (steamId == 0 || g_interfaces.pSteamFriendsWrapper == nullptr)
			{
				return std::string();
			}
			const char* const name = g_interfaces.pSteamFriendsWrapper->GetFriendPersonaName(CSteamID(steamId));
			return name != nullptr ? std::string(name) : std::string();
		}

		void Finalize(const RankedCharacterRecord& after, bool rowShowsMatch)
		{
			Entry entry = g_pending.context;
			entry.unixTime = static_cast<int64_t>(std::time(nullptr));
			entry.lpBefore = g_pending.before.totalLp;
			entry.rankBefore = g_pending.before.visibleRank;
			entry.unrankedBefore = g_pending.before.isUnranked;
			entry.lpAfter = after.totalLp;
			entry.rankAfter = after.visibleRank;
			entry.unrankedAfter = after.isUnranked;

			if (rowShowsMatch)
			{
				entry.result = after.wins > g_pending.before.wins ? 'W' : 'L';
			}
			else
			{
				// The row never showed the match: judge by LP alone, which cannot tell a loss
				// that cost nothing from a draw, hence '?'.
				const int32_t delta = entry.LpDelta();
				entry.result = delta > 0 ? 'W' : (delta < 0 ? 'L' : '?');
			}

			g_pending = PendingMatch();

			EnsureLoaded();
			g_entries.push_back(entry);
			++g_revision;
			Append(entry);

			LOG(1, "[RankHistory] recorded %c vs %s (%llu): char=%u LP %u -> %u (%+d) rank %u -> %u%s\n",
				entry.result, entry.opponentName.empty() ? "?" : entry.opponentName.c_str(),
				static_cast<unsigned long long>(entry.opponentSteamId), entry.characterId,
				entry.lpBefore, entry.lpAfter, entry.LpDelta(), entry.rankBefore, entry.rankAfter,
				rowShowsMatch ? "" : " (from LP only: the ranked row had not updated)");
		}
	}

	void OnMatchInit()
	{
		RoomManager* const room = g_interfaces.pRoomManager;
		if (room == nullptr || !room->IsRoomFunctional() || room->IsThisPlayerSpectator())
		{
			g_pending = PendingMatch();
			return;
		}

		// A match whose upload never reached us: record it now if the row moved since.
		if (g_pending.active)
		{
			RankedCharacterRecord now;
			if (ReadRankedCharacterRecord(g_pending.context.characterId, &now) && now.matches != g_pending.before.matches)
			{
				Finalize(now, true);
			}
		}

		const uint16_t localIndex = room->GetThisPlayerMatchPlayerIndex();
		const int localCharacter = ReadCharacterIndex(localIndex);
		if (localCharacter < 0)
		{
			g_pending = PendingMatch();
			return;
		}

		PendingMatch pending;
		if (!ReadRankedCharacterRecord(static_cast<uint32_t>(localCharacter), &pending.before))
		{
			g_pending = PendingMatch();
			return;
		}

		pending.active = true;
		pending.context.localSteamId = GetLocalSteamId();
		pending.context.characterId = static_cast<uint32_t>(localCharacter);

		const std::vector<const RoomMemberEntry*> opponents = room->GetOtherRoomMemberEntriesInCurrentMatch();
		if (!opponents.empty() && opponents[0] != nullptr)
		{
			pending.context.opponentSteamId = opponents[0]->steamId;
			pending.context.opponentCharacterId = ReadCharacterIndex(opponents[0]->matchPlayerIndex);
			pending.context.opponentName = ResolveName(opponents[0]->steamId);

			uint32_t internalRank = 0;
			if (g_interfaces.pSteamMatchmakingWrapper != nullptr &&
				g_interfaces.pSteamMatchmakingWrapper->GetCachedRankedHostLevel(opponents[0]->steamId, &internalRank))
			{
				pending.context.opponentVisibleRank = static_cast<int>(InternalRankToVisibleRank(internalRank, false));
			}
		}

		g_pending = pending;
	}

	void OnRankedUploadCompleted(uint32_t characterId)
	{
		if (!g_pending.active || characterId != g_pending.context.characterId)
		{
			return;
		}
		g_pending.finalizeRequested = true;
		g_pending.finalizeRequestTickMs = GetTickCount64();
	}

	void OnUpdate()
	{
		if (!g_pending.active || !g_pending.finalizeRequested)
		{
			return;
		}

		RankedCharacterRecord after;
		if (!ReadRankedCharacterRecord(g_pending.context.characterId, &after))
		{
			return;
		}

		if (after.matches != g_pending.before.matches)
		{
			Finalize(after, true);
		}
		else if (GetTickCount64() - g_pending.finalizeRequestTickMs > kRowSettleTimeoutMs)
		{
			Finalize(after, false);
		}
	}

	const std::vector<Entry>& GetEntries()
	{
		EnsureLoaded();
		return g_entries;
	}

	unsigned GetRevision()
	{
		EnsureLoaded();
		return g_revision;
	}

	size_t CountForLocalAccount()
	{
		EnsureLoaded();
		const uint64_t localSteamId = GetLocalSteamId();
		size_t count = 0;
		for (const Entry& entry : g_entries)
		{
			if (BelongsToLocalAccount(entry, localSteamId))
			{
				++count;
			}
		}
		return count;
	}

	bool ClearLocalAccount()
	{
		EnsureLoaded();
		const uint64_t localSteamId = GetLocalSteamId();
		std::vector<Entry> kept;
		for (const Entry& entry : g_entries)
		{
			if (!BelongsToLocalAccount(entry, localSteamId))
			{
				kept.push_back(entry);
			}
		}
		const size_t removed = g_entries.size() - kept.size();
		g_entries.swap(kept);
		++g_revision;

		bool ok = true;
		if (g_entries.empty())
		{
			ok = (std::remove(kHistoryFile) == 0) || (GetFileAttributesA(kHistoryFile) == INVALID_FILE_ATTRIBUTES);
		}
		else
		{
			// Written to a temporary file first, so a failure cannot lose the other accounts' matches.
			const std::string temp = std::string(kHistoryFile) + ".tmp";
			FILE* file = nullptr;
			if (fopen_s(&file, temp.c_str(), "wb") == 0 && file != nullptr)
			{
				fprintf(file, "%s\n", kHeader);
				for (const Entry& entry : g_entries)
				{
					WriteLine(file, entry);
				}
				fclose(file);
				ok = MoveFileExA(temp.c_str(), kHistoryFile, MOVEFILE_REPLACE_EXISTING) != FALSE;
			}
			else
			{
				ok = false;
			}
		}

		LOG(1, "[RankHistory] cleared %zu match(es) of account %llu (%zu kept for other accounts)%s\n",
			removed, static_cast<unsigned long long>(localSteamId), g_entries.size(), ok ? "" : " - FILE NOT REWRITTEN");
		return ok;
	}

	uint64_t GetLocalSteamId()
	{
		if (g_interfaces.pSteamUserWrapper == nullptr)
		{
			return 0;
		}
		return g_interfaces.pSteamUserWrapper->GetSteamID().ConvertToUint64();
	}
}
