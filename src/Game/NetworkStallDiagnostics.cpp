#include "NetworkStallDiagnostics.h"

#include "Core/Settings.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Network/RankedListConnectionFilter.h"

#include <Windows.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// Addresses below are RVAs (VA - module base 0x00400000). Traced statically via
// Ghidra headless decompiles in docs/Research/DCodeBugGhidraReport.txt through
// DCodeBug9GhidraReport.txt; full narrative in docs/Research/DCodeNetworkStallBug.md.
namespace
{
	// Network user data singleton. Same RVA as RankedProgressWindow.cpp's kNetworkUserDataRva
	// (returned by 004A0FE0, Ghidra VA of the backing global is 00CAD0C0).
	constexpr uintptr_t kNetworkUserDataRva = 0x008AD0C0;

	// Per-room-member row: netUserData + kRoomRowBaseOffset + slot * kRoomRowStride.
	// (FUN_0049D560 uses this same row to serve D-Code / rank-prediction display data.)
	// The row's first 0x6800 bytes are the member's profile blob itself (contains the
	// 0x28 per-character 0x180-stride ranked entries at +0xD4), transferred over
	// GAMESTEAM_COnlineStorageTransfer and validated with a 16-bit ones'-complement
	// checksum (FUN_0040DF10) -- the same checksum the save-data code uses.
	constexpr uintptr_t kRoomRowBaseOffset = 0x2326C;
	constexpr uintptr_t kRoomRowStride = 0x68A4;
	constexpr size_t kProfileBlobSize = 0x6800;
	// Row -> per-slot async fetch sub-object pointer, and the fetch state field within it.
	// State machine (FUN_004A25C0): 0=idle, 1=request queued, 2=request sent/awaiting
	// completion, 3=ready, 6=response received but rejected (size != 0x6800 or checksum
	// failure). State 6 wipes the blob (FUN_004A0D50) and wedges the slot until process
	// exit: FUN_004A0B80 returns 100 (hard error) and FUN_004A1AB0 reports "busy" for
	// state 6, so the game never re-triggers a fetch. Captured live in
	// docs/Research/Debug_DCodeError1.txt (slot 0, 3->2->6, 2026-07-11 20:50:50).
	constexpr uintptr_t kRoomRowSubObjectPtrOffset = 0x68A0;
	constexpr uintptr_t kSubObjectFetchStateOffset = 0xCC;
	// Transport status area handed to COnlineStorageTransfer: +0xD0 is the received
	// size FUN_004A25C0 compares against 0x6800; +0xD4..+0xE8 are adjacent context.
	constexpr uintptr_t kSubObjectRecvSizeOffset = 0xD0;
	constexpr int kSubObjectContextDwords = 7; // 0xD0..0xE8 inclusive
	constexpr int kRoomSlotCount = 2;     // self + opponent, 1v1 ranked (poll layer)
	constexpr int kRoomSlotCountMax = 6;  // the per-frame pump FUN_0049D440 walks 6 rows

	// Generic warning/error popup message-key buffer (FUN_006983E0 -> FUN_00698420 ->
	// FUN_00450C70 copies the key string here). Every popup shown through this system
	// ("Failed to connect to room", room creation errors, etc., if routed through it)
	// lands in this single 64-byte null-terminated buffer. VA 0x01500BD8.
	constexpr uintptr_t kPopupMessageBufferRva = 0x01100BD8;
	constexpr size_t kPopupMessageBufferSize = 0x40;

	// Auto-save trigger flag driving GAME_CSaveTask::update_task (FUN_004B9F70).
	// 0=idle, 1=start automatic write, 2=write in progress, 3=finalize.
	// Ghidra VA 0x00EA97C8 -> RVA 0xAA97C8. (An earlier revision of this file had
	// 0xA97C8 -- a dropped digit -- which is why Debug_DCodeError1.txt shows the
	// trigger as constant garbage, -1956749403.)
	constexpr uintptr_t kAutoSaveTriggerRva = 0x00AA97C8;

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

	// ---- Tick-hook layer (OnFetchTickEnter) ----

	constexpr ULONGLONG kState2StallMs = 15000; // generous; healthy fetches finish in ~1-3s
	constexpr int kMaxAutoRecoveries = 3;       // per slot per process, guards a corrupt-peer retry storm

	struct SlotTrack
	{
		int32_t lastState = -2;
		ULONGLONG stateSinceMs = 0;
		bool haveSnapshot = false;
		bool stallHandled = false;
		int autoRecoveries = 0;
		int32_t snapshotCtx[kSubObjectContextDwords] = {};
		uint8_t snapshotBlob[kProfileBlobSize] = {};
	};

	SlotTrack g_slotTracks[kRoomSlotCountMax];

	// ---- Persistent incident sink ----
	// DEBUG.txt is recreated on every game launch, so anything the user doesn't
	// harvest immediately is lost. Every important [DCodeTick] line therefore
	// also goes to BBCF_IM\DCodeIncidents.log (append-only, survives across
	// sessions), and on each failure the current DEBUG.txt is snapshotted to a
	// timestamped copy automatically.

	bool g_incidentSessionHeaderWritten = false;
	int g_debugSnapshotsThisSession = 0;
	constexpr int kMaxDebugSnapshotsPerSession = 5;

	void AppendToIncidentFile(const char* message)
	{
		const HANDLE hFile = CreateFileW(L"BBCF_IM\\DCodeIncidents.log", FILE_APPEND_DATA,
			FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			return;
		}
		SYSTEMTIME st;
		GetLocalTime(&st);
		char line[1200];
		const int len = sprintf_s(line, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
			message);
		if (len > 0)
		{
			DWORD written = 0;
			WriteFile(hFile, line, static_cast<DWORD>(len), &written, nullptr);
		}
		CloseHandle(hFile);
	}

	// Mirrors the message to DEBUG.txt (via LOG) and DCodeIncidents.log.
	void IncidentPrintf(const char* fmt, ...)
	{
		char message[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf_s(message, _TRUNCATE, fmt, args);
		va_end(args);

		LOG(1, "%s", message);

		if (!g_incidentSessionHeaderWritten)
		{
			g_incidentSessionHeaderWritten = true;
			AppendToIncidentFile("==== session start (first D-code event this game launch) ====\n");
		}
		AppendToIncidentFile(message);
	}

	// Preserves the full context around a failure before the next game launch
	// overwrites DEBUG.txt. The logger flushes after every line, so the on-disk
	// file is current at the moment of the copy.
	void SnapshotDebugLog(int slot)
	{
		if (g_debugSnapshotsThisSession >= kMaxDebugSnapshotsPerSession)
		{
			IncidentPrintf("[DCodeTick] DEBUG.txt snapshot skipped (cap of %d per session reached)\n",
				kMaxDebugSnapshotsPerSession);
			return;
		}
		SYSTEMTIME st;
		GetLocalTime(&st);
		wchar_t path[MAX_PATH];
		swprintf_s(path, L"BBCF_IM\\DEBUG_DCodeIncident_%04u%02u%02u_%02u%02u%02u_slot%d.txt",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, slot);
		if (CopyFileW(L"BBCF_IM\\DEBUG.txt", path, FALSE))
		{
			++g_debugSnapshotsThisSession;
			IncidentPrintf("[DCodeTick] DEBUG.txt snapshotted to BBCF_IM\\DEBUG_DCodeIncident_%04u%02u%02u_%02u%02u%02u_slot%d.txt\n",
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, slot);
		}
		else
		{
			IncidentPrintf("[DCodeTick] DEBUG.txt snapshot failed, error %u\n", GetLastError());
		}
	}

	// Replica of the game's own payload check FUN_0040DF10: 16-bit ones'-complement
	// sum over the buffer; valid payloads sum to 0xFFFF.
	uint16_t ProfileChecksum16(const uint8_t* buf, size_t size)
	{
		uint32_t sum = 0;
		const size_t words = size / 2;
		for (size_t i = 0; i < words; ++i)
		{
			uint16_t word;
			memcpy(&word, buf + i * 2, sizeof(word));
			sum += word;
			sum = (sum & 0xFFFF) + (sum >> 16);
		}
		return static_cast<uint16_t>(sum);
	}

	int DeriveSlotIndex(uintptr_t row, uintptr_t moduleBase)
	{
		const uintptr_t firstRow = moduleBase + kNetworkUserDataRva + kRoomRowBaseOffset;
		if (row < firstRow)
		{
			return -1;
		}
		const uintptr_t delta = row - firstRow;
		if (delta % kRoomRowStride != 0)
		{
			return -1;
		}
		const uintptr_t slot = delta / kRoomRowStride;
		return slot < kRoomSlotCountMax ? static_cast<int>(slot) : -1;
	}

	void LogBlobHexdump(const char* tag, const uint8_t* blob, size_t bytes)
	{
		char line[3 * 16 + 1];
		for (size_t off = 0; off < bytes; off += 16)
		{
			char* p = line;
			for (size_t i = 0; i < 16 && off + i < bytes; ++i)
			{
				p += sprintf_s(p, 4, "%02X ", blob[off + i]);
			}
			IncidentPrintf("%s +0x%04X: %s\n", tag, static_cast<unsigned>(off), line);
		}
	}

	void DumpBlobToFile(int slot, const uint8_t* blob, size_t size)
	{
		const ULONGLONG tick = GetTickCount64();
		wchar_t path[MAX_PATH];
		swprintf_s(path, L"BBCF_IM\\DCodeBlobFail_slot%d_tick%llu.bin", slot, tick);
		const HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			IncidentPrintf("[DCodeTick] blob dump failed, CreateFileW error %u\n", GetLastError());
			return;
		}
		DWORD written = 0;
		WriteFile(hFile, blob, static_cast<DWORD>(size), &written, nullptr);
		CloseHandle(hFile);
		IncidentPrintf("[DCodeTick] pre-wipe payload dumped to BBCF_IM\\DCodeBlobFail_slot%d_tick%llu.bin (%u bytes)\n",
			slot, tick, written);
	}

	void LogSubObjectContext(const char* tag, int slot, const int32_t* ctx)
	{
		IncidentPrintf("%s slot %d transport ctx +0xD0..+0xE8: %08X %08X %08X %08X %08X %08X %08X\n",
			tag, slot, ctx[0], ctx[1], ctx[2], ctx[3], ctx[4], ctx[5], ctx[6]);
	}

	// Handles both failure shapes with the same evidence dump + optional recovery.
	void HandleSlotFailure(const char* kind, int slot, SlotTrack& track, uint8_t* subObject)
	{
		int32_t liveCtx[kSubObjectContextDwords] = {};
		if (!IsBadReadPtr(subObject + kSubObjectRecvSizeOffset, sizeof(liveCtx)))
		{
			memcpy(liveCtx, subObject + kSubObjectRecvSizeOffset, sizeof(liveCtx));
		}

		IncidentPrintf("[DCodeTick] !!! %s on slot %d (autoRecoveries so far %d)\n", kind, slot, track.autoRecoveries);
		LogSubObjectContext("[DCodeTick] live", slot, liveCtx);

		if (track.haveSnapshot)
		{
			const uint16_t sum = ProfileChecksum16(track.snapshotBlob, kProfileBlobSize);
			IncidentPrintf("[DCodeTick] snapshot (last in-flight tick): checksum16=0x%04X (valid=0xFFFF), recvSize=0x%X\n",
				sum, track.snapshotCtx[0]);
			LogSubObjectContext("[DCodeTick] snap", slot, track.snapshotCtx);
			LogBlobHexdump("[DCodeTick] snap blob", track.snapshotBlob, 0x40);
			DumpBlobToFile(slot, track.snapshotBlob, kProfileBlobSize);
		}
		else
		{
			IncidentPrintf("[DCodeTick] no in-flight snapshot available for slot %d\n", slot);
		}

		if (Settings::settingsIni.dcodeAutoRecover && track.autoRecoveries < kMaxAutoRecoveries)
		{
			// State 0 is the exact precondition the game's own display path
			// (FUN_0049D560 via FUN_004A1AB0) uses to justify issuing a fresh
			// fetch, so this only re-arms an existing retry path.
			*reinterpret_cast<int32_t*>(subObject + kSubObjectFetchStateOffset) = 0;
			++track.autoRecoveries;
			IncidentPrintf("[DCodeTick] auto-recover: slot %d fetch state forced to 0 (retry %d/%d)\n",
				slot, track.autoRecoveries, kMaxAutoRecoveries);
		}
		else if (Settings::settingsIni.dcodeAutoRecover)
		{
			IncidentPrintf("[DCodeTick] auto-recover budget exhausted for slot %d, leaving state as-is\n", slot);
		}

		// Last: the copy now contains every line above.
		SnapshotDebugLog(slot);
	}
}

void NetworkStallDiagnostics::OnFetchTickEnter(void* rowPtr)
{
	if (!IsLoggingEnabled() && !Settings::settingsIni.dcodeAutoRecover)
	{
		return;
	}

	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	if (!moduleBase || rowPtr == nullptr)
	{
		return;
	}

	uint8_t* const row = static_cast<uint8_t*>(rowPtr);
	const int slot = DeriveSlotIndex(reinterpret_cast<uintptr_t>(row), moduleBase);
	if (slot < 0)
	{
		return; // not a netUserData room row we track
	}

	if (IsBadReadPtr(row + kRoomRowSubObjectPtrOffset, sizeof(void*)))
	{
		return;
	}
	uint8_t* const subObject = *reinterpret_cast<uint8_t* const*>(row + kRoomRowSubObjectPtrOffset);
	if (subObject == nullptr || IsBadReadPtr(subObject + kSubObjectFetchStateOffset, sizeof(int32_t)))
	{
		return;
	}

	SlotTrack& track = g_slotTracks[slot];
	const int32_t state = *reinterpret_cast<const int32_t*>(subObject + kSubObjectFetchStateOffset);
	const ULONGLONG now = GetTickCount64();

	// While a request is in flight the row buffer already contains whatever the
	// transport has written; the tick that completes the exchange validates and,
	// on failure, wipes it before returning. Snapshotting at entry therefore
	// captures the exact payload the game is about to accept or reject.
	if (state == 2 && !IsBadReadPtr(row, kProfileBlobSize))
	{
		memcpy(track.snapshotBlob, row, kProfileBlobSize);
		if (!IsBadReadPtr(subObject + kSubObjectRecvSizeOffset, sizeof(track.snapshotCtx)))
		{
			memcpy(track.snapshotCtx, subObject + kSubObjectRecvSizeOffset, sizeof(track.snapshotCtx));
		}
		track.haveSnapshot = true;
	}

	if (state != track.lastState)
	{
		int32_t recvSize = -1;
		if (!IsBadReadPtr(subObject + kSubObjectRecvSizeOffset, sizeof(int32_t)))
		{
			recvSize = *reinterpret_cast<const int32_t*>(subObject + kSubObjectRecvSizeOffset);
		}
		IncidentPrintf("[DCodeTick] slot %d fetch state %d -> %d (recvSize=0x%X, heldPrevFor=%llums)\n",
			slot, track.lastState, state, recvSize,
			track.stateSinceMs != 0 ? now - track.stateSinceMs : 0);

		if (state == 3)
		{
			// Success baseline: log what a healthy accepted payload looks like.
			if (!IsBadReadPtr(row, kProfileBlobSize))
			{
				IncidentPrintf("[DCodeTick] slot %d accepted payload checksum16=0x%04X (valid=0xFFFF)\n",
					slot, ProfileChecksum16(row, kProfileBlobSize));
			}
			track.autoRecoveries = 0;
		}
		else if (state == 6 && track.lastState != -2)
		{
			HandleSlotFailure("state 6 (payload rejected)", slot, track, subObject);
		}

		track.stateSinceMs = now;
		track.stallHandled = false;
		// Re-read: HandleSlotFailure may have forced the state back to 0.
		track.lastState = *reinterpret_cast<const int32_t*>(subObject + kSubObjectFetchStateOffset);
		return;
	}

	// Silent-stall watchdog: request handed to the transport but no completion
	// signal ever arrives (the originally theorized failure shape).
	if (state == 2 && !track.stallHandled && track.stateSinceMs != 0 && now - track.stateSinceMs > kState2StallMs)
	{
		track.stallHandled = true;
		HandleSlotFailure("state 2 stall (no completion signal)", slot, track, subObject);
		track.lastState = *reinterpret_cast<const int32_t*>(subObject + kSubObjectFetchStateOffset);
		track.stateSinceMs = now;
	}
}

extern "C" void __cdecl DCodeFetchTickEnterThunk(void* row)
{
	NetworkStallDiagnostics::OnFetchTickEnter(row);
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
	// Kept as a coarse backup for the tick-hook layer above (it still works if the
	// DCodeFetchTick signature scan ever fails on a patched exe).
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
