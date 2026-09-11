#pragma once

#include <cstdint>

/* The Steam leaderboard names BBCF itself uses for ranked.

   These are not invented by the mod: the game holds the 36 per-character names in one
   table in .rdata (file offset 0x49C14C, eight bytes apart, in character-id order), and
   the ranked screens call FindLeaderboard with exactly those strings. A code that does
   not match is not a wrong-looking name, it is a leaderboard that does not exist, and
   Steam answers "not found" - which is how twelve characters silently showed an empty
   board while the other twenty-four worked.

   Several codes are not the abbreviation you would guess (Noel is NO, not NL; Nine is
   PH for Phantom; Izayoi is IZ while Izanami is MI), so do not "correct" one from the
   character's name. Read it back out of BBCF.exe:

     strings -a -t x BBCF.exe | grep -E 'RANK_[A-Z]{2}$' | sort

   Both the leaderboard browser and the ranked progress window resolve names through
   here. They used to keep a copy each, and the copies were the same wrong table. */
namespace RankLeaderboard
{
	// Sentinel character id for the combined "RANK_ALL" board. Not a real character.
	constexpr uint32_t kAllCharacterId = 64u;

	constexpr int kNumCharacters = 36;

	// Two-letter code for a character id, or nullptr if the id names no board.
	inline const char* Code(uint32_t characterId)
	{
		static const char* const kCodes[kNumCharacters] =
		{
			"RG", "JN", "NO", "RC", "TK", "TG",
			"LC", "AR", "BN", "CA", "HA", "NY",
			"TB", "HZ", "MU", "MK", "VH", "PT",
			"RL", "IZ", "AM", "BL", "AZ", "KG",
			"KK", "TE", "CE", "RM", "HB", "PH",
			"NT", "MI", "SU", "ES", "MA", "JB",
		};

		if (characterId < kNumCharacters)
			return kCodes[characterId];
		if (characterId == kAllCharacterId)
			return "ALL";
		return nullptr;
	}
}
