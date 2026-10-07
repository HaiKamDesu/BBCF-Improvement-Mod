#include "XInputRuntime.h"

namespace
{
        using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
        using XInputGetCapabilities_t = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

        XInputGetState_t ResolveXInputGetState()
        {
                static XInputGetState_t fn = []() -> XInputGetState_t {
                        const wchar_t* dlls[] = {
                                L"xinput9_1_0.dll",
                                L"xinput1_4.dll",
                                L"xinput1_3.dll",
                        };

                        for (const wchar_t* dll : dlls)
                        {
                                HMODULE module = LoadLibraryW(dll);
                                if (!module)
                                {
                                        continue;
                                }

                                auto* resolved = reinterpret_cast<XInputGetState_t>(
                                        GetProcAddress(module, "XInputGetState"));
                                if (resolved)
                                {
                                        return resolved;
                                }
                        }

                        return nullptr;
                }();

                return fn;
        }
}

static XInputGetCapabilities_t ResolveXInputGetCapabilities()
{
        static XInputGetCapabilities_t fn = []() -> XInputGetCapabilities_t {
                const wchar_t* dlls[] = {
                        L"xinput1_4.dll",
                        L"xinput1_3.dll",
                        L"xinput9_1_0.dll",
                };

                for (const wchar_t* dll : dlls)
                {
                        HMODULE module = LoadLibraryW(dll);
                        if (!module)
                        {
                                continue;
                        }

                        auto* resolved = reinterpret_cast<XInputGetCapabilities_t>(
                                GetProcAddress(module, "XInputGetCapabilities"));
                        if (resolved)
                        {
                                return resolved;
                        }
                }

                return nullptr;
        }();

        return fn;
}

DWORD XInputRuntime::GetState(DWORD userIndex, XINPUT_STATE* state)
{
        XInputGetState_t fn = ResolveXInputGetState();
        if (!fn)
        {
                return ERROR_DEVICE_NOT_CONNECTED;
        }

        return fn(userIndex, state);
}

DWORD XInputRuntime::GetCapabilities(DWORD userIndex, XINPUT_CAPABILITIES* capabilities)
{
        XInputGetCapabilities_t fn = ResolveXInputGetCapabilities();
        if (!fn)
        {
                return ERROR_DEVICE_NOT_CONNECTED;
        }

        return fn(userIndex, 0, capabilities);
}
