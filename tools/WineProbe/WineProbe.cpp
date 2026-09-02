// WineProbe - one-shot capability report for the BBCF Improvement Mod on Wine/Proton.
//
// Answers, without the game running and without a GPU, every open question the
// Linux investigation left. Build 32-bit: the mod is Win32, and MFT/HID
// availability is per-bitness.
//
// Design rule: nothing that might be absent is statically imported. Every risky
// module is LoadLibrary'd so a missing DLL is a reported line, never a launch
// failure. This program must always run and always print a report.

#include <windows.h>
#include <setupapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------- output

static int g_indent = 0;
static void Section(const char* name)
{
    std::printf("\n========================================================\n");
    std::printf("  %s\n", name);
    std::printf("========================================================\n");
}
static void Line(const char* key, const char* fmt, ...)
{
    std::printf("  %-38s ", key);
    va_list a; va_start(a, fmt); std::vprintf(fmt, a); va_end(a);
    std::printf("\n");
}
static void Note(const char* fmt, ...)
{
    std::printf("      ");
    va_list a; va_start(a, fmt); std::vprintf(fmt, a); va_end(a);
    std::printf("\n");
}
static const char* YN(bool b) { return b ? "YES" : "NO"; }

// Collected as each section runs; printed as a skimmable verdict block at the end.
static struct {
    bool onWine = false;
    bool mfStaticImportsOk = false;
    int  wmaEncoders = -1;      // -1 = not tested
    bool opusDecoder = false;
    bool shellZipWorks = false;
    bool minidumpUsable = false;
    int  hidDevices = -1;
    int  hidWithContainerId = -1;
    bool devPropExported = false;
} g;

static std::string Narrow(const wchar_t* w)
{
    if (!w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
    return s;
}

// ---------------------------------------------------------------- GUIDs
// Defined by hand so the probe links without mfuuid/dxguid.

static const GUID MFMediaType_Audio_       = {0x73647561,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID MFAudioFormat_PCM_       = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID MFAudioFormat_Float_     = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID MFAudioFormat_WMAudioV8_ = {0x00000161,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID MFAudioFormat_WMAudioV9_ = {0x00000162,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID MFAudioFormat_Opus_      = {0x0000704F,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID MFAudioFormat_AAC_       = {0x00001610,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID MFAudioFormat_MP3_       = {0x00000055,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};

static const GUID MFT_CATEGORY_AUDIO_ENCODER_ = {0x91c64bd0,0xf91e,0x4d8c,{0x92,0x76,0xdb,0x24,0x82,0x79,0xd9,0x75}};
static const GUID MFT_CATEGORY_AUDIO_DECODER_ = {0x9ea73fb4,0xef7a,0x4559,{0x8d,0x5d,0x71,0x9d,0x8f,0x04,0x26,0xc7}};
static const GUID MFT_FRIENDLY_NAME_Attribute_= {0x314ffbae,0x5b41,0x4c95,{0x9c,0x19,0x4e,0x7d,0x58,0x6f,0xac,0xe3}};

// The exact CLSID the mod uses in AudioDecode.cpp.
static const CLSID CLSID_MSOpusDecoder_ = {0x63E17C10,0x2D43,0x4C42,{0x8F,0xE3,0x8D,0x8B,0x63,0xE4,0x6A,0x6A}};

static const CLSID CLSID_Shell_     = {0x13709620,0xC279,0x11CE,{0xA4,0x9E,0x44,0x45,0x53,0x54,0x00,0x00}};
static const IID   IID_IDispatch_   = {0x00020400,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

static const GUID GUID_DEVINTERFACE_HID_ = {0x4D1E55B2,0xF16F,0x11CF,{0x88,0xCB,0x00,0x11,0x11,0x00,0x00,0x30}};

typedef struct { GUID fmtid; DWORD pid; } DEVPROPKEY_T;
static const DEVPROPKEY_T DEVPKEY_Device_ContainerId_ =
    {{0x8c7ed206,0x3f8a,0x4827,{0xb3,0xab,0xae,0x9e,0x1f,0xae,0xfc,0x6c}}, 2};

// ---------------------------------------------------------------- 1. platform

static void ProbePlatform()
{
    Section("1. PLATFORM");

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    typedef const char* (CDECL *wine_get_version_t)(void);
    typedef void (CDECL *wine_get_host_version_t)(const char**, const char**);

    bool onWine = false;
    if (ntdll)
    {
        wine_get_version_t gv = (wine_get_version_t)GetProcAddress(ntdll, "wine_get_version");
        if (gv)
        {
            onWine = true;
            g.onWine = true;
            Line("Running under Wine", "YES  (version %s)", gv());
            wine_get_host_version_t ghv =
                (wine_get_host_version_t)GetProcAddress(ntdll, "wine_get_host_version");
            if (ghv)
            {
                const char* sysname = NULL; const char* release = NULL;
                ghv(&sysname, &release);
                Line("Host OS", "%s %s", sysname ? sysname : "?", release ? release : "?");
            }
        }
    }
    if (!onWine)
        Line("Running under Wine", "NO  (native Windows - baseline run)");

    static const wchar_t* kEnv[] = {
        L"STEAM_COMPAT_DATA_PATH", L"STEAM_COMPAT_CLIENT_INSTALL_PATH",
        L"SteamDeck", L"SteamOS", L"SteamGameId", L"SteamAppId",
        L"PROTON_USE_WINED3D", L"PROTON_LOG", L"WINEPREFIX", L"WINEDLLOVERRIDES",
        L"XDG_SESSION_TYPE", L"GAMESCOPE_WAYLAND_DISPLAY",
    };
    for (size_t i = 0; i < sizeof(kEnv)/sizeof(kEnv[0]); ++i)
    {
        wchar_t buf[1024] = {};
        DWORD n = GetEnvironmentVariableW(kEnv[i], buf, 1024);
        if (n > 0 && n < 1024)
            Line(Narrow(kEnv[i]).c_str(), "%s", Narrow(buf).c_str());
    }

    Line("ANSI code page (CP_ACP)", "%u", (unsigned)GetACP());
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(NULL, self, MAX_PATH);
    Line("Probe path", "%s", Narrow(self).c_str());
    Line("Probe bitness", "%u-bit", (unsigned)(sizeof(void*) * 8));
}

// ---------------------------------------------------------------- 2. modules

static void ProbeModules()
{
    Section("2. MODULE AVAILABILITY");
    Note("The mod statically imports mfplat/mfreadwrite/mf. If any of those");
    Note("is missing the DLL cannot load at all, with no error the user sees.");
    std::printf("\n");

    struct Entry { const wchar_t* dll; const char* why; };
    static const Entry kMods[] = {
        { L"mfplat.dll",      "STATIC IMPORT - audio decode/encode" },
        { L"mfreadwrite.dll", "STATIC IMPORT - source reader" },
        { L"mf.dll",          "STATIC IMPORT - media foundation core" },
        { L"dbghelp.dll",     "crash minidumps" },
        { L"dinput8.dll",     "controller enumeration" },
        { L"hid.dll",         "delay-loaded - HID strings" },
        { L"setupapi.dll",    "delay-loaded - device enumeration" },
        { L"cfgmgr32.dll",    "delay-loaded - device tree" },
        { L"d3dx9_43.dll",    "delay-loaded - sprite/effect hooks" },
        { L"xinput1_4.dll",   "pad state" },
        { L"xinput1_3.dll",   "pad state (fallback)" },
        { L"xinput9_1_0.dll", "pad state (fallback)" },
        { L"winmm.dll",       "joystick + timers" },
        { L"wininet.dll",     "update checks" },
        { L"shell32.dll",     "zip extraction, URL opening" },
        { L"comdlg32.dll",    "file dialogs" },
    };

    int staticOk = 0;
    for (size_t i = 0; i < sizeof(kMods)/sizeof(kMods[0]); ++i)
    {
        HMODULE h = LoadLibraryW(kMods[i].dll);
        if (h && i < 3) ++staticOk;   // mfplat / mfreadwrite / mf
        char key[80];
        std::snprintf(key, sizeof(key), "%s", Narrow(kMods[i].dll).c_str());
        if (h)
        {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(h, path, MAX_PATH);
            Line(key, "OK    (%s)", Narrow(path).c_str());
        }
        else
        {
            Line(key, "MISSING  err=%lu  <-- %s", GetLastError(), kMods[i].why);
        }
    }
    g.mfStaticImportsOk = (staticOk == 3);
}

// ---------------------------------------------------------------- 3. media foundation

typedef HRESULT (STDAPICALLTYPE *MFStartup_t)(ULONG, DWORD);
typedef HRESULT (STDAPICALLTYPE *MFShutdown_t)(void);
typedef HRESULT (STDAPICALLTYPE *MFTEnumEx_t)(GUID, UINT32,
    const MFT_REGISTER_TYPE_INFO*, const MFT_REGISTER_TYPE_INFO*,
    IMFActivate***, UINT32*);

static void DumpActivates(IMFActivate** acts, UINT32 count)
{
    for (UINT32 i = 0; i < count; ++i)
    {
        if (!acts[i]) continue;
        WCHAR* name = NULL; UINT32 len = 0;
        if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute_, &name, &len)) && name)
        {
            Note("- %s", Narrow(name).c_str());
            CoTaskMemFree(name);
        }
        else
        {
            Note("- (unnamed MFT)");
        }
    }
}

static UINT32 CountEncoders(MFTEnumEx_t enumEx, const GUID& outSubtype, const char* label)
{
    MFT_REGISTER_TYPE_INFO in  = { MFMediaType_Audio_, MFAudioFormat_PCM_ };
    MFT_REGISTER_TYPE_INFO out = { MFMediaType_Audio_, outSubtype };
    IMFActivate** acts = NULL; UINT32 n = 0;
    HRESULT hr = enumEx(MFT_CATEGORY_AUDIO_ENCODER_, MFT_ENUM_FLAG_ALL, &in, &out, &acts, &n);
    if (FAILED(hr))
    {
        Line(label, "ENUM FAILED  hr=0x%08lX", (unsigned long)hr);
        return 0;
    }
    Line(label, "%u encoder(s)", n);
    if (n) DumpActivates(acts, n);
    for (UINT32 i = 0; i < n; ++i) if (acts[i]) acts[i]->Release();
    if (acts) CoTaskMemFree(acts);
    return n;
}

static void ProbeMediaFoundation()
{
    Section("3. MEDIA FOUNDATION  <-- the custom-music blocker");

    HMODULE mfplat = LoadLibraryW(L"mfplat.dll");
    if (!mfplat) { Line("mfplat.dll", "MISSING - nothing here can be tested"); return; }

    MFStartup_t  pStartup  = (MFStartup_t) GetProcAddress(mfplat, "MFStartup");
    MFShutdown_t pShutdown = (MFShutdown_t)GetProcAddress(mfplat, "MFShutdown");
    MFTEnumEx_t  pEnumEx   = (MFTEnumEx_t) GetProcAddress(mfplat, "MFTEnumEx");
    Line("MFStartup exported",  "%s", YN(pStartup  != NULL));
    Line("MFTEnumEx exported",  "%s", YN(pEnumEx   != NULL));
    if (!pStartup || !pEnumEx) return;

    HRESULT hr = pStartup(((0x0002 << 16) | 0x0070), 0); // MF_VERSION, MFSTARTUP_FULL
    Line("MFStartup", "hr=0x%08lX %s", (unsigned long)hr, SUCCEEDED(hr) ? "(ok)" : "(FAILED)");
    if (FAILED(hr)) return;

    std::printf("\n  -- The exact call CustomMusicConverter.cpp:167 makes --\n");
    UINT32 wma8 = CountEncoders(pEnumEx, MFAudioFormat_WMAudioV8_, "PCM -> WMAudioV8 (what the mod needs)");
    g.wmaEncoders = (int)wma8;

    std::printf("\n  -- Nearby alternatives --\n");
    CountEncoders(pEnumEx, MFAudioFormat_WMAudioV9_, "PCM -> WMAudioV9");
    CountEncoders(pEnumEx, MFAudioFormat_AAC_,       "PCM -> AAC");

    std::printf("\n  -- Every audio ENCODER this runtime has --\n");
    {
        IMFActivate** acts = NULL; UINT32 n = 0;
        HRESULT h2 = pEnumEx(MFT_CATEGORY_AUDIO_ENCODER_, MFT_ENUM_FLAG_ALL, NULL, NULL, &acts, &n);
        if (SUCCEEDED(h2)) { Line("Total audio encoders", "%u", n); DumpActivates(acts, n);
            for (UINT32 i = 0; i < n; ++i) if (acts[i]) acts[i]->Release();
            if (acts) CoTaskMemFree(acts); }
        else Line("Total audio encoders", "ENUM FAILED hr=0x%08lX", (unsigned long)h2);
    }

    std::printf("\n  -- Every audio DECODER this runtime has --\n");
    {
        IMFActivate** acts = NULL; UINT32 n = 0;
        HRESULT h2 = pEnumEx(MFT_CATEGORY_AUDIO_DECODER_, MFT_ENUM_FLAG_ALL, NULL, NULL, &acts, &n);
        if (SUCCEEDED(h2)) { Line("Total audio decoders", "%u", n); DumpActivates(acts, n);
            for (UINT32 i = 0; i < n; ++i) if (acts[i]) acts[i]->Release();
            if (acts) CoTaskMemFree(acts); }
        else Line("Total audio decoders", "ENUM FAILED hr=0x%08lX", (unsigned long)h2);
    }

    std::printf("\n  -- Opus decoder (AudioDecode.cpp:481) --\n");
    {
        IUnknown* unk = NULL;
        HRESULT h2 = CoCreateInstance(CLSID_MSOpusDecoder_, NULL, CLSCTX_INPROC_SERVER,
                                      IID_IUnknown, (void**)&unk);
        Line("CoCreateInstance(MSOpusDecoder)", "hr=0x%08lX %s",
             (unsigned long)h2, SUCCEEDED(h2) ? "AVAILABLE" : "NOT AVAILABLE");
        g.opusDecoder = SUCCEEDED(h2);
        if (unk) unk->Release();
    }

    std::printf("\n  VERDICT: custom music / BGM replacement is %s on this runtime.\n",
                wma8 ? "POSSIBLE (a WMA encoder exists)" : "DEAD (no WMA encoder)");
    if (pShutdown) pShutdown();
}

// ---------------------------------------------------------------- 4. shell zip

static unsigned long Crc32(const unsigned char* d, size_t n)
{
    static unsigned long tab[256]; static bool built = false;
    if (!built) {
        for (unsigned long i = 0; i < 256; ++i) {
            unsigned long c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        built = true;
    }
    unsigned long c = 0xFFFFFFFFUL;
    for (size_t i = 0; i < n; ++i) c = tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

static void PutU16(std::vector<unsigned char>& v, unsigned x)
{ v.push_back((unsigned char)(x & 0xFF)); v.push_back((unsigned char)((x >> 8) & 0xFF)); }
static void PutU32(std::vector<unsigned char>& v, unsigned long x)
{ for (int i = 0; i < 4; ++i) v.push_back((unsigned char)((x >> (8 * i)) & 0xFF)); }

// One stored (uncompressed) entry: probe.txt -> "ok"
static std::vector<unsigned char> BuildTestZip()
{
    const char* name = "probe.txt";
    const char* body = "ok";
    const unsigned nameLen = 9, bodyLen = 2;
    const unsigned long crc = Crc32((const unsigned char*)body, bodyLen);

    std::vector<unsigned char> z;
    PutU32(z, 0x04034B50); PutU16(z, 20); PutU16(z, 0); PutU16(z, 0);
    PutU16(z, 0); PutU16(z, 0x21);
    PutU32(z, crc); PutU32(z, bodyLen); PutU32(z, bodyLen);
    PutU16(z, nameLen); PutU16(z, 0);
    z.insert(z.end(), name, name + nameLen);
    z.insert(z.end(), body, body + bodyLen);

    const unsigned long cdOff = (unsigned long)z.size();
    PutU32(z, 0x02014B50); PutU16(z, 20); PutU16(z, 20); PutU16(z, 0); PutU16(z, 0);
    PutU16(z, 0); PutU16(z, 0x21);
    PutU32(z, crc); PutU32(z, bodyLen); PutU32(z, bodyLen);
    PutU16(z, nameLen); PutU16(z, 0); PutU16(z, 0);
    PutU16(z, 0); PutU16(z, 0); PutU32(z, 0); PutU32(z, 0);
    z.insert(z.end(), name, name + nameLen);

    const unsigned long cdSize = (unsigned long)z.size() - cdOff;
    PutU32(z, 0x06054B50); PutU16(z, 0); PutU16(z, 0); PutU16(z, 1); PutU16(z, 1);
    PutU32(z, cdSize); PutU32(z, cdOff); PutU16(z, 0);
    return z;
}

static void ProbeShellZip()
{
    Section("4. SHELL ZIP EXTRACTION  <-- the updater blocker");
    Note("PackageStager.cpp:469 uses Shell.Application to unpack the update.");
    std::printf("\n");

    wchar_t tmpDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    std::wstring zipPath = std::wstring(tmpDir) + L"bbcf_probe.zip";
    std::wstring outDir  = std::wstring(tmpDir) + L"bbcf_probe_out";
    CreateDirectoryW(outDir.c_str(), NULL);

    std::vector<unsigned char> zip = BuildTestZip();
    HANDLE h = CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { Line("Write test zip", "FAILED err=%lu", GetLastError()); return; }
    DWORD wrote = 0;
    WriteFile(h, zip.data(), (DWORD)zip.size(), &wrote, NULL);
    CloseHandle(h);
    Line("Test zip written", "%lu bytes -> %s", (unsigned long)wrote, Narrow(zipPath.c_str()).c_str());

    IDispatch* shell = NULL;
    HRESULT hr = CoCreateInstance(CLSID_Shell_, NULL, CLSCTX_INPROC_SERVER, IID_IDispatch_, (void**)&shell);
    Line("CoCreateInstance(Shell.Application)", "hr=0x%08lX %s",
         (unsigned long)hr, SUCCEEDED(hr) ? "created" : "FAILED");
    if (FAILED(hr) || !shell)
    {
        Note("VERDICT: updater extraction is DEAD - no Shell.Application at all.");
        g.shellZipWorks = false;
        return;
    }

    // shell->NameSpace(zipPath) via IDispatch::Invoke
    DISPID dispid = 0;
    OLECHAR* member = (OLECHAR*)L"NameSpace";
    hr = shell->GetIDsOfNames(IID_NULL, &member, 1, LOCALE_USER_DEFAULT, &dispid);
    Line("IDispatch::GetIDsOfNames(NameSpace)", "hr=0x%08lX", (unsigned long)hr);
    if (SUCCEEDED(hr))
    {
        VARIANT arg; VariantInit(&arg);
        arg.vt = VT_BSTR; arg.bstrVal = SysAllocString(zipPath.c_str());
        DISPPARAMS dp = {}; dp.cArgs = 1; dp.rgvarg = &arg;
        VARIANT result; VariantInit(&result);
        hr = shell->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                           &dp, &result, NULL, NULL);
        const bool gotFolder = SUCCEEDED(hr) && result.vt == VT_DISPATCH && result.pdispVal;
        Line("NameSpace(<zip>) returns a Folder", "hr=0x%08lX  %s",
             (unsigned long)hr, gotFolder ? "YES" : "NO");
        g.shellZipWorks = gotFolder;
        if (!gotFolder)
            Note("This is the exact failure: Wine's shell32 has no zip folder handler.");
        VariantClear(&result);
        VariantClear(&arg);
    }
    shell->Release();

    if (g.shellZipWorks)
        std::printf("\n  VERDICT: shell extraction works here; the updater is fine on this runtime.\n");
    else
        std::printf("\n  VERDICT: updater is DEAD. Replace with in-process inflate (zlib already vendored).\n");
}

// ---------------------------------------------------------------- 5. dbghelp

typedef BOOL (WINAPI *MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, DWORD,
                                           void*, void*, void*);

static void ProbeCrashDumps()
{
    Section("5. CRASH DUMPS");

    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    Line("dbghelp.dll", "%s", dbg ? "loaded" : "MISSING");
    if (!dbg) return;

    MiniDumpWriteDump_t pDump = (MiniDumpWriteDump_t)GetProcAddress(dbg, "MiniDumpWriteDump");
    Line("MiniDumpWriteDump exported", "%s", YN(pDump != NULL));
    if (!pDump) { Note("VERDICT: no minidumps on this runtime."); return; }

    wchar_t tmpDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    std::wstring dmp = std::wstring(tmpDir) + L"bbcf_probe.dmp";
    HANDLE hf = CreateFileW(dmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) { Line("Create dump file", "FAILED err=%lu", GetLastError()); return; }

    // The same flag set crashdump.cpp asks for.
    const DWORD flags = 0x00000002 /*WithFullMemory*/ | 0x00000800 /*WithFullMemoryInfo*/
                      | 0x00000004 /*WithHandleData*/ | 0x00001000 /*WithThreadInfo*/
                      | 0x00002000 /*WithUnloadedModules*/ | 0x00000001 /*WithDataSegs*/;
    const BOOL ok = pDump(GetCurrentProcess(), GetCurrentProcessId(), hf, flags, NULL, NULL, NULL);
    const DWORD err = GetLastError();
    LARGE_INTEGER size = {};
    GetFileSizeEx(hf, &size);
    CloseHandle(hf);

    Line("MiniDumpWriteDump", "%s  err=%lu", ok ? "returned TRUE" : "returned FALSE", err);
    Line("Dump size", "%lld bytes", (long long)size.QuadPart);

    unsigned char magic[4] = {};
    HANDLE hr2 = CreateFileW(dmp.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hr2 != INVALID_HANDLE_VALUE)
    {
        DWORD got = 0; ReadFile(hr2, magic, 4, &got, NULL); CloseHandle(hr2);
        const bool mdmp = (got == 4 && magic[0]=='M' && magic[1]=='D' && magic[2]=='M' && magic[3]=='P');
        Line("Header is 'MDMP'", "%s", YN(mdmp));
        g.minidumpUsable = mdmp && size.QuadPart > 65536;
        if (!mdmp) Note("A dump without a valid header cannot be opened by any debugger.");
    }
    Note("A large, well-formed dump here means Linux crash reports are usable.");
    Note("A tiny or malformed one means we should write a text report instead.");
}

// ---------------------------------------------------------------- 6. controllers

typedef HDEVINFO (WINAPI *SetupDiGetClassDevsW_t)(const GUID*, PCWSTR, HWND, DWORD);
typedef BOOL (WINAPI *SetupDiEnumDeviceInterfaces_t)(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL (WINAPI *SetupDiGetDeviceInterfaceDetailW_t)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *SetupDiGetDeviceInstanceIdW_t)(HDEVINFO, PSP_DEVINFO_DATA, PWSTR, DWORD, PDWORD);
typedef BOOL (WINAPI *SetupDiGetDevicePropertyW_t)(HDEVINFO, PSP_DEVINFO_DATA, const DEVPROPKEY_T*, ULONG*, PBYTE, DWORD, PDWORD, DWORD);
typedef BOOL (WINAPI *SetupDiDestroyDeviceInfoList_t)(HDEVINFO);
typedef BOOLEAN (WINAPI *HidD_GetProductString_t)(HANDLE, PVOID, ULONG);
typedef UINT (WINAPI *joyGetNumDevs_t)(void);
typedef MMRESULT (WINAPI *joyGetDevCapsW_t)(UINT_PTR, LPJOYCAPSW, UINT);

static void ProbeControllers()
{
    Section("6. CONTROLLER STACK  <-- why the hooks were disabled");
    Note("Every call the mod makes during enumeration, in order, with results.");
    Note("Plug a pad in before running this or most sections will be empty.");
    std::printf("\n");

    // --- SetupAPI walk ---
    HMODULE si = LoadLibraryW(L"setupapi.dll");
    HMODULE hidDll = LoadLibraryW(L"hid.dll");
    Line("setupapi.dll", "%s", si ? "loaded" : "MISSING");
    Line("hid.dll", "%s", hidDll ? "loaded" : "MISSING");

    if (si)
    {
        SetupDiGetClassDevsW_t             pGetClassDevs = (SetupDiGetClassDevsW_t)GetProcAddress(si, "SetupDiGetClassDevsW");
        SetupDiEnumDeviceInterfaces_t      pEnumIf       = (SetupDiEnumDeviceInterfaces_t)GetProcAddress(si, "SetupDiEnumDeviceInterfaces");
        SetupDiGetDeviceInterfaceDetailW_t pIfDetail     = (SetupDiGetDeviceInterfaceDetailW_t)GetProcAddress(si, "SetupDiGetDeviceInterfaceDetailW");
        SetupDiGetDeviceInstanceIdW_t      pInstId       = (SetupDiGetDeviceInstanceIdW_t)GetProcAddress(si, "SetupDiGetDeviceInstanceIdW");
        SetupDiGetDevicePropertyW_t        pDevProp      = (SetupDiGetDevicePropertyW_t)GetProcAddress(si, "SetupDiGetDevicePropertyW");
        SetupDiDestroyDeviceInfoList_t     pDestroy      = (SetupDiDestroyDeviceInfoList_t)GetProcAddress(si, "SetupDiDestroyDeviceInfoList");

        Line("SetupDiGetDevicePropertyW exported", "%s", YN(pDevProp != NULL));
        g.devPropExported = (pDevProp != NULL);
        if (!pDevProp)
            Note("Absent here => the mod's ContainerID lookup can never work.");

        if (pGetClassDevs && pEnumIf && pIfDetail)
        {
            HDEVINFO set = pGetClassDevs(&GUID_DEVINTERFACE_HID_, NULL, NULL,
                                         0x00000010 /*DIGCF_DEVICEINTERFACE*/ | 0x00000002 /*DIGCF_PRESENT*/);
            if (set == INVALID_HANDLE_VALUE)
            {
                Line("SetupDiGetClassDevsW(HID)", "FAILED err=%lu", GetLastError());
            }
            else
            {
                int found = 0, withContainer = 0;
                for (DWORD idx = 0; ; ++idx)
                {
                    SP_DEVICE_INTERFACE_DATA ifd = {}; ifd.cbSize = sizeof(ifd);
                    if (!pEnumIf(set, NULL, &GUID_DEVINTERFACE_HID_, idx, &ifd)) break;
                    ++found;

                    DWORD need = 0;
                    pIfDetail(set, &ifd, NULL, 0, &need, NULL);
                    if (!need || need > 4096) continue;
                    std::vector<unsigned char> buf(need);
                    PSP_DEVICE_INTERFACE_DETAIL_DATA_W det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
                    det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                    SP_DEVINFO_DATA dev = {}; dev.cbSize = sizeof(dev);
                    if (!pIfDetail(set, &ifd, det, need, NULL, &dev)) continue;

                    std::printf("\n      [device %d] %s\n", found, Narrow(det->DevicePath).c_str());

                    if (pInstId)
                    {
                        wchar_t iid[512] = {}; DWORD n = 0;
                        if (pInstId(set, &dev, iid, 512, &n))
                            std::printf("        instance id   : %s\n", Narrow(iid).c_str());
                        else
                            std::printf("        instance id   : FAILED err=%lu\n", GetLastError());
                    }
                    if (pDevProp)
                    {
                        ULONG type = 0; BYTE gbuf[64] = {}; DWORD req = 0;
                        if (pDevProp(set, &dev, &DEVPKEY_Device_ContainerId_, &type, gbuf, sizeof(gbuf), &req, 0))
                        {
                            ++withContainer;
                            GUID* g = (GUID*)gbuf;
                            std::printf("        ContainerID   : {%08lX-%04X-%04X-...}  <-- present\n",
                                        (unsigned long)g->Data1, g->Data2, g->Data3);
                        }
                        else
                        {
                            std::printf("        ContainerID   : ABSENT err=%lu  <-- suspect\n", GetLastError());
                        }
                    }
                    if (hidDll)
                    {
                        HidD_GetProductString_t pProd =
                            (HidD_GetProductString_t)GetProcAddress(hidDll, "HidD_GetProductString");
                        if (pProd)
                        {
                            HANDLE hd = CreateFileW(det->DevicePath, 0,
                                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                                    OPEN_EXISTING, 0, NULL);
                            if (hd != INVALID_HANDLE_VALUE)
                            {
                                wchar_t prod[256] = {};
                                if (pProd(hd, prod, sizeof(prod)))
                                    std::printf("        product       : %s\n", Narrow(prod).c_str());
                                else
                                    std::printf("        product       : FAILED err=%lu\n", GetLastError());
                                CloseHandle(hd);
                            }
                            else
                            {
                                std::printf("        open handle   : FAILED err=%lu\n", GetLastError());
                            }
                        }
                    }
                }
                std::printf("\n");
                Line("HID interfaces enumerated", "%d", found);
                Line("...of those with a ContainerID", "%d", withContainer);
                g.hidDevices = found;
                g.hidWithContainerId = withContainer;
                if (found > 0 && withContainer == 0)
                    Note("CONFIRMED: device correlation by ContainerID cannot work here.");
                if (pDestroy) pDestroy(set);
            }
        }
    }

    // --- Raw input ---
    std::printf("\n");
    UINT count = 0;
    if (GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1)
        Line("GetRawInputDeviceList", "FAILED err=%lu", GetLastError());
    else
    {
        Line("GetRawInputDeviceList", "%u device(s)", count);
        std::vector<RAWINPUTDEVICELIST> list(count ? count : 1);
        if (count && GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) != (UINT)-1)
        {
            int kb = 0, mouse = 0, hid = 0;
            for (UINT i = 0; i < count; ++i)
            {
                if (list[i].dwType == RIM_TYPEKEYBOARD) ++kb;
                else if (list[i].dwType == RIM_TYPEMOUSE) ++mouse;
                else ++hid;
            }
            Note("keyboards=%d  mice=%d  other HID=%d", kb, mouse, hid);
        }
    }

    // --- winmm joystick ---
    HMODULE mm = LoadLibraryW(L"winmm.dll");
    if (mm)
    {
        joyGetNumDevs_t pNum = (joyGetNumDevs_t)GetProcAddress(mm, "joyGetNumDevs");
        joyGetDevCapsW_t pCaps = (joyGetDevCapsW_t)GetProcAddress(mm, "joyGetDevCapsW");
        if (pNum)
        {
            UINT n = pNum();
            Line("joyGetNumDevs", "%u slot(s)", n);
            if (pCaps)
            {
                int live = 0;
                for (UINT i = 0; i < n && i < 16; ++i)
                {
                    JOYCAPSW caps = {};
                    if (pCaps(i, &caps, sizeof(caps)) == JOYERR_NOERROR)
                    { ++live; Note("slot %u: %s", i, Narrow(caps.szPname).c_str()); }
                }
                Line("...with a device attached", "%d", live);
            }
        }
    }

    // --- XInput ---
    static const wchar_t* kXi[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (size_t i = 0; i < 3; ++i)
    {
        HMODULE x = LoadLibraryW(kXi[i]);
        if (!x) continue;
        typedef DWORD (WINAPI *XIGet_t)(DWORD, void*);
        XIGet_t p = (XIGet_t)GetProcAddress(x, "XInputGetState");
        if (!p) continue;
        unsigned char state[16] = {};
        int connected = 0;
        for (DWORD u = 0; u < 4; ++u) if (p(u, state) == ERROR_SUCCESS) ++connected;
        Line(Narrow(kXi[i]).c_str(), "loaded, %d pad(s) connected", connected);
        break;
    }

    // --- DirectInput ---
    HMODULE di = LoadLibraryW(L"dinput8.dll");
    if (di)
    {
        typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
        DI8Create_t p = (DI8Create_t)GetProcAddress(di, "DirectInput8Create");
        Line("DirectInput8Create exported", "%s", YN(p != NULL));
    }
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    SetConsoleOutputCP(CP_UTF8);
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    std::printf("BBCF Improvement Mod - Wine/Proton capability probe\n");
    std::printf("Built %s %s\n", __DATE__, __TIME__);

    ProbePlatform();
    ProbeModules();
    ProbeMediaFoundation();
    ProbeShellZip();
    ProbeCrashDumps();
    ProbeControllers();

    Section("SUMMARY");
    std::printf("  Runtime                : %s\n", g.onWine ? "Wine/Proton" : "native Windows (baseline)");
    std::printf("\n");
    std::printf("  [%s] Mod DLL can load          - mfplat/mfreadwrite/mf all present\n",
                g.mfStaticImportsOk ? "PASS" : "FAIL");
    std::printf("  [%s] Custom music / BGM swap   - %d PCM->WMAudioV8 encoder(s)\n",
                g.wmaEncoders > 0 ? "PASS" : "FAIL", g.wmaEncoders);
    std::printf("  [%s] Auto-updater              - Shell.Application zip extraction\n",
                g.shellZipWorks ? "PASS" : "FAIL");
    std::printf("  [%s] Opus decoding             - MSOpusDecoder MFT\n",
                g.opusDecoder ? "PASS" : "FAIL");
    std::printf("  [%s] Crash minidumps           - well-formed dump written\n",
                g.minidumpUsable ? "PASS" : "FAIL");
    if (g.hidDevices >= 0)
        std::printf("  [%s] Controller correlation    - %d/%d HID devices carry a ContainerID\n",
                    (g.hidDevices > 0 && g.hidWithContainerId == g.hidDevices) ? "PASS" : "FAIL",
                    g.hidWithContainerId, g.hidDevices);
    else
        std::printf("  [----] Controller correlation    - HID enumeration did not run\n");

    std::printf("\n========================================================\n");
    std::printf("  END OF REPORT\n");
    std::printf("========================================================\n");

    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}
