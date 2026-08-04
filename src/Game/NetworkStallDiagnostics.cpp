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

	// CSaveDataManager singleton getter (FUN_004B9770, no-arg singleton accessor,
	// same calling convention as RankedProgressWindow.cpp's kRankedTableBaseFnRva).
	// Note (2026-07-14): live logging proved the "auto-save trigger global"
	// DAT_00EA97C8 (RVA 0xAA97C8) read by GAME_CSaveTask::update_task IS
	// manager+kSaveActionRunningOffset -- the manager is statically allocated, so
	// the "global" and the field move in lockstep. Save requests are made by tiny
	// helpers (FUN_004BB2C0 sets nextAction=7, FUN_004BB410 sets 1, FUN_004BB300
	// sets 2, mode param stored at +0x1B11F8), all driven by the save-task state
	// machine FUN_006C4990 (pumped per frame by FUN_006C4880). See
	// DCodeBug10GhidraReport.txt.
	constexpr uintptr_t kSaveDataManagerGetterRva = 0x000B9770;
	constexpr uintptr_t kSaveActionRunningOffset = 0x1B11F0;
	constexpr uintptr_t kSaveNextActionOffset = 0x1B11F4;
	constexpr uintptr_t kSaveModeParamOffset = 0x1B11F8;

	// Local player's net color (square color) and its progression counter, stored
	// in the static netUserData block. Same offsets as NetworkSquareColorWindow.cpp.
	constexpr uintptr_t kNetColorOffset = 0x0194;
	constexpr uintptr_t kNetColorCounterOffset = 0x0195;

	// ---- TUS ("Title User Storage") disabled latch -- the progress-reset cause ----
	// DAT_00CF77A8 (Ghidra VA) -> RVA 0x8F77A8. A process-wide flag meaning
	// "network profile storage is unavailable". Set to 1 when the own-profile sync
	// runs out of retries (FUN_004B0970 at 0x4B0ACE: dec [esi+0x20] starting from
	// 0xBB8 = 3000 ticks, then latch) and on sibling error paths (0x4AC098,
	// 0x4AFED2, 0x4B0AB2); cleared to 0 only on a successful sync (0x4B0A1A).
	// While set:
	//   - FUN_004A96D0 (upload my 0x6800 profile blob as L"bbdc.dat" via
	//     FUN_004B9210 -> uei::ThinkLogicStrategyUploadTUS) returns immediately,
	//     so ranked/net-color progress is NEVER made durable. A restart then loads
	//     the last successfully uploaded profile == the reported "progress reset".
	//   - FUN_004B8CF0 / FUN_004B8D30 short-circuit, so D-Code reads stop working.
	// Nothing but a process restart clears it if no sync ever succeeds again.
	// Full derivation: docs/Research/DCodeNetworkStallBug.md (phases 18-23).
	constexpr uintptr_t kTusDisabledGateRva = 0x008F77A8;

	bool IsDiagnosticsEnabled()
	{
		return Settings::settingsIni.enableInDevelopmentFeatures;
	}

	int32_t g_lastSlotFetchState[kRoomSlotCount] = { -2, -2 }; // -2 = never observed, -1 = unreadable
	int32_t g_lastSaveActionRunning = -2;
	int32_t g_lastSaveNextAction = -2;
	ULONGLONG g_lastSaveFileWriteTime = 0;
	int32_t g_lastTusGate = -2;
	int g_tusGateClears = 0;
	ULONGLONG g_lastTusGateClearMs = 0;
	constexpr int kMaxTusGateClears = 20;          // per process
	constexpr ULONGLONG kTusGateClearCooldownMs = 10000;
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

	// ---- Forced-failure test mode (DCodeForceFailureOnce) ----
	// Offset chosen inside the per-character entry array (row+0xD4..), away from
	// the +0x8 magic and +0xC0 header fields the validator also touches.
	constexpr uintptr_t kForcedCorruptionOffset = 0x1000;
	constexpr size_t kForcedCorruptionBytes = 16;
	int g_forcedFailureSlot = -1;
	bool g_forcedFailureDone = false;

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

	// AASTEAM_CUserManagedStorage singleton (DAT_00A29E30, created by
	// thunk_FUN_00422cd0). Its +4 points to the AASTEAM_CUMSTask worker (0x110
	// bytes, ctor FUN_00422410) that performs the actual Steam RemoteStorage
	// FileShare / UGCDownload for the D-Code profile blobs. Field map from
	// FUN_00422E70 (poll) / FUN_00422A10 (submit) / FUN_00422CC0 (result getter),
	// DCodeBug12/13GhidraReport.txt:
	//   +0x1C done flag, +0x1D busy flag, +0x30..0x8F request block (0x60 bytes,
	//   includes UGC handle / steamID), +0x90/+0x94 request ids, +0xB8 Steam
	//   EResult, +0xC0 bit0 = error latch (poll returns 100 -> state 6).
	constexpr uintptr_t kUserManagedStorageSingletonRva = 0x00629E30;
	// Steam work manager (DAT_00A5A050, getter FUN_00427CD0) driving the actual
	// RemoteStorage UGCDownload/FileShare for bbdc.dat.
	constexpr uintptr_t kSteamWorkMgrRva = 0x0065A050;
	constexpr uintptr_t kUMSWorkerRequestBlockOffset = 0x30;
	constexpr size_t kUMSWorkerRequestBlockSize = 0x60;

	void LogUMSWorkerState(uintptr_t moduleBase)
	{
		const uint8_t* const* const singletonPtr =
			reinterpret_cast<const uint8_t* const*>(moduleBase + kUserManagedStorageSingletonRva);
		if (IsBadReadPtr(singletonPtr, sizeof(void*)) || *singletonPtr == nullptr)
		{
			IncidentPrintf("[DCodeTick] UMS singleton unreadable\n");
			return;
		}
		const uint8_t* const ums = *singletonPtr;
		if (IsBadReadPtr(ums + 4, sizeof(void*)))
		{
			IncidentPrintf("[DCodeTick] UMS worker pointer unreadable\n");
			return;
		}
		const uint8_t* const worker = *reinterpret_cast<const uint8_t* const*>(ums + 4);
		if (worker == nullptr || IsBadReadPtr(worker, 0x110))
		{
			IncidentPrintf("[DCodeTick] UMS worker null/unreadable (%p)\n", worker);
			return;
		}

		IncidentPrintf("[DCodeTick] UMS worker: done=%u busy=%u reqIds=%08X/%08X steamEResult=%d resultAux=%08X errFlags=%02X recv=%08X/%08X\n",
			worker[0x1C], worker[0x1D],
			*reinterpret_cast<const uint32_t*>(worker + 0x90),
			*reinterpret_cast<const uint32_t*>(worker + 0x94),
			*reinterpret_cast<const int32_t*>(worker + 0xB8),
			*reinterpret_cast<const uint32_t*>(worker + 0xBC),
			worker[0xC0],
			*reinterpret_cast<const uint32_t*>(worker + 0xC8),
			*reinterpret_cast<const uint32_t*>(worker + 0xCC));

		char hex[3 * kUMSWorkerRequestBlockSize + 1];
		char* p = hex;
		for (size_t i = 0; i < kUMSWorkerRequestBlockSize; ++i)
		{
			p += sprintf_s(p, 4, "%02X ", worker[kUMSWorkerRequestBlockOffset + i]);
		}
		IncidentPrintf("[DCodeTick] UMS request block +0x30: %s\n", hex);

		// One level deeper: the Steam work manager singleton (DAT_00A5A050,
		// getter FUN_00427CD0) that the bbdc paths poll. +4 = completion state
		// written by the Steam CallResult (7 dl-done, 8 share-done, 9 dl-empty,
		// 0xB error; anything else after the 3x3s poll = CallResult never fired),
		// +0xD0/+0xD4 steamID and +0xD8/+0xDC UGC handle of the current request,
		// +0xE4 current work item. See DCodeBug17GhidraReport.txt.
		const uint8_t* const workMgr = reinterpret_cast<const uint8_t*>(moduleBase + kSteamWorkMgrRva);
		if (!IsBadReadPtr(workMgr, 0xE8))
		{
			IncidentPrintf("[DCodeTick] SteamWorkMgr: state=%d steamId=%08X%08X ugcHandle=%08X%08X workItem=%08X\n",
				*reinterpret_cast<const int32_t*>(workMgr + 4),
				*reinterpret_cast<const uint32_t*>(workMgr + 0xD4),
				*reinterpret_cast<const uint32_t*>(workMgr + 0xD0),
				*reinterpret_cast<const uint32_t*>(workMgr + 0xDC),
				*reinterpret_cast<const uint32_t*>(workMgr + 0xD8),
				*reinterpret_cast<const uint32_t*>(workMgr + 0xE4));
		}
	}

	// ---- TUS latch handling (shared by the 200ms poll and the failure handler) ----

	int32_t* TusGatePtr(uintptr_t moduleBase)
	{
		int32_t* const gate = reinterpret_cast<int32_t*>(moduleBase + kTusDisabledGateRva);
		return IsBadReadPtr(gate, sizeof(int32_t)) ? nullptr : gate;
	}

	// Re-arms every wedged slot so the game's own retry path issues fresh fetches.
	// Called right after the latch is cleared: while the latch was set the lower
	// layer was disabled, so any state-6 slot and any consumed retry budget was
	// collateral damage, not evidence of a bad peer.
	void ReArmAllSlots(uintptr_t moduleBase)
	{
		const uint8_t* const netUserData = reinterpret_cast<const uint8_t*>(moduleBase + kNetworkUserDataRva);
		int rearmed = 0;
		for (int slot = 0; slot < kRoomSlotCountMax; ++slot)
		{
			g_slotTracks[slot].autoRecoveries = 0;
			g_slotTracks[slot].stallHandled = false;

			const uint8_t* const row = netUserData + kRoomRowBaseOffset + slot * kRoomRowStride;
			if (IsBadReadPtr(row + kRoomRowSubObjectPtrOffset, sizeof(void*)))
			{
				continue;
			}
			uint8_t* const subObject = *reinterpret_cast<uint8_t* const*>(row + kRoomRowSubObjectPtrOffset);
			if (subObject == nullptr || IsBadReadPtr(subObject + kSubObjectFetchStateOffset, sizeof(int32_t)))
			{
				continue;
			}
			int32_t* const state = reinterpret_cast<int32_t*>(subObject + kSubObjectFetchStateOffset);
			if (*state == 6)
			{
				*state = 0; // the precondition the game itself uses to re-issue a fetch
				g_slotTracks[slot].lastState = 0;
				++rearmed;
			}
		}
		IncidentPrintf("[TusGate] re-armed %d wedged slot(s) and reset all retry budgets\n", rearmed);
	}

	// Returns true if the latch was found set (whether or not we cleared it).
	bool HandleTusGate(uintptr_t moduleBase, ULONGLONG now, const char* progress)
	{
		int32_t* const gate = TusGatePtr(moduleBase);
		if (gate == nullptr)
		{
			return false;
		}

		const int32_t value = *gate;
		if (value != g_lastTusGate)
		{
			if (value != 0)
			{
				IncidentPrintf("[TusGate] !!! network profile storage DISABLED (%d -> %d): profile uploads are being skipped, progress earned from here would NOT persist. %s\n",
					g_lastTusGate, value, progress ? progress : "");
			}
			else
			{
				IncidentPrintf("[TusGate] network profile storage available (%d -> %d), %s\n",
					g_lastTusGate, value, progress ? progress : "");
			}
			g_lastTusGate = value;
		}

		if (value == 0)
		{
			return false;
		}

		if (!Settings::settingsIni.dcodeTusGateAutoClear)
		{
			return true;
		}

		if (g_tusGateClears >= kMaxTusGateClears)
		{
			return true; // give up quietly; the transition above was already logged
		}
		if (g_lastTusGateClearMs != 0 && (now - g_lastTusGateClearMs) < kTusGateClearCooldownMs)
		{
			return true; // cooling down, try again on a later poll
		}

		// 0 is the value the game itself writes once a sync succeeds (0x4B0A1A),
		// so this only puts the subsystem back into a state the game produces.
		*gate = 0;
		++g_tusGateClears;
		g_lastTusGateClearMs = now;
		g_lastTusGate = 0;
		IncidentPrintf("[TusGate] auto-clear: latch reset to 0 (%d/%d this session), profile uploads re-enabled\n",
			g_tusGateClears, kMaxTusGateClears);
		ReArmAllSlots(moduleBase);
		return true;
	}

	// ---- Profile upload observation ----
	// Reads the shared CUMSTask worker: +0x90 nonzero = an upload/share request
	// (FUN_00423950 path, i.e. our own bbdc.dat going out), +0x94 nonzero = a
	// download.
	//
	// 2026-08-03 finding (third-party report, v8.2): uploads can fail FOREVER
	// without the DAT_00CF77A8 latch (kTusDisabledGateRva) ever setting, so the
	// TusGate auto-clear cannot help this case. Root cause traced statically
	// (DCodeBug25/26GhidraReport.txt): the upload strategy's tick method
	// (uei::ThinkLogicStrategyUploadTUS::vftable+0x1C, FUN_0042EDD0) checksums
	// the OWN local profile buffer with the same FUN_0040DF10 check used to
	// validate downloads, BEFORE attempting any Steam call. If that checksum is
	// already invalid, it sets the shared error state immediately -- no Steam
	// round-trip happens at all. The buffer is netUserData+0xD0
	// (FUN_0049D5C0() == FUN_004A0FE0()+0xD0, the SAME live singleton this file
	// reads everywhere else, not a stack copy) -- i.e. the player's own
	// in-memory profile blob. Nothing rewrites that region between attempts, so
	// once it goes checksum-invalid it stays invalid for the rest of the
	// process, and every retry fails identically forever.
	//
	// This is NOT yet an automatic fix: we don't know what makes the buffer
	// invalid or whether overwriting it live is safe, so for now this only
	// detects and evidences the condition (dumping the buffer once per streak)
	// so the next capture can show the actual corrupted bytes. A raw one-line-
	// per-attempt log of this would be unusable in practice -- the report that
	// found this had 7503 near-identical failure lines in one session -- so
	// logging is rate-limited to the first few occurrences plus periodic
	// heartbeats.
	uint32_t g_lastUploadReqId = 0;
	bool g_uploadInFlight = false;
	int g_uploadConsecutiveFailures = 0;
	constexpr int kUploadFailureFullLogCount = 3;     // log full detail this many times
	constexpr int kUploadFailureHeartbeatEvery = 200; // then only a periodic count

	// Own-profile buffer at netUserData+0xD0 (see FUN_0049D5C0 in the comment
	// above). Distinct from the per-slot room-row blob this file already reads.
	constexpr uintptr_t kOwnProfileBufferOffset = 0xD0;

	void ObserveProfileUploads(uintptr_t moduleBase, const char* progress)
	{
		const uint8_t* const* const singletonPtr =
			reinterpret_cast<const uint8_t* const*>(moduleBase + kUserManagedStorageSingletonRva);
		if (IsBadReadPtr(singletonPtr, sizeof(void*)) || *singletonPtr == nullptr)
		{
			return;
		}
		const uint8_t* const ums = *singletonPtr;
		if (IsBadReadPtr(ums + 4, sizeof(void*)))
		{
			return;
		}
		const uint8_t* const worker = *reinterpret_cast<const uint8_t* const*>(ums + 4);
		if (worker == nullptr || IsBadReadPtr(worker, 0xC4))
		{
			return;
		}

		const uint32_t uploadReq = *reinterpret_cast<const uint32_t*>(worker + 0x90);
		const uint8_t done = worker[0x1C];
		const uint8_t busy = worker[0x1D];
		const uint8_t errFlags = worker[0xC0];

		if (uploadReq != 0 && !g_uploadInFlight)
		{
			g_uploadInFlight = true;
			g_lastUploadReqId = uploadReq;
		}
		else if (uploadReq != 0 && g_uploadInFlight && done != 0 && busy == 0)
		{
			g_uploadInFlight = false;
			const bool failed = (errFlags & 1) != 0;

			if (!failed)
			{
				if (g_uploadConsecutiveFailures > 0)
				{
					IncidentPrintf("[Upload] profile upload recovered after %d consecutive failure(s), %s\n",
						g_uploadConsecutiveFailures, progress ? progress : "");
				}
				else
				{
					IncidentPrintf("[Upload] profile upload finished ok, %s\n", progress ? progress : "");
				}
				g_uploadConsecutiveFailures = 0;
				return;
			}

			++g_uploadConsecutiveFailures;
			const bool logFull = g_uploadConsecutiveFailures <= kUploadFailureFullLogCount ||
				(g_uploadConsecutiveFailures % kUploadFailureHeartbeatEvery) == 0;
			if (!logFull)
			{
				return;
			}

			IncidentPrintf("[Upload] !!! profile upload FAILED (errFlags=%02X, streak=%d), %s\n",
				errFlags, g_uploadConsecutiveFailures, progress ? progress : "");

			if (g_uploadConsecutiveFailures <= kUploadFailureFullLogCount)
			{
				const uint8_t* const netUserData =
					reinterpret_cast<const uint8_t*>(moduleBase + kNetworkUserDataRva);
				const uint8_t* const ownBuffer = netUserData + kOwnProfileBufferOffset;
				if (!IsBadReadPtr(ownBuffer, kProfileBlobSize))
				{
					const uint16_t sum = ProfileChecksum16(ownBuffer, kProfileBlobSize);
					IncidentPrintf("[Upload] own profile buffer (netUserData+0x%X) checksum16=0x%04X (valid=0xFFFF)\n",
						static_cast<unsigned>(kOwnProfileBufferOffset), sum);
					LogBlobHexdump("[Upload] own buffer", ownBuffer, 0x40);
				}
				else
				{
					IncidentPrintf("[Upload] own profile buffer unreadable\n");
				}
			}
		}
		else if (uploadReq == 0)
		{
			g_uploadInFlight = false;
		}
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

		// Steam-level ground truth: the CUMSTask worker's EResult tells us WHY
		// the transfer failed (stale UGC handle, rate limit, IO failure, ...).
		const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
		if (moduleBase != 0)
		{
			LogUMSWorkerState(moduleBase);
		}

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

		// Deal with the underlying cause before the symptom. While the TUS latch is
		// set the transfer layer is disabled, so retrying the fetch cannot succeed
		// and must not consume this slot's budget -- that is what exhausted every
		// budget in the 2026-07-30 report while the real problem went unaddressed.
		// HandleTusGate also re-arms wedged slots, which restarts this fetch.
		bool latchHandled = false;
		if (moduleBase != 0 && HandleTusGate(moduleBase, GetTickCount64(), nullptr))
		{
			latchHandled = true;
			IncidentPrintf("[DCodeTick] slot %d failure attributed to the TUS latch; not counted against its retry budget\n",
				slot);
		}

		if (!latchHandled)
		{
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

	// TEST ONLY (DCodeForceFailureOnce=1): sabotage the first in-flight fetch of
	// the session by corrupting the receive buffer, so the game's own checksum
	// validation rejects it -- an authentic state-6 wedge on demand, to verify
	// detection + auto-recovery end-to-end. Corrupts local memory only; nothing
	// is sent to the peer. Fires once per launch.
	if (Settings::settingsIni.dcodeForceFailureOnce && !g_forcedFailureDone)
	{
		if (g_forcedFailureSlot == -1 && state == 2)
		{
			g_forcedFailureSlot = slot;
			IncidentPrintf("[DCodeTick] TEST: DCodeForceFailureOnce armed, corrupting in-flight payload on slot %d\n", slot);
		}
		if (g_forcedFailureSlot == slot)
		{
			if (state == 2)
			{
				if (!IsBadWritePtr(row + kForcedCorruptionOffset, kForcedCorruptionBytes))
				{
					memset(row + kForcedCorruptionOffset, 0xA5, kForcedCorruptionBytes);
				}
			}
			else
			{
				g_forcedFailureDone = true;
				IncidentPrintf("[DCodeTick] TEST: slot %d left state 2 (now %d), sabotage disarmed for this launch\n",
					slot, state);
			}
		}
	}

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

	// --- Save machinery + on-disk save watch ---
	// Promoted out of the dev gate after the 2026-07-13 rollback happened with a
	// fully healthy fetch log: the save timeline of every session must survive in
	// DCodeIncidents.log. Each line carries the current net color/counter so a
	// future rollback shows exactly what progress existed at each save event.
	{
		char progress[64];
		const uint8_t* const netColorBase = reinterpret_cast<const uint8_t*>(moduleBase + kNetworkUserDataRva);
		if (!IsBadReadPtr(netColorBase + kNetColorOffset, 2))
		{
			sprintf_s(progress, "netcolor=%u counter=%u",
				netColorBase[kNetColorOffset], netColorBase[kNetColorCounterOffset]);
		}
		else
		{
			sprintf_s(progress, "netcolor=unreadable");
		}

		// --- TUS disabled latch: the actual progress-reset switch ---
		// Watched unconditionally, and also checked at failure time inside
		// HandleSlotFailure so recovery happens on the same frame as the symptom.
		HandleTusGate(moduleBase, now, progress);

		// --- Profile upload activity ---
		// FUN_004A96D0 rebuilds the whole 0x6800 profile from live state before
		// submitting, so a single successful upload after recovery persists all
		// accumulated progress -- nothing needs replaying. Watch the shared CUMSTask
		// worker for upload requests (+0x90 nonzero = share/upload) to confirm they
		// resume once the latch is cleared.
		ObserveProfileUploads(moduleBase, progress);

		// CSaveDataManager action state (actionRunning doubles as the "auto-save
		// trigger global" -- same memory, see header comment).
		typedef void* (__cdecl* SaveDataManagerGetterFn)();
		const SaveDataManagerGetterFn saveDataManagerGetter =
			reinterpret_cast<SaveDataManagerGetterFn>(moduleBase + kSaveDataManagerGetterRva);
		const uint8_t* const saveDataManager = reinterpret_cast<const uint8_t*>(saveDataManagerGetter());
		if (saveDataManager != nullptr && !IsBadReadPtr(saveDataManager + kSaveActionRunningOffset, sizeof(int32_t) * 3))
		{
			const int32_t actionRunning = *reinterpret_cast<const int32_t*>(saveDataManager + kSaveActionRunningOffset);
			const int32_t nextAction = *reinterpret_cast<const int32_t*>(saveDataManager + kSaveNextActionOffset);
			const int32_t modeParam = *reinterpret_cast<const int32_t*>(saveDataManager + kSaveModeParamOffset);
			if (actionRunning != g_lastSaveActionRunning || nextAction != g_lastSaveNextAction)
			{
				IncidentPrintf("[SaveWatch] save manager actionRunning %d -> %d, nextAction %d -> %d, mode=%d, %s\n",
					g_lastSaveActionRunning, actionRunning, g_lastSaveNextAction, nextAction, modeParam, progress);
				g_lastSaveActionRunning = actionRunning;
				g_lastSaveNextAction = nextAction;
			}
		}

		// bbsave.dat on disk -- filesystem ground truth that a save actually
		// reached the file, independent of any in-memory state machine.
		WIN32_FILE_ATTRIBUTE_DATA saveAttr = {};
		if (GetFileAttributesExW(L"Save\\bbsave.dat", GetFileExInfoStandard, &saveAttr))
		{
			const ULONGLONG writeTime =
				(static_cast<ULONGLONG>(saveAttr.ftLastWriteTime.dwHighDateTime) << 32) |
				saveAttr.ftLastWriteTime.dwLowDateTime;
			if (g_lastSaveFileWriteTime == 0)
			{
				g_lastSaveFileWriteTime = writeTime; // baseline, don't log old state as an event
				IncidentPrintf("[SaveWatch] bbsave.dat baseline (size=%u), %s\n",
					saveAttr.nFileSizeLow, progress);
			}
			else if (writeTime != g_lastSaveFileWriteTime)
			{
				g_lastSaveFileWriteTime = writeTime;
				IncidentPrintf("[SaveWatch] bbsave.dat WRITTEN (size=%u), %s\n",
					saveAttr.nFileSizeLow, progress);
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
