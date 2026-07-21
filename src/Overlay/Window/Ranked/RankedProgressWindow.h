#pragma once

#include "Overlay/Window/RankedProgressOverlayState.h"

#include <imgui.h>

#include <cstdint>
#include <string>

struct RankedProgressOverlaySnapshot
{
	bool active = false;
	bool isUnranked = true;
	uint32_t rowIndex = 0xFFFFFFFFu;
	uint32_t selectorValue = 0xFFFFFFFFu;
	uint32_t cursorValue = 0xFFFFFFFFu;
	uint32_t currentRank = 0;
	uint32_t previousRank = 0;
	uint32_t nextRank = 0;
	uint32_t currentLp = 0;
	uint32_t lowerThreshold = 0;
	uint32_t nextThreshold = 0;
	uint32_t remainingLp = 0;
	uint32_t promotionCounter = 0;
	uint32_t promotionCounterLimit = 0;
	uint32_t demotionCounter = 0;
	uint32_t demotionCounterLimit = 0;
	uint32_t rawPackedField00 = 0;
	uint32_t packedSubscore = 0;
	uint32_t rawLowerThreshold = 0;
	uint32_t rawUpperThreshold = 0;
	uint32_t cumulativeBase = 0;
	uint32_t rankSpan = 0;
	uint32_t rawField04 = 0;
	uint32_t rawField0C = 0;
	uint32_t rawField10 = 0;
	uint32_t rawField14 = 0;
	uint32_t rawField18 = 0;
	uint32_t rawField20 = 0;
	uint32_t rawFieldE0 = 0;
	uint32_t rawFieldE4 = 0;
	uint32_t rawFieldE8 = 0;
	uint32_t rawFieldEC = 0;
	uint32_t earnedPoints = 0;
	uint32_t totalPoints = 0;
	uint32_t remainingPoints = 0;
	uint32_t metadataNextRank = 0;
	uint32_t debugFieldF4 = 0;
	int networkState = -1;
	int networkState1 = -1;
	float progress = 0.0f;
};

void DrawRankedMatchesMainMenuSection();
bool CaptureRankedProgressOverlaySnapshot(RankedProgressOverlaySnapshot* outSnapshot);
void DrawRankedProgressOverlayStandalone();
bool TriggerRankedProgressAutomationAnimation(uint32_t characterId, int32_t lpDelta);

// Shared rank display helpers (internal rank -> visible rank -> label/color),
// reused by other windows that need to show a player's rank consistently
// with the ranked progress overlay.
uint32_t InternalRankToVisibleRank(uint32_t internalRank, bool isUnranked);
std::string FormatVisibleRankLabel(uint32_t visibleRank, bool isUnranked);
ImVec4 GetVisibleRankColor(uint32_t visibleRank, bool isUnranked);

// Converts a leaderboard entry's packed score into the same "total LP" number
// shown in the bottom-left ranked progress overlay for the local player:
// cumulative LP of all ranks below internalRank, plus this rank's progress
// (packedSubscore clamped to this rank's [lowerBound, upperBound) span).
// packedSubscore is the low 16 bits of LeaderboardEntry_t::m_nScore.
// Returns false (and leaves *outTotalLp untouched) if internalRank has no
// known LP bounds (e.g. out of table range).
bool ComputeTotalLpFromPackedScore(uint32_t internalRank, uint32_t packedSubscore, uint32_t* outTotalLp);
