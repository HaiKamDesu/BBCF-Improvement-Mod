#pragma once
#include "IWindow.h"

#include "Core/Settings.h"
#include "Overlay/WindowContainer/WindowContainer.h"

#include "imgui.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

class MainWindow : public IWindow
{
public:
	MainWindow(const std::string& windowTitle, bool windowClosable,
		WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0);

	~MainWindow() override = default;

protected:
	void BeforeDraw() override;
	void Draw() override;

private:
	void DrawUtilButtons() const;
	void DrawCurrentPlayersCount() const;
	void DrawLinkButtons() const;
	void DrawSettingsIniButton();
	void DrawSettingsIniModal();
        void DrawCustomPalettesSection() const;
        void DrawHitboxOverlaySection() const;
        void DrawGameplaySettingSection() const;
        void DrawRankedMatchesSection() const;
        void DrawAvatarSection() const;
        void DrawFrameAdvantageSection() const;
        void DrawFrameHistorySection() const;
        void DrawControllerSettingSection() const;
        void DrawLanguageSelector();
	const ImVec2 BTN_SIZE = ImVec2(60, 20);
	WindowContainer* m_pWindowContainer = nullptr;
	settingsIni_t m_settingsDraft{};
	enum class SettingsSortOrder : int { Default = 0, Ascending, Descending };
	SettingsSortOrder m_settingsSortOrder = SettingsSortOrder::Default;
	bool m_needsRestart = false;
	ImGuiTextFilter m_settingsFilter;
	struct SettingRow {
		std::string name;
		std::function<bool()> draw;               // draws widget; returns true if changed this frame
		std::function<bool()> differsFromOriginal; // returns true if draft != original
		bool isRestartRequired;
	};
	std::vector<SettingRow> m_settingRows;
};
