#include "crashdump.h"
#include "interfaces.h"
#include "logger.h"
#include "Settings.h"
#include "dllmain.h"
#include "ControllerOverrideManager.h"
#include "DirectInputWrapper.h"
#include "Localization.h"
#include "WineCheck.h"

#include "Hooks/hooks_detours.h"
#include "Hooks/hooks_battle_input.h"
#include "Hooks/hooks_system_input.h"
#include "Overlay/WindowManager.h"

#include <exception>
#include <mutex>
#include <Windows.h>

static void EarlyDebug(const char* msg)
{
	OutputDebugStringA(msg);

	// Also try a minimal file write next to the DLL (no CRT, no mutex, no std::string).
	HANDLE f = CreateFileA("BBCF_IM_early.txt", GENERIC_WRITE, FILE_SHARE_READ, nullptr,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f != INVALID_HANDLE_VALUE)
	{
		SetFilePointer(f, 0, nullptr, FILE_END);
		DWORD w = 0;
		WriteFile(f, msg, (DWORD)strlen(msg), &w, nullptr);
		WriteFile(f, "\r\n", 2, &w, nullptr);
		CloseHandle(f);
	}
}


HMODULE hOriginalDinput = nullptr;
DirectInput8Create_t orig_DirectInput8Create = nullptr;
bool LoadOriginalDinputDll();

// Ensures the original dinput8.dll is loaded before the game calls our export.
static std::once_flag g_dinputInitOnce;

static bool EnsureOriginalDinputLoaded()
{
	std::call_once(g_dinputInitOnce, []() {
		// This must be safe to call even if our init thread hasn't run yet.
		LoadOriginalDinputDll();
		});

	return orig_DirectInput8Create != nullptr;
}

HRESULT WINAPI DirectInput8Create(HINSTANCE hinstHandle, DWORD version, const IID& r_iid, LPVOID* outWrapper, LPUNKNOWN pUnk)
{
	EarlyDebug("[BBCF_IM] Entered DirectInput8Create");

	// Avoid relying on logging here: this can be called before settings/logger init.
	if (!EnsureOriginalDinputLoaded())
	{
		MessageBoxA(nullptr, "BBCF_IM: Failed to load original dinput8.dll (orig_DirectInput8Create is null).", "BBCFIM", MB_OK);
		return E_FAIL;
	}

	EarlyDebug("[BBCF_IM] Calling orig_DirectInput8Create...");

	HRESULT ret = orig_DirectInput8Create(hinstHandle, version, r_iid, outWrapper, pUnk);

	EarlyDebug("[BBCF_IM] Returned from orig_DirectInput8Create");

	if (SUCCEEDED(ret) && outWrapper && *outWrapper)
	{
		if (r_iid == IID_IDirectInput8A)
		{
			*outWrapper = new DirectInput8AWrapper(static_cast<IDirectInput8A*>(*outWrapper));
		}
		else if (r_iid == IID_IDirectInput8W)
		{
			*outWrapper = new DirectInput8WWrapper(static_cast<IDirectInput8W*>(*outWrapper));
		}
	}

	return ret;
}

void CreateCustomDirectories()
{
	LOG(1, "CreateCustomDirectories\n");

	CreateDirectory(L"BBCF_IM", NULL);
}

void BBCF_IM_Shutdown()
{
	LOG(1, "BBCF_IM_Shutdown\n");

	WindowManager::GetInstance().Shutdown();
	CleanupInterfaces();
	closeLogger();
}

bool LoadOriginalDinputDll()
{
	if (Settings::settingsIni.dinputDllWrapper == "none" || Settings::settingsIni.dinputDllWrapper == "")
	{
		char dllPath[MAX_PATH];
		GetSystemDirectoryA(dllPath, MAX_PATH);
		strcat_s(dllPath, "\\dinput8.dll");

		hOriginalDinput = LoadLibraryA(dllPath);
	}
	else
	{
		LOG(2, "Loading dinput wrapper: %s\n", Settings::settingsIni.dinputDllWrapper.c_str());
		hOriginalDinput = LoadLibraryA(Settings::settingsIni.dinputDllWrapper.c_str());
	}

	if (!hOriginalDinput)
	{
		return false;
	}

	orig_DirectInput8Create = (DirectInput8Create_t)GetProcAddress(hOriginalDinput, "DirectInput8Create");

	if (!orig_DirectInput8Create)
	{
		return false;
	}

	LOG(1, "orig_DirectInput8Create: 0x%p\n", orig_DirectInput8Create);

	return true;
}

DWORD WINAPI BBCF_IM_Start(HMODULE hModule)
{
	EarlyDebug("[BBCF_IM] BBCF_IM_Start thread entered");

	try
	{
		EarlyDebug("[BBCF_IM] About to load settings.ini");
		if (!Settings::loadSettingsFile())
		{
			ExitProcess(0);
		}
		EarlyDebug("[BBCF_IM] settings.ini loaded OK");
		EarlyDebug("[BBCF_IM] Wine check");
		const bool wineLikely = WineCheck();
		EarlyDebug("[BBCF_IM] Wine check OK");
		if (wineLikely && !Settings::settingsIni.ForceEnableControllerSettingHooks && Settings::settingsIni.EnableControllerHooks)
		{
			LOG(1, "Wine/Proton detected; disabling controller hooks before initialization.\n");
			Settings::changeSetting("EnableControllerHooks", "0");
			Settings::settingsIni.EnableControllerHooks = 0;
		}

		EarlyDebug("[BBCF_IM] Setting logging");
		SetLoggingEnabled(Settings::settingsIni.generateDebugLogs);
		EarlyDebug("[BBCF_IM] Set logging OK");
		ForceLog("[Init] Logging configured (generateDebugLogs=%d).\n", Settings::settingsIni.generateDebugLogs);

		if (Settings::WasDebugLoggingSettingMissing())
		{
			LOG(2, "GenerateDebugLogs setting missing in settings.ini; defaulting to enabled and adding it automatically.\n");
			Settings::changeSetting("GenerateDebugLogs", Settings::settingsIni.generateDebugLogs ? "1" : "0");
		}

		LOG(1, "Starting BBCF_IM_Start thread\n");
		ForceLog("[Init] Starting initialization thread.\n");

		CreateCustomDirectories();
		ForceLog("[Init] Custom directories ensured.\n");
		SetUnhandledExceptionFilter(UnhandledExFilter);
		ForceLog("[Init] Unhandled exception filter installed.\n");

		logSettingsIni();
		Settings::initSavedSettings();
		ForceLog("[Init] Settings initialized and saved settings loaded.\n");

		Localization::Initialize(Settings::settingsIni.language);
		ForceLog("[Init] Localization initialized for language %s.\n", Settings::settingsIni.language.c_str());

		if (!LoadOriginalDinputDll())
		{
			MessageBoxA(nullptr, "Could not load original dinput8.dll!", "BBCFIM", MB_OK);
			ForceLog("[Init] Failed to load original dinput8.dll; aborting.\n");
			ExitProcess(0);
		}

		EarlyDebug("[BBCF_IM] About to place detours hooks");
		if (!placeHooks_detours())
		{
			MessageBoxA(nullptr, "Failed IAT hook", "BBCFIM", MB_OK);
			ForceLog("[Init] Detours hook placement failed; aborting.\n");
			ExitProcess(0);
		}

		EarlyDebug("[BBCF_IM] Detours hooks placed OK");
		// Install battle input hook (P1/P2 input write site)
		if (!Hook_BattleInput())
		{
			// For now, don't hard-fail the entire mod - just log it.
			// If you prefer, you can pop a MessageBox+ExitProcess instead.
			LOG(2, "BBCF_IM_Start: Hook_BattleInput failed; P2 input PoC disabled.\n");
			ForceLog("[Init] Hook_BattleInput failed; continuing without P2 input hook.\n");
		}

		if (!InstallSystemInputHook())
		{
			LOG(2, "BBCF_IM_Start: InstallSystemInputHook failed; system input override disabled.\n");
			ForceLog("[Init] InstallSystemInputHook failed; continuing without system input override.\n");
		}

		LOG(1, "GetBbcfBaseAdress() = 0x%p\n", reinterpret_cast<void*>(GetBbcfBaseAdress()));
		ForceLog("[Init] Hooks installed; base address resolved to 0x%p.\n", reinterpret_cast<void*>(GetBbcfBaseAdress()));

		g_interfaces.pPaletteManager = new PaletteManager();
		ForceLog("[Init] PaletteManager constructed.\n");
	}
	catch (const std::exception& ex)
	{
		ForceLog("[Crash] Unhandled C++ exception: %s\n", ex.what());
	}
	catch (...)
	{
		ForceLog("[Crash] Unhandled non-standard exception in BBCF_IM_Start.\n");
	}

	return 0;
}

BOOL WINAPI DllMain(HMODULE hinstDLL, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
		case DLL_PROCESS_ATTACH:
		{
			EarlyDebug("[BBCF_IM] DllMain PROCESS_ATTACH begin");

			DisableThreadLibraryCalls(hinstDLL);
			EarlyDebug("[BBCF_IM] DisableThreadLibraryCalls done");

			SetUnhandledExceptionFilter(UnhandledExFilter);
			EarlyDebug("[BBCF_IM] UnhandledExceptionFilter installed in DllMain");

			HANDLE hThread = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)BBCF_IM_Start, hinstDLL, 0, nullptr);
			if (!hThread)
			{
				EarlyDebug("[BBCF_IM] CreateThread FAILED");
			}
			else
			{
				EarlyDebug("[BBCF_IM] CreateThread OK");
				CloseHandle(hThread);
				EarlyDebug("[BBCF_IM] CloseHandle(thread) OK");
			}

			EarlyDebug("[BBCF_IM] DllMain PROCESS_ATTACH end");
			break;
		}

		case DLL_PROCESS_DETACH:
			BBCF_IM_Shutdown();
			// Do NOT FreeLibrary(hOriginalDinput) here; the game may still be using it during shutdown.
			break;
	}
	return TRUE;
}