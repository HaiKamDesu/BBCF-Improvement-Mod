#include "WineCheck.h"

#include "logger.h"

#include <Windows.h>

bool WineCheck()
{
    HKEY hKey;
    LONG lRes = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WINE", 0, KEY_READ, &hKey);
    bool WineLikely = (lRes == ERROR_SUCCESS);

    if (WineLikely)
    {
        LOG(1, "Wine is Most Likely Being Used.\n");
    }
    else
    {
        LOG(1, "Wine Has Not Been Detected.\n");
    }

    return WineLikely;
}

