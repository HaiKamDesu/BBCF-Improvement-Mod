#include "hooks_detours.h"

#include "HookManager.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "D3D9EXWrapper/ID3D9Wrapper_Sprite.h"
#include "D3D9EXWrapper/ID3DXWrapper_Effect.h"
#include "D3D9EXWrapper/ID3D9EXWrapper.h"
#include "Game/FrameStallDiagnostics.h"
#include "Hooks/hooks_bbcf.h"
#include "Network/RankedListConnectionFilter.h"
#include "SteamApiWrapper/steamApiWrappers.h"

#include <detours.h>
#include <sstream>
#include <tlhelp32.h>
#include <winver.h>

#pragma comment(lib, "detours.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")

typedef HRESULT(__stdcall* Direct3DCreate9Ex_t)(UINT SDKVersion, IDirect3D9Ex**);
typedef IDirect3D9* (__stdcall* Direct3DCreate9_t)(UINT SDKVersion);
typedef HRESULT(APIENTRY* D3DXCreateEffect_t)(LPDIRECT3DDEVICE9, LPCVOID, UINT, CONST D3DXMACRO*, LPD3DXINCLUDE, DWORD, LPD3DXEFFECTPOOL, LPD3DXEFFECT*, LPD3DXBUFFER*);
typedef HRESULT(WINAPI* D3DXCreateSprite_t)(LPDIRECT3DDEVICE9 pDevice, LPD3DXSPRITE* ppSprite);
typedef SteamAPICall_t(__fastcall* RequestLobbyList_t)(ISteamMatchmaking*);
typedef bool (WINAPI* SteamAPI_Init_t)();
typedef void (__cdecl* SteamAPI_RunCallbacks_t)();
typedef void (__cdecl* SteamAPI_RegisterCallResult_t)(class CCallbackBase* pCallback, SteamAPICall_t hAPICall);
typedef void (__cdecl* SteamAPI_UnregisterCallResult_t)(class CCallbackBase* pCallback, SteamAPICall_t hAPICall);
typedef HWND(__stdcall* CreateWindowExW_t)(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName,
	DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam);
typedef void* (__cdecl* SteamInternal_CreateInterface_t)(const char* ver);
typedef ISteamClient* (__cdecl* SteamClient_t)();
typedef ISteamUserStats* (__cdecl* SteamUserStats_t)();
typedef ISteamUserStats* (__cdecl* SteamAPI_ISteamClient_GetISteamUserStats_t)(intptr_t instancePtr, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion);
typedef bool (__cdecl* SteamAPI_ISteamUserStats_StoreStats_t)(intptr_t instancePtr);
typedef SteamAPICall_t (__cdecl* SteamAPI_ISteamUserStats_FindOrCreateLeaderboard_t)(intptr_t instancePtr, const char* pchLeaderboardName, ELeaderboardSortMethod eLeaderboardSortMethod, ELeaderboardDisplayType eLeaderboardDisplayType);
typedef SteamAPICall_t (__cdecl* SteamAPI_ISteamUserStats_FindLeaderboard_t)(intptr_t instancePtr, const char* pchLeaderboardName);
typedef SteamAPICall_t (__cdecl* SteamAPI_ISteamUserStats_UploadLeaderboardScore_t)(intptr_t instancePtr, SteamLeaderboard_t hSteamLeaderboard, ELeaderboardUploadScoreMethod eLeaderboardUploadScoreMethod, int32 nScore, const int32* pScoreDetails, int cScoreDetailsCount);

Direct3DCreate9Ex_t orig_Direct3DCreate9Ex;
Direct3DCreate9_t orig_Direct3DCreate9;

// Defined below; the import-table fallback needs its address before that point.
HRESULT __stdcall hook_Direct3DCreate9Ex(UINT sdkVers, IDirect3D9Ex** pD3DEx);
D3DXCreateEffect_t orig_D3DXCreateEffect;
D3DXCreateSprite_t orig_D3DXCreateSprite;
RequestLobbyList_t orig_RequestLobbyList;
SteamAPI_Init_t orig_SteamAPI_Init;
SteamAPI_RunCallbacks_t orig_SteamAPI_RunCallbacks;
SteamAPI_RegisterCallResult_t orig_SteamAPI_RegisterCallResult;
SteamAPI_UnregisterCallResult_t orig_SteamAPI_UnregisterCallResult;
CreateWindowExW_t orig_CreateWindowExW;
SteamInternal_CreateInterface_t orig_SteamInternal_CreateInterface;
SteamClient_t orig_SteamClient;
SteamUserStats_t orig_SteamUserStats;
SteamAPI_ISteamClient_GetISteamUserStats_t orig_SteamAPI_ISteamClient_GetISteamUserStats;
SteamAPI_ISteamUserStats_StoreStats_t orig_SteamAPI_ISteamUserStats_StoreStats;
SteamAPI_ISteamUserStats_FindOrCreateLeaderboard_t orig_SteamAPI_ISteamUserStats_FindOrCreateLeaderboard;
SteamAPI_ISteamUserStats_FindLeaderboard_t orig_SteamAPI_ISteamUserStats_FindLeaderboard;
SteamAPI_ISteamUserStats_UploadLeaderboardScore_t orig_SteamAPI_ISteamUserStats_UploadLeaderboardScore;

namespace
{
	constexpr SteamLeaderboard_t kRankAllLeaderboardHandle = static_cast<SteamLeaderboard_t>(1759932);

	// ---------------------------------------------------------------------
	// Hook installation diagnostics.
	//
	// "Successfully hooked X" only ever meant "GetProcAddress returned an
	// address"; the DetourFunction result was never checked, so a silently
	// failed detour was indistinguishable from a working one in DEBUG.txt.
	// These helpers report which module we actually patched, what the
	// prologue looked like before and after, and whether the trampoline
	// came back non-null.
	// ---------------------------------------------------------------------

	std::string FormatBytes(const BYTE* bytes, size_t count)
	{
		static const char* kHex = "0123456789ABCDEF";
		std::string out;
		out.reserve(count * 3);
		for (size_t i = 0; i < count; ++i)
		{
			if (i)
			{
				out.push_back(' ');
			}
			out.push_back(kHex[(bytes[i] >> 4) & 0xF]);
			out.push_back(kHex[bytes[i] & 0xF]);
		}
		return out;
	}

	// Reads up to 16 bytes at addr without faulting on unreadable pages.
	bool ReadPrologue(PBYTE addr, BYTE* out, size_t count)
	{
		if (!addr)
		{
			return false;
		}

		SIZE_T read = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), addr, out, count, &read) || read != count)
		{
			return false;
		}
		return true;
	}

	void LogPrologue(PBYTE addr, const char* funcName, const char* phase)
	{
		BYTE bytes[16] = {};
		if (!ReadPrologue(addr, bytes, sizeof(bytes)))
		{
			LOG(2, "[HookDiag] %s prologue %s: <unreadable at 0x%p>\n", funcName, phase, addr);
			return;
		}
		LOG(2, "[HookDiag] %s prologue %s: %s\n", funcName, phase, FormatBytes(bytes, sizeof(bytes)).c_str());
	}

	// A stock d3d9.dll reports CompanyName "Microsoft Corporation". A wrapper
	// (dgVoodoo, DXVK, ReShade, an OEM shim) almost never does, so this one
	// line usually identifies a hijacked module without anyone having to go
	// looking through the game folder by hand.
	std::string GetFileVersionSummary(const char* path)
	{
		DWORD ignored = 0;
		const DWORD size = GetFileVersionInfoSizeA(path, &ignored);
		if (!size)
		{
			return "<no version info>";
		}

		std::string buffer(size, '\0');
		if (!GetFileVersionInfoA(path, 0, size, &buffer[0]))
		{
			return "<version info read failed>";
		}

		struct LangCodePage { WORD language; WORD codePage; };
		LangCodePage* translation = nullptr;
		UINT translationLen = 0;
		if (!VerQueryValueA(&buffer[0], "\\VarFileInfo\\Translation",
			reinterpret_cast<LPVOID*>(&translation), &translationLen) || translationLen < sizeof(LangCodePage))
		{
			return "<no translation block>";
		}

		char subBlock[64] = {};
		std::ostringstream out;
		const char* kFields[] = { "CompanyName", "ProductName", "FileVersion", "FileDescription" };
		for (const char* field : kFields)
		{
			sprintf_s(subBlock, "\\StringFileInfo\\%04x%04x\\%s",
				translation->language, translation->codePage, field);

			char* value = nullptr;
			UINT valueLen = 0;
			if (VerQueryValueA(&buffer[0], subBlock, reinterpret_cast<LPVOID*>(&value), &valueLen) && value)
			{
				out << field << "='" << value << "' ";
			}
		}

		const std::string result = out.str();
		return result.empty() ? "<no string fields>" : result;
	}

	// Names the module that owns an arbitrary code address. This is what turns
	// "the prologue jumps to 0x74EE5AF0" into an actual culprit.
	std::string DescribeAddressOwner(PBYTE addr)
	{
		if (!addr)
		{
			return "<null>";
		}

		HMODULE owner = nullptr;
		if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(addr), &owner) || !owner)
		{
			return "<no owning module - heap/trampoline>";
		}

		char path[MAX_PATH] = {};
		if (!GetModuleFileNameA(owner, path, MAX_PATH))
		{
			strcpy_s(path, "<unknown>");
		}

		std::ostringstream out;
		out << "module='" << path << "' base=0x" << std::hex << (void*)owner
			<< " offset=+0x" << (DWORD)(addr - (PBYTE)owner);
		return out.str();
	}

	// If addr starts with a relative JMP (E9), decode it and report where it
	// actually lands and who owns that code.
	void LogJumpTarget(PBYTE addr, const char* funcName, const char* phase)
	{
		BYTE bytes[5] = {};
		if (!ReadPrologue(addr, bytes, sizeof(bytes)))
		{
			return;
		}

		if (bytes[0] != 0xE9)
		{
			LOG(0, "[HookDiag] %s (%s) does not start with a JMP - not currently hooked\n",
				funcName, phase);
			return;
		}

		LONG rel = 0;
		memcpy(&rel, &bytes[1], sizeof(rel));
		PBYTE target = addr + 5 + rel;
		LOG(0, "[HookDiag] %s (%s) JMPs to 0x%p -> %s\n",
			funcName, phase, target, DescribeAddressOwner(target).c_str());
	}

	void LogModuleIdentity(const char* moduleName)
	{
		HMODULE hModule = GetModuleHandleA(moduleName);
		if (!hModule)
		{
			LOG(0, "[HookDiag] Module '%s' is NOT loaded\n", moduleName);
			return;
		}

		char path[MAX_PATH] = {};
		if (!GetModuleFileNameA(hModule, path, MAX_PATH))
		{
			strcpy_s(path, "<GetModuleFileNameA failed>");
			LOG(0, "[HookDiag] Module '%s' base=0x%p path='%s'\n", moduleName, hModule, path);
			return;
		}

		LOG(0, "[HookDiag] Module '%s' base=0x%p path='%s'\n", moduleName, hModule, path);
		LOG(0, "[HookDiag] Module '%s' version: %s\n", moduleName, GetFileVersionSummary(path).c_str());
	}

	// Full loaded-module dump. On a stock install this is noise; on a broken
	// one it is the fastest way to spot a proxy d3d9/dxgi, an injected
	// overlay, or a translation layer sitting between the game and the
	// module we patched.
	void LogLoadedModules(const char* phase)
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
		if (snapshot == INVALID_HANDLE_VALUE)
		{
			LOG(0, "[HookDiag] Module snapshot (%s) failed err=0x%08X\n", phase, GetLastError());
			return;
		}

		// This project builds with UNICODE defined, so MODULEENTRY32 resolves to
		// the W variant and its strings are WCHAR. Narrow-printing them yields
		// one character per field, so convert explicitly.
		MODULEENTRY32W entry = {};
		entry.dwSize = sizeof(entry);
		int count = 0;
		if (Module32FirstW(snapshot, &entry))
		{
			do
			{
				char name[MAX_PATH] = {};
				char path[MAX_PATH] = {};
				WideCharToMultiByte(CP_UTF8, 0, entry.szModule, -1, name, sizeof(name), nullptr, nullptr);
				WideCharToMultiByte(CP_UTF8, 0, entry.szExePath, -1, path, sizeof(path), nullptr, nullptr);

				LOG(2, "[HookDiag][%s] Module[%d] base=0x%p size=%u name='%s' path='%s'\n",
					phase, count, entry.modBaseAddr, entry.modBaseSize, name, path);
				count++;
			} while (Module32NextW(snapshot, &entry));
		}
		CloseHandle(snapshot);
		LOG(0, "[HookDiag][%s] Loaded module count=%d\n", phase, count);
	}

	// Filenames that, sitting next to BBCF.exe, mean something is intercepting
	// the graphics or input path before the mod ever sees it.
	bool IsKnownWrapperDll(const std::string& lowerName)
	{
		static const char* kWrappers[] = {
			"d3d9.dll", "d3d8.dll", "d3d10.dll", "d3d11.dll", "d3d12.dll",
			"dxgi.dll", "opengl32.dll", "ddraw.dll", "dsound.dll", "winmm.dll",
			"version.dll", "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll",
			"reshade.dll", "reshade32.dll", "reshade64.dll",
			"dgvoodoo.dll", "dxvk.dll", "nvapi.dll", "nvapi64.dll",
		};

		for (const char* wrapper : kWrappers)
		{
			if (lowerName == wrapper)
			{
				return true;
			}
		}
		return false;
	}

	// Lists every DLL/ASI sitting next to BBCF.exe, flagging the ones that are
	// known proxy names. Answers "do you have ReShade or dgVoodoo installed?"
	// without having to ask the reporter to go check.
	void LogGameFolderContents()
	{
		char exePath[MAX_PATH] = {};
		if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH))
		{
			LOG(0, "[HookDiag] GetModuleFileNameA(NULL) failed err=0x%08X\n", GetLastError());
			return;
		}

		std::string folder(exePath);
		const size_t slash = folder.find_last_of("\\/");
		if (slash == std::string::npos)
		{
			LOG(0, "[HookDiag] Could not derive game folder from '%s'\n", exePath);
			return;
		}
		folder.erase(slash);

		LOG(0, "[HookDiag] Game exe='%s'\n", exePath);
		LOG(0, "[HookDiag] Game folder='%s'\n", folder.c_str());

		const std::string pattern = folder + "\\*.*";
		WIN32_FIND_DATAA findData = {};
		HANDLE find = FindFirstFileA(pattern.c_str(), &findData);
		if (find == INVALID_HANDLE_VALUE)
		{
			LOG(0, "[HookDiag] Game folder enumeration failed err=0x%08X\n", GetLastError());
			return;
		}

		int wrapperCount = 0;
		do
		{
			if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				continue;
			}

			std::string name(findData.cFileName);
			std::string lowerName;
			lowerName.reserve(name.size());
			for (char c : name)
			{
				lowerName.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
			}

			const bool isDll = lowerName.size() > 4 &&
				(lowerName.compare(lowerName.size() - 4, 4, ".dll") == 0 ||
				 lowerName.compare(lowerName.size() - 4, 4, ".asi") == 0);
			if (!isDll)
			{
				continue;
			}

			const bool isWrapper = IsKnownWrapperDll(lowerName);
			const std::string fullPath = folder + "\\" + name;
			LOG(0, "[HookDiag] GameFolderDll%s name='%s' size=%u version: %s\n",
				isWrapper ? " [KNOWN WRAPPER NAME]" : "",
				name.c_str(), findData.nFileSizeLow,
				GetFileVersionSummary(fullPath.c_str()).c_str());

			if (isWrapper)
			{
				wrapperCount++;
			}
		} while (FindNextFileA(find, &findData));

		FindClose(find);
		LOG(0, "[HookDiag] Game folder wrapper-named DLL count=%d\n", wrapperCount);
	}

	// GetVersionEx lies under compatibility shims; RtlGetVersion does not,
	// which matters when the report is "my PC came with a custom mini Windows".
	void LogOsVersion()
	{
		typedef LONG(WINAPI* RtlGetVersion_t)(PRTL_OSVERSIONINFOW);

		HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
		RtlGetVersion_t pRtlGetVersion = hNtdll
			? (RtlGetVersion_t)GetProcAddress(hNtdll, "RtlGetVersion")
			: nullptr;

		if (!pRtlGetVersion)
		{
			LOG(0, "[HookDiag] RtlGetVersion unavailable\n");
			return;
		}

		RTL_OSVERSIONINFOW info = {};
		info.dwOSVersionInfoSize = sizeof(info);
		if (pRtlGetVersion(&info) != 0)
		{
			LOG(0, "[HookDiag] RtlGetVersion failed\n");
			return;
		}

		LOG(0, "[HookDiag] OS version %u.%u build %u platform=%u\n",
			info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber, info.dwPlatformId);

		BOOL isWow64 = FALSE;
		IsWow64Process(GetCurrentProcess(), &isWow64);
		LOG(0, "[HookDiag] WOW64=%d\n", isWow64 ? 1 : 0);
	}

	// GPU name via EnumDisplayDevices, driver version/date/provider straight
	// from the display class registry key. Neither needs a working D3D device,
	// which is the whole point - the D3D path is what is broken.
	void LogDisplayAdapters()
	{
		DISPLAY_DEVICEA device = {};
		device.cb = sizeof(device);
		// No EDD_GET_DEVICE_INTERFACE_NAME: for adapters that swaps DeviceID
		// from the PCI\VEN_xxxx&DEV_xxxx hardware ID to an interface path,
		// and the hardware ID is what identifies the GPU.
		for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &device, 0); ++i)
		{
			LOG(0, "[HookDiag] DisplayAdapter[%u] name='%s' string='%s' id='%s' flags=0x%08X\n",
				i, device.DeviceName, device.DeviceString, device.DeviceID, device.StateFlags);
			device.cb = sizeof(device);
		}

		HKEY classKey = nullptr;
		const char* kDisplayClass =
			"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kDisplayClass, 0, KEY_READ, &classKey) != ERROR_SUCCESS)
		{
			LOG(0, "[HookDiag] Could not open display class registry key\n");
			return;
		}

		char subKeyName[64] = {};
		DWORD subKeyLen = sizeof(subKeyName);
		for (DWORD i = 0; RegEnumKeyExA(classKey, i, subKeyName, &subKeyLen,
			nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS; ++i)
		{
			subKeyLen = sizeof(subKeyName);

			HKEY driverKey = nullptr;
			if (RegOpenKeyExA(classKey, subKeyName, 0, KEY_READ, &driverKey) != ERROR_SUCCESS)
			{
				continue;
			}

			const char* kValues[] = { "DriverDesc", "DriverVersion", "DriverDate", "ProviderName" };
			std::ostringstream out;
			bool any = false;
			for (const char* valueName : kValues)
			{
				char data[256] = {};
				DWORD dataLen = sizeof(data);
				DWORD type = 0;
				if (RegQueryValueExA(driverKey, valueName, nullptr, &type,
					reinterpret_cast<LPBYTE>(data), &dataLen) == ERROR_SUCCESS && type == REG_SZ)
				{
					out << valueName << "='" << data << "' ";
					any = true;
				}
			}
			RegCloseKey(driverKey);

			if (any)
			{
				LOG(0, "[HookDiag] DisplayDriver[%s] %s\n", subKeyName, out.str().c_str());
			}
		}
		RegCloseKey(classKey);
	}

	void LogD3DDiagnostics(const char* phase)
	{
		LOG(0, "[HookDiag] === D3D diagnostics (%s) ===\n", phase);
		LogModuleIdentity("d3d9.dll");
		LogModuleIdentity("d3dx9_43.dll");
		LogModuleIdentity("dxgi.dll");
		LogModuleIdentity("d3d11.dll");

		HMODULE hM_d3d9 = GetModuleHandleA("d3d9.dll");
		if (hM_d3d9)
		{
			PBYTE pEx = (PBYTE)GetProcAddress(hM_d3d9, "Direct3DCreate9Ex");
			PBYTE pNonEx = (PBYTE)GetProcAddress(hM_d3d9, "Direct3DCreate9");
			LOG(0, "[HookDiag] d3d9 exports: Direct3DCreate9Ex=0x%p Direct3DCreate9=0x%p\n", pEx, pNonEx);
			LogPrologue(pEx, "Direct3DCreate9Ex", phase);
			LogJumpTarget(pEx, "Direct3DCreate9Ex", phase);
			LogPrologue(pNonEx, "Direct3DCreate9", phase);
			LogJumpTarget(pNonEx, "Direct3DCreate9", phase);
		}
	}

	// ---------------------------------------------------------------------
	// Hook eviction watchdog.
	//
	// On at least one reporter's machine another resident hooker owns the
	// d3d9 create functions, we detour over it, and it then re-applies its
	// own patch - silently dropping us out of the call chain before the game
	// ever creates a device. Snapshot what we wrote, then re-check cheaply
	// from the Steam callback pump so the log captures when it happens and
	// who took over.
	// ---------------------------------------------------------------------

	// ---------------------------------------------------------------------
	// Import-table fallback hook.
	//
	// nvd3d9wrap.dll (NVIDIA Optimus) patches the Direct3DCreate9Ex prologue
	// inside d3d9.dll. Both it and us fight over the same 5 bytes, and it
	// re-applies last, so our detour stops being in the call chain. It does
	// NOT touch the game's import table, though - so redirecting the IAT slot
	// the game actually calls through is uncontested and order-independent.
	//
	// The original value we displace still routes through nvd3d9wrap, so
	// Optimus keeps doing its GPU-selection job; we just stop being evicted
	// from the front of the chain.
	// ---------------------------------------------------------------------

	void** g_iatSlotDirect3DCreate9Ex = nullptr;
	Direct3DCreate9Ex_t g_iatDisplacedDirect3DCreate9Ex = nullptr;
	bool g_iatHookInstalled = false;

	// Walks a loaded module's import directory and returns the address of the
	// IAT slot for dllName!funcName, or null if the module does not import it
	// by name (e.g. it resolves the export via GetProcAddress instead).
	void** FindImportSlot(HMODULE module, const char* dllName, const char* funcName)
	{
		if (!module || !dllName || !funcName)
		{
			return nullptr;
		}

		PBYTE base = reinterpret_cast<PBYTE>(module);
		IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		{
			return nullptr;
		}

		IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
		{
			return nullptr;
		}

		const IMAGE_DATA_DIRECTORY& dir =
			nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress || !dir.Size)
		{
			return nullptr;
		}

		IMAGE_IMPORT_DESCRIPTOR* desc =
			reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);

		for (; desc->Name; ++desc)
		{
			const char* importedDll = reinterpret_cast<const char*>(base + desc->Name);
			if (_stricmp(importedDll, dllName) != 0)
			{
				continue;
			}

			// OriginalFirstThunk holds the names; FirstThunk is the live IAT.
			// A bound/stripped image can have no name table, in which case we
			// cannot match by name and must give up rather than guess.
			if (!desc->OriginalFirstThunk || !desc->FirstThunk)
			{
				LOG(0, "[HookDiag][IAT] '%s' imported but has no name thunk - cannot match by name\n",
					importedDll);
				continue;
			}

			IMAGE_THUNK_DATA* nameThunk =
				reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
			IMAGE_THUNK_DATA* addrThunk =
				reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

			for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addrThunk)
			{
				if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal))
				{
					continue;
				}

				IMAGE_IMPORT_BY_NAME* imported =
					reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + nameThunk->u1.AddressOfData);
				if (strcmp(reinterpret_cast<const char*>(imported->Name), funcName) == 0)
				{
					return reinterpret_cast<void**>(&addrThunk->u1.Function);
				}
			}
		}

		return nullptr;
	}

	// Only accept a slot whose current value lives in a real module and is
	// executable. If it points somewhere we cannot account for, we leave it
	// alone rather than risk corrupting a call the game depends on.
	bool IsPlausibleImportTarget(void* value)
	{
		if (!value)
		{
			return false;
		}

		HMODULE owner = nullptr;
		if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(value), &owner) || !owner)
		{
			return false;
		}

		MEMORY_BASIC_INFORMATION info = {};
		if (!VirtualQuery(value, &info, sizeof(info)) || info.State != MEM_COMMIT)
		{
			return false;
		}

		const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
			PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		return (info.Protect & executable) != 0;
	}

	// True when the export's prologue is already a JMP into some module other
	// than the one that exports it - i.e. somebody hooked it before we got
	// here, and we are about to patch on top of their patch. Must be called
	// BEFORE our own detour goes in, otherwise the owner is us.
	bool IsExportForeignOwned(PBYTE addr, HMODULE owningModule)
	{
		BYTE bytes[5] = {};
		if (!addr || !ReadPrologue(addr, bytes, sizeof(bytes)) || bytes[0] != 0xE9)
		{
			return false;
		}

		LONG rel = 0;
		memcpy(&rel, &bytes[1], sizeof(rel));
		PBYTE target = addr + 5 + rel;

		HMODULE targetOwner = nullptr;
		if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(target), &targetOwner))
		{
			return false;
		}

		return targetOwner != owningModule;
	}

	bool InstallD3DIatHook(const char* reason)
	{
		if (g_iatHookInstalled)
		{
			return true;
		}

		HMODULE gameModule = GetModuleHandleA(nullptr);
		void** slot = FindImportSlot(gameModule, "d3d9.dll", "Direct3DCreate9Ex");

		// BBCF.exe is Steam-DRM wrapped, so it is not guaranteed that the
		// import we want lives in the main image's table. If it does not,
		// sweep the loaded modules for whoever actually imports it - skipping
		// d3d9 itself and our own DLL (we link d3d9.lib, and patching our own
		// slot would make the hook call itself).
		if (!slot)
		{
			LOG(0, "[HookDiag][IAT] Main image does not import d3d9.dll!Direct3DCreate9Ex by name;"
				" sweeping other modules\n");

			HMODULE selfModule = nullptr;
			GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&InstallD3DIatHook), &selfModule);
			HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");

			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
			if (snapshot != INVALID_HANDLE_VALUE)
			{
				MODULEENTRY32W entry = {};
				entry.dwSize = sizeof(entry);
				if (Module32FirstW(snapshot, &entry))
				{
					do
					{
						HMODULE candidate = entry.hModule;
						if (candidate == selfModule || candidate == d3d9Module || candidate == gameModule)
						{
							continue;
						}

						void** found = FindImportSlot(candidate, "d3d9.dll", "Direct3DCreate9Ex");
						if (found)
						{
							char name[MAX_PATH] = {};
							WideCharToMultiByte(CP_UTF8, 0, entry.szExePath, -1, name, sizeof(name), nullptr, nullptr);
							LOG(0, "[HookDiag][IAT] Found slot in '%s'\n", name);
							slot = found;
							break;
						}
					} while (Module32NextW(snapshot, &entry));
				}
				CloseHandle(snapshot);
			}
		}

		if (!slot)
		{
			LOG(0, "[HookDiag][IAT] No import slot for d3d9.dll!Direct3DCreate9Ex anywhere"
				" - fallback unavailable (reason='%s')\n", reason);
			return false;
		}

		void* current = *slot;
		LOG(0, "[HookDiag][IAT] Slot at 0x%p currently -> 0x%p (%s)\n",
			slot, current, DescribeAddressOwner(reinterpret_cast<PBYTE>(current)).c_str());

		if (!IsPlausibleImportTarget(current))
		{
			LOG(0, "[HookDiag][IAT] Slot value is not a plausible code address - refusing to patch\n");
			return false;
		}

		if (current == reinterpret_cast<void*>(&hook_Direct3DCreate9Ex))
		{
			LOG(0, "[HookDiag][IAT] Slot already points at our hook\n");
			g_iatHookInstalled = true;
			return true;
		}

		DWORD oldProtect = 0;
		if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
		{
			LOG(0, "[HookDiag][IAT] VirtualProtect failed err=0x%08X - fallback not installed\n",
				GetLastError());
			return false;
		}

		g_iatDisplacedDirect3DCreate9Ex = reinterpret_cast<Direct3DCreate9Ex_t>(current);
		*slot = reinterpret_cast<void*>(&hook_Direct3DCreate9Ex);

		DWORD ignored = 0;
		VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
		FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

		g_iatSlotDirect3DCreate9Ex = slot;
		g_iatHookInstalled = true;

		LOG(0, "[HookDiag][IAT] Fallback hook INSTALLED (reason='%s'); displaced target 0x%p\n",
			reason, current);
		return true;
	}

	struct WatchedHook
	{
		PBYTE target = nullptr;
		const char* name = nullptr;
		BYTE expected[5] = {};
		bool valid = false;
		bool isDirect3DCreate9Ex = false;
		int reportsRemaining = 5;
	};

	WatchedHook g_watchedD3DCreate9Ex;
	WatchedHook g_watchedD3DCreate9;

	void ArmWatchdog(WatchedHook& watch, PBYTE target, const char* name, bool isDirect3DCreate9Ex = false)
	{
		watch.target = target;
		watch.name = name;
		watch.isDirect3DCreate9Ex = isDirect3DCreate9Ex;
		watch.valid = target && ReadPrologue(target, watch.expected, sizeof(watch.expected));
		if (watch.valid)
		{
			LOG(2, "[HookDiag] Watchdog armed for %s expecting %s\n",
				name, FormatBytes(watch.expected, sizeof(watch.expected)).c_str());
		}
	}

	void CheckWatchdog(WatchedHook& watch)
	{
		if (!watch.valid || watch.reportsRemaining <= 0)
		{
			return;
		}

		BYTE current[5] = {};
		if (!ReadPrologue(watch.target, current, sizeof(current)))
		{
			return;
		}

		if (memcmp(current, watch.expected, sizeof(current)) == 0)
		{
			return;
		}

		watch.reportsRemaining--;
		LOG(0, "[HookDiag] HOOK EVICTED: %s prologue changed from %s to %s\n",
			watch.name,
			FormatBytes(watch.expected, sizeof(watch.expected)).c_str(),
			FormatBytes(current, sizeof(current)).c_str());
		LogJumpTarget(watch.target, watch.name, "evicted");

		// enum: 0 = Off, 1 = Automatic, 2 = Always (settings.def D3D9IatFallbackHook)
		if (watch.isDirect3DCreate9Ex && Settings::settingsIni.d3d9IatFallbackHook != 0)
		{
			InstallD3DIatHook("export detour evicted");
		}

		// Track the new value so a second overwrite is reported too, rather
		// than spamming the same transition every frame.
		memcpy(watch.expected, current, sizeof(current));
	}

	void CheckD3DWatchdogs()
	{
		CheckWatchdog(g_watchedD3DCreate9Ex);
		CheckWatchdog(g_watchedD3DCreate9);
	}

	// Installs a detour and reports the outcome honestly: the trampoline
	// pointer plus a before/after prologue comparison. If the prologue is
	// unchanged after the call, the hook is not live no matter what
	// DetourFunction returned.
	PBYTE InstallDetour(PBYTE target, PBYTE hook, const char* funcName)
	{
		if (!target)
		{
			LOG(0, "[HookDiag] %s: no target address, detour NOT installed\n", funcName);
			return nullptr;
		}

		BYTE before[16] = {};
		const bool readBefore = ReadPrologue(target, before, sizeof(before));
		LogPrologue(target, funcName, "pre-detour");

		PBYTE trampoline = (PBYTE)DetourFunction(target, hook);

		if (!trampoline)
		{
			LOG(0, "[HookDiag] DetourFunction FAILED for %s (target=0x%p) - hook is NOT installed\n",
				funcName, target);
		}
		else
		{
			LOG(0, "[HookDiag] DetourFunction OK for %s target=0x%p trampoline=0x%p\n",
				funcName, target, trampoline);
		}

		LogPrologue(target, funcName, "post-detour");

		BYTE after[16] = {};
		if (readBefore && ReadPrologue(target, after, sizeof(after)))
		{
			if (memcmp(before, after, sizeof(before)) == 0)
			{
				LOG(0, "[HookDiag] WARNING: %s prologue UNCHANGED after detour - target is not patched\n",
					funcName);
			}
		}

		return trampoline;
	}

	std::string FormatFlatDetails(const int32* details, int count)
	{
		std::ostringstream out;
		out << "[";
		for (int i = 0; i < 16; ++i)
		{
			if (i != 0)
			{
				out << ",";
			}
			if (details && i < count)
			{
				out << details[i];
			}
			else
			{
				out << "-";
			}
		}
		out << "]";
		return out.str();
	}

	std::string GetFlatLeaderboardLabel(SteamLeaderboard_t handle)
	{
		const std::string knownName = GetLeaderboardHandleName(handle);
		std::ostringstream out;
		out << "handle=" << static_cast<unsigned long long>(handle);
		if (!knownName.empty())
		{
			out << " name='" << knownName << "'";
		}
		else if (handle == kRankAllLeaderboardHandle)
		{
			out << " name='RANK_ALL'";
		}
		return out.str();
	}
}

static bool HookOptionalDetour(PBYTE addr, const char* funcName)
{
	if (!addr)
	{
		LOG(2, "[STEAM][OptionalHook] Skipping missing export %s\n", funcName ? funcName : "<null>");
		return true;
	}

	// Note: this only reports that the export resolved. Whether the detour
	// actually got installed is reported separately by InstallDetour.
	LOG(2, "Resolved export %s at 0x%p\n", funcName, addr);
	return true;
}

HRESULT __stdcall hook_Direct3DCreate9Ex(UINT sdkVers, IDirect3D9Ex** pD3DEx)
{
	LOG(1, "Direct3DCreate9EX pD3DEx: 0x%p\n", pD3DEx);

	// Reachable two ways: the export detour, or the import-table fallback when
	// another hooker evicted that detour. Prefer the Detours trampoline - it
	// stays valid even after eviction, because it holds a copy of the bytes as
	// they were when we patched (i.e. still chaining into whoever hooked the
	// export before us). Only fall back to the displaced IAT value if the
	// export detour never got installed at all.
	Direct3DCreate9Ex_t chain = orig_Direct3DCreate9Ex
		? orig_Direct3DCreate9Ex
		: g_iatDisplacedDirect3DCreate9Ex;

	if (!chain)
	{
		LOG(0, "Direct3DCreate9Ex hook called without original function\n");
		return E_FAIL;
	}
	HRESULT retval = chain(sdkVers, pD3DEx); // real one

	if (SUCCEEDED(retval) && pD3DEx && *pD3DEx)
	{
		Direct3D9ExWrapper* ret = new Direct3D9ExWrapper(&*pD3DEx);
	}
	return retval;
}

// Diagnostic only: the mod wraps the D3D9Ex path exclusively. If a machine
// ends up on the legacy non-Ex entry point instead, nothing downstream ever
// reaches the wrapper and the overlay never appears. This hook creates no
// wrapper - it only records that the call happened.
IDirect3D9* __stdcall hook_Direct3DCreate9(UINT sdkVers)
{
	LOG(0, "[HookDiag] Direct3DCreate9 (non-Ex) called - the mod only wraps the Ex path\n");
	if (!orig_Direct3DCreate9)
	{
		LOG(0, "[HookDiag] Direct3DCreate9 hook called without original function\n");
		return nullptr;
	}
	IDirect3D9* result = orig_Direct3DCreate9(sdkVers);
	LOG(0, "[HookDiag] Direct3DCreate9 returned 0x%p\n", result);
	return result;
}

HRESULT APIENTRY hook_D3DXCreateEffect(LPDIRECT3DDEVICE9 pDevice, LPCVOID pSrcData, UINT SrcDataLen,
	CONST D3DXMACRO* pDefines, LPD3DXINCLUDE pInclude, DWORD Flags, LPD3DXEFFECTPOOL pPool, LPD3DXEFFECT* ppEffect,
	LPD3DXBUFFER* ppCompilationErrors)
{
	LOG(7, "D3DXCreateEffect\n");
	if (!orig_D3DXCreateEffect)
	{
		return E_FAIL;
	}
	HRESULT hR = orig_D3DXCreateEffect(pDevice, pSrcData, SrcDataLen, pDefines, pInclude, Flags, pPool, ppEffect, ppCompilationErrors);
	if (SUCCEEDED(hR) && ppEffect && *ppEffect)
	{
		ID3DXEffectWrapper* ret = new ID3DXEffectWrapper(&ppEffect);
	}

	return hR;
}

HRESULT WINAPI hook_D3DXCreateSprite(LPDIRECT3DDEVICE9 pDevice, LPD3DXSPRITE* ppSprite)
{
	LOG(7, "D3DXCreateSprite\n");
	if (!orig_D3DXCreateSprite)
	{
		return E_FAIL;
	}
	HRESULT hR = orig_D3DXCreateSprite(pDevice, ppSprite);
	if (SUCCEEDED(hR) && ppSprite && *ppSprite)
	{
		ID3DXSpriteWrapper* ret = new ID3DXSpriteWrapper(&ppSprite);
	}
	return hR;
}

bool __cdecl hook_SteamAPI_ISteamUserStats_StoreStats(intptr_t instancePtr)
{
	LOG(2, "[STEAM][FlatUserStats] StoreStats instance=0x%p\n", reinterpret_cast<void*>(instancePtr));
	return orig_SteamAPI_ISteamUserStats_StoreStats ? orig_SteamAPI_ISteamUserStats_StoreStats(instancePtr) : false;
}

SteamAPICall_t __cdecl hook_SteamAPI_ISteamUserStats_FindOrCreateLeaderboard(intptr_t instancePtr, const char* pchLeaderboardName, ELeaderboardSortMethod eLeaderboardSortMethod, ELeaderboardDisplayType eLeaderboardDisplayType)
{
	const SteamAPICall_t call = orig_SteamAPI_ISteamUserStats_FindOrCreateLeaderboard ?
		orig_SteamAPI_ISteamUserStats_FindOrCreateLeaderboard(instancePtr, pchLeaderboardName, eLeaderboardSortMethod, eLeaderboardDisplayType) : 0;
	RegisterSteamApiCallLabel(call, std::string("FlatFindOrCreateLeaderboard:") + (pchLeaderboardName ? pchLeaderboardName : "<null>"));
	LOG(2, "[STEAM][FlatUserStats] FindOrCreateLeaderboard instance=0x%p name='%s' sort=%d display=%d call=%llu\n",
		reinterpret_cast<void*>(instancePtr),
		pchLeaderboardName ? pchLeaderboardName : "<null>",
		static_cast<int>(eLeaderboardSortMethod),
		static_cast<int>(eLeaderboardDisplayType),
		static_cast<unsigned long long>(call));
	return call;
}

void* __cdecl hook_SteamInternal_CreateInterface(const char* ver)
{
	void* const result = orig_SteamInternal_CreateInterface ? orig_SteamInternal_CreateInterface(ver) : nullptr;
	LOG(2, "[STEAM][FlatAcquire] SteamInternal_CreateInterface ver='%s' result=%p\n",
		ver ? ver : "<null>",
		result);
	if (ver && strcmp(ver, "SteamClient017") == 0)
	{
		ObserveSteamClientInterface(reinterpret_cast<ISteamClient*>(result), "SteamInternal_CreateInterface");
	}
	return result;
}

ISteamClient* __cdecl hook_SteamClient()
{
	ISteamClient* const result = orig_SteamClient ? orig_SteamClient() : nullptr;
	LOG(2, "[STEAM][FlatAcquire] SteamClient result=%p\n", result);
	ObserveSteamClientInterface(result, "SteamClient");
	return result;
}

ISteamUserStats* __cdecl hook_SteamUserStats()
{
	ISteamUserStats* const result = orig_SteamUserStats ? orig_SteamUserStats() : nullptr;
	LOG(2, "[STEAM][FlatAcquire] SteamUserStats result=%p\n", result);
	ObserveSteamUserStatsInterface(result, "SteamUserStats");
	return result;
}

ISteamUserStats* __cdecl hook_SteamAPI_ISteamClient_GetISteamUserStats(intptr_t instancePtr, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion)
{
	ISteamUserStats* const result = orig_SteamAPI_ISteamClient_GetISteamUserStats ?
		orig_SteamAPI_ISteamClient_GetISteamUserStats(instancePtr, hSteamUser, hSteamPipe, pchVersion) : nullptr;
	LOG(2, "[STEAM][FlatAcquire] GetISteamUserStats client=%p user=%d pipe=%d version='%s' result=%p\n",
		reinterpret_cast<void*>(instancePtr),
		static_cast<int>(hSteamUser),
		static_cast<int>(hSteamPipe),
		pchVersion ? pchVersion : "<null>",
		result);
	ObserveSteamUserStatsInterface(result, "SteamAPI_ISteamClient_GetISteamUserStats");
	return result;
}

SteamAPICall_t __cdecl hook_SteamAPI_ISteamUserStats_FindLeaderboard(intptr_t instancePtr, const char* pchLeaderboardName)
{
	const SteamAPICall_t call = orig_SteamAPI_ISteamUserStats_FindLeaderboard ?
		orig_SteamAPI_ISteamUserStats_FindLeaderboard(instancePtr, pchLeaderboardName) : 0;
	RegisterSteamApiCallLabel(call, std::string("FlatFindLeaderboard:") + (pchLeaderboardName ? pchLeaderboardName : "<null>"));
	LOG(2, "[STEAM][FlatUserStats] FindLeaderboard instance=0x%p name='%s' call=%llu\n",
		reinterpret_cast<void*>(instancePtr),
		pchLeaderboardName ? pchLeaderboardName : "<null>",
		static_cast<unsigned long long>(call));
	return call;
}

SteamAPICall_t __cdecl hook_SteamAPI_ISteamUserStats_UploadLeaderboardScore(intptr_t instancePtr, SteamLeaderboard_t hSteamLeaderboard, ELeaderboardUploadScoreMethod eLeaderboardUploadScoreMethod, int32 nScore, const int32* pScoreDetails, int cScoreDetailsCount)
{
	LOG(2, "[RANK][UploadObserved] reason='FlatUploadLeaderboardScore' instance=0x%p %s method=%d score=%d detailsCount=%d details=%s\n",
		reinterpret_cast<void*>(instancePtr),
		GetFlatLeaderboardLabel(hSteamLeaderboard).c_str(),
		static_cast<int>(eLeaderboardUploadScoreMethod),
		nScore,
		cScoreDetailsCount,
		FormatFlatDetails(pScoreDetails, cScoreDetailsCount).c_str());

	if (hSteamLeaderboard == kRankAllLeaderboardHandle)
	{
		RankedProbeNoteUpload();
		RankedProbeDumpSummary("FlatUploadLeaderboardScore:RANK_ALL");
	}

	const SteamAPICall_t call = orig_SteamAPI_ISteamUserStats_UploadLeaderboardScore ?
		orig_SteamAPI_ISteamUserStats_UploadLeaderboardScore(instancePtr, hSteamLeaderboard, eLeaderboardUploadScoreMethod, nScore, pScoreDetails, cScoreDetailsCount) : 0;
	RegisterSteamApiCallLabel(call, std::string("FlatUploadLeaderboardScore:") + GetFlatLeaderboardLabel(hSteamLeaderboard));
	LOG(2, "[STEAM][FlatUserStats] UploadLeaderboardScore instance=0x%p %s method=%d score=%d detailsCount=%d details=%s call=%llu\n",
		reinterpret_cast<void*>(instancePtr),
		GetFlatLeaderboardLabel(hSteamLeaderboard).c_str(),
		static_cast<int>(eLeaderboardUploadScoreMethod),
		nScore,
		cScoreDetailsCount,
		FormatFlatDetails(pScoreDetails, cScoreDetailsCount).c_str(),
		static_cast<unsigned long long>(call));
	return call;
}

DWORD SteamMatchmakingFuncJmpBackAddr = 0;
void __declspec(naked)GetSteamMatchmaking()
{
	LOG_ASM(2, "GetSteamMatchmaking\n");

	__asm
	{
		call dword ptr[eax + 28h]
		/////
		pushad
		add esi, 10h
		mov g_tempVals.ppSteamMatchmaking, esi
		popad
		/////
		mov[esi + 10h], eax
		jmp[SteamMatchmakingFuncJmpBackAddr]
	}
}

DWORD SteamNetworkingFuncJmpBackAddr = 0;
void __declspec(naked)GetSteamNetworking()
{
	LOG_ASM(2, "GetSteamNetworking\n");

	__asm
	{
		call dword ptr[eax + 40h]
		/////
		pushad
		add esi, 20h
		mov g_tempVals.ppSteamNetworking, esi
		popad
		/////
		mov[esi + 20h], eax
		jmp[SteamNetworkingFuncJmpBackAddr]
	}
}

DWORD SteamUserFuncJmpBackAddr = 0;
void __declspec(naked)GetSteamUser()
{
	LOG_ASM(2, "GetSteamUser\n");

	__asm
	{
		call dword ptr[eax + 14h]
		/////
		pushad
		add esi, 4h
		mov g_tempVals.ppSteamUser, esi
		popad
		/////
		mov[esi + 4h], eax
		jmp[SteamUserFuncJmpBackAddr]
	}
}

DWORD SteamFriendsFuncJmpBackAddr = 0;
void __declspec(naked)GetSteamFriends()
{
	LOG_ASM(2, "GetSteamFriends\n");

	__asm
	{
		call dword ptr[eax + 20h]
		/////
		pushad
		add esi, 8h
		mov g_tempVals.ppSteamFriends, esi
		popad
		/////
		mov[esi + 8h], eax
		jmp[SteamFriendsFuncJmpBackAddr]
	}
}

DWORD SteamUtilsFuncJmpBackAddr = 0;
void __declspec(naked)GetSteamUtils()
{
	LOG_ASM(2, "GetSteamUtils\n");

	__asm
	{
		call dword ptr[eax + 24h]
		/////
		pushad
		add esi, 0Ch
		mov g_tempVals.ppSteamUtils, esi
		popad
		/////
		mov[esi + 0Ch], eax
		jmp[SteamUtilsFuncJmpBackAddr]
	}
}

DWORD SteamUserStatsFuncJmpBackAddr = 0;
void __declspec(naked)GetSteamUserStats()
{
	LOG_ASM(2, "GetSteamUserStats\n");

	__asm
	{
		call dword ptr[eax + 34h]
		mov[esi + 14h], eax
		/////
		pushad
		lea eax, [esi + 14h]
		mov g_tempVals.ppSteamUserStats, eax
		push eax
		call RefreshSteamUserStatsWrapperSlot
		add esp, 4
		popad
		/////
		jmp[SteamUserStatsFuncJmpBackAddr]
	}
}

bool WINAPI hook_SteamAPI_Init()
{
	LOG(1, "SteamAPI_Init\n");

	if (!orig_SteamAPI_Init)
	{
		LOG(2, "SteamAPI_Init hook called without original function\n");
		return false;
	}

	bool ret = orig_SteamAPI_Init();

	SteamMatchmakingFuncJmpBackAddr = HookManager::SetHook("SteamMatchmaking", "\xff\x50\x28\x89\x46\x10\x85\xc0", "xxxxxxxx", 6, GetSteamMatchmaking);
	
	SteamNetworkingFuncJmpBackAddr = HookManager::SetHook("SteamNetworking", "\xff\x50\x40\x89\x46\x20\x85\xc0", "xxxxxxxx", 6, GetSteamNetworking);
	
	SteamUserFuncJmpBackAddr = HookManager::SetHook("SteamUser", "\xff\x50\x14\x89\x46\x04", "xxxxxx", 6, GetSteamUser);
	
	SteamFriendsFuncJmpBackAddr = HookManager::SetHook("SteamFriends", "\xff\x50\x20\x89\x46\x08", "xxxxxx", 6, GetSteamFriends);
	
	SteamUtilsFuncJmpBackAddr = HookManager::SetHook("SteamUtils", "\xff\x50\x24\x89\x46\x0c", "xxxxxx", 6, GetSteamUtils);
	
	SteamUserStatsFuncJmpBackAddr = HookManager::SetHook("SteamUserStats", "\xff\x50\x34\x89\x46\x14", "xxxxxx", 6, GetSteamUserStats);

	return ret;
}

void __cdecl hook_SteamAPI_RunCallbacks()
{
	if (!orig_SteamAPI_RunCallbacks)
	{
		return;
	}

	// This hook runs on the render thread but outside EndScene, so without
	// the timing below it lands in the frame-stall report's unattributed
	// remainder - which is exactly where report 2's ~119ms stalls ended up.
	// Steam's own callback dispatch and our added work are timed separately:
	// the former is not our cost, but it can block, so it needs its own line.
	{
		FrameStallDiagnostics::ScopedSection section(FrameStallDiagnostics::Section_SteamNative);
		orig_SteamAPI_RunCallbacks();
	}

	FrameStallDiagnostics::ScopedSection section(FrameStallDiagnostics::Section_SteamPump);

	// Cheap 5-byte compare; reports at most a handful of times.
	CheckD3DWatchdogs();

	// Post-pump tick: while the ranked-list filter holds a proxied lobby-list
	// result (probing owners behind the game's native "Searching" popup), poll
	// probe progress and deliver the count-patched result to the game's own
	// handler once settled. Bounded internally by the probe timeout.
	RankedListConnectionFilter::GetInstance().OnSteamCallbacksPump();
}

void __cdecl hook_SteamAPI_RegisterCallResult(CCallbackBase* pCallback, SteamAPICall_t hAPICall)
{
	if (!orig_SteamAPI_RegisterCallResult)
	{
		return;
	}

	// Substitute the ranked-list filter's proxy for the game's handler when this
	// registration is for the lobby-list call the filter armed; everything else
	// (including the mod's own CCallResults) passes through untouched.
	CCallbackBase* const actual = RankedListConnectionFilter::GetInstance().SubstituteRegisterCallResult(
		pCallback, static_cast<uint64_t>(hAPICall));
	orig_SteamAPI_RegisterCallResult(actual, hAPICall);
}

void __cdecl hook_SteamAPI_UnregisterCallResult(CCallbackBase* pCallback, SteamAPICall_t hAPICall)
{
	if (!orig_SteamAPI_UnregisterCallResult)
	{
		return;
	}

	CCallbackBase* const actual = RankedListConnectionFilter::GetInstance().SubstituteUnregisterCallResult(
		pCallback, static_cast<uint64_t>(hAPICall));
	orig_SteamAPI_UnregisterCallResult(actual, hAPICall);
}

HWND WINAPI hook_CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName,
	DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
{
	LOG(7, "CreateWindowExW\n");
	if (!orig_CreateWindowExW)
	{
		LOG(2, "CreateWindowExW hook called without original function\n");
		return nullptr;
	}
	static int counter = 1;
	HWND hWnd = orig_CreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
	if (SUCCEEDED(hWnd))
	{
		LOG(7, "\tSuccess: 0x%p\n", hWnd);
		if (counter == 2) // 2nd created window should be the correct one according to process hacker
		{
			LOG(2, "Correct window: 0x%p\n", hWnd);
			g_gameProc.hWndGameWindow = hWnd;

			// Second sweep, now that the game is past early startup: catches a
			// d3d9/dxgi proxy loaded after placeHooks_detours ran, and shows
			// whether our Direct3DCreate9Ex patch is still in place or has been
			// overwritten by something that hooked after us.
			LogLoadedModules("gameWindow");
			LogD3DDiagnostics("gameWindow");
		}
	}
	counter++;
	return hWnd;
}

bool placeHooks_detours()
{
	LOG(1, "placeHooks_detours\n");

	LogOsVersion();
	LogDisplayAdapters();
	LogGameFolderContents();
	LogLoadedModules("placeHooks");
	LogD3DDiagnostics("placeHooks");

	HMODULE hM_d3d9 = GetModuleHandleA("d3d9.dll");
	HMODULE hM_d3dx9_43 = GetModuleHandleA("d3dx9_43.dll");
	HMODULE hM_steam_api = GetModuleHandleA("steam_api.dll");
	HMODULE hM_user32 = GetModuleHandleA("user32.dll");

	PBYTE pDirect3DCreate9Ex = hM_d3d9 ? (PBYTE)GetProcAddress(hM_d3d9, "Direct3DCreate9Ex") : nullptr;
	PBYTE pDirect3DCreate9 = hM_d3d9 ? (PBYTE)GetProcAddress(hM_d3d9, "Direct3DCreate9") : nullptr;
	PBYTE pD3DXCreateEffect = hM_d3dx9_43 ? (PBYTE)GetProcAddress(hM_d3dx9_43, "D3DXCreateEffect") : nullptr;
	PBYTE pD3DXCreateSprite = hM_d3dx9_43 ? (PBYTE)GetProcAddress(hM_d3dx9_43, "D3DXCreateSprite") : nullptr;
	PBYTE pSteamAPI_Init = hM_steam_api ? (PBYTE)GetProcAddress(hM_steam_api, "SteamAPI_Init") : nullptr;
	PBYTE pSteamAPI_RunCallbacks = hM_steam_api ? (PBYTE)GetProcAddress(hM_steam_api, "SteamAPI_RunCallbacks") : nullptr;
	PBYTE pSteamAPI_RegisterCallResult = hM_steam_api ? (PBYTE)GetProcAddress(hM_steam_api, "SteamAPI_RegisterCallResult") : nullptr;
	PBYTE pSteamAPI_UnregisterCallResult = hM_steam_api ? (PBYTE)GetProcAddress(hM_steam_api, "SteamAPI_UnregisterCallResult") : nullptr;
	// [DISABLED: Steam acquisition diagnostics - sections 58-65; all paths confirmed installed but zero calls observed; removing reduces injection surface]
	// PBYTE pSteamInternal_CreateInterface = (PBYTE)GetProcAddress(hM_steam_api, "SteamInternal_CreateInterface");
	// PBYTE pSteamClient = (PBYTE)GetProcAddress(hM_steam_api, "SteamClient");
	// PBYTE pSteamUserStats = (PBYTE)GetProcAddress(hM_steam_api, "SteamUserStats");
	// PBYTE pSteamAPI_ISteamClient_GetISteamUserStats = (PBYTE)GetProcAddress(hM_steam_api, "SteamAPI_ISteamClient_GetISteamUserStats");
	// PBYTE pSteamAPI_ISteamUserStats_StoreStats = (PBYTE)GetProcAddress(hM_steam_api, "SteamAPI_ISteamUserStats_StoreStats");
	// PBYTE pSteamAPI_ISteamUserStats_FindOrCreateLeaderboard = (PBYTE)GetProcAddress(hM_steam_api, "SteamAPI_ISteamUserStats_FindOrCreateLeaderboard");
	// PBYTE pSteamAPI_ISteamUserStats_FindLeaderboard = (PBYTE)GetProcAddress(hM_steam_api, "SteamAPI_ISteamUserStats_FindLeaderboard");
	PBYTE pSteamAPI_ISteamUserStats_UploadLeaderboardScore = hM_steam_api ? (PBYTE)GetProcAddress(hM_steam_api, "SteamAPI_ISteamUserStats_UploadLeaderboardScore") : nullptr;
	PBYTE pCreateWindowExW = hM_user32 ? (PBYTE)GetProcAddress(hM_user32, "CreateWindowExW") : nullptr;

	HookOptionalDetour((PBYTE)pDirect3DCreate9Ex, "Direct3DCreate9Ex");
	HookOptionalDetour((PBYTE)pDirect3DCreate9, "Direct3DCreate9");
	HookOptionalDetour((PBYTE)pD3DXCreateEffect, "D3DXCreateEffect");
	HookOptionalDetour((PBYTE)pD3DXCreateSprite, "D3DXCreateSprite");
	HookOptionalDetour((PBYTE)pSteamAPI_Init, "SteamAPI_Init");
	HookOptionalDetour((PBYTE)pSteamAPI_RunCallbacks, "SteamAPI_RunCallbacks");
	HookOptionalDetour((PBYTE)pSteamAPI_RegisterCallResult, "SteamAPI_RegisterCallResult");
	HookOptionalDetour((PBYTE)pSteamAPI_UnregisterCallResult, "SteamAPI_UnregisterCallResult");
	// [DISABLED: acquisition diagnostic HookOptionalDetour calls - sections 58-65]
	// if (!HookOptionalDetour((PBYTE)pSteamInternal_CreateInterface, "SteamInternal_CreateInterface")) return false;
	// if (!HookOptionalDetour((PBYTE)pSteamClient, "SteamClient")) return false;
	// if (!HookOptionalDetour((PBYTE)pSteamUserStats, "SteamUserStats")) return false;
	// if (!HookOptionalDetour((PBYTE)pSteamAPI_ISteamClient_GetISteamUserStats, "SteamAPI_ISteamClient_GetISteamUserStats")) return false;
	// if (!HookOptionalDetour((PBYTE)pSteamAPI_ISteamUserStats_StoreStats, "SteamAPI_ISteamUserStats_StoreStats")) return false;
	// if (!HookOptionalDetour((PBYTE)pSteamAPI_ISteamUserStats_FindOrCreateLeaderboard, "SteamAPI_ISteamUserStats_FindOrCreateLeaderboard")) return false;
	// if (!HookOptionalDetour((PBYTE)pSteamAPI_ISteamUserStats_FindLeaderboard, "SteamAPI_ISteamUserStats_FindLeaderboard")) return false;
	HookOptionalDetour((PBYTE)pSteamAPI_ISteamUserStats_UploadLeaderboardScore, "SteamAPI_ISteamUserStats_UploadLeaderboardScore");
	HookOptionalDetour((PBYTE)pCreateWindowExW, "CreateWindowExW");

	// Sampled before our own detour lands, or the answer is always "us".
	const bool foreignOwnerAtStartup = IsExportForeignOwned(pDirect3DCreate9Ex, hM_d3d9);
	if (foreignOwnerAtStartup)
	{
		LOG(0, "[HookDiag] Direct3DCreate9Ex is already hooked by another module before we install\n");
	}

	orig_Direct3DCreate9Ex = (Direct3DCreate9Ex_t)InstallDetour(pDirect3DCreate9Ex, (LPBYTE)hook_Direct3DCreate9Ex, "Direct3DCreate9Ex");
	orig_Direct3DCreate9 = (Direct3DCreate9_t)InstallDetour(pDirect3DCreate9, (LPBYTE)hook_Direct3DCreate9, "Direct3DCreate9");
	ArmWatchdog(g_watchedD3DCreate9Ex, pDirect3DCreate9Ex, "Direct3DCreate9Ex", true);
	ArmWatchdog(g_watchedD3DCreate9, pDirect3DCreate9, "Direct3DCreate9");

	// Decide up front whether the export is contested. Waiting for the
	// watchdog to observe an eviction is a race we can lose - on the reporting
	// machine nvd3d9wrap re-hooked ~330ms after us, before the Steam callback
	// pump had even started ticking - so if someone already owned the export
	// when we got here, install the import-table fallback immediately rather
	// than hoping to notice in time.
	//
	// enum: 0 = Off, 1 = Automatic, 2 = Always (settings.def D3D9IatFallbackHook)
	const int iatMode = Settings::settingsIni.d3d9IatFallbackHook;
	if (iatMode == 2)
	{
		InstallD3DIatHook("setting = Always");
	}
	else if (iatMode == 1 && foreignOwnerAtStartup)
	{
		InstallD3DIatHook("export already owned by a foreign module at startup");
	}
	else
	{
		LOG(2, "[HookDiag][IAT] Fallback not needed at startup (mode=%d, foreignOwner=%d)\n",
			iatMode, foreignOwnerAtStartup ? 1 : 0);
	}
	orig_D3DXCreateEffect = (D3DXCreateEffect_t)InstallDetour(pD3DXCreateEffect, (LPBYTE)hook_D3DXCreateEffect, "D3DXCreateEffect");
	orig_D3DXCreateSprite = (D3DXCreateSprite_t)InstallDetour(pD3DXCreateSprite, (LPBYTE)hook_D3DXCreateSprite, "D3DXCreateSprite");
	orig_SteamAPI_Init = (SteamAPI_Init_t)InstallDetour(pSteamAPI_Init, (LPBYTE)hook_SteamAPI_Init, "SteamAPI_Init");
	orig_SteamAPI_RunCallbacks = (SteamAPI_RunCallbacks_t)InstallDetour(pSteamAPI_RunCallbacks, (LPBYTE)hook_SteamAPI_RunCallbacks, "SteamAPI_RunCallbacks");
	orig_SteamAPI_RegisterCallResult = (SteamAPI_RegisterCallResult_t)InstallDetour(pSteamAPI_RegisterCallResult, (LPBYTE)hook_SteamAPI_RegisterCallResult, "SteamAPI_RegisterCallResult");
	orig_SteamAPI_UnregisterCallResult = (SteamAPI_UnregisterCallResult_t)InstallDetour(pSteamAPI_UnregisterCallResult, (LPBYTE)hook_SteamAPI_UnregisterCallResult, "SteamAPI_UnregisterCallResult");
	// [DISABLED: acquisition diagnostic DetourFunction installs - sections 58-65]
	// if (pSteamInternal_CreateInterface) orig_SteamInternal_CreateInterface = ...
	// if (pSteamClient) orig_SteamClient = ...
	// if (pSteamUserStats) orig_SteamUserStats = ...
	// if (pSteamAPI_ISteamClient_GetISteamUserStats) orig_SteamAPI_ISteamClient_GetISteamUserStats = ...
	// if (pSteamAPI_ISteamUserStats_StoreStats) orig_SteamAPI_ISteamUserStats_StoreStats = ...
	// if (pSteamAPI_ISteamUserStats_FindOrCreateLeaderboard) orig_SteamAPI_ISteamUserStats_FindOrCreateLeaderboard = ...
	// if (pSteamAPI_ISteamUserStats_FindLeaderboard) orig_SteamAPI_ISteamUserStats_FindLeaderboard = ...
	orig_SteamAPI_ISteamUserStats_UploadLeaderboardScore = (SteamAPI_ISteamUserStats_UploadLeaderboardScore_t)InstallDetour(pSteamAPI_ISteamUserStats_UploadLeaderboardScore, (LPBYTE)hook_SteamAPI_ISteamUserStats_UploadLeaderboardScore, "SteamAPI_ISteamUserStats_UploadLeaderboardScore");
	orig_CreateWindowExW = (CreateWindowExW_t)InstallDetour(pCreateWindowExW, (LPBYTE)hook_CreateWindowExW, "CreateWindowExW");

	return true;
}
