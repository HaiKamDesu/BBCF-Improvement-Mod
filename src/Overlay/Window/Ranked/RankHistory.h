#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Every ranked match played with the mod, kept in BBCF_IM\RankHistory.tsv (one line per match,
// append-only, so a crash can lose at most the match in progress).
//
// How a match is recorded: at match init the local character, the opponent and the character's
// ranked row (rank, LP, wins, matches) are captured. The row is then re-read every 250 ms, and the
// moment its match count goes up the difference is the entry: wins going up = a win, matches
// going up without wins = a loss, LP and rank before/after straight from the row. Player rooms never
// produce an entry, because only ranked matches change the ranked row and upload a ranked score.
namespace RankHistory
{
	struct Entry
	{
		int64_t unixTime = 0;          // UTC seconds, when the match ended
		uint64_t localSteamId = 0;
		uint32_t characterId = 0;
		uint64_t opponentSteamId = 0;
		std::string opponentName;
		int opponentCharacterId = -1;  // -1 = unknown
		int opponentVisibleRank = -1;  // -1 = unknown
		char result = '?';             // 'W', 'L' or '?'
		uint32_t lpBefore = 0;
		uint32_t lpAfter = 0;
		uint32_t rankBefore = 0;       // visible rank (FormatVisibleRankLabel)
		uint32_t rankAfter = 0;
		bool unrankedBefore = false;
		bool unrankedAfter = false;

		int32_t LpDelta() const { return static_cast<int32_t>(lpAfter) - static_cast<int32_t>(lpBefore); }
		bool RankedUp() const { return (unrankedBefore && !unrankedAfter) || (!unrankedAfter && rankAfter > rankBefore); }
		bool RankedDown() const { return !unrankedBefore && !unrankedAfter && rankAfter < rankBefore; }
	};

	// Game-thread hooks.
	void OnMatchInit();
	void OnUpdate();
	void OnRankedUploadCompleted(uint32_t characterId);

	// Oldest first. Loaded from disk on first use.
	const std::vector<Entry>& GetEntries();
	// Bumped whenever an entry is added, so a window can cache what it derives from the list.
	unsigned GetRevision();
	uint64_t GetLocalSteamId();
	// How many recorded matches belong to the Steam account playing now (or to no account).
	size_t CountForLocalAccount();
	// Deletes this account's matches, from memory and from the file. Other Steam accounts on the
	// same PC keep theirs. Returns false if the file could not be rewritten (memory is still cleared).
	bool ClearLocalAccount();
}
