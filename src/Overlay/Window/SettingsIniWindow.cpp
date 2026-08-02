#include "SettingsIniWindow.h"

#include "Core/logger.h"
#include "Overlay/imgui_utils.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace {
	struct SettingMetadata
	{
		const char* iniKey;
		const char* displayName;
		const char* category;
		const char* tooltip;
	};

	static const SettingMetadata kSettingMetadata[] = {
		{ "ToggleButton", "Main menu hotkey", "Hotkeys", "Function key used to show or hide the main Improvement Mod menu." },
		{ "ToggleOnlineButton", "Online menu hotkey", "Hotkeys", "Function key used to show or hide the online info window." },
		{ "ToggleHUDButton", "HUD hotkey", "Hotkeys", "Function key used to toggle the in-game HUD." },
		{ "SaveStateKeybind", "Save state hotkey", "Hotkeys", "Key used to save replay takeover or training state data." },
		{ "LoadStateKeybind", "Load state hotkey", "Hotkeys", "Key used to load saved replay takeover or training state data." },
		{ "LoadReplayStateKeybind", "Load replay state hotkey", "Hotkeys", "Key used to load replay takeover state data." },
		{ "freezeFrameKeybind", "Freeze frame hotkey", "Hotkeys", "Key used to pause frame stepping tools." },
		{ "stepFramesKeybind", "Step frame hotkey", "Hotkeys", "Key used to advance one frame while frame stepping is active." },
		{ "DinputDllWrapper", "DInput DLL wrapper", "System", "Optional external dinput DLL to load before the mod starts. Use none unless you need a wrapper." },
		{ "RenderingWidth", "Rendering width", "Graphics", "Backbuffer width used by the mod when a custom viewport mode is active." },
		{ "RenderingHeight", "Rendering height", "Graphics", "Backbuffer height used by the mod when a custom viewport mode is active." },
		{ "Viewport", "Viewport mode", "Graphics", "Selects how the game viewport is sized. Some modes use the custom rendering width and height." },
		{ "AntiAliasing", "Anti-aliasing", "Graphics", "Direct3D anti-aliasing mode override. -1 keeps the game's default behavior." },
		{ "V-sync", "V-sync", "Graphics", "Enables vertical sync. Disable for lower latency; enable to reduce tearing." },
		{ "MenuSize", "Menu size", "Interface", "Changes the default overlay menu scale and minimum window size." },
		{ "Notifications", "Notifications", "Interface", "Shows mod notification popups and status messages." },
		{ "CheckUpdates", "Check for updates", "Interface", "Checks GitHub releases for a newer Improvement Mod version." },
		{ "GenerateDebugLogs", "Generate debug logs", "Debug", "Writes DEBUG.txt with detailed runtime information for troubleshooting." },
		{ "DCodeAutoRecover", "D-Code auto recovery", "Debug", "Detects the D-Code fetch wedge (profile payload rejected / stalled), dumps evidence to BBCF_IM, and forces the game's own retry path. Max 3 retries per slot per session." },
		{ "SpectatorSyncHooksEnabled", "Spectator desync fix", "Debug", "While spectating, repeats the players' last known inputs for a moment when new ones haven't arrived yet, instead of letting the game guess with empty inputs (which permanently desyncs the spectated match). Only affects spectator mode. Turn off to fall back to vanilla (unfixed) behavior. Requires a game restart to take effect." },
		{ "DCodeForceFailureOnce", "Force D-Code failure (test)", "Debug", "TEST ONLY: corrupts the first in-flight profile fetch after launch so the game rejects it, to verify D-Code failure detection and auto recovery. Fires once per launch while enabled." },
		{ "DebugLogSessionHistory", "Debug log session history", "Debug", "Number of previous sessions' DEBUG.txt files kept in BBCF_IM\\DebugHistory (rotated at launch). 0 overwrites DEBUG.txt every launch like before." },
		{ "DCodeTusGateAutoClear", "Recover profile uploads", "Ranked", "When the game latches its network-profile-storage-unavailable flag, reset it so your profile keeps uploading. Without this, D-Codes stop appearing and any ranked/net-color progress earned afterwards is lost when you restart." },
		{ "ShowRankedProgress", "Show ranked progress", "Ranked", "Shows the ranked progress overlay while ranked data is available." },
		{ "ShowSquareColorProgress", "Show square color progress", "Ranked", "Shows square color progress information when available." },
		{ "ShowRankedPrediction", "Show ranked prediction", "Ranked", "Shows the rank change prediction overlay during ranked matches." },
		{ "RankedProgressShowMatches", "Show match count", "Ranked", "Shows total matches in the ranked progress overlay." },
		{ "RankedProgressShowWins", "Show wins", "Ranked", "Shows wins in the ranked progress overlay." },
		{ "RankedProgressShowLosses", "Show losses", "Ranked", "Shows losses in the ranked progress overlay." },
		{ "RankedProgressShowWinrate", "Show winrate", "Ranked", "Shows winrate percentage in the ranked progress overlay." },
		{ "RankedProgressShowCharacterLeaderboardPlacement", "Show character leaderboard place", "Ranked", "Shows character leaderboard placement in the ranked progress overlay." },
		{ "RankedProgressShowGlobalLeaderboardPlacement", "Show global leaderboard place", "Ranked", "Shows global leaderboard placement in the ranked progress overlay." },
		{ "RankedAutomationHarnessEnabled", "Enable ranked automation harness", "Ranked Debug", "Enables ranked UI automation test hooks. Intended for development and testing." },
		{ "RankedAutomationHarnessHotkey", "Ranked automation hotkey", "Ranked Debug", "Hotkey used to trigger the ranked automation harness." },
		{ "RankedAutomationHarnessAutorun", "Ranked automation autorun", "Ranked Debug", "Runs the ranked automation harness automatically when its conditions are met." },
		{ "RankedAutomationHarnessQuitOnFinish", "Quit after ranked automation", "Ranked Debug", "Closes the game when the ranked automation harness finishes." },
		{ "URT_RE_TraceEnabled", "Replay takeover trace", "Replay Takeover Debug", "Writes replay takeover reverse-engineering trace logs." },
		{ "URT_RE_TraceLevel", "Replay takeover trace level", "Replay Takeover Debug", "Controls replay takeover trace verbosity. Higher values write more detail." },
		{ "URT_RE_MaxFileMB", "Replay takeover trace max file MB", "Replay Takeover Debug", "Maximum size of a replay takeover trace log before rotation." },
		{ "URT_RE_MaxBackups", "Replay takeover trace backups", "Replay Takeover Debug", "Number of old replay takeover trace logs kept during rotation." },
		{ "URT_RE_AllowSizeMismatchProbe", "Allow size mismatch probe", "Replay Takeover Debug", "Allows replay takeover probe paths even when state sizes do not match. Development only." },
		{ "URT_RE_AllowUnsafeProbeLoad", "Allow unsafe probe load", "Replay Takeover Debug", "Allows unsafe replay takeover probe loads. Development only; can crash." },
		{ "EnableInDevelopmentFeatures", "Enable in-development features", "System", "Shows experimental features that may be incomplete or unstable." },
		{ "EnableRankedListConnectionFilter", "Ranked list connection filter", "Ranked", "Enables connection-quality filtering in the ranked list filter window." },
		{ "ShowRankedListFilterWindow", "Show ranked list filter window", "Ranked", "Opens the ranked list filter window when the ranked player list is visible." },
		{ "ShowRankedListHiddenPopup", "Show hidden players popup", "Ranked", "Remembers whether the hidden-players popup was left open, so it reopens automatically next time." },
		{ "RankedListSortMode", "Ranked list sort mode", "Ranked", "Sort mode used by the ranked list filter window." },
		{ "RankedListNetworkFilter", "Ranked list network filter", "Ranked", "Hides ranked list players whose Delay rating (0-4) is below this level. 0 shows everyone; players without a measured rating stay visible until it resolves." },
		{ "HideUnmetRequirementRooms", "Hide unmet-requirement rooms", "Ranked", "Hides rooms whose minimum connection quality your measured Delay rating does not meet, so everything left in the ranked list is actually joinable." },
		{ "LoadForeignPalettesToggleDefault", "Load foreign palettes by default", "Palettes", "Default state for loading opponent or foreign custom palettes." },
		{ "AllowPaletteDownloads", "Allow palette downloads", "Palettes", "Allows opponents to save your visible custom palette from the match UI. -1 = not chosen yet (the mod asks once in-game), 0 = no, 1 = yes." },
		{ "SwapControllerPos", "Swap controller positions", "Controller", "Swaps local controller positions. Disabled on startup if known crash risk is detected." },
		{ "EnableControllerHooks", "Enable controller hooks", "Controller", "Enables controller setting hooks used by the mod's controller tools." },
		{ "ForceEnableControllerSettingHooks", "Force controller hooks", "Controller", "Overrides Wine/Proton safety detection and forces controller hooks on." },
		{ "PrimaryKeyboardDeviceId", "Primary keyboard device", "Controller", "Raw input device id treated as the primary keyboard." },
		{ "IgnoredKeyboardIds", "Ignored keyboards", "Controller", "Comma-separated keyboard device ids ignored by keyboard separation tools." },
		{ "KeyboardRenameMap", "Keyboard rename map", "Controller", "Saved display names for detected keyboard devices." },
		{ "KeyboardMappings", "Keyboard mappings", "Controller", "Serialized keyboard-to-player mappings for local keyboard separation." },
		{ "AutomaticallyUpdateControllers", "Auto update controllers", "Controller", "Refreshes controller assignments automatically when devices or Steam Input state changes." },
		{ "AutoRefreshSteamGraceMs", "Steam refresh grace time", "Controller", "Delay before controller auto-refresh reacts to Steam Input changes." },
		{ "AutoRefreshFollowupDelayMs", "Refresh follow-up delay", "Controller", "Delay before a follow-up controller refresh after the first refresh." },
		{ "AutoRefreshTimerIntervalMs", "Refresh timer interval", "Controller", "Polling interval used by controller auto-refresh." },
		{ "UploadReplayData", "Upload replay data", "Replay Database", "Controls replay upload consent. -1 means ask, 0 disables, 1 enables." },
		{ "UploadReplayDataHost", "Replay upload host", "Replay Database", "Server host used for replay database uploads." },
		{ "UploadReplayDataEndpoint", "Replay upload endpoint", "Replay Database", "HTTP endpoint path used for replay database uploads." },
		{ "UploadReplayDataPort", "Replay upload port", "Replay Database", "Server port used for replay database uploads." },
		{ "autoArchive", "Auto archive replays", "Replay Database", "Automatically archives saved replays when supported by the replay tools." },
		{ "FrameHistoryWidth", "Frame history width", "Frame History", "Width of each frame history input cell." },
		{ "FrameHistoryHeight", "Frame history height", "Frame History", "Height of each frame history input cell." },
		{ "FrameHistorySpacing", "Frame history spacing", "Frame History", "Spacing between frame history input cells." },
		{ "FrameHistoryAutoReset", "Frame history auto reset", "Frame History", "Resets frame history automatically when the configured reset condition is met." },
		{ "FrameHistoryEnabled", "Enable frame history", "Frame History", "Shows the frame history overlay." },
		{ "FrameHistoryCountEmptyFrames", "Count empty frame history frames", "Frame History", "Includes frames with no input in frame history counts." },
		{ "Language", "Language", "Interface", "Language code used by the overlay localization system." },
		{ "UnlimitedPlaybackTriggerKeyCode", "Unlimited playback trigger key", "Unlimited Playback", "Virtual key code used to trigger unlimited playback actions." },
		{ "UnlimitedPlaybackLoopKeyCode", "Unlimited playback loop key", "Unlimited Playback", "Virtual key code used to toggle unlimited playback loop mode." },
		{ "UnlimitedPlaybackLoopSetupSeconds", "Loop setup seconds", "Unlimited Playback", "Seconds before loop start reserved for setup." },
		{ "UnlimitedPlaybackLoopEndingSeconds", "Loop ending seconds", "Unlimited Playback", "Seconds near loop end reserved before restart." },
		{ "UnlimitedPlaybackLoopRestartLabState", "Restart lab state on loop", "Unlimited Playback", "Reloads lab state when an unlimited playback loop restarts." },
		{ "UnlimitedPlaybackLoopRestartMode", "Loop restart mode", "Unlimited Playback", "Selects how unlimited playback restarts when looping." },
		{ "D3D9IatFallbackHook", "D3D9 import-table fallback hook", "Graphics", "Rescues the overlay on PCs where another program (notably NVIDIA Optimus' nvd3d9wrap.dll on dual-GPU laptops) hooks Direct3D before the mod and knocks the mod back out, so the game runs fine but no mod window ever appears. Automatic only acts on affected PCs and does nothing everywhere else - leave it there unless you are debugging. Requires a restart." },
		{ "PlatinumVoiceChoice", "Platinum voice choice", "Other", "Picks which voice (Sena or Luna) YOUR Platinum uses instead of the game's random roll. OFFLINE ONLY by default (training, vs CPU, replays, local versus) - there it never affects gameplay and is completely safe. It does NOT apply in online matches unless you enable 'Platinum voice: apply ONLINE' below, which is known to cause desyncs. Applies only to the Platinum you control." },
		{ "PlatinumVoiceOnlineDesyncUnsafe", "Platinum voice: apply ONLINE (CAUSES DESYNC)", "Other", "DANGER - leave OFF. Makes the Platinum voice choice above also apply in ONLINE matches. This is KNOWN TO DESYNC: the game will detect a desync and kick BOTH players out of the match. There is no working desync-safe online version yet; this switch only exists so that work can be continued. Do NOT enable this for normal online play - you will ruin matches for yourself and your opponent." },
	};

	static const SettingMetadata* GetSettingMetadata(const char* iniKey)
	{
		for (const SettingMetadata& metadata : kSettingMetadata)
			if (_stricmp(metadata.iniKey, iniKey) == 0)
				return &metadata;
		return nullptr;
	}

	static const char* const kSettingsCategories[] = {
		"Interface",
		"Hotkeys",
		"Graphics",
		"Controller",
		"Ranked",
		"Frame History",
		"Replay Database",
		"Unlimited Playback",
		"Palettes",
		"System",
		"Debug",
		"Ranked Debug",
		"Replay Takeover Debug",
		"Other",
		nullptr
	};

	// Named-option metadata for int settings that should render as a dropdown instead of
	// a raw number box. Register a setting here (by ini key) to make it an enum in the
	// Settings window; the underlying storage is still a plain `int` (settings.def is
	// unchanged), only the UI widget and the reset/default label change.
	struct SettingEnumOption
	{
		int value;
		const char* label;
	};

	struct SettingEnumMetadata
	{
		const char* iniKey;
		const SettingEnumOption* options;
		int optionCount;
	};

	static const SettingEnumOption kD3D9IatFallbackHookOptions[] = {
		{ 0, "Off" },
		{ 1, "Automatic (recommended)" },
		{ 2, "Always" },
	};

	static const SettingEnumOption kPlatinumVoiceChoiceOptions[] = {
		{ 0, "Default (random)" },
		{ 1, "Luna" },
		{ 2, "Sena" },
	};

	static const SettingEnumMetadata kSettingEnumMetadata[] = {
		{ "PlatinumVoiceChoice", kPlatinumVoiceChoiceOptions, _countof(kPlatinumVoiceChoiceOptions) },
		{ "D3D9IatFallbackHook", kD3D9IatFallbackHookOptions, _countof(kD3D9IatFallbackHookOptions) },
	};

	static const SettingEnumMetadata* GetSettingEnumMetadata(const char* iniKey)
	{
		for (const SettingEnumMetadata& metadata : kSettingEnumMetadata)
			if (_stricmp(metadata.iniKey, iniKey) == 0)
				return &metadata;
		return nullptr;
	}

	static const char* SettingEnumLabelForValue(const SettingEnumMetadata& metadata, int value)
	{
		for (int i = 0; i < metadata.optionCount; ++i)
			if (metadata.options[i].value == value)
				return metadata.options[i].label;
		return "Unknown";
	}

	static bool DrawValueWidget(const char* id, bool& val)
	{
		return ImGui::Checkbox(id, &val);
	}

	// Plain int widget. Settings with enum metadata are routed to DrawEnumValueWidget
	// instead (see DrawValueWidgetForSetting) - this overload stays the fallback for
	// ordinary numeric int settings.
	static bool DrawValueWidget(const char* id, int& val)
	{
		ImGui::PushItemWidth(-1);
		const bool changed = ImGui::InputInt(id, &val);
		ImGui::PopItemWidth();
		return changed;
	}

	static bool DrawEnumValueWidget(const char* id, int& val, const SettingEnumMetadata& metadata)
	{
		bool changed = false;
		ImGui::PushItemWidth(-1);
		if (ImGui::BeginCombo(id, SettingEnumLabelForValue(metadata, val)))
		{
			for (int i = 0; i < metadata.optionCount; ++i)
			{
				const bool selected = metadata.options[i].value == val;
				if (ImGui::Selectable(metadata.options[i].label, selected))
				{
					val = metadata.options[i].value;
					changed = true;
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::PopItemWidth();
		return changed;
	}

	// Dispatch used by the settings.def X-macro: int settings with registered enum
	// metadata render as a combo box; everything else falls back to DrawValueWidget.
	template <typename T>
	static bool DrawValueWidgetForSetting(const char*, const char* id, T& val)
	{
		return DrawValueWidget(id, val);
	}

	static bool DrawValueWidgetForSetting(const char* iniKey, const char* id, int& val)
	{
		if (const SettingEnumMetadata* enumMetadata = GetSettingEnumMetadata(iniKey))
			return DrawEnumValueWidget(id, val, *enumMetadata);
		return DrawValueWidget(id, val);
	}

	static bool DrawValueWidget(const char* id, float& val)
	{
		ImGui::PushItemWidth(-1);
		const bool changed = ImGui::InputFloat(id, &val, 0.0f, 0.0f, "%.4f");
		ImGui::PopItemWidth();
		return changed;
	}

	static bool DrawValueWidget(const char* id, std::string& val)
	{
		char buf[1024];
		strncpy_s(buf, sizeof(buf), val.c_str(), _TRUNCATE);
		ImGui::PushItemWidth(-1);
		const bool changed = ImGui::InputText(id, buf, sizeof(buf));
		ImGui::PopItemWidth();
		if (changed)
			val = buf;
		return changed;
	}

	static bool IsRestartRequired(const char* iniKey)
	{
		static const char* const kRestartKeys[] = {
			"RenderingWidth", "RenderingHeight", "Viewport", "AntiAliasing", "V-sync", "MenuSize",
			"DinputDllWrapper",
			"SwapControllerPos", "EnableControllerHooks", "ForceEnableControllerSettingHooks",
			"PrimaryKeyboardDeviceId", "IgnoredKeyboardIds", "KeyboardRenameMap", "KeyboardMappings",
			"SpectatorSyncHooksEnabled",
			"D3D9IatFallbackHook",
			nullptr
		};
		for (int i = 0; kRestartKeys[i]; ++i)
			if (_stricmp(iniKey, kRestartKeys[i]) == 0)
				return true;
		return false;
	}

	// Parse a settings.def default-value literal into each supported type.
	static void SettingAssignFromString(bool& val, const char* str) { val = atoi(str) != 0; }
	static void SettingAssignFromString(int& val, const char* str) { val = atoi(str); }
	static void SettingAssignFromString(float& val, const char* str) { val = (float)atof(str); }
	static void SettingAssignFromString(std::string& val, const char* str) { val = str; }

	static std::string SettingValueToString(bool val) { return val ? "1" : "0"; }
	static std::string SettingValueToString(int val) { return std::to_string(val); }
	static std::string SettingValueToString(float val)
	{
		std::ostringstream oss;
		oss << val;
		return oss.str();
	}
	static std::string SettingValueToString(const std::string& val) { return val; }

	static void RestartGame()
	{
		wchar_t exePath[MAX_PATH] = {};
		GetModuleFileNameW(NULL, exePath, MAX_PATH);
		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};
		CreateProcessW(exePath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		ExitProcess(0);
	}
}

void SettingsIniWindow::DrawOpenButton()
{
	if (!ImGui::Button("Settings"))
		return;

	BuildRows();
	ImGui::OpenPopup("Settings##modal");
}

void SettingsIniWindow::BuildRows()
{
	m_settingsDraft = Settings::settingsIni;
	m_settingsFilter.Clear();
	m_settingRows.clear();
	m_needsRestart = false;

#define SETTING(_type, _var, _inistring, _defaultval) \
	const SettingMetadata* metadata_##_var = GetSettingMetadata(_inistring); \
	m_settingRows.push_back({ \
		_inistring, \
		metadata_##_var ? metadata_##_var->displayName : _inistring, \
		metadata_##_var ? metadata_##_var->category : "Other", \
		metadata_##_var ? metadata_##_var->tooltip : "No description available.", \
		_defaultval, \
		[this]() -> bool { \
			char buf[128] = "##"; \
			strncat_s(buf, sizeof(buf), _inistring, _TRUNCATE); \
			return DrawValueWidgetForSetting(_inistring, buf, m_settingsDraft._var); \
		}, \
		[this]() -> bool { return m_settingsDraft._var != Settings::settingsIni._var; }, \
		[this]() { SettingAssignFromString(m_settingsDraft._var, _defaultval); }, \
		IsRestartRequired(_inistring) \
	});
#include "Core/settings.def"
#undef SETTING
}

void SettingsIniWindow::DrawModal()
{
	if (m_settingRows.empty())
		return;

	const float footerHeight = m_needsRestart ? 64.0f : 40.0f;

	ImGui::SetNextWindowSize(ImVec2(760, 620), ImGuiCond_Always);
	if (!ImGui::BeginPopupModal("Settings##modal", nullptr, ImGuiWindowFlags_NoResize))
		return;

	ImGui::TextUnformatted("Settings");
	ImGui::SameLine();
	ImGui::TextDisabled("Hover (?) for details. Right-click a setting name to reset it to its default.");
	m_settingsFilter.Draw("Search by name, category, or key##settings_filter", -1.0f);

	ImGui::BeginChild("##settings_scroll", ImVec2(0, -footerHeight), true);

	bool anyRestartRowChangedThisFrame = false;
	bool anyVisibleRow = false;
	const ImGuiStyle& style = ImGui::GetStyle();
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, style.FramePadding.y + 1.0f));

	for (int categoryIndex = 0; kSettingsCategories[categoryIndex]; ++categoryIndex)
	{
		const char* category = kSettingsCategories[categoryIndex];
		int visibleInCategory = 0;
		for (const SettingRow& row : m_settingRows)
		{
			const bool filterMatch =
				m_settingsFilter.PassFilter(row.name.c_str()) ||
				m_settingsFilter.PassFilter(row.displayName.c_str()) ||
				m_settingsFilter.PassFilter(row.category.c_str()) ||
				m_settingsFilter.PassFilter(row.tooltip.c_str());
			if (_stricmp(row.category.c_str(), category) == 0 && filterMatch)
				++visibleInCategory;
		}
		if (visibleInCategory == 0)
			continue;

		anyVisibleRow = true;
		char header[128];
		snprintf(header, sizeof(header), "%s (%d)", category, visibleInCategory);
		const bool open = ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen);
		if (!open)
			continue;

		ImGui::PushID(category);
		// Tables rather than Columns: SetColumnWidth ran every frame, so dragging a separator was
		// undone before the next frame drew. Here the pixel widths are only the initial layout.
		const ImGuiTableFlags settingsTableFlags =
			ImGuiTableFlags_Resizable |
			ImGuiTableFlags_BordersInnerV |
			ImGuiTableFlags_BordersInnerH |
			ImGuiTableFlags_RowBg;

		if (!ImGui::BeginTable("##settings_cols", 3, settingsTableFlags))
		{
			ImGui::PopID();
			continue;
		}

		ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, 295.0f);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 315.0f);
		ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();

		for (SettingRow& row : m_settingRows)
		{
			const bool filterMatch =
				m_settingsFilter.PassFilter(row.name.c_str()) ||
				m_settingsFilter.PassFilter(row.displayName.c_str()) ||
				m_settingsFilter.PassFilter(row.category.c_str()) ||
				m_settingsFilter.PassFilter(row.tooltip.c_str());
			if (_stricmp(row.category.c_str(), category) != 0 || !filterMatch)
				continue;

			ImGui::PushID(row.name.c_str());
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(row.displayName.c_str());
			if (ImGui::BeginPopupContextItem("##reset_ctx"))
			{
				char resetLabel[192];
				const SettingEnumMetadata* enumMetadata = GetSettingEnumMetadata(row.name.c_str());
				const char* defaultLabel = row.defaultValue.empty() ? "empty" : row.defaultValue.c_str();
				if (enumMetadata)
					defaultLabel = SettingEnumLabelForValue(*enumMetadata, atoi(row.defaultValue.c_str()));
				snprintf(resetLabel, sizeof(resetLabel), "Reset to default (%s)", defaultLabel);
				if (ImGui::MenuItem(resetLabel))
				{
					row.resetToDefault();
					if (row.isRestartRequired)
						anyRestartRowChangedThisFrame = true;
				}
				ImGui::EndPopup();
			}
			ImGui::SameLine();
			ImGui::ShowHelpMarker(row.tooltip.c_str());
			ImGui::TextDisabled("%s", row.name.c_str());

			ImGui::TableSetColumnIndex(1);
			if (row.draw() && row.isRestartRequired)
				anyRestartRowChangedThisFrame = true;

			ImGui::TableSetColumnIndex(2);
			if (row.isRestartRequired)
				ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "Restart");
			else
				ImGui::TextDisabled("Live");

			ImGui::PopID();
		}

		ImGui::EndTable();
		ImGui::Spacing();
		ImGui::PopID();
	}
	ImGui::PopStyleVar();

	if (!anyVisibleRow)
		ImGui::TextDisabled("No settings match the current search.");

	if (anyRestartRowChangedThisFrame)
	{
		m_needsRestart = false;
		for (SettingRow& row : m_settingRows)
			if (row.isRestartRequired && row.differsFromOriginal())
			{
				m_needsRestart = true;
				break;
			}
	}

	ImGui::EndChild();

	if (m_needsRestart)
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
			"Some changes require a restart to take effect.");

	const float saveButtonWidth = m_needsRestart ? 150.0f : 120.0f;
	const float footerWidth = 120.0f + saveButtonWidth + 120.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
	ImGui::SetCursorPosX((std::max)(ImGui::GetStyle().WindowPadding.x,
		(ImGui::GetWindowWidth() - footerWidth) * 0.5f));

	if (ImGui::Button("Cancel", ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();

	ImGui::SameLine();

	const char* saveLabel = m_needsRestart ? "Save and Restart" : "Save";
	if (ImGui::Button(saveLabel, ImVec2(saveButtonWidth, 0)))
	{
		const bool debugLogsWereEnabled = Settings::settingsIni.generateDebugLogs;

#define SETTING(_type, _var, _inistring, _defaultval) \
		if (m_settingsDraft._var != Settings::settingsIni._var) { \
			Settings::changeSetting(_inistring, SettingValueToString(m_settingsDraft._var)); \
			Settings::settingsIni._var = m_settingsDraft._var; \
		}
#include "Core/settings.def"
#undef SETTING

		if (Settings::settingsIni.generateDebugLogs != debugLogsWereEnabled)
			SetLoggingEnabled(Settings::settingsIni.generateDebugLogs);

		// Push the settings that subsystems read through g_modVals (keybinds, frame history
		// sizing, palette/upload toggles). Without this they'd only take effect on the next
		// D3D device reset, even though the window labels them "Live".
		Settings::applyRuntimeSettings();

		ImGui::CloseCurrentPopup();

		if (m_needsRestart)
			RestartGame();
	}

	ImGui::SameLine();

	if (ImGui::Button("Restart Game", ImVec2(120, 0)))
		RestartGame();

	ImGui::EndPopup();
}
