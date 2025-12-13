#include "MainWindow.h"

#include "FrameAdvantage/FrameAdvantage.h"
#include "HitboxOverlay.h"
#include "PaletteEditorWindow.h"
#include "FrameHistory/FrameHistoryWindow.h"
#include "FrameAdvantage/FrameAdvantageWindow.h"

#include "Core/Settings.h"
#include "Core/logger.h"
#include "Core/info.h"
#include "Core/interfaces.h"
#include "Core/utils.h"
#include "Core/Localization.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Window/ControllerSettings/ControllerSettingsSection.h"
#include "Overlay/Widget/ActiveGameModeWidget.h"
#include "Overlay/Widget/GameModeSelectWidget.h"
#include "Overlay/Widget/StageSelectWidget.h"

#include <Windows.h>

#include "imgui_internal.h"

#include <array>
#include <sstream>
#include <utility>
#include <cstring>

MainWindow::MainWindow(const std::string& windowTitle, bool windowClosable, WindowContainer& windowContainer, ImGuiWindowFlags windowFlags)
	: IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer)
{
	m_windowTitle = MOD_WINDOW_TITLE;
	m_windowTitle += " ";
	m_windowTitle += MOD_VERSION_NUM;

#ifdef _DEBUG
	m_windowTitle += " (DEBUG)";
#endif

	m_windowTitle += "###MainTitle"; // Set unique identifier
}

void MainWindow::BeforeDraw()
{
	ImGui::SetWindowPos(m_windowTitle.c_str(), ImVec2(12, 20), ImGuiCond_FirstUseEver);

	ImVec2 windowSizeConstraints;
	switch (Settings::settingsIni.menusize)
	{
	case 1:
		windowSizeConstraints = ImVec2(250, 190);
		break;
	case 3:
		windowSizeConstraints = ImVec2(400, 230);
		break;
	default:
		windowSizeConstraints = ImVec2(330, 230);
	}

	ImGui::SetNextWindowSizeConstraints(windowSizeConstraints, ImVec2(1000, 1000));
}

void MainWindow::Draw()
{
        ImGui::Text(L("Toggle me with %s").c_str(), Settings::settingsIni.togglebutton.c_str());
        ImGui::Text(L("Toggle Online with %s").c_str(), Settings::settingsIni.toggleOnlineButton.c_str());
        ImGui::Text(L("Toggle HUD with %s").c_str(), Settings::settingsIni.toggleHUDbutton.c_str());
        ImGui::Separator();

        ImGui::VerticalSpacing(5);

        DrawLanguageSelector();

        ImGui::HorizontalSpacing();
        bool generateDebugLogs = Settings::settingsIni.generateDebugLogs;
        if (ImGui::Checkbox(L("Generate Debug Logs").c_str(), &generateDebugLogs))
        {
                Settings::settingsIni.generateDebugLogs = generateDebugLogs;
                Settings::changeSetting("GenerateDebugLogs", generateDebugLogs ? "1" : "0");
                SetLoggingEnabled(generateDebugLogs);
        }
        ImGui::SameLine();
        ImGui::ShowHelpMarker(L("Write DEBUG.txt with detailed runtime information. Saved to settings.ini for future sessions.").c_str());

	ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("P$"); ImGui::SameLine();
	if (g_gameVals.pGameMoney)
	{
		ImGui::InputInt("##P$", *&g_gameVals.pGameMoney);
	}

	ImGui::VerticalSpacing(5);

        if (ImGui::Button(L("Online").c_str(), BTN_SIZE))
        {
                m_pWindowContainer->GetWindow(WindowType_Room)->ToggleOpen();
        }

	ImGui::VerticalSpacing(5);

	DrawGameplaySettingSection();
	DrawCustomPalettesSection();
	DrawHitboxOverlaySection();
	DrawFrameHistorySection();
	DrawFrameAdvantageSection();
	DrawAvatarSection();
	DrawControllerSettingSection();
	DrawLoadedSettingsValuesSection();
	DrawUtilButtons();

	ImGui::VerticalSpacing(5);

        DrawCurrentPlayersCount();
        DrawLinkButtons();
}

void MainWindow::DrawLanguageSelector()
{
        ImGui::HorizontalSpacing();

        const auto& options = Localization::GetAvailableLanguages();

        int currentIndex = 0;
        for (size_t i = 0; i < options.size(); ++i)
        {
                if (options[i].code == Localization::GetCurrentLanguage())
                {
                        currentIndex = static_cast<int>(i);
                        break;
                }
        }

        const auto& currentOption = options[currentIndex];
        std::string preview = currentOption.displayName;

        std::string pendingLanguage;
        bool shouldReload = false;

        if (ImGui::BeginCombo(L("Language").c_str(), preview.c_str()))
        {
                for (size_t i = 0; i < options.size(); ++i)
                {
                        const auto& option = options[i];
                        const bool optionComplete = option.complete;
                        const size_t missingKeys = option.missingKeys;
                        const std::string languageCode = option.code;

                        std::string label = option.displayName;
                        if (!optionComplete)
                        {
                                label = FormatText(L("Language incomplete label").c_str(), option.displayName.c_str(), missingKeys);
                        }

                        if (!optionComplete)
                        {
                                ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                        }

                        bool selected = currentIndex == static_cast<int>(i);
                        if (ImGui::Selectable(label.c_str(), selected))
                        {
                                pendingLanguage = languageCode;
                                shouldReload = true;
                                currentIndex = static_cast<int>(i);
                        }

                        if (!optionComplete)
                        {
                                ImGui::PopStyleVar();
                                ImGui::PopItemFlag();
                        }
                }

                ImGui::EndCombo();
        }

        if (shouldReload)
        {
                Localization::Reload(pendingLanguage);
                Settings::settingsIni.language = Localization::GetCurrentLanguage();
                Settings::changeSetting("Language", Settings::settingsIni.language);
        }

        ImGui::SameLine();
        ImGui::ShowHelpMarker(L("Language selection help").c_str());
}

void MainWindow::DrawUtilButtons() const
{
#ifdef _DEBUG
        if (ImGui::Button("DEBUG", BTN_SIZE))
        {
                m_pWindowContainer->GetWindow(WindowType_Debug)->ToggleOpen();
        }
#endif

        if (ImGui::Button(L("Log").c_str(), BTN_SIZE))
        {
                m_pWindowContainer->GetWindow(WindowType_Log)->ToggleOpen();
        }
        if (ImGui::Button(L("States").c_str(), BTN_SIZE))
        {
                m_pWindowContainer->GetWindow(WindowType_Scr)->ToggleOpen();
        }
}

void MainWindow::DrawCurrentPlayersCount() const
{
        ImGui::Text("%s", L("Current online players:").c_str());
        ImGui::SameLine();

        std::string currentPlayersCount = g_interfaces.pSteamApiHelper ? g_interfaces.pSteamApiHelper->GetCurrentPlayersCountString() : L("<No data>");
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", currentPlayersCount.c_str());
}

void MainWindow::DrawAvatarSection() const
{
        if (!ImGui::CollapsingHeader(L("Avatar settings").c_str()))
                return;

        if (g_gameVals.playerAvatarAddr == NULL && g_gameVals.playerAvatarColAddr == NULL && g_gameVals.playerAvatarAcc1 == NULL && g_gameVals.playerAvatarAcc2 == NULL)
        {
                ImGui::HorizontalSpacing();
                ImGui::TextDisabled(L("CONNECT TO NETWORK MODE FIRST").c_str());
        }
        else
        {
                ImGui::HorizontalSpacing(); ImGui::SliderInt(L("Avatar").c_str(), g_gameVals.playerAvatarAddr, 0, 0x2F);
                ImGui::HorizontalSpacing(); ImGui::SliderInt(L("Color").c_str(), g_gameVals.playerAvatarColAddr, 0, 0x3);
                ImGui::HorizontalSpacing(); ImGui::SliderByte(L("Accessory 1").c_str(), g_gameVals.playerAvatarAcc1, 0, 0xCF);
                ImGui::HorizontalSpacing(); ImGui::SliderByte(L("Accessory 2").c_str(), g_gameVals.playerAvatarAcc2, 0, 0xCF);
        }
}


void MainWindow::DrawFrameHistorySection() const
{
        if (!ImGui::CollapsingHeader(L("FrameHistory").c_str()))
                return;

	if (!isFrameHistoryEnabledInCurrentState()) {
		ImGui::HorizontalSpacing();
                ImGui::TextDisabled(L("YOU ARE NOT IN A MATCH, IN TRAINING MODE OR REPLAY THEATER!").c_str());
		return;
	}
	if (g_interfaces.player1.IsCharDataNullPtr() || g_interfaces.player2.IsCharDataNullPtr()) {
		ImGui::HorizontalSpacing();
                ImGui::TextDisabled(L("THERE WAS AN ERROR LOADING ONE/BOTH OF THE CHARACTERS").c_str());
		return;
	}
	//if (g_interfaces.player1.GetData()->charIndex == g_interfaces.player2.GetData()->charIndex) {
	//	ImGui::HorizontalSpacing();
	//	ImGui::TextDisabled("THIS FEATURE CURRENTLY DOES NOT SUPPORT MIRRORS! IF IT ISN'T A MIRROR THERE WAS AN ERROR LOADING ONE OF THE CHARACTERS");
	//	return;
	//}
	static bool isOpen = false;

	FrameHistoryWindow* frameHistWin = m_pWindowContainer->GetWindow<FrameHistoryWindow>(WindowType_FrameHistory);


	ImGui::HorizontalSpacing();
        ImGui::Checkbox(L("Enable##framehistory_section").c_str(), &isOpen);
        ImGui::SameLine();
        ImGui::ShowHelpMarker(L("FrameHistory help").c_str());
	if (isOpen)
	{
		frameHistWin->Open();
	}
	else
	{
		frameHistWin->Close();
	}

	ImGui::HorizontalSpacing();
        ImGui::Checkbox(L("Auto Reset##Reset after each idle frame").c_str(), &frameHistWin->resetting);
        ImGui::SameLine();
        ImGui::ShowHelpMarker(L("FrameHistory auto reset help").c_str());

	ImGui::HorizontalSpacing();
        if (ImGui::SliderFloat(L("Box width").c_str(), &frameHistWin->width, 1., 100.)) {
                Settings::changeSetting("FrameHistoryWidth", std::to_string(frameHistWin->width));
        }
        ImGui::HorizontalSpacing();
        if (ImGui::SliderFloat(L("Box height").c_str(), &frameHistWin->height, 1., 100.)) {
                Settings::changeSetting("FrameHistoryHeight", std::to_string(frameHistWin->height));
        }
        ImGui::HorizontalSpacing();
        if (ImGui::SliderFloat(L("spacing").c_str(), &frameHistWin->spacing, 1., 100.)) {
                Settings::changeSetting("FrameHistorySpacing", std::to_string(frameHistWin->spacing));
        };


}



void MainWindow::DrawFrameAdvantageSection() const
{
        if (!ImGui::CollapsingHeader(L("Framedata").c_str()))
                return;

	if (!isInMatch())
	{
		ImGui::HorizontalSpacing();
                ImGui::TextDisabled(L("YOU ARE NOT IN MATCH!").c_str());
                return;
        }
        else if (!(*g_gameVals.pGameMode == GameMode_Training || *g_gameVals.pGameMode == GameMode_ReplayTheater))
        {
                ImGui::HorizontalSpacing();
                ImGui::TextDisabled(L("YOU ARE NOT IN TRAINING MODE OR REPLAY THEATER!").c_str());
                return;
        }

	if (!g_gameVals.pEntityList)
		return;

	static bool isFrameAdvantageOpen = false;
	ImGui::HorizontalSpacing();
        ImGui::Checkbox(L("Enable##framedata_section").c_str(), &isFrameAdvantageOpen);
        //ImGui::Checkbox("Enable##framedata_section", &isFrameAdvantageOpen);
        ImGui::HorizontalSpacing();
        ImGui::Checkbox(L("Advantage on stagger hit").c_str(), &idleActionToggles.ukemiStaggerHit);

	if (isFrameAdvantageOpen)
	{
		m_pWindowContainer->GetWindow(WindowType_FrameAdvantage)->Open();

	}
	else
	{
		m_pWindowContainer->GetWindow(WindowType_FrameAdvantage)->Close();
		//frameAdvWin->Close();
	}
}

void MainWindow::DrawCustomPalettesSection() const
{
        if (!ImGui::CollapsingHeader(L("Custom palettes").c_str()))
                return;

	if (!isInMatch())
	{
		ImGui::HorizontalSpacing();
                ImGui::TextDisabled(L("YOU ARE NOT IN MATCH!").c_str());
        }
        else
        {
                ImGui::HorizontalSpacing();
                m_pWindowContainer->GetWindow<PaletteEditorWindow>(WindowType_PaletteEditor)->ShowAllPaletteSelections("Main");
	}

	ImGui::VerticalSpacing(15);
	ImGui::HorizontalSpacing();
	m_pWindowContainer->GetWindow<PaletteEditorWindow>(WindowType_PaletteEditor)->ShowReloadAllPalettesButton();

	if (isPaletteEditingEnabledInCurrentState())
	{
		ImGui::HorizontalSpacing();

                if (ImGui::Button(L("Palette editor").c_str()))
                        m_pWindowContainer->GetWindow(WindowType_PaletteEditor)->ToggleOpen();
        }
}

void MainWindow::DrawHitboxOverlaySection() const
{
        if (!ImGui::CollapsingHeader(L("Hitbox overlay").c_str()))
                return;

	if (!isHitboxOverlayEnabledInCurrentState())
	{
		ImGui::HorizontalSpacing();
                ImGui::TextDisabled(L("YOU ARE NOT IN TRAINING, VERSUS, OR REPLAY!").c_str());
                return;
        }

	static bool isOpen = false;

	ImGui::HorizontalSpacing();
        if (ImGui::Checkbox(L("Enable##hitbox_overlay_section").c_str(), &isOpen))
        {
		if (isOpen)
		{
			m_pWindowContainer->GetWindow(WindowType_HitboxOverlay)->Open();
		}
		else
		{
			g_gameVals.isFrameFrozen = false;
			m_pWindowContainer->GetWindow(WindowType_HitboxOverlay)->Close();
		}
	}

	if (isOpen)
	{
		ImGui::VerticalSpacing(10);

                        if (!g_interfaces.player1.IsCharDataNullPtr() && !g_interfaces.player2.IsCharDataNullPtr())
                        {
                                ImGui::HorizontalSpacing();
                                ImGui::Checkbox(L("Player1").c_str(), &m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->drawCharacterHitbox[0]);
                                ImGui::HoverTooltip(getCharacterNameByIndexA(g_interfaces.player1.GetData()->charIndex).c_str());
                                ImGui::SameLine(); ImGui::HorizontalSpacing();
                                ImGui::TextUnformatted(g_interfaces.player1.GetData()->currentAction);

                                ImGui::HorizontalSpacing();
                                ImGui::Checkbox(L("Player2").c_str(), &m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->drawCharacterHitbox[1]);
                                ImGui::HoverTooltip(getCharacterNameByIndexA(g_interfaces.player2.GetData()->charIndex).c_str());
                                ImGui::SameLine(); ImGui::HorizontalSpacing();
                                ImGui::TextUnformatted(g_interfaces.player2.GetData()->currentAction);
		}

		ImGui::VerticalSpacing(10);

		ImGui::HorizontalSpacing();
		m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->DrawRectThicknessSlider();

		ImGui::HorizontalSpacing();
		m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->DrawRectFillTransparencySlider();

		ImGui::HorizontalSpacing();
                ImGui::Checkbox(L("Draw hitbox/hurtbox").c_str(),
                        &m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->drawHitboxHurtbox);
                ImGui::HorizontalSpacing();
                ImGui::Checkbox(L("Draw origin").c_str(),
                        &m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->drawOriginLine);
                ImGui::SameLine();
                ImGui::ShowHelpMarker(L("The point in space where your character resides. \nImportant!: This is a single point, the render is composed of two lines to facilitate viewing, the actual point is where the two lines touch.").c_str());
                ImGui::HorizontalSpacing();
                ImGui::Checkbox(L("Draw collision").c_str(),
                        &m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->drawCollisionBoxes);
                ImGui::SameLine();
                ImGui::ShowHelpMarker(L("Defines collision between objects/characters. Also used for throw range checks.").c_str());
                ImGui::HorizontalSpacing();
                ImGui::Checkbox(L("Draw throw/range boxes").c_str(),
                        &m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->drawRangeCheckBoxes);
                ImGui::SameLine();
                ImGui::ShowHelpMarker(L("Throw range help").c_str());
		ImGui::VerticalSpacing();

		ImGui::HorizontalSpacing();
                ImGui::Checkbox(L("Freeze frame:").c_str(), &g_gameVals.isFrameFrozen);
                if (ImGui::IsKeyPressed(g_modVals.freeze_frame_keycode))
                        g_gameVals.isFrameFrozen ^= 1;

		if (g_gameVals.pFrameCount)
		{
			ImGui::SameLine();
                        ImGui::Text("%d", *g_gameVals.pFrameCount);
                        ImGui::SameLine();
                        if (ImGui::Button(L("Reset").c_str()))
                        {
                                *g_gameVals.pFrameCount = 0;
                                g_gameVals.framesToReach = 0;
                        }
		}

		if (g_gameVals.isFrameFrozen)
		{
			static int framesToStep = 1;
			ImGui::HorizontalSpacing();
                        if (ImGui::Button(L("Step frames").c_str()) || ImGui::IsKeyPressed(g_modVals.step_frames_keycode))
                        {
                                g_gameVals.framesToReach = *g_gameVals.pFrameCount + framesToStep;
                        }

			ImGui::SameLine();
			ImGui::SliderInt("", &framesToStep, 1, 60);
		}
	}
}

void MainWindow::DrawGameplaySettingSection() const
{
        if (!ImGui::CollapsingHeader(L("Gameplay settings header").c_str()))
                return;

	if (!isInMatch() && !isOnVersusScreen() && !isOnReplayMenuScreen() && !isOnCharacterSelectionScreen())
	{
		ImGui::HorizontalSpacing();
                ImGui::TextDisabled(L("YOU ARE NOT IN MATCH!").c_str());

                ImGui::HorizontalSpacing();
                ImGui::TextDisabled(L("YOU ARE NOT IN REPLAY MENU!").c_str());

                ImGui::HorizontalSpacing();
                ImGui::TextDisabled(L("YOU ARE NOT IN CHARACTER SELECTION!").c_str());

		return;
	}

	if (isStageSelectorEnabledInCurrentState())
	{
		ImGui::HorizontalSpacing();
		StageSelectWidget();
		ImGui::VerticalSpacing(10);
	}

	ImGui::HorizontalSpacing();
	ActiveGameModeWidget();

	if (isGameModeSelectorEnabledInCurrentState())
	{
		bool isThisPlayerSpectator = g_interfaces.pRoomManager->IsRoomFunctional() && g_interfaces.pRoomManager->IsThisPlayerSpectator();

		if (!isThisPlayerSpectator)
		{
			ImGui::HorizontalSpacing();
			GameModeSelectWidget();
		}
	}

	if (isInMatch())
	{
		ImGui::VerticalSpacing(10);
		ImGui::HorizontalSpacing();
                ImGui::Checkbox(L("Hide HUD checkbox").c_str(), (bool*)g_gameVals.pIsHUDHidden);
        }
}
void MainWindow::DrawControllerSettingSection() const {
        if (!ImGui::CollapsingHeader(L("Controller Settings").c_str()))
                return;

	ControllerSettings::DrawSection();
} // DrawControllerSettingSection

void MainWindow::DrawLinkButtons() const
{
	//ImGui::ButtonUrl("Replay Database", REPLAY_DB_FRONTEND, BTN_SIZE);
        if (*g_gameVals.pGameMode == GameMode_ReplayTheater) {
                if (ImGui::Button(L("Toggle Rewind").c_str()))
                        m_pWindowContainer->GetWindow(WindowType_ReplayRewind)->ToggleOpen();
        }
        ImGui::ButtonUrl(L("Replay Database").c_str(), REPLAY_DB_FRONTEND);
        ImGui::SameLine();
        if (ImGui::Button(L("Enable/Disable Upload").c_str())) {
                m_pWindowContainer->GetWindow(WindowType_ReplayDBPopup)->ToggleOpen();
        }


        ImGui::ButtonUrl(L("Discord").c_str(), MOD_LINK_DISCORD, BTN_SIZE);

        ImGui::SameLine();
        ImGui::ButtonUrl(L("Forum").c_str(), MOD_LINK_FORUM, BTN_SIZE);

        ImGui::SameLine();
        ImGui::ButtonUrl(L("GitHub").c_str(), MOD_LINK_GITHUB, BTN_SIZE);

}

void MainWindow::DrawLoadedSettingsValuesSection() const
{
        if (!ImGui::CollapsingHeader(L("Loaded settings.ini values").c_str()))
                return;

	// Not using ImGui columns here because they are bugged if the window has always_autoresize flag. The window 
	// starts extending to infinity, if the left edge of the window touches any edges of the screen

	std::ostringstream oss;

	ImGui::BeginChild("loaded_settings", ImVec2(0, 300.0f), true, ImGuiWindowFlags_HorizontalScrollbar);

	//X-Macro
#define SETTING(_type, _var, _inistring, _defaultval) \
	oss << " " << _inistring; \
	ImGui::TextUnformatted(oss.str().c_str()); ImGui::SameLine(ImGui::GetWindowWidth() * 0.6f); \
	oss.str(""); \
	oss << "= " << Settings::settingsIni.##_var; \
	ImGui::TextUnformatted(oss.str().c_str()); ImGui::Separator(); \
	oss.str("");
#include "Core/settings.def"
#undef SETTING

	ImGui::EndChild();
}
