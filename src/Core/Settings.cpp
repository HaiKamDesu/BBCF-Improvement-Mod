#include "Settings.h"
#include "logger.h"
#include "keycodes.h"
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

namespace
{
bool IsSettingMissingInIni(LPCWSTR key, LPCWSTR filename)
{
        WCHAR buffer[2];
        DWORD charsRead = GetPrivateProfileString(L"Settings", key, L"", buffer, ARRAYSIZE(buffer), filename);
        return charsRead == 0;
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


	if (Settings::settingsIni.uploadReplayDataHost == "50.118.225.175") {
		Settings::changeSetting("UploadReplayDataHost", "89.167.76.6");
		Settings::settingsIni.uploadReplayDataHost = "89.167.76.6";
	}
	g_modVals.enableForeignPalettes = Settings::settingsIni.loadforeignpalettes;
	g_modVals.allowPaletteDownloads = Settings::settingsIni.allowPaletteDownloads;
	g_modVals.save_states_save_keycode = Settings::getButtonValue(settingsIni.saveStateKeybind);
	g_modVals.save_states_load_keycode = Settings::getButtonValue(settingsIni.loadStateKeybind);
	g_modVals.replay_takeover_load_keycode = Settings::getButtonValue(settingsIni.loadReplayStateKeybind);
	g_modVals.freeze_frame_keycode = Settings::getButtonValue(Settings::settingsIni.freezeFrameKeybind);
	g_modVals.step_frames_keycode = Settings::getButtonValue(Settings::settingsIni.stepFramesKeybind);
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
	//pPresentationParameters->Windowed = !Settings::settingsIni.fullscreen;

	pPresentationParameters->PresentationInterval = settingsIni.vsync ? D3DPRESENT_INTERVAL_DEFAULT : D3DPRESENT_INTERVAL_IMMEDIATE;
	
	
	//pPresentationParameters->Windowed = !settingsIni.fullscreen;
	//if (settingsIni.fullscreen)
	//{
	//	pPresentationParameters->FullScreen_RefreshRateInHz = 60; // savedSettings.adapterRefreshRate;
	//}
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
	CString strINIPath;
	ForceLog("[Init][Settings] loadSettingsFile enter\n");

	_wfullpath((wchar_t*)strINIPath.GetBuffer(MAX_PATH), L"settings.ini", MAX_PATH);
	strINIPath.ReleaseBuffer();
	{
		CT2CA iniPathAnsi(strINIPath);
		ForceLog("[Init][Settings] resolved path='%s'\n", iniPathAnsi.m_psz ? iniPathAnsi.m_psz : "<null>");
	}

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

        //X-Macro
#define SETTING(_type, _var, _inistring, _defaultval) \
        ForceLog("[Init][Settings] reading " _inistring "\n"); \
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

        // Seed new keys into existing settings.ini files that pre-date them.
        // Without this, the first save via changeSetting hits the slow "append" path and
        // any I/O failure silently discards the value. After seeding, the key already
        // exists so subsequent saves use the fast "replace" path.
        if (IsSettingMissingInIni(L"FrameHistoryEnabled", strINIPath)) {
                ForceLog("[Init][Settings] seeding missing key FrameHistoryEnabled\n");
                changeSetting("FrameHistoryEnabled", settingsIni.frameHistoryEnabled ? "1" : "0");
        }
        if (IsSettingMissingInIni(L"FrameHistoryAutoReset", strINIPath)) {
                ForceLog("[Init][Settings] seeding missing key FrameHistoryAutoReset\n");
                changeSetting("FrameHistoryAutoReset", settingsIni.frameHistoryAutoReset ? "1" : "0");
        }

	ForceLog("[Init][Settings] raw settings read complete\n");

	// Set buttons back to default if their values are incorrect
	if (settingsIni.togglebutton.length() != 2 || settingsIni.togglebutton[0] != 'F')
		settingsIni.togglebutton = "F1";

	if (settingsIni.toggleOnlineButton.length() != 2 || settingsIni.toggleOnlineButton[0] != 'F')
		settingsIni.toggleOnlineButton = "F2";

	if (settingsIni.toggleHUDbutton.length() != 2 || settingsIni.toggleHUDbutton[0] != 'F')
		settingsIni.toggleHUDbutton = "F3";



	
	
        if (settingsIni.swapControllerPos)
        {
                LOG(1, "Settings::loadSettingsFile - SwapControllerPos forced off due to a known startup crash issue.\n");
        }
        settingsIni.swapControllerPos = false;
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

short Settings::getButtonValue(std::string button)
{
        auto maybe_keycode = keycode_mapper.find(button);
        if (maybe_keycode != keycode_mapper.end())
                return maybe_keycode->second;
        else
                return 112;

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
	// Resolve absolute path — WritePrivateProfileString ignores relative paths (looks in
	// Windows dir instead of CWD), so we must pass a full path.
	wchar_t wAbsPath[MAX_PATH] = {};
	if (_wfullpath(wAbsPath, L"settings.ini", MAX_PATH) == nullptr) {
		LOG(2, "[error] Settings::changeSetting: Unable to resolve absolute path.");
		return 1;
	}

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
