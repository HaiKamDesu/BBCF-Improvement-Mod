#include "EntityDiagnostics.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Game/CharData.h"
#include "Game/characters.h"
#include "Game/gamestates.h"

#include <set>
#include <string>

namespace
{
	// Matches the hitbox overlay's own climb. Returns how many links away the owning player is, or
	// -1 when the chain never reaches one - which is the whole point of logging this.
	const int kMaxOwnerDepth = 16;

	// A session's worth of distinct entity situations. Bounded so a long session cannot turn
	// DEBUG.txt into the unbounded log FrameStallIncidents.log once became.
	const size_t kMaxLoggedSignatures = 400;

	std::set<std::string> g_seenSignatures;
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
		g_seenSignatures.clear();
		g_budgetExhaustedLogged = false;

		LOG(2, "[Entities] Matchup P1 %s (%d) vs P2 %s (%d). Cataloguing entities; each distinct "
		       "character/action/depth/boxes combination is reported once.\n"
		       "[Entities] columns: slot, entity, owner, depth to player (- = overlay cannot reach "
		       "it), status, hurt/hit boxes, verdict, action\n",
			getCharacterNameByIndexA(p1->charIndex).c_str(), p1->charIndex,
			getCharacterNameByIndexA(p2->charIndex).c_str(), p2->charIndex);
	}

	if (g_seenSignatures.size() >= kMaxLoggedSignatures)
	{
		if (!g_budgetExhaustedLogged)
		{
			g_budgetExhaustedLogged = true;
			LOG(2, "[Entities] %zu distinct entity situations logged this matchup; suppressing the "
			       "rest to keep DEBUG.txt bounded.\n", kMaxLoggedSignatures);
		}
		return;
	}

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

		char signature[192];
		sprintf_s(signature, "%d|%d|%s|%d|%d|%u|%u",
			entity->charIndex,
			isCharacter ? 1 : 0,
			action.c_str(),
			depth,
			entity->unknownStatus1,
			entity->hurtboxCount > 0 ? 1u : 0u,
			entity->hitboxCount > 0 ? 1u : 0u);

		if (!g_seenSignatures.insert(signature).second)
		{
			continue;
		}

		char depthText[8];
		if (depth < 0)
		{
			strcpy_s(depthText, "-");
		}
		else
		{
			sprintf_s(depthText, "%d", depth);
		}

		LOG(2, "[Entities] slot %3d  0x%08X  owner 0x%08X  depth %-2s  status %d  %u/%u  %-21s  "
		       "charIndex %d  %s\n",
			i,
			(unsigned int)entity,
			(unsigned int)entity->ownerEntity,
			depthText,
			entity->unknownStatus1,
			entity->hurtboxCount,
			entity->hitboxCount,
			verdict,
			entity->charIndex,
			action.c_str());

		if (g_seenSignatures.size() >= kMaxLoggedSignatures)
		{
			break;
		}
	}
}
