#include "ProfileSync.h"

#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/logger.h"
#include "Core/Settings.h"
#include "Core/utils.h"
#include "Core/info.h"
#include "Core/SaveDataSafety.h"
#include "Game/GhidraDefs.h"
#include "Game/NetworkStallDiagnostics.h"
#include "Hooks/HookManager.h"
#include "Overlay/NotificationBar/NotificationBar.h"
#include "SteamApiWrapper/SteamUserWrapper.h"

#include <Windows.h>
#include <wincrypt.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace
{
	// ---- The task object (the outer network task, `this` of FUN_004B0970) ----
	//
	// Shared by every outer network task; FUN_004A6F70 is the driver. It copies +0x0C
	// into +0x08 and clears +0x0C before each call, so +0x08 is "first tick in this
	// state" and writing +0x0C = 1 asks for that on the next tick.
	constexpr size_t kTaskState = 0x04;         // inner state, the switch below
	constexpr size_t kTaskEntering = 0x08;
	constexpr size_t kTaskReenter = 0x0C;
	constexpr size_t kTaskTimer = 0x20;         // frames left for a fetch or upload
	constexpr size_t kTaskDialogAnswer = 0xB0;  // button picked: 1 OK, 2 Cancel, 3 Create New
	constexpr size_t kTaskScreen = 0xB4;        // which "connecting" screen is up
	constexpr size_t kTaskDialogId = 0xB8;      // message id; a dialog shows while set
	constexpr size_t kTaskDialogButton1 = 0xBC;
	constexpr size_t kTaskDialogButton2 = 0xC0;
	constexpr size_t kTaskDialogButton3 = 0xC4;
	constexpr size_t kTaskDialogExtra = 0xC8;
	constexpr size_t kTaskDlcChecked = 0x414;

	// Inner states. 1, 3 and 4 do not exist in the game either.
	enum SyncState : int32_t
	{
		State_Start = 0,
		State_InitError = 2,      // "could not connect", then leave network mode
		State_Fetch = 5,          // download the own profile
		State_Upload = 6,         // upload the own profile
		State_AskCreate = 7,      // the destructive vanilla prompt; never entered any more
		State_AskRetry = 8,
		State_AskRecreate = 9,
		State_AskRestore = 0xA,   // dormant in the game too
		State_AskReturn = 0xB,
		State_UploadAfterDlc = 0xC,
		State_DlcQuery = 0xD,
		State_DlcNotice = 0xE,
		State_NewDlcNotice = 0xF,
		State_Done = 0x10,
	};

	// Message ids (table 0x9DE870) and button labels (0x9DE9CC).
	constexpr int32_t kMsgInitError = 0x05;
	constexpr int32_t kMsgAskCreate = 0x30;   // "Cannot find player information. ... create a new profile?"
	constexpr int32_t kMsgAskRetry = 0x31;    // "Failed to retrieve player information. ... try again?"
	constexpr int32_t kMsgAskRecreate = 0x32; // "... the information on the server will be reset. ..."
	constexpr int32_t kMsgAskRestore = 0x33;
	constexpr int32_t kMsgAskReturn = 0x34;   // "Return to the Main Menu. ..."
	constexpr int32_t kMsgDlcNotice = 0x4F;
	constexpr int32_t kMsgNewDlcNotice = 0x50;
	constexpr int32_t kButtonOk = 1;
	constexpr int32_t kButtonCancel = 2;
	constexpr int32_t kButtonCreateNew = 3;

	constexpr int32_t kScreenNetwork = 0x44;
	constexpr int32_t kScreenFetching = 0x45;
	constexpr int32_t kScreenUploading = 0x46;

	constexpr int32_t kFetchFrames = 3000; // ~50 s, the game's budget for one attempt
	constexpr int32_t kPollPending = -100;
	constexpr int32_t kPollFailed = 100;

	// Own profile loader states (sub-object at blob+0x68A0, +0xCC).
	constexpr int32_t kLoaderFromServer = 3;
	constexpr int32_t kLoaderDefault = 4;
	constexpr int32_t kLoaderFailed = 6;

	// Work manager results (mgr+4) as the download leaves them.
	constexpr int32_t kWorkReadNoData = 9;
	constexpr int32_t kWorkRequestFailed = 0xB;

	constexpr size_t kProfileSize = SIZE_ProfileBlob;

	// ---- Retry policy ----
	constexpr int kMaxAutoRetries = 3;
	const ULONGLONG kRetryDelayMs[kMaxAutoRetries] = { 2000, 4000, 6000 };

	// ---- Game functions ----
	typedef void** (__cdecl* NetInterfaceHolderFn)();
	typedef void* (__thiscall* VirtualGetFn)(void* self);
	typedef void (__thiscall* VirtualVoidFn)(void* self);
	typedef void* (__cdecl* NetManagerFn)();
	typedef uint8_t* (__thiscall* GetOwnBlobFn)(void* netManager);
	typedef void (__thiscall* NetManagerVoidFn)(void* netManager);
	typedef int32_t (__thiscall* FetchPollFn)(void* netManager);
	typedef void (__fastcall* BlobFn)(uint8_t* blob);
	typedef int32_t (__fastcall* BlobIntFn)(uint8_t* blob);
	typedef int32_t (__cdecl* ChecksumFn)(const uint8_t* data, uint32_t size);
	typedef void (__fastcall* TaskFn)(uint8_t* task);
	typedef int32_t (__fastcall* TaskIntFn)(uint8_t* task);
	typedef void (__thiscall* TaskSwitchFn)(uint8_t* task, int32_t taskId);
	typedef int32_t (__thiscall* UploadFn)(uint8_t* task);
	typedef void* (__cdecl* SingletonFn)();
	typedef int32_t (__thiscall* UploadPollFn)(void* transfer, uint8_t* blob);
	typedef int32_t (__thiscall* ObjectIntFn)(void* self);
	typedef void (__thiscall* ObjectVoidFn)(void* self);
	typedef int32_t (__thiscall* MessagePostFn)(void* queue, int32_t message, int32_t target);
	typedef void (__cdecl* LogErrorFn)(const char* name);
	typedef void (__cdecl* VoidFn)();

	uintptr_t Base()
	{
		return reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	}

	template <typename T>
	T Game(uintptr_t rva)
	{
		return reinterpret_cast<T>(Base() + rva);
	}

	int32_t& Field(uint8_t* object, size_t offset)
	{
		return *reinterpret_cast<int32_t*>(object + offset);
	}

	int32_t& TusLatch()
	{
		return *reinterpret_cast<int32_t*>(Base() + ADDR_TusLatch);
	}

	uint8_t* OwnBlob(void* netManager)
	{
		return Game<GetOwnBlobFn>(ADDR_GetOwnProfileBlob)(netManager);
	}

	// -1 when the loader is not there yet.
	int32_t LoaderState(const uint8_t* blob)
	{
		if (blob == nullptr || IsBadReadPtr(blob + OFFSET_ProfileLoaderPtr, sizeof(void*)))
		{
			return -1;
		}
		const uint8_t* const loader = *reinterpret_cast<const uint8_t* const*>(blob + OFFSET_ProfileLoaderPtr);
		if (loader == nullptr || IsBadReadPtr(loader + OFFSET_ProfileLoaderState, sizeof(int32_t)))
		{
			return -1;
		}
		return *reinterpret_cast<const int32_t*>(loader + OFFSET_ProfileLoaderState);
	}

	// FUN_004A9D20, then the driver re-enters on the next tick.
	void GoTo(uint8_t* task, int32_t state)
	{
		Field(task, kTaskState) = state;
		Field(task, kTaskReenter) = 1;
		Game<TaskFn>(ADDR_NetTaskClearRequests)(task);
	}

	void ShowDialog(uint8_t* task, int32_t message, int32_t button1, int32_t button2, int32_t button3)
	{
		Field(task, kTaskDialogId) = message;
		Field(task, kTaskDialogButton1) = button1;
		Field(task, kTaskDialogButton2) = button2;
		Field(task, kTaskDialogButton3) = button3;
		Field(task, kTaskDialogExtra) = 0;
	}

	bool DialogBusy(uint8_t* task)
	{
		return Game<TaskIntFn>(ADDR_NetTaskDialogBusy)(task) != 0;
	}

	void Notify(const std::string& text)
	{
		if (g_notificationBar != nullptr)
		{
			g_notificationBar->AddNotification("%s", text.c_str());
		}
	}

	// ---- What the own profile buffer holds ----

	enum OwnProfile : int
	{
		Own_Unknown,          // no sync this process yet: vanilla rules
		Own_Loading,          // being downloaded; the buffer is zeroed meanwhile
		Own_FromServer,
		Own_Failed,           // download failed; the buffer is the zeroed default
		Own_RestoredBackup,
		Own_CreatedByPlayer,  // Create New, confirmed twice
	};

	std::atomic<int> g_own{ Own_Unknown };
	bool g_installed = false;

	const char* OwnName(int own)
	{
		switch (own)
		{
		case Own_Unknown:         return "unknown";
		case Own_Loading:         return "loading";
		case Own_FromServer:      return "from server";
		case Own_Failed:          return "failed";
		case Own_RestoredBackup:  return "restored from backup";
		case Own_CreatedByPlayer: return "created by the player";
		default:                  return "?";
		}
	}

	void SetOwn(int own, const char* why)
	{
		const int previous = g_own.exchange(own);
		if (previous != own)
		{
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] own profile: %s -> %s (%s)\n",
				OwnName(previous), OwnName(own), why);
		}
	}

	// ---- Local backup ----
	//
	// Same file format as the game's own (never written) Save/bbdc.dat, FUN_007791B0 /
	// FUN_00779020: 64-byte SHA-512 of the plaintext, then the 0x6800-byte profile
	// RC4-encrypted with a key derived from SHA-512({0x84A9E134, 0x7B8315F0, SteamID64}).
	// It therefore only opens for the account that wrote it. Kept in the mod's folder,
	// not Save/, so the vanilla game never finds it: its restore path would take the
	// player online with uploads off, which nobody should be able to switch on at will.

	uint64_t SteamId()
	{
		if (g_interfaces.pSteamUserWrapper == nullptr)
		{
			return 0;
		}
		return g_interfaces.pSteamUserWrapper->GetSteamID().ConvertToUint64();
	}

	std::wstring BackupPath(uint64_t steamId)
	{
		wchar_t name[64];
		swprintf_s(name, L"BBCF_IM\\ProfileBackup\\bbdc_%llu.dat", static_cast<unsigned long long>(steamId));
		return GamePathW(name);
	}

	bool Sha512(const void* data, DWORD size, uint8_t out[64])
	{
		HCRYPTPROV provider = 0;
		HCRYPTHASH hash = 0;
		bool ok = false;
		if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
			CryptCreateHash(provider, CALG_SHA_512, 0, 0, &hash) &&
			CryptHashData(hash, static_cast<const BYTE*>(data), size, 0))
		{
			DWORD length = 64;
			ok = CryptGetHashParam(hash, HP_HASHVAL, out, &length, 0) && length == 64;
		}
		if (hash != 0) CryptDestroyHash(hash);
		if (provider != 0) CryptReleaseContext(provider, 0);
		return ok;
	}

	// FUN_00778EE0 / FUN_00778E00: RC4 keyed by CryptDeriveKey over a SHA-512 of `secret`.
	bool Rc4(const uint8_t secret[64], uint8_t* data, DWORD size, bool encrypt)
	{
		HCRYPTPROV provider = 0;
		HCRYPTHASH hash = 0;
		HCRYPTKEY key = 0;
		bool ok = false;
		if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
			CryptCreateHash(provider, CALG_SHA_512, 0, 0, &hash) &&
			CryptHashData(hash, secret, 64, 0) &&
			CryptDeriveKey(provider, CALG_RC4, hash, CRYPT_EXPORTABLE, &key))
		{
			DWORD length = size;
			ok = encrypt
				? CryptEncrypt(key, 0, TRUE, 0, data, &length, size) != FALSE
				: CryptDecrypt(key, 0, TRUE, 0, data, &length) != FALSE;
		}
		if (key != 0) CryptDestroyKey(key);
		if (hash != 0) CryptDestroyHash(hash);
		if (provider != 0) CryptReleaseContext(provider, 0);
		return ok;
	}

	bool AccountSecret(uint64_t steamId, uint8_t out[64])
	{
		uint8_t material[16];
		const uint32_t salt[2] = { 0x84A9E134u, 0x7B8315F0u };
		memcpy(material, salt, 8);
		memcpy(material + 8, &steamId, 8);
		return Sha512(material, sizeof(material), out);
	}

	// FUN_0040DF10.
	bool ChecksumValid(const uint8_t* blob)
	{
		uint32_t sum = 0;
		for (size_t i = 0; i < kProfileSize / 2; ++i)
		{
			sum += reinterpret_cast<const uint16_t*>(blob)[i];
			sum = (sum & 0xFFFF) + (sum >> 16);
		}
		return static_cast<uint16_t>(sum) == 0xFFFF;
	}

	// FUN_0040DEC0 without touching the +4 stamp: the copy is the revision the server
	// has, not a new one.
	void Reseal(uint8_t* blob)
	{
		uint16_t* const words = reinterpret_cast<uint16_t*>(blob);
		words[0] = 0;
		words[1] = 0;
		uint32_t sum = 0;
		for (size_t i = 0; i < kProfileSize / 2; ++i)
		{
			sum += words[i];
			sum = (sum & 0xFFFF) + (sum >> 16);
		}
		words[0] = static_cast<uint16_t>(~sum);
	}

	std::mutex g_backupMutex;

	// ---- Stale-backup guard ----
	//
	// The game writes the time of every save into the profile itself (+0x0C, 64-bit,
	// FUN_004A2740 just before each upload). The newest one this machine has ever confirmed
	// is kept in the registry, and a backup older than it is never restored. Without this, a
	// copy of the backup file kept aside before a losing session could be put back later.
	constexpr size_t kProfileSaveTime = 0x0C;
	const wchar_t* const kMarkKey = L"Software\\BBCF_IM\\ProfileBackup";

	uint64_t SaveTimeOf(const uint8_t* profile)
	{
		uint64_t value = 0;
		memcpy(&value, profile + kProfileSaveTime, sizeof(value));
		return value;
	}

	bool ReadMark(uint64_t steamId, uint64_t* out)
	{
		wchar_t name[32];
		swprintf_s(name, L"%llu", static_cast<unsigned long long>(steamId));
		DWORD size = sizeof(*out);
		return RegGetValueW(HKEY_CURRENT_USER, kMarkKey, name, RRF_RT_REG_QWORD, nullptr, out, &size) == ERROR_SUCCESS;
	}

	void RaiseMark(uint64_t steamId, uint64_t saveTime)
	{
		uint64_t current = 0;
		if (ReadMark(steamId, &current) && current >= saveTime)
		{
			return;
		}
		wchar_t name[32];
		swprintf_s(name, L"%llu", static_cast<unsigned long long>(steamId));
		HKEY key = nullptr;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, kMarkKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
		{
			RegSetValueExW(key, name, 0, REG_QWORD, reinterpret_cast<const BYTE*>(&saveTime), sizeof(saveTime));
			RegCloseKey(key);
		}
	}

	// Encrypts `profile` (resealed copy) for `steamId` and replaces `path` in one step.
	bool WriteProfileFile(const std::wstring& path, const uint8_t* profile, uint64_t steamId)
	{
		std::vector<uint8_t> plain(profile, profile + kProfileSize);
		Reseal(plain.data());
		if (!ChecksumValid(plain.data()))
		{
			return false;
		}

		uint8_t header[64];
		uint8_t secret[64];
		if (!Sha512(plain.data(), static_cast<DWORD>(kProfileSize), header) || !AccountSecret(steamId, secret))
		{
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] backup not written: CryptoAPI unavailable\n");
			return false;
		}
		std::vector<uint8_t> body = plain;
		if (!Rc4(secret, body.data(), static_cast<DWORD>(kProfileSize), true))
		{
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] backup not written: encryption failed\n");
			return false;
		}

		const std::lock_guard<std::mutex> lock(g_backupMutex);
		CreateDirectoryW(GamePathW(L"BBCF_IM").c_str(), nullptr);
		CreateDirectoryW(GamePathW(L"BBCF_IM\\ProfileBackup").c_str(), nullptr);
		const std::wstring temp = path + L".tmp";
		HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] backup not written: cannot create file (error %lu)\n", GetLastError());
			return false;
		}
		DWORD written1 = 0;
		DWORD written2 = 0;
		const bool ok = WriteFile(file, header, sizeof(header), &written1, nullptr) && written1 == sizeof(header) &&
			WriteFile(file, body.data(), static_cast<DWORD>(kProfileSize), &written2, nullptr) && written2 == kProfileSize &&
			FlushFileBuffers(file);
		CloseHandle(file);
		// Replaced in one step, so a crash mid-write leaves the previous backup intact.
		if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			DeleteFileW(temp.c_str());
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] backup not written: write failed (error %lu)\n", GetLastError());
			return false;
		}
		return true;
	}

	bool WriteBackup(const uint8_t* profile, const char* why)
	{
		const uint64_t steamId = SteamId();
		if (!Settings::settingsIni.profileBackup || steamId == 0 || !WriteProfileFile(BackupPath(steamId), profile, steamId))
		{
			return false;
		}
		RaiseMark(steamId, SaveTimeOf(profile));
		NetworkStallDiagnostics::IncidentLog("[ProfileSync] backup refreshed (%s)\n", why);
		return true;
	}

	// The decrypted, verified profile in `path` for the current account, or false.
	bool ReadProfileFile(const std::wstring& path, std::vector<uint8_t>* out, SYSTEMTIME* savedLocal)
	{
		const uint64_t steamId = SteamId();
		if (steamId == 0)
		{
			return false;
		}
		const std::lock_guard<std::mutex> lock(g_backupMutex);
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		uint8_t header[64];
		std::vector<uint8_t> body(kProfileSize);
		DWORD read1 = 0;
		DWORD read2 = 0;
		FILETIME written = {};
		const bool ok = ReadFile(file, header, sizeof(header), &read1, nullptr) && read1 == sizeof(header) &&
			ReadFile(file, body.data(), static_cast<DWORD>(kProfileSize), &read2, nullptr) && read2 == kProfileSize &&
			GetFileTime(file, nullptr, nullptr, &written);
		CloseHandle(file);
		if (!ok)
		{
			return false;
		}

		uint8_t secret[64];
		uint8_t check[64];
		if (!AccountSecret(steamId, secret) ||
			!Rc4(secret, body.data(), static_cast<DWORD>(kProfileSize), false) ||
			!Sha512(body.data(), static_cast<DWORD>(kProfileSize), check) ||
			memcmp(check, header, sizeof(check)) != 0 ||
			!ChecksumValid(body.data()))
		{
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] %ls does not verify for this account; ignored\n", path.c_str());
			return false;
		}
		if (savedLocal != nullptr)
		{
			FILETIME local = {};
			FileTimeToLocalFileTime(&written, &local);
			FileTimeToSystemTime(&local, savedLocal);
		}
		*out = std::move(body);
		return true;
	}

	bool ReadBackup(std::vector<uint8_t>* out, SYSTEMTIME* savedLocal)
	{
		const uint64_t steamId = SteamId();
		return steamId != 0 && ReadProfileFile(BackupPath(steamId), out, savedLocal);
	}

	// ---- Pre-update snapshot (see SaveDataSafety.h) ----
	//
	// The network profile as the server held it the first time this mod version downloaded
	// it, before this version could upload anything. Never overwritten. Restoring one is an
	// explicit rollback the player asks for from the Online page.

	std::wstring SnapshotProfilePath(const std::string& version, uint64_t steamId)
	{
		wchar_t name[64];
		swprintf_s(name, L"\\profile_%llu.dat", static_cast<unsigned long long>(steamId));
		return SaveDataSafety::SnapshotFolder(version) + name;
	}

	std::wstring PendingSnapshotMarker()
	{
		return GamePathW(L"BBCF_IM\\ProfileBackup\\preupdate\\restore_profile.pending");
	}

	void TakeSnapshotOnce(const uint8_t* profile)
	{
		const uint64_t steamId = SteamId();
		if (steamId == 0)
		{
			return;
		}
		const std::wstring path = SnapshotProfilePath(MOD_VERSION, steamId);
		if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			return;
		}
		CreateDirectoryW(GamePathW(L"BBCF_IM\\ProfileBackup\\preupdate").c_str(), nullptr);
		CreateDirectoryW(SaveDataSafety::SnapshotFolder(MOD_VERSION).c_str(), nullptr);
		if (WriteProfileFile(path, profile, steamId))
		{
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] first download under %s: pre-update snapshot of the network profile saved\n", MOD_VERSION);
		}
	}

	std::string ReadPendingSnapshotRestore()
	{
		HANDLE file = CreateFileW(PendingSnapshotMarker().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			return std::string();
		}
		char buffer[64] = {};
		DWORD read = 0;
		ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
		CloseHandle(file);
		std::string version(buffer, read);
		while (!version.empty() && (version.back() == '\n' || version.back() == '\r' || version.back() == ' '))
		{
			version.pop_back();
		}
		return version;
	}

	bool HaveBackup()
	{
		std::vector<uint8_t> unused;
		return Settings::settingsIni.profileBackup && ReadBackup(&unused, nullptr);
	}

	// The copy of the blob taken as an upload starts, committed once it is confirmed.
	std::mutex g_pendingMutex;
	std::vector<uint8_t> g_pendingUpload;

	// ---- The failure path ----

	enum FetchOutcome
	{
		Outcome_NoData,   // the server answered and holds nothing (result 9)
		Outcome_Refused,  // refused / HTTP error, usually a dropped session (result 0xB)
		Outcome_NoAnswer, // nothing within the download's 3 x 3 s
	};

	const char* OutcomeName(FetchOutcome outcome)
	{
		switch (outcome)
		{
		case Outcome_NoData:   return "server has no data";
		case Outcome_Refused:  return "request refused";
		default:               return "no answer";
		}
	}

	int g_testFailuresDone = 0;
	bool g_testFailureActive = false;

	FetchOutcome ClassifyFailure()
	{
		if (g_testFailureActive)
		{
			return Outcome_Refused;
		}
		const int32_t* const result = reinterpret_cast<const int32_t*>(Base() + ADDR_WorkMgrResult);
		const int32_t value = IsBadReadPtr(result, sizeof(int32_t)) ? 0 : *result;
		if (value == kWorkReadNoData) return Outcome_NoData;
		if (value == kWorkRequestFailed) return Outcome_Refused;
		return Outcome_NoAnswer;
	}

	int g_autoRetries = 0;
	ULONGLONG g_retryAtMs = 0;

	void ResetRetries()
	{
		g_autoRetries = 0;
		g_retryAtMs = 0;
	}

	void StartFetch(uint8_t* task, void* netManager)
	{
		uint8_t* const blob = OwnBlob(netManager);
		Game<BlobFn>(ADDR_ProfileLoaderReset)(blob);
		Game<NetManagerVoidFn>(ADDR_OwnProfileFetchStart)(netManager);
		Field(task, kTaskTimer) = kFetchFrames;
		SetOwn(Own_Loading, "download started");
	}

	bool RestoreBackup(void* netManager)
	{
		std::vector<uint8_t> backup;
		SYSTEMTIME saved = {};
		if (!Settings::settingsIni.profileBackup || !ReadBackup(&backup, &saved))
		{
			return false;
		}
		uint64_t mark = 0;
		if (ReadMark(SteamId(), &mark) && SaveTimeOf(backup.data()) < mark)
		{
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] backup is older than a profile this machine already confirmed; not restored\n");
			return false;
		}
		uint8_t* const blob = OwnBlob(netManager);
		memcpy(blob, backup.data(), kProfileSize);
		// FUN_004A1DD0: the game's own post-download step - verifies the checksum, applies
		// its fix-ups and marks the loader as loaded from the server.
		if (Game<BlobIntFn>(ADDR_DCodePayloadValidator)(blob) == 0 || LoaderState(blob) != kLoaderFromServer)
		{
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] backup rejected by the game's validator\n");
			return false;
		}
		char when[64];
		snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u",
			saved.wYear, saved.wMonth, saved.wDay, saved.wHour, saved.wMinute);
		NetworkStallDiagnostics::IncidentLog("[ProfileSync] restored the profile from the backup saved %s; uploading it\n", when);
		char text[512];
		snprintf(text, sizeof(text),
			L("The profile server has no profile for this account. Restored it from the local backup saved %s and uploaded it.").c_str(),
			when);
		Notify(text);
		return true;
	}

	// The player asked to go back to a pre-update snapshot. Runs once the server copy has
	// loaded, so the session is healthy; the caller then uploads it. The server copy being
	// replaced is kept first, so this can be undone the same way.
	bool RestoreSnapshot(const std::string& version, void* netManager)
	{
		DeleteFileW(PendingSnapshotMarker().c_str()); // once, even if what follows fails
		const uint64_t steamId = SteamId();
		std::vector<uint8_t> snapshot;
		if (steamId == 0 || !ReadProfileFile(SnapshotProfilePath(version, steamId), &snapshot, nullptr))
		{
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] snapshot restore from before %s: no usable snapshot for this account\n", version.c_str());
			Notify(L("Couldn't restore your network profile: there is no snapshot for this Steam account."));
			return false;
		}

		uint8_t* const blob = OwnBlob(netManager);
		SYSTEMTIME now = {};
		GetLocalTime(&now);
		wchar_t name[96];
		swprintf_s(name, L"BBCF_IM\\ProfileBackup\\replaced\\profile_%llu_%04u%02u%02u-%02u%02u%02u.dat",
			static_cast<unsigned long long>(steamId), now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
		CreateDirectoryW(GamePathW(L"BBCF_IM\\ProfileBackup\\replaced").c_str(), nullptr);
		if (!WriteProfileFile(GamePathW(name), blob, steamId))
		{
			Notify(L("Couldn't restore your network profile: the current one could not be saved first."));
			return false;
		}

		memcpy(blob, snapshot.data(), kProfileSize);
		if (Game<BlobIntFn>(ADDR_DCodePayloadValidator)(blob) == 0 || LoaderState(blob) != kLoaderFromServer)
		{
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] snapshot rejected by the game's validator\n");
			return false;
		}
		SetOwn(Own_RestoredBackup, "pre-update snapshot");
		TusLatch() = 0;
		NetworkStallDiagnostics::IncidentLog("[ProfileSync] restored the network profile from the snapshot taken before %s; the replaced one is in %ls; uploading\n",
			version.c_str(), name);
		char text[512];
		snprintf(text, sizeof(text), L("Your network profile was restored to the snapshot taken before %s and is being uploaded.").c_str(), version.c_str());
		Notify(text);
		return true;
	}

	// The download failed. Vanilla jumps straight to "create a new profile?" here.
	void OnFetchFailed(uint8_t* task, void* netManager)
	{
		const FetchOutcome outcome = ClassifyFailure();
		TusLatch() = 1; // as vanilla: nothing may upload or read D-Codes meanwhile
		SetOwn(Own_Failed, OutcomeName(outcome));

		if (g_autoRetries < kMaxAutoRetries)
		{
			const ULONGLONG delay = kRetryDelayMs[g_autoRetries];
			++g_autoRetries;
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] own profile download failed (%s), retry %d/%d in %llu ms\n",
				OutcomeName(outcome), g_autoRetries, kMaxAutoRetries, delay);
			// A refused request is nearly always a session the server dropped, and an
			// empty answer can be one too; a fresh sign-in costs nothing if it was not.
			NetworkStallDiagnostics::RequestRelogin("own profile download failed at network entry");
			if (g_autoRetries == 1)
			{
				Notify(L("Couldn't load your network profile from the server. Retrying..."));
			}
			g_retryAtMs = GetTickCount64() + delay;
			Field(task, kTaskScreen) = kScreenFetching;
			return;
		}

		NetworkStallDiagnostics::IncidentLog("[ProfileSync] own profile download failed (%s) after %d retries\n",
			OutcomeName(outcome), kMaxAutoRetries);

		// The one case the backup exists for: the server answers fine and has nothing.
		// Restored and uploaded, so the server has the profile again.
		if (outcome == Outcome_NoData && !g_testFailureActive && RestoreBackup(netManager))
		{
			SetOwn(Own_RestoredBackup, "server had no data");
			TusLatch() = 0;
			GoTo(task, State_Upload);
			Field(task, kTaskScreen) = kScreenFetching;
			return;
		}

		Notify(HaveBackup()
			? L("The profile server isn't returning your profile right now. Your profile is not lost and is backed up locally. Choose OK to try again.")
			: L("The profile server isn't returning your profile right now. Your profile is not lost. Choose OK to try again; only choose Create New if this account has never played online."));
		GoTo(task, State_AskRetry);
		Field(task, kTaskScreen) = kScreenFetching;
	}

	// ---- The task ----

	// FUN_004B0970, reimplemented. Identical to the game except where marked.
	void __fastcall ProfileSyncTask(uint8_t* task)
	{
		void** const holder = Game<NetInterfaceHolderFn>(ADDR_NetInterfaceHolder)();
		void* const netInterface = *holder;
		void** const vtable = *reinterpret_cast<void***>(netInterface);
		uint8_t* const netObject = static_cast<uint8_t*>(reinterpret_cast<VirtualGetFn>(vtable[0x18 / 4])(netInterface));
		reinterpret_cast<VirtualVoidFn>((*reinterpret_cast<void***>(*holder))[0x1C / 4])(*holder);
		void* const netManager = Game<NetManagerFn>(ADDR_NetManager)();

		const bool entering = Field(task, kTaskEntering) != 0;

		switch (Field(task, kTaskState))
		{
		case State_Start:
			Field(task, kTaskDlcChecked) = 0;
			ResetRetries(); // [mod]
			GoTo(task, State_Fetch);
			Field(task, kTaskScreen) = kScreenNetwork;
			return;

		case State_InitError:
			if (entering)
			{
				ShowDialog(task, kMsgInitError, kButtonOk, 0, 0);
				return;
			}
			if (!DialogBusy(task))
			{
				Game<TaskSwitchFn>(ADDR_NetTaskSwitch)(task, 0xE);
			}
			return;

		case State_Fetch:
		{
			if (entering)
			{
				StartFetch(task, netManager);
				Field(task, kTaskScreen) = kScreenFetching;
				return;
			}

			// [mod] waiting out a retry delay, then the same thing entering does.
			if (g_retryAtMs != 0)
			{
				if (GetTickCount64() >= g_retryAtMs)
				{
					g_retryAtMs = 0;
					StartFetch(task, netManager);
				}
				Field(task, kTaskScreen) = kScreenFetching;
				return;
			}

			int32_t result = Game<FetchPollFn>(ADDR_OwnProfileFetchPoll)(netManager);
			g_testFailureActive = false;
			if (result != kPollPending && g_testFailuresDone < Settings::settingsIni.profileSyncTestFailures)
			{
				// TEST ONLY: leave things exactly as the loader's own failure does.
				++g_testFailuresDone;
				g_testFailureActive = true;
				uint8_t* const blob = OwnBlob(netManager);
				Game<BlobFn>(ADDR_DCodeBlobReset)(blob);
				*reinterpret_cast<int32_t*>(*reinterpret_cast<uint8_t**>(blob + OFFSET_ProfileLoaderPtr) + OFFSET_ProfileLoaderState) = kLoaderFailed;
				result = kPollFailed;
				NetworkStallDiagnostics::IncidentLog("[ProfileSync] TEST: forced own profile download failure %d/%d\n",
					g_testFailuresDone, Settings::settingsIni.profileSyncTestFailures);
			}
			if (result == kPollPending)
			{
				if (--Field(task, kTaskTimer) >= 0)
				{
					Field(task, kTaskScreen) = kScreenFetching;
					return;
				}
				// Out of time for this attempt: vanilla gives up without touching anything.
				TusLatch() = 1;
				SetOwn(Own_Failed, "download timed out"); // [mod]
				GoTo(task, State_InitError);
				Field(task, kTaskScreen) = kScreenFetching;
				return;
			}

			if (Field(netObject, 0xC) != 0 || result < kPollFailed)
			{
				TusLatch() = 0;
				// [mod] remember where the profile came from, and back it up.
				uint8_t* const blob = OwnBlob(netManager);
				if (LoaderState(blob) == kLoaderFromServer)
				{
					SetOwn(Own_FromServer, "downloaded");
					if (g_autoRetries > 0)
					{
						NetworkStallDiagnostics::IncidentLog("[ProfileSync] own profile loaded after %d retr%s\n",
							g_autoRetries, g_autoRetries == 1 ? "y" : "ies");
						Notify(L("Your network profile loaded."));
					}
					TakeSnapshotOnce(blob);
					const std::string restoreFrom = ReadPendingSnapshotRestore();
					if (!restoreFrom.empty() && RestoreSnapshot(restoreFrom, netManager))
					{
						ResetRetries();
						GoTo(task, State_Upload);
						Field(task, kTaskScreen) = kScreenFetching;
						return;
					}
					WriteBackup(blob, "downloaded from the server");
				}
				else
				{
					SetOwn(Own_Failed, "accepted without a profile");
				}
				ResetRetries();
				GoTo(task, State_Done);
				Field(task, kTaskScreen) = kScreenFetching;
				return;
			}

			// [mod] vanilla: try Save/bbdc.dat, else build a blank profile and ask
			// "create a new profile?" (state 7). See OnFetchFailed.
			OnFetchFailed(task, netManager);
			return;
		}

		case State_Upload:
		case State_UploadAfterDlc:
		{
			uint8_t* const blob = OwnBlob(netManager);
			const bool afterDlc = Field(task, kTaskState) == State_UploadAfterDlc;
			if (entering)
			{
				// FUN_004A96D0, behind the upload guard below.
				if (Game<UploadFn>(ADDR_UploadOwnProfile)(task) >= kPollFailed)
				{
					GoTo(task, afterDlc ? State_Done : State_InitError);
				}
				Field(task, kTaskTimer) = kFetchFrames;
				Field(task, kTaskScreen) = kScreenUploading;
				return;
			}

			--Field(task, kTaskTimer);
			void* const transfer = Game<SingletonFn>(ADDR_OnlineStorageTransferSingleton)();
			const int32_t result = Game<UploadPollFn>(ADDR_OwnProfileUploadPoll)(transfer, blob);
			if (afterDlc)
			{
				if (result != kPollPending || Field(task, kTaskTimer) < 0)
				{
					GoTo(task, State_Done);
				}
				Field(task, kTaskScreen) = kScreenUploading;
				return;
			}
			if (result == kPollPending)
			{
				if (Field(task, kTaskTimer) >= 0)
				{
					Field(task, kTaskScreen) = kScreenUploading;
					return;
				}
				GoTo(task, State_InitError);
			}
			else
			{
				GoTo(task, result < kPollFailed ? State_Done : State_InitError);
			}
			Field(task, kTaskScreen) = kScreenUploading;
			return;
		}

		case State_AskCreate:
			// Unreachable from this task; kept so a state written from outside behaves
			// as the game would.
			if (entering)
			{
				ShowDialog(task, kMsgAskCreate, kButtonOk, kButtonCancel, 0);
				return;
			}
			if (!DialogBusy(task))
			{
				if (Field(task, kTaskDialogAnswer) == kButtonOk)
				{
					TusLatch() = 0;
					SetOwn(Own_CreatedByPlayer, "chose create new"); // [mod]
					GoTo(task, State_Upload);
				}
				else
				{
					GoTo(task, State_AskReturn);
				}
			}
			return;

		case State_AskRetry:
			if (entering)
			{
				ShowDialog(task, kMsgAskRetry, kButtonOk, kButtonCancel, kButtonCreateNew);
				return;
			}
			if (!DialogBusy(task))
			{
				const int32_t answer = Field(task, kTaskDialogAnswer);
				if (answer == kButtonOk)
				{
					ResetRetries(); // [mod]
					GoTo(task, State_Fetch);
				}
				else
				{
					GoTo(task, answer == kButtonCancel ? State_AskReturn : State_AskRecreate);
				}
			}
			return;

		case State_AskRecreate:
			if (entering)
			{
				ShowDialog(task, kMsgAskRecreate, kButtonOk, kButtonCancel, 0);
				return;
			}
			if (!DialogBusy(task))
			{
				if (Field(task, kTaskDialogAnswer) == kButtonOk)
				{
					// [mod] vanilla went to the upload with the latch still set and no
					// default built, so it entered online with an empty profile that the
					// mod's latch auto-clear would later upload. Do what was asked.
					uint8_t* const blob = OwnBlob(netManager);
					Game<BlobFn>(ADDR_ProfileBuildDefault)(blob);
					TusLatch() = 0;
					SetOwn(Own_CreatedByPlayer, "confirmed create new twice");
					GoTo(task, State_Upload);
				}
				else
				{
					ResetRetries(); // [mod]
					GoTo(task, State_Fetch);
				}
			}
			return;

		case State_AskRestore:
			// Dormant in the game; nothing enters it.
			if (entering)
			{
				ShowDialog(task, kMsgAskRestore, kButtonOk, kButtonCancel, kButtonCreateNew);
				return;
			}
			if (!DialogBusy(task))
			{
				const int32_t answer = Field(task, kTaskDialogAnswer);
				if (answer == kButtonOk)
				{
					GoTo(task, State_Done);
				}
				else
				{
					GoTo(task, answer == kButtonCancel ? State_AskReturn : State_AskRecreate);
				}
			}
			return;

		case State_AskReturn:
			if (entering)
			{
				ShowDialog(task, kMsgAskReturn, kButtonOk, kButtonCancel, 0);
				return;
			}
			if (!DialogBusy(task))
			{
				if (Field(task, kTaskDialogAnswer) == kButtonOk)
				{
					GoTo(task, State_InitError);
				}
				else
				{
					ResetRetries(); // [mod]
					GoTo(task, State_Fetch);
				}
			}
			return;

		case State_DlcQuery:
		{
			void* const transfer = Game<SingletonFn>(ADDR_OnlineStorageTransferSingleton)();
			if (entering)
			{
				Game<ObjectVoidFn>(ADDR_DlcQueryStart)(transfer);
				Field(task, kTaskDlcChecked) = 1;
				Field(task, kTaskScreen) = kScreenNetwork;
				return;
			}
			if (Game<ObjectIntFn>(ADDR_DlcQueryPoll)(transfer) < 0)
			{
				Field(task, kTaskScreen) = kScreenNetwork;
				return;
			}
			Game<ObjectVoidFn>(ADDR_DlcRefresh)(Game<SingletonFn>(ADDR_DlcRefreshHolder)());
			uint8_t* const save = static_cast<uint8_t*>(Game<SingletonFn>(ADDR_DlcFlagsHolder)());
			bool newDlc = false;
			if (save[0x200270] != 0 && static_cast<uint8_t*>(Game<SingletonFn>(ADDR_DlcFlagsHolder)())[0x200271] != 0)
			{
				void* const queue = Game<SingletonFn>(ADDR_MessageQueueHolder)();
				newDlc = Game<MessagePostFn>(ADDR_MessageQueuePost)(queue, 0x20, -1) != 0;
			}
			GoTo(task, newDlc ? State_NewDlcNotice : State_Done);
			Field(netObject, 0x1170) =
				*reinterpret_cast<int32_t*>(static_cast<uint8_t*>(Game<SingletonFn>(ADDR_DlcFlagsHolder)()) + 0x20026C);
			Field(task, kTaskScreen) = kScreenNetwork;
			return;
		}

		case State_DlcNotice:
			if (entering)
			{
				ShowDialog(task, kMsgDlcNotice, kButtonOk, 0, 0);
				return;
			}
			if (!DialogBusy(task))
			{
				bool newDlc = false;
				if (static_cast<uint8_t*>(Game<SingletonFn>(ADDR_DlcFlagsHolder)())[0x200270] != 0 &&
					static_cast<uint8_t*>(Game<SingletonFn>(ADDR_DlcFlagsHolder)())[0x200271] != 0)
				{
					void* const queue = Game<SingletonFn>(ADDR_MessageQueueHolder)();
					newDlc = Game<MessagePostFn>(ADDR_MessageQueuePost)(queue, 0x20, -1) != 0;
				}
				GoTo(task, newDlc ? State_NewDlcNotice : State_Done);
			}
			return;

		case State_NewDlcNotice:
			if (entering)
			{
				ShowDialog(task, kMsgNewDlcNotice, kButtonOk, 0, 0);
				return;
			}
			if (!DialogBusy(task))
			{
				GoTo(task, State_Done);
			}
			return;

		case State_Done:
		{
			uint8_t* const dlcHolder = static_cast<uint8_t*>(Game<SingletonFn>(ADDR_DCodeDlcHolder)());
			void* const dlcState = reinterpret_cast<void*>(*reinterpret_cast<uintptr_t*>(dlcHolder + 0x29C4) + 0x576A8);
			if (Game<ObjectIntFn>(ADDR_DCodeDlcCheck)(dlcState) != 0)
			{
				Game<LogErrorFn>(ADDR_NetLogError)(reinterpret_cast<const char*>(Base() + ADDR_Str_DCodeDLCCheckError));
				GoTo(task, State_UploadAfterDlc);
				Field(task, kTaskScreen) = kScreenNetwork;
				return;
			}
			if (Field(task, kTaskDlcChecked) == 0)
			{
				GoTo(task, State_DlcQuery);
				Field(task, kTaskScreen) = kScreenNetwork;
				return;
			}
			uint8_t* const session = static_cast<uint8_t*>(Game<SingletonFn>(ADDR_NetSessionInfo)());
			if (Field(session, 0xA854) == 0)
			{
				Field(static_cast<uint8_t*>(Game<SingletonFn>(ADDR_NetSessionInfo)()), 0xA854) = 1;
				Game<VoidFn>(ADDR_NetSessionReady)();
			}
			Game<TaskSwitchFn>(ADDR_NetTaskSwitch)(task, 1);
			Field(task, kTaskScreen) = kScreenNetwork;
			return;
		}

		default:
			return;
		}
	}

	// ---- Upload guard: entry of FUN_004A96D0 ----

	DWORD g_uploadJmpBack = 0;
	int g_uploadBlocked = 0;
	bool g_blockNotified = false;

	int __cdecl UploadGuard()
	{
		if (!ProfileSync::OwnProfileSafeToUpload())
		{
			NetworkStallDiagnostics::IncidentLog("[ProfileSync] !!! blocked a profile upload: own profile is %s, loader %d\n",
				OwnName(g_own.load()), LoaderState(reinterpret_cast<uint8_t*>(g_gameVals.playerAvatarBaseAddr)));
			if (!g_blockNotified)
			{
				g_blockNotified = true;
				Notify(L("Blocked an upload of an empty network profile, so it can't overwrite your real profile on the server."));
			}
			return 1;
		}

		// The latch makes the game skip the upload by itself; only a real one is copied.
		if (TusLatch() == 0 && Settings::settingsIni.profileBackup && g_gameVals.playerAvatarBaseAddr != 0)
		{
			const uint8_t* const blob = reinterpret_cast<const uint8_t*>(g_gameVals.playerAvatarBaseAddr);
			const std::lock_guard<std::mutex> lock(g_pendingMutex);
			g_pendingUpload.assign(blob, blob + kProfileSize);
		}
		return 0;
	}

	void __declspec(naked) UploadOwnProfileHook()
	{
		__asm
		{
			pushfd
			pushad
			call UploadGuard
			mov g_uploadBlocked, eax
			popad
			popfd
			cmp g_uploadBlocked, 0
			jne blocked
			// the 9 bytes the JMP replaced
			push ebp
			mov ebp, esp
			sub esp, 0D0h
			jmp[g_uploadJmpBack]
		blocked:
			// what the function itself returns when the latch is set: nothing queued
			xor eax, eax
			ret
		}
	}

	// Refuses to patch anything but the exact bytes this was written against.
	bool BytesMatch(uintptr_t rva, const uint8_t* expected, size_t size)
	{
		const uint8_t* const at = reinterpret_cast<const uint8_t*>(Base() + rva);
		return !IsBadReadPtr(at, size) && memcmp(at, expected, size) == 0;
	}
}

void ProfileSync::InstallHooks()
{
	if (!Settings::settingsIni.profileSyncRewrite || g_installed)
	{
		return;
	}

	// FUN_004B0970: push ebx / push esi / push edi / mov esi,ecx / call ..., and the
	// `cmp eax,10h` of its state switch 0x29 bytes in.
	static const uint8_t kTaskHead[] = { 0x53, 0x56, 0x57, 0x8B, 0xF1, 0xE8 };
	static const uint8_t kTaskSwitch[] = { 0x83, 0xF8, 0x10, 0x0F, 0x87 };
	// FUN_004A96D0: push ebp / mov ebp,esp / sub esp,0D0h / mov eax,[__security_cookie]
	static const uint8_t kUploadHead[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, 0xA1 };
	if (!BytesMatch(ADDR_ProfileSyncTask, kTaskHead, sizeof(kTaskHead)) ||
		!BytesMatch(ADDR_ProfileSyncTask + 0x29, kTaskSwitch, sizeof(kTaskSwitch)) ||
		!BytesMatch(ADDR_UploadOwnProfile, kUploadHead, sizeof(kUploadHead)))
	{
		LOG(1, "[ProfileSync] game code differs from the RE'd build; profile sync left vanilla\n");
		return;
	}

	// The guard goes in first: without it the rewritten task must not run.
	g_uploadJmpBack = HookManager::SetHook("ProfileUploadGuard",
		static_cast<DWORD>(Base() + ADDR_UploadOwnProfile), static_cast<int>(SIZE_UploadOwnProfilePrologue),
		reinterpret_cast<void*>(UploadOwnProfileHook));
	if (g_uploadJmpBack == 0)
	{
		LOG(1, "[ProfileSync] upload guard not installed; profile sync left vanilla\n");
		return;
	}
	// Our function is entered by the game's own `call`, takes ecx and returns with a plain
	// `ret`, exactly like the original - so the JMP never comes back.
	if (HookManager::SetHook("ProfileSyncTask", static_cast<DWORD>(Base() + ADDR_ProfileSyncTask), 5,
		reinterpret_cast<void*>(ProfileSyncTask)) == 0)
	{
		LOG(1, "[ProfileSync] task replacement not installed; profile sync left vanilla (upload guard stays)\n");
	}
	g_installed = true;
	LOG(1, "[ProfileSync] network-entry profile sync replaced, upload guard installed\n");
}

bool ProfileSync::OwnProfileSafeToUpload()
{
	if (!g_installed)
	{
		return true;
	}
	const int own = g_own.load();
	if (own == Own_Loading || own == Own_Failed)
	{
		return false;
	}
	const int32_t loader = LoaderState(reinterpret_cast<const uint8_t*>(g_gameVals.playerAvatarBaseAddr));
	if (loader == kLoaderFailed)
	{
		return false;
	}
	if (loader == kLoaderDefault && own != Own_CreatedByPlayer)
	{
		return false;
	}
	return true;
}

void ProfileSync::OnProfileUploadConfirmed()
{
	std::vector<uint8_t> copy;
	{
		const std::lock_guard<std::mutex> lock(g_pendingMutex);
		copy.swap(g_pendingUpload);
	}
	if (copy.size() == kProfileSize)
	{
		// The copy was taken before FUN_004A2740 stamped this upload's save time into the
		// blob; carry it over, so the backup matches what the server now holds.
		if (g_gameVals.playerAvatarBaseAddr != 0)
		{
			memcpy(copy.data() + kProfileSaveTime,
				reinterpret_cast<const uint8_t*>(g_gameVals.playerAvatarBaseAddr) + kProfileSaveTime, sizeof(uint64_t));
		}
		WriteBackup(copy.data(), "upload confirmed by the server");
	}
}

bool ProfileSync::HasSnapshot(const std::string& version)
{
	const uint64_t steamId = SteamId();
	return steamId != 0 && GetFileAttributesW(SnapshotProfilePath(version, steamId).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ProfileSync::ScheduleSnapshotRestore(const std::string& version)
{
	if (!g_installed || !HasSnapshot(version))
	{
		return false;
	}
	HANDLE file = CreateFileW(PendingSnapshotMarker().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		return false;
	}
	DWORD written = 0;
	const std::string line = version + "\n";
	const BOOL ok = WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
	CloseHandle(file);
	return ok != FALSE;
}

void ProfileSync::CancelSnapshotRestore()
{
	DeleteFileW(PendingSnapshotMarker().c_str());
}

std::string ProfileSync::PendingSnapshotRestore()
{
	return ReadPendingSnapshotRestore();
}

bool ProfileSync::IsInstalled()
{
	return g_installed;
}
