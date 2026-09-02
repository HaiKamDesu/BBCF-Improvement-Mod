#include "RuntimePlatform.h"

#include "Settings.h"

#include <Windows.h>

namespace
{
        bool HasEnvironmentVariable(const wchar_t* name)
        {
                return GetEnvironmentVariableW(name, nullptr, 0) > 0;
        }
}

bool IsWineOrProton()
{
        static int cached = -1;
        if (cached >= 0)
        {
                return cached != 0;
        }

        // The authoritative check: Wine and Proton always export this from ntdll, even in
        // prefixes where the SOFTWARE\WINE key is absent and no Proton variable is set.
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const bool wineNtdll = ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;

        HKEY hKey = nullptr;
        const bool wineRegistry =
                RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WINE", 0, KEY_READ, &hKey) == ERROR_SUCCESS;
        if (hKey)
        {
                RegCloseKey(hKey);
        }

        const bool protonEnv =
                HasEnvironmentVariable(L"STEAM_COMPAT_CLIENT_INSTALL_PATH") ||
                HasEnvironmentVariable(L"STEAM_COMPAT_DATA_PATH") ||
                HasEnvironmentVariable(L"PROTON_LOG");

        cached = (wineNtdll || wineRegistry || protonEnv) ? 1 : 0;
        return cached != 0;
}

bool IsSafeToUseControllerHooks()
{
        return !IsWineOrProton();
}

bool IsControllerHooksRuntimeAllowed()
{
        // The force flag has to be read BEFORE the platform check, not after it. Testing
        // the platform first made the flag unable to do the one thing it exists for -
        // turning the hooks back on under Wine - because that branch returned false before
        // the flag was ever looked at. Users setting it to 1 saw nothing happen.
        if (Settings::settingsIni.ForceEnableControllerSettingHooks)
        {
                return true;
        }

        if (!IsSafeToUseControllerHooks())
        {
                return false;
        }

        return Settings::settingsIni.EnableControllerHooks;
}

