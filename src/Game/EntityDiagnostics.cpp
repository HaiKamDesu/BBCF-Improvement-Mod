#include "EntityDiagnostics.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/Settings.h"
#include "Core/utils.h"
#include "Game/CharData.h"
#include "Game/characters.h"
#include "Game/gamestates.h"

#include <map>
#include <string>

namespace
{
	// Matches the hitbox overlay's own climb. Returns how many links away the owning player is, or
	// -1 when the chain never reaches one - which is the whole point of logging this.
	const int kMaxOwnerDepth = 16;

	// Bounded so a long session cannot turn DEBUG.txt into the unbounded log
	// FrameStallIncidents.log once became. A change trace costs more lines than a catalogue of
	// distinct situations did, so the allowance is larger.
	const size_t kMaxLoggedLines = 1500;

	// Last reported situation per entity slot. A line is written when a slot's situation changes,
	// which turns the log into an ordered timeline rather than an unordered set: the question
	// "does the staff ever reach its 6-hitbox swing frames" cannot be answered by a catalogue that
	// collapses repeats, because a situation that never occurs and one that occurs and is
	// collapsed look identical.
	std::map<int, std::string> g_lastBySlot;
	size_t g_linesLogged = 0;
	bool g_budgetExhaustedLogged = false;
	int g_lastP1CharIndex = -1;
	int g_lastP2CharIndex = -1;

	int OwnerDepth(CharData* entity)
	{
		if (!entity)
		{
			return -1;
		}

		CharData* owner = entity->ownerEntity;
		for (int depth = 0; owner && depth < kMaxOwnerDepth; depth++)
		{
			if (owner == (CharData*)g_gameVals.pEntityList[0] ||
			    owner == (CharData*)g_gameVals.pEntityList[1])
			{
				return depth;
			}

			if (owner->ownerEntity == owner)
			{
				break;
			}

			owner = owner->ownerEntity;
		}

		return -1;
	}

	// The current sprite name is packed into an int64, so it carries at most 8 characters -
	// 'rod202_03' arrives as 'rod202_0'. Enough to identify the animation alongside the box counts.
	std::string SpriteName(int64_t packed)
	{
		char buf[9] = {};
		memcpy(buf, &packed, 8);
		for (int i = 0; i < 8; i++)
		{
			if (buf[i] != '\0' && (buf[i] < 0x20 || buf[i] > 0x7E))
			{
				buf[i] = '?';
			}
		}
		return std::string(buf);
	}


	// The game's own "how many active hitboxes does this entity have right now" routine, at
	// BBCF.exe+0x18C3E0. Found by tracing every access to the state-property bitfield at
	// CharData+0x25C: this is the only place the engine reads the 0x200/0x400 suppression bits,
	// and the hit-detection sweep at +0x1594DD calls it to decide which entities are worth
	// testing for collisions. It is therefore ground truth for "can these boxes hit".
	//
	// The overlay's own test looks at one field. This one reads six:
	//
	//   [0x4BC]/[0x150]  an exemption pair checked first
	//   [0x480],[0x484]  the active box range; the return value is end - start
	//   [0x254] & 0x100  attack-enabled, a positively set bit the overlay has never looked at
	//   [0x25C] & 0x4000600   the suppression bits (0x200 | 0x400 | 0x4000000)
	//   [0x25C] & 0x80
	//   [0x488] == 5     the Astral special case
	//
	// It is a __thiscall taking only the entity, reads no globals, takes no locks and writes
	// nothing, which is what makes it safe enough to call from the render thread for a
	// diagnostic. It is still gated behind a setting that defaults to off.
	typedef int(__thiscall* GetActiveHitboxCountFn)(void* entity);

	const uintptr_t kGetActiveHitboxCountRva = 0x0018C3E0;

	// push esi / mov esi,ecx / cmp byte ptr [esi+4BCh],0 / je +0Ch. Verified against the shipped
	// executable. If the bytes ever differ we are not looking at the function we think we are, so
	// the probe disables itself rather than calling into the middle of something else.
	const unsigned char kGetActiveHitboxCountSig[] = {
		0x56, 0x8B, 0xF1, 0x80, 0xBE, 0xBC, 0x04, 0x00, 0x00, 0x00, 0x74, 0x0C
	};

	GetActiveHitboxCountFn g_getActiveHitboxCount = nullptr;
	bool g_probeResolved = false;

	GetActiveHitboxCountFn ResolveProbe()
	{
		if (g_probeResolved)
		{
			return g_getActiveHitboxCount;
		}
		g_probeResolved = true;

		char* base = GetBbcfBaseAdress();
		if (!base)
		{
			return nullptr;
		}

		unsigned char* target = (unsigned char*)(base + kGetActiveHitboxCountRva);
		if (memcmp(target, kGetActiveHitboxCountSig, sizeof(kGetActiveHitboxCountSig)) != 0)
		{
			LOG(0, "[EntityProbe] byte signature mismatch at base+0x%08X; probe disabled. "
			       "Expected 56 8B F1 80 BE BC 04 00 00 00 74 0C, found %02X %02X %02X %02X %02X "
			       "%02X %02X %02X %02X %02X %02X %02X\n",
				(unsigned int)kGetActiveHitboxCountRva,
				target[0], target[1], target[2],  target[3],  target[4],  target[5],
				target[6], target[7], target[8],  target[9],  target[10], target[11]);
			return nullptr;
		}

		g_getActiveHitboxCount = (GetActiveHitboxCountFn)target;
		LOG(1, "[EntityProbe] resolved the game's active-hitbox routine at base+0x%08X "
		       "(signature verified).\n", (unsigned int)kGetActiveHitboxCountRva);
		return g_getActiveHitboxCount;
	}

	// Truncated copy of a fixed-width, not necessarily NUL-terminated field in game memory.
	std::string SafeFixedString(const char* field, size_t maxLen)
	{
		size_t len = 0;
		while (len < maxLen && field[len] != '\0')
		{
			len++;
		}
		return std::string(field, len);
	}
}

void EntityDiagnostics::Update()
{
	if (!IsLoggingEnabled() || !g_gameVals.pEntityList || !g_gameVals.pGameState)
	{
		return;
	}

	if (*g_gameVals.pGameState != GameState_InMatch)
	{
		return;
	}

	CharData* p1 = (CharData*)g_gameVals.pEntityList[0];
	CharData* p2 = (CharData*)g_gameVals.pEntityList[1];
	if (!p1 || !p2)
	{
		return;
	}

	// New matchup: start a fresh catalogue so the log reads as one block per matchup instead of
	// carrying suppression across character changes and appearing to lose entities.
	if (p1->charIndex != g_lastP1CharIndex || p2->charIndex != g_lastP2CharIndex)
	{
		g_lastP1CharIndex = p1->charIndex;
		g_lastP2CharIndex = p2->charIndex;
		g_lastBySlot.clear();
		g_linesLogged = 0;
		g_budgetExhaustedLogged = false;

		LOG(2, "[Entities] Matchup P1 %s (%d) vs P2 %s (%d). Tracing entity changes; a line is "
		       "written whenever a slot's sprite, boxes, flags or action change.\n"
		       "[Entities] columns: slot, entity, owner, depth to player (- = overlay cannot reach "
		       "it), status, hurt/hit boxes, state flags, live hitbox (what the overlay believes), "
		       "engine (the game's own active hitbox count, -1 = probe off), [254] attack-enabled "
		       "word, active box range, verdict, actionTime, sprite, action\n",
			getCharacterNameByIndexA(p1->charIndex).c_str(), p1->charIndex,
			getCharacterNameByIndexA(p2->charIndex).c_str(), p2->charIndex);
	}

	if (g_linesLogged >= kMaxLoggedLines)
	{
		if (!g_budgetExhaustedLogged)
		{
			g_budgetExhaustedLogged = true;
			LOG(2, "[Entities] %zu lines logged this matchup; suppressing the rest to keep "
			       "DEBUG.txt bounded.\n", kMaxLoggedLines);
		}
		return;
	}

	// Resolved once, and only when asked for - this calls into game code from the render thread,
	// so it stays off unless a developer has deliberately turned it on.
	GetActiveHitboxCountFn probe = Settings::settingsIni.entityHitboxProbeEnabled
		? ResolveProbe()
		: nullptr;

	for (int i = 0; i < g_gameVals.entityCount; i++)
	{
		CharData* entity = (CharData*)g_gameVals.pEntityList[i];
		if (!entity)
		{
			continue;
		}

		const bool isCharacter = i < 2;
		const bool isActive = entity->unknownStatus1 == 1 && entity->pJonbEntryBegin != nullptr;

		// Slots the game is not currently using carry stale data; reporting them would bury the
		// entities that matter.
		if (!isCharacter && !isActive)
		{
			continue;
		}

		const int depth = OwnerDepth(entity);
		const std::string action = SafeFixedString(entity->currentAction, 20);

		// Whether the boxes are actually live, by the same test frame history now uses to decide
		// that a puppet's owner is attacking. An entity keeps its sprite's hitbox rectangles while
		// the script has switched attacking off - Litchi's staff idles in RodNeutral holding 20 of
		// them - so the flags are logged next to the count rather than the count being trusted.
		const unsigned int stateFlags = entity->bitflags_for_curr_state_properties_or_smth;
		const bool liveHitbox = entity->hitboxCount > 0 && (stateFlags & (0x400 | 0x200)) == 0;

		// Ground truth, straight from the engine, plus the fields it decides on. 'engine' is the
		// active hitbox count the game itself would use; 'live' above is what the overlay believes.
		// Where the two disagree is the whole question.
		int engineCount = -1;
		unsigned int attackEnabled = 0;
		int boxRangeStart = 0;
		int boxRangeEnd = 0;

		// The shipped collision file for lc202_09 declares 5 hurtboxes and 1 hitbox. The mod reads
		// the hurtbox count correctly and the hitbox count as 0, so the pair is not simply
		// misaligned - something else is going on. These five raw words bracket the counts so the
		// real field can be identified by matching it against the collision file, and [0x224] is
		// the handle the engine's own box enumerator consults as a SECOND source of boxes
		// (0058BBD0 sums the entity's own collection with that one), which is the leading
		// candidate for where the missing hitbox actually lives.
		unsigned int rawWords[5] = {};
		unsigned int secondCollection = 0;
		if (probe)
		{
			engineCount = probe(entity);
			const char* raw = (const char*)entity;
			attackEnabled = *(const unsigned int*)(raw + 0x254);
			boxRangeStart = *(const int*)(raw + 0x480);
			boxRangeEnd = *(const int*)(raw + 0x484);
			for (int w = 0; w < 5; w++)
			{
				rawWords[w] = *(const unsigned int*)(raw + 0x90 + w * 4);
			}
			secondCollection = *(const unsigned int*)(raw + 0x224);
		}

		// Exactly the decision the overlay makes, recorded next to the data it made it from. An
		// entity carrying boxes with no reachable owner is the failure case worth hunting.
		const char* verdict;
		if (depth < 0)
		{
			verdict = (entity->hitboxCount > 0 || entity->hurtboxCount > 0)
				? "UNREACHABLE-HAS-BOXES"
				: "unreachable";
		}
		else if (depth == 0)
		{
			verdict = "drawn(direct)";
		}
		else
		{
			verdict = "drawn(via-chain)";
		}

		const std::string sprite = SpriteName(entity->currentSprite);

		// Exact counts and the exact flag word, not just "has boxes" - the whole point is to catch
		// the frame where a count changes.
		char signature[256];
		sprintf_s(signature, "%s|%s|%d|%d|%u|%u|0x%08X|%d|0x%08X|%d|%d|%u,%u,%u,%u,%u|0x%08X",
			action.c_str(),
			sprite.c_str(),
			depth,
			entity->unknownStatus1,
			entity->hurtboxCount,
			entity->hitboxCount,
			stateFlags,
			engineCount,
			attackEnabled,
			boxRangeStart,
			boxRangeEnd,
			rawWords[0], rawWords[1], rawWords[2], rawWords[3], rawWords[4],
			secondCollection);

		std::string& previous = g_lastBySlot[i];
		if (previous == signature)
		{
			continue;
		}
		previous = signature;

		char depthText[8];
		if (depth < 0)
		{
			strcpy_s(depthText, "-");
		}
		else
		{
			sprintf_s(depthText, "%d", depth);
		}

		LOG(2, "[Entities] slot %3d  0x%08X  owner 0x%08X  depth %-2s  status %d  %u/%u  "
		       "flags 0x%08X live %d  engine %-3d [254]=0x%08X box[%d,%d)  "
		       "raw90=%u,%u,%u,%u,%u [224]=0x%08X  %-21s  t%-4d  %-9s  charIndex %d  %s\n",
			i,
			(unsigned int)entity,
			(unsigned int)entity->ownerEntity,
			depthText,
			entity->unknownStatus1,
			entity->hurtboxCount,
			entity->hitboxCount,
			stateFlags,
			liveHitbox ? 1 : 0,
			engineCount,
			attackEnabled,
			boxRangeStart,
			boxRangeEnd,
			rawWords[0], rawWords[1], rawWords[2], rawWords[3], rawWords[4],
			secondCollection,
			verdict,
			entity->actionTime,
			sprite.c_str(),
			entity->charIndex,
			action.c_str());

		if (++g_linesLogged >= kMaxLoggedLines)
		{
			break;
		}
	}
}
