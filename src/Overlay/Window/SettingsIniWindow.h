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
	};

	void BuildRows();

	settingsIni_t m_settingsDraft{};
	bool m_needsRestart = false;
	ImGuiTextFilter m_settingsFilter;
	std::vector<SettingRow> m_settingRows;
};
