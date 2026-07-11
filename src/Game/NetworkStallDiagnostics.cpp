#include "NetworkStallDiagnostics.h"

#include "Core/Settings.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Network/RankedListConnectionFilter.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

// Addresses below are RVAs (VA - module base 0x00400000). Traced statically via
// Ghidra headless decompiles in docs/Research/DCodeBugGhidraReport.txt through
// DCodeBug7GhidraReport.txt; full narrative in docs/Research/DCodeNetworkStallBug.md.
namespace
{
	// Network user data singleton. Same RVA as RankedProgressWindow.cpp's kNetworkUserDataRva
	// (returned by 004A0FE0, Ghidra VA of the backing global is 00CAD0C0).
	constexpr uintptr_t kNetworkUserDataRva = 0x008AD0C0;

	// Per-room-member row: netUserData + kRoomRowBaseOffset + slot * kRoomRowStride.
	// (FUN_0049D560 uses this same row to serve D-Code / rank-prediction display data.)
	constexpr uintptr_t kRoomRowBaseOffset = 0x2326C;
	constexpr uintptr_t kRoomRowStride = 0x68A4;
	// Row -> per-slot async fetch sub-object pointer, and the fetch state field within it.
	// State machine (FUN_004A25C0): 0=idle, 1=request queued, 2=request sent/awaiting
	// completion (can wedge here forever if the transport silently drops the exchange),
	// 3=ready, 6=error.
	constexpr uintptr_t kRoomRowSubObjectPtrOffset = 0x68A0;
	constexpr uintptr_t kSubObjectFetchStateOffset = 0xCC;
	constexpr int kRoomSlotCount = 2; // self + opponent, 1v1 ranked

	// Generic warning/error popup message-key buffer (FUN_006983E0 -> FUN_00698420 ->
	// FUN_00450C70 copies the key string here). Every popup shown through this system
	// ("Failed to connect to room", room creation errors, etc., if routed through it)
	// lands in this single 64-byte null-terminated buffer. VA 0x01500BD8.
	constexpr uintptr_t kPopupMessageBufferRva = 0x01100BD8;
	constexpr size_t kPopupMessageBufferSize = 0x40;

	// Auto-save trigger flag driving GAME_CSaveTask::update_task (FUN_004B9F70).
	// 0=idle, 1=start automatic write, 2=write in progress, 3=finalize.
	// This is the candidate link between the network stall and the ranked-progress
	// rollback: if it never leaves 0 after a match completes while a room slot is
	// wedged at fetch state 2, that's the rollback mechanism.
	constexpr uintptr_t kAutoSaveTriggerRva = 0x00A97C8;

	// CSaveDataManager singleton getter (FUN_004B9770, no-arg singleton accessor,
	// same calling convention as RankedProgressWindow.cpp's kRankedTableBaseFnRva).
	constexpr uintptr_t kSaveDataManagerGetterRva = 0x000B9770;
	constexpr uintptr_t kSaveActionRunningOffset = 0x1B11F0;
	constexpr uintptr_t kSaveNextActionOffset = 0x1B11F4;

	bool IsDiagnosticsEnabled()
	{
		return Settings::settingsIni.enableInDevelopmentFeatures;
	}

	int32_t g_lastSlotFetchState[kRoomSlotCount] = { -2, -2 }; // -2 = never observed, -1 = unreadable
	int32_t g_lastAutoSaveTrigger = -2;
	int32_t g_lastSaveActionRunning = -2;
	int32_t g_lastSaveNextAction = -2;
	char g_lastPopupMessage[kPopupMessageBufferSize + 1] = {};
	bool g_havePopupMessage = false;
	ULONGLONG g_lastPollTickMs = 0;
	constexpr ULONGLONG kPollIntervalMs = 200;
}

void NetworkStallDiagnostics::OnUpdate()
{
	// Room slot identity tracking and the popup-message watch below feed
	// RankedListConnectionFilter (a normal, non-dev-gated feature), so they run
	// regardless of enableInDevelopmentFeatures. Only the verbose diagnostic
	// logging (fetch-state transitions, save manager internals) is dev-gated.
	const bool diagnosticsEnabled = IsDiagnosticsEnabled();

	const ULONGLONG now = GetTickCount64();
	if (g_lastPollTickMs != 0 && (now - g_lastPollTickMs) < kPollIntervalMs)
	{
		return;
	}
	g_lastPollTickMs = now;

	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	if (!moduleBase)
	{
		return;
	}

	// --- Per-room-member async fetch state (drives D-Code / rank-prediction display) ---
	const uint8_t* const netUserData = reinterpret_cast<const uint8_t*>(moduleBase + kNetworkUserDataRva);
	for (int slot = 0; slot < kRoomSlotCount; ++slot)
	{
		const uint8_t* const row = netUserData + kRoomRowBaseOffset + slot * kRoomRowStride;
		int32_t fetchState = -1; // -1 = row/sub-object pointer currently unreadable

		if (!IsBadReadPtr(row + kRoomRowSubObjectPtrOffset, sizeof(void*)))
		{
			const uint8_t* const subObject = *reinterpret_cast<uint8_t* const*>(row + kRoomRowSubObjectPtrOffset);
			if (subObject != nullptr && !IsBadReadPtr(subObject + kSubObjectFetchStateOffset, sizeof(int32_t)))
			{
				fetchState = *reinterpret_cast<const int32_t*>(subObject + kSubObjectFetchStateOffset);
			}
		}

		if (diagnosticsEnabled && fetchState != g_lastSlotFetchState[slot])
		{
			LOG(1, "[NetStall] room slot %d fetch state %d -> %d\n", slot, g_lastSlotFetchState[slot], fetchState);
		}
		g_lastSlotFetchState[slot] = fetchState;
	}

	if (diagnosticsEnabled)
	{
		// --- Auto-save trigger flag ---
		const int32_t* const autoSaveTriggerPtr = reinterpret_cast<const int32_t*>(moduleBase + kAutoSaveTriggerRva);
		if (!IsBadReadPtr(autoSaveTriggerPtr, sizeof(int32_t)))
		{
			const int32_t autoSaveTrigger = *autoSaveTriggerPtr;
			if (autoSaveTrigger != g_lastAutoSaveTrigger)
			{
				LOG(1, "[NetStall] auto-save trigger %d -> %d\n", g_lastAutoSaveTrigger, autoSaveTrigger);
				g_lastAutoSaveTrigger = autoSaveTrigger;
			}
		}

		// --- CSaveDataManager save action state ---
		typedef void* (__cdecl* SaveDataManagerGetterFn)();
		const SaveDataManagerGetterFn saveDataManagerGetter =
			reinterpret_cast<SaveDataManagerGetterFn>(moduleBase + kSaveDataManagerGetterRva);
		const uint8_t* const saveDataManager = reinterpret_cast<const uint8_t*>(saveDataManagerGetter());
		if (saveDataManager != nullptr && !IsBadReadPtr(saveDataManager + kSaveActionRunningOffset, sizeof(int32_t) * 2))
		{
			const int32_t actionRunning = *reinterpret_cast<const int32_t*>(saveDataManager + kSaveActionRunningOffset);
			const int32_t nextAction = *reinterpret_cast<const int32_t*>(saveDataManager + kSaveNextActionOffset);
			if (actionRunning != g_lastSaveActionRunning || nextAction != g_lastSaveNextAction)
			{
				LOG(1, "[NetStall] save manager actionRunning %d -> %d, nextAction %d -> %d\n",
					g_lastSaveActionRunning, actionRunning, g_lastSaveNextAction, nextAction);
				g_lastSaveActionRunning = actionRunning;
				g_lastSaveNextAction = nextAction;
			}
		}
	}

	// --- Generic warning/error popup message key (catches "Failed to connect to
	// room", room creation errors, etc. if routed through FUN_006983E0) ---
	const char* const popupMessageBuffer = reinterpret_cast<const char*>(moduleBase + kPopupMessageBufferRva);
	if (!IsBadReadPtr(popupMessageBuffer, kPopupMessageBufferSize))
	{
		char currentMessage[kPopupMessageBufferSize + 1] = {};
		memcpy(currentMessage, popupMessageBuffer, kPopupMessageBufferSize);
		currentMessage[kPopupMessageBufferSize] = '\0';

		if (!g_havePopupMessage || strncmp(currentMessage, g_lastPopupMessage, kPopupMessageBufferSize) != 0)
		{
			if (diagnosticsEnabled)
			{
				LOG(1, "[NetStall] popup message \"%s\" -> \"%s\"\n", g_lastPopupMessage, currentMessage);
			}
			memcpy(g_lastPopupMessage, currentMessage, sizeof(g_lastPopupMessage));
			g_havePopupMessage = true;

			// "RankMatchLeaveMyself" is the observed symptom of the RTT-check timeout
			// behind "Failed to connect to room" (see docs/Research/ - RMSR_CheckingRTT
			// -> RankMatchLeaveMyself, ~34s apart). Marks whoever we most recently
			// attempted JoinLobby() on (see SteamMatchmakingWrapper::JoinLobby).
			if (strcmp(currentMessage, "RankMatchLeaveMyself") == 0)
			{
				RankedListConnectionFilter::GetInstance().NotifyConnectionAttemptFailed("RankMatchLeaveMyself");
			}
		}
	}
}
