#pragma once

#include "Core/HotkeyManager.h"
#include "Core/Settings.h"

#include "imgui.h"

#include <functional>
#include <string>
#include <vector>

class SettingsIniWindow
{
public:
	void DrawOpenButton();

	// Opens the popup without a button, optionally pre-filtered. Used by the mod menu's
	// feature search, so searching for something that turned out to be a setting still takes
	// you to it. Must be called from the same window DrawModal() is called in.
	void Open(const char* initialFilter = nullptr);

	void DrawModal();

private:
	struct SettingRow
	{
		std::string name;
		std::string displayName;
		std::string category;
		std::string tooltip;
		std::string defaultValue;
		std::function<bool()> draw;
		std::function<bool()> differsFromOriginal;
		std::function<void()> resetToDefault;
		bool isRestartRequired;
		// Drawn before the rest of its category. Used for the one or two settings people go
		// looking for by name rather than by scrolling.
		bool pinnedFirst;
	};

	void BuildRows();

	settingsIni_t m_settingsDraft{};
	bool m_needsRestart = false;
	ImGuiTextFilter m_settingsFilter;
	std::vector<SettingRow> m_settingRows;
};
