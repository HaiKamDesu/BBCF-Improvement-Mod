#include "Settings.h"
#include "logger.h"
#include "HotkeyManager.h"
#include "EmbeddedResources.h"
#include "utils.h"
#include <regex>
#include "Core/interfaces.h"

#include <atlstr.h>
#include <ctime>
#include <iostream>
#include <fstream>
#include "stringapiset.h"

#define VIEWPORT_DEFAULT 1

settingsIni_t Settings::settingsIni = {};
savedSettings_t Settings::savedSettings = {};
bool Settings::debugLoggingSettingMissing = false;
bool Settings::settingsFileLoaded = false;

namespace
{
bool IsSettingMissingInIni(LPCWSTR key, LPCWSTR filename)
{
        WCHAR buffer[2];
        DWORD charsRead = GetPrivateProfileString(L"Settings", key, L"", buffer, ARRAYSIZE(buffer), filename);
        return charsRead == 0;
}

// A release is only dinput8.dll plus the updater - every config file the mod reads is
// compiled into the DLL (see resource/resource.rc) and written out here.
struct EmbeddedIni
{
        const wchar_t* resourceName;
        const wchar_t* iniPath;      // game-folder-relative, created only when absent
        const wchar_t* defaultPath;  // updater's merge baseline, rewritten every launch
};

const EmbeddedIni kEmbeddedInis[] = {
        { L"settings.ini", L"settings.ini", L"BBCF_IM\\Updater\\defaults\\settings.ini.default" },
        { L"palettes.ini", L"palettes.ini", L"BBCF_IM\\Updater\\defaults\\palettes.ini.default" },
};

// Everything below is wide, deliberately, and must stay that way.
//
// This was the one part of settings that went through the narrow GamePath(). That string is
// the game directory in the ANSI code page (see the comment on ResolveGameDirectory), so any
// install whose path the code page cannot represent - a Cyrillic or accented Windows user
// name, Proton's Z:\home\<user>\... - arrives here mangled. GetFileAttributesA then reports
// the ini as missing, CreateDirectoryA/ofstream cannot create it either, and the launch ends
// with no settings.ini and no palettes.ini on a perfectly clean install. Reading them was
// never affected: loadSettingsFile resolves through GamePathW, and so does the logger, which
// is why such a report arrives with a DEBUG.txt in it and no config files beside it.
std::string NarrowForLog(const std::wstring& wide)
{
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), NULL, 0, NULL, NULL);
        if (needed <= 0)
                return std::string();

        std::string out;
        out.resize(needed);
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &out[0], needed, NULL, NULL);
        return out;
}

// Creates every missing directory along `absolutePath`'s parent chain. Win32 rather
// than std::filesystem: this project builds on the toolset's default language standard,
// which predates <filesystem>.
void CreateParentDirectories(const std::wstring& absolutePath)
{
        const size_t lastSlash = absolutePath.find_last_of(L"\\/");
        if (lastSlash == std::wstring::npos)
                return;

        const std::wstring parent = absolutePath.substr(0, lastSlash);
        for (size_t i = 0; i < parent.size(); ++i)
        {
                if (parent[i] != L'\\' && parent[i] != L'/')
                        continue;
                // Skip the root itself ("C:\") so we never call CreateDirectory on a drive.
                if (i > 0 && parent[i - 1] != L':')
                        CreateDirectoryW(parent.substr(0, i).c_str(), NULL);
        }
        CreateDirectoryW(parent.c_str(), NULL);
}

// Returns the Windows error on failure, 0 on success. The reason matters: "no settings.ini"
// reads the same in a report whether the path was unrepresentable or the folder was read-only,
// and those need opposite answers.
DWORD WriteTextFile(const std::wstring& absolutePath, const std::string& contents)
{
        CreateParentDirectories(absolutePath);

        const HANDLE file = CreateFileW(absolutePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
                return GetLastError() ? GetLastError() : ERROR_OPEN_FAILED;

        DWORD written = 0;
        const BOOL ok = WriteFile(file, contents.data(), (DWORD)contents.size(), &written, NULL);
        const DWORD error = ok ? ERROR_SUCCESS : (GetLastError() ? GetLastError() : ERROR_WRITE_FAULT);
        CloseHandle(file);

        if (error == ERROR_SUCCESS && written != contents.size())
                return ERROR_WRITE_FAULT;

        return error;
}

void WriteEmbeddedConfigFiles()
{
        for (const EmbeddedIni& ini : kEmbeddedInis)
        {
                const std::string iniPathForLog = NarrowForLog(ini.iniPath);

                std::string contents;
                if (!LoadEmbeddedResource(ini.resourceName, contents) || contents.empty())
                {
                        ForceLog("[Init][Settings] embedded template '%s' not found in the DLL\n", iniPathForLog.c_str());
                        continue;
                }

                // First run: no ini on disk yet, so lay down the template before anything reads it.
                const std::wstring iniPath = GamePathW(ini.iniPath);
                if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                        const DWORD error = WriteTextFile(iniPath, contents);
                        ForceLog("[Init][Settings] creating missing '%s' at '%s' (ok=%d err=%lu)\n",
                                iniPathForLog.c_str(), NarrowForLog(iniPath).c_str(),
                                error == ERROR_SUCCESS ? 1 : 0, error);
                }

                // The updater's three-way merge wants an "old default" that matches the build
                // actually running, so rewrite it unconditionally instead of shipping a copy
                // that can drift out of sync with the DLL.
                const std::wstring defaultPath = GamePathW(ini.defaultPath);
                const DWORD defaultError = WriteTextFile(defaultPath, contents);
                if (defaultError != ERROR_SUCCESS)
                {
                        ForceLog("[Init][Settings] failed to write '%s' (err=%lu)\n",
                                NarrowForLog(ini.defaultPath).c_str(), defaultError);
                }
        }
}
}


void Settings::applySettingsIni(D3DPRESENT_PARAMETERS* pPresentationParameters)
{
	if (settingsIni.viewport != VIEWPORT_DEFAULT)
	{
		pPresentationParameters->BackBufferHeight = settingsIni.renderheight;
		pPresentationParameters->BackBufferWidth = settingsIni.renderwidth;
	}

	switch (Settings::settingsIni.antialiasing)
	{
	case 0:
		pPresentationParameters->MultiSampleType = D3DMULTISAMPLE_NONE;
		pPresentationParameters->MultiSampleQuality = 0;
		break;
		//case 2:
		//	pPresentationParameters->MultiSampleType = D3DMULTISAMPLE_2_SAMPLES;
		//	break;
	case 4:
		pPresentationParameters->MultiSampleType = D3DMULTISAMPLE_4_SAMPLES;
		break;
	case 5:
	default:
		break;
	}


	applyRuntimeSettings();

	pPresentationParameters->PresentationInterval = settingsIni.vsync ? D3DPRESENT_INTERVAL_DEFAULT : D3DPRESENT_INTERVAL_IMMEDIATE;


	//pPresentationParameters->Windowed = !settingsIni.fullscreen;
	//if (settingsIni.fullscreen)
	//{
	//	pPresentationParameters->FullScreen_RefreshRateInHz = 60; // savedSettings.adapterRefreshRate;
	//}
}

// Mirrors the settings that subsystems read from g_modVals instead of from settingsIni.
// Called both on device create/reset and right after the in-game settings window saves,
// so those settings really do take effect live instead of waiting for the next reset.
void Settings::applyRuntimeSettings()
{
	if (Settings::settingsIni.uploadReplayDataHost == "50.118.225.175") {
		Settings::changeSetting("UploadReplayDataHost", "89.167.76.6");
		Settings::settingsIni.uploadReplayDataHost = "89.167.76.6";
	}
	g_modVals.enableForeignPalettes = Settings::settingsIni.loadforeignpalettes;
	g_modVals.allowPaletteDownloads = Settings::settingsIni.allowPaletteDownloads;
	// Hotkeys no longer live in g_modVals: HotkeyManager owns every binding and every
	// consumer asks it directly, so re-reading them is one call.
	HotkeyManager::ReloadFromSettings();
	g_modVals.uploadReplayData = Settings::settingsIni.uploadReplayData;
	g_modVals.frame_history_width = Settings::settingsIni.FrameHistoryWidth;
	g_modVals.frame_history_height = Settings::settingsIni.FrameHistoryHeight;
	g_modVals.frame_history_spacing = Settings::settingsIni.FrameHistorySpacing;
	g_modVals.frame_history_auto_reset = Settings::settingsIni.frameHistoryAutoReset;

	//CA2W pszwide (host_c_str);
	g_modVals.uploadReplayDataHost = Settings::settingsIni.uploadReplayDataHost;
	//std::string str2 = Settings::settingsIni.uploadReplayDataEndpoint;
	//CA2W pszwide2(str2.c_str());
	g_modVals.uploadReplayDataEndpoint = Settings::settingsIni.uploadReplayDataEndpoint;
	g_modVals.uploadReplayDataPort = Settings::settingsIni.uploadReplayDataPort;
	g_modVals.uploadReplayDataUseTls = Settings::settingsIni.uploadReplayDataUseTls;
}

int Settings::readSettingsFilePropertyInt(LPCWSTR key, LPCWSTR defaultVal, LPCWSTR filename)
{
	CString strNotificationPopups;
	GetPrivateProfileString(_T("Settings"), key, defaultVal, strNotificationPopups.GetBuffer(MAX_PATH), MAX_PATH, filename);
	strNotificationPopups.ReleaseBuffer();
	return _ttoi(strNotificationPopups);
}

float Settings::readSettingsFilePropertyFloat(LPCWSTR key, LPCWSTR defaultVal, LPCWSTR filename)
{
	CString strCustomHUDScale;
	GetPrivateProfileString(_T("Settings"), key, defaultVal, strCustomHUDScale.GetBuffer(MAX_PATH), MAX_PATH, filename);
	strCustomHUDScale.ReleaseBuffer();
	return _ttof(strCustomHUDScale);
}

std::string Settings::readSettingsFilePropertyString(LPCWSTR key, LPCWSTR defaultVal, LPCWSTR filename)
{
	// Bigger buffer so huge settings like KeyboardMappings don't get truncated
	const DWORD BUF_SIZE = 16384;

	CString strBuffer;
	GetPrivateProfileString(
		_T("Settings"),
		key,
		defaultVal,
		strBuffer.GetBuffer(BUF_SIZE),
		BUF_SIZE,
		filename
	);
	strBuffer.ReleaseBuffer();

	CT2CA pszConvertedAnsiString(strBuffer);
	return pszConvertedAnsiString.m_psz;
}


bool Settings::loadSettingsFile()
{
	ForceLog("[Init][Settings] loadSettingsFile enter\n");

	WriteEmbeddedConfigFiles();

	// Anchored to the game folder, never the working directory - the shell can move the
	// CWD out from under us while a file dialog is open (see GamePath in utils.h).
	const std::wstring iniPathWide = GamePathW(L"settings.ini");
	CString strINIPath(iniPathWide.c_str());
	// UTF-8, not CT2CA. The ANSI conversion turned every character the code page cannot
	// represent into '?', so the one line a report is read for showed a path that was not
	// the path - and it looked like the mangling this file was just fixed for, on an
	// install where nothing was wrong.
	ForceLog("[Init][Settings] resolved path='%s'\n", NarrowForLog(iniPathWide).c_str());

        if (GetFileAttributes(strINIPath) == 0xFFFFFFFF)
        {
                ForceLog("[Init][Settings] settings.ini missing, using defaults\n");
        }
        else
        {
	ForceLog("[Init][Settings] settings.ini exists\n");
        }

        void* iniPtr = 0;

        debugLoggingSettingMissing = IsSettingMissingInIni(L"GenerateDebugLogs", strINIPath);
	ForceLog("[Init][Settings] debug logging missing=%d\n", debugLoggingSettingMissing ? 1 : 0);

        // Keys the file does not contain at all. GetPrivateProfileString cannot tell "absent"
        // from "present and equal to the default", so without this a settings.ini that
        // predates a feature looks identical in a log to one that opted out of it - and the
        // difference is the whole bug: a first-launch prompt is triggered by -1, and -1 is
        // what an absent key reads as. It also makes the standard support answer ("open
        // settings.ini, search for AllowPaletteDownloads, set it to 0") fail silently,
        // because on an upgraded install there is no such line to find. v3.110's shipped
        // settings.ini has no AllowPaletteDownloads at all, and dropping a new dinput8.dll
        // onto that install - keeping the old settings.ini - is how users get there.
        std::string missingKeys;
        int missingCount = 0;

        //X-Macro
#define SETTING(_type, _var, _inistring, _defaultval) \
        ForceLog("[Init][Settings] reading " _inistring "\n"); \
        if (IsSettingMissingInIni(L##_inistring, strINIPath)) { \
                ++missingCount; \
                if (missingKeys.size() < 900) { \
                        if (!missingKeys.empty()) missingKeys += ", "; \
                        missingKeys += _inistring; } } \
        iniPtr = &settingsIni.##_var; \
        if(strcmp(#_type, "bool") == 0) { \
		*(bool*)iniPtr = readSettingsFilePropertyInt(L##_inistring, L##_defaultval, strINIPath) != 0; } \
	else if(strcmp(#_type, "int") == 0) { \
		*(int*)iniPtr = readSettingsFilePropertyInt(L##_inistring, L##_defaultval, strINIPath); } \
	else if(strcmp(#_type, "float") == 0) { \
		*(float*)iniPtr = readSettingsFilePropertyFloat(L##_inistring, L##_defaultval, strINIPath); } \
	else if (strcmp(#_type, "std::string") == 0) { \
		*(std::string*)iniPtr = readSettingsFilePropertyString(L##_inistring, L##_defaultval, strINIPath); } \
        ForceLog("[Init][Settings] finished " _inistring "\n");
#include "settings.def"
#undef SETTING

        if (debugLoggingSettingMissing)
        {
                settingsIni.generateDebugLogs = true;
        }

        if (missingCount > 0)
        {
                ForceLog("[Init][Settings] %d key(s) are not present in settings.ini and fell back "
                         "to their built-in default: %s%s\n",
                         missingCount, missingKeys.c_str(),
                         (missingKeys.size() >= 900) ? ", ..." : "");
        }

	ForceLog("[Init][Settings] raw settings read complete\n");

	// Hotkey values are no longer validated here. They used to be forced back to F1/F2/F3
	// whenever they were not exactly "F<digit>", which is what made those three menu keys
	// impossible to bind to anything else. HotkeyManager parses them instead, and an
	// unparseable value simply reads as "not bound" rather than being silently overwritten.

        if (settingsIni.swapControllerPos)
        {
                LOG(1, "Settings::loadSettingsFile - SwapControllerPos forced off due to a known startup crash issue.\n");
        }
        settingsIni.swapControllerPos = false;
	settingsFileLoaded = true;
	ForceLog("[Init][Settings] loadSettingsFile success\n");

        return true;
}

void Settings::initSavedSettings()
{
	LOG(7, "initSavedSettings\n");

	switch (settingsIni.viewport)
	{
	case 2:
		LOG(7, " - case 2\n");
		savedSettings.newSourceRect.right = settingsIni.renderwidth;
		savedSettings.newSourceRect.bottom = settingsIni.renderheight;
		savedSettings.newViewport.Width = settingsIni.renderwidth;;
		savedSettings.newViewport.Height = settingsIni.renderheight;
		break;
	case 3:
		LOG(7, " - case 3\n");
		savedSettings.newSourceRect.right = 1280;
		savedSettings.newSourceRect.bottom = 768;
		savedSettings.newViewport.Width = 1280;
		savedSettings.newViewport.Height = 768;
		break;
	case 1:
	default:
		LOG(7, " - case 1, default\n");
		//in this case the value is set in Direct3DDevice9ExWrapper::CreateRenderTargetEx!
		break;
	}
	savedSettings.origViewportRes.x = 0.0;
	savedSettings.origViewportRes.y = 0.0;

	savedSettings.isDuelFieldSprite = false;

	savedSettings.isFiltering = false;
}

bool Settings::WasDebugLoggingSettingMissing()
{
        return debugLoggingSettingMissing;
}
// changeSetting: write a key=value pair into the [Settings] section of settings.ini.
//
// Previous implementation used fstream line-by-line replace/append. That broke silently
// when the user's settings.ini contained multiple [Settings] section headers (an artifact
// of the auto-updater merging new template blocks). New keys were appended at end-of-file
// (under a later [Settings] header) while GetPrivateProfileString only ever reads from the
// FIRST [Settings] section — so the written value was never read back on next startup.
//
// WritePrivateProfileStringW targets the first matching section by spec, handles both
// updating existing keys and inserting new ones, and requires no temp-file dance.
int Settings::changeSetting(std::string setting_name, std::string new_value) {
	// Absolute path required — WritePrivateProfileString ignores relative paths (looks in
	// the Windows dir instead of the CWD), and it has to be the game folder rather than the
	// working directory so it matches the file loadSettingsFile reads.
	const std::wstring absPath = GamePathW(L"settings.ini");
	const wchar_t* wAbsPath = absPath.c_str();

	wchar_t wKey[512] = {};
	wchar_t wVal[4096] = {};
	MultiByteToWideChar(CP_ACP, 0, setting_name.c_str(), -1, wKey, 512);
	MultiByteToWideChar(CP_ACP, 0, new_value.c_str(), -1, wVal, 4096);

	if (!WritePrivateProfileStringW(L"Settings", wKey, wVal, wAbsPath)) {
		LOG(2, "[error] Settings::changeSetting: WritePrivateProfileStringW failed (GLE=%lu).", GetLastError());
		return 1;
	}

	LOG(2, "Settings::changeSetting: File updated successfully.");
	return 0;
}
