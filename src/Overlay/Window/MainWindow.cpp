#include "MainWindow.h"

#include "FrameAdvantage/FrameAdvantage.h"
#include "HitboxOverlay.h"
#include "PaletteEditorWindow.h"
#include "FrameHistory/FrameHistoryWindow.h"
#include "FrameAdvantage/FrameAdvantageWindow.h"
#include "Ranked/RankedProgressWindow.h"
#include "SettingsIniWindow.h"

#include "Core/HotkeyManager.h"
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
#include "Updater/UpdateCoordinator.h"
#include "Audio/MusicManager.h"

#include <Windows.h>

#include "imgui_internal.h"

#include <string>
#include <utility>

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
	ImGui::Text(Messages.Toggle_me_with_s(),
		HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_ToggleMainWindow)).c_str());
	ImGui::Text(Messages.Toggle_Online_with_s(),
		HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_ToggleOnlineWindow)).c_str());
	ImGui::Text(Messages.Toggle_HUD_with_s(),
		HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_ToggleHud)).c_str());
	ImGui::Separator();

	ImGui::VerticalSpacing(5);

	DrawLanguageSelector();

	ImGui::HorizontalSpacing();
	m_settingsIniWindow.DrawOpenButton();
	ImGui::SameLine();
	m_palettesConfigWindow.DrawOpenButton();

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("P$"); ImGui::SameLine();
	if (g_gameVals.pGameMoney)
	{
		ImGui::InputInt("##P$", *&g_gameVals.pGameMoney);
		ImGui::SameLine();
		ImGui::ShowHelpMarker(Messages.Player_money_tooltip());
	}

	ImGui::VerticalSpacing(5);

	DrawGameplaySettingSection();
	DrawRankedMatchesSection();
	DrawCustomPalettesSection();
	DrawHitboxOverlaySection();
	DrawFrameHistorySection();
	DrawFrameAdvantageSection();
	DrawAvatarSection();
	DrawControllerSettingSection();
	DrawMusicSection();
	m_settingsIniWindow.DrawModal();
	m_palettesConfigWindow.DrawModal();
	DrawUtilButtons();

	ImGui::VerticalSpacing(5);

	DrawCurrentPlayersCount();
	DrawLinkButtons();
	Updater::UpdateCoordinator::GetInstance().DrawSkippedMainMenuLink();
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

	if (ImGui::BeginCombo(Messages.Language(), preview.c_str()))
	{
		for (size_t i = 0; i < options.size(); ++i)
		{
			const auto& option = options[i];
			const bool optionComplete = option.complete;
			const std::string languageCode = option.code;

			std::string label = option.displayName;
			if (!optionComplete)
			{
				label += Messages.Language_incomplete_label();
			}

			if (!optionComplete)
			{
				ImGui::BeginDisabled();
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
				ImGui::EndDisabled();
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
	ImGui::ShowHelpMarker(Messages.Language_selection_help());
}

void MainWindow::DrawUtilButtons() const
{
#ifdef _DEBUG
	const bool showDebugButton = true;
#else
	// Debug builds always get the DEBUG window; other builds only show it once
	// the user opts into dev/diagnostic tooling (settings.def: EnableInDevelopmentFeatures).
	const bool showDebugButton = Settings::settingsIni.enableInDevelopmentFeatures;
#endif
	if (showDebugButton)
	{
		if (ImGui::Button("DEBUG", BTN_SIZE))
		{
			m_pWindowContainer->GetWindow(WindowType_Debug)->ToggleOpen();
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(Messages.Debug_window_tooltip());
		ImGui::SameLine();
	}

	if (ImGui::Button(Messages.Log(), BTN_SIZE))
	{
		m_pWindowContainer->GetWindow(WindowType_Log)->ToggleOpen();
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(Messages.Log_window_tooltip());
	ImGui::SameLine();
	if (ImGui::Button(Messages.States(), BTN_SIZE))
	{
		m_pWindowContainer->GetWindow(WindowType_Scr)->ToggleOpen();
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(Messages.States_window_tooltip());

}

void MainWindow::DrawCurrentPlayersCount() const
{
	ImGui::Text("%s", Messages.Current_online_players());
	ImGui::SameLine();

	std::string currentPlayersCount = g_interfaces.pSteamApiHelper ? g_interfaces.pSteamApiHelper->GetCurrentPlayersCountString() : Messages.No_data();
	ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", currentPlayersCount.c_str());
}

void MainWindow::DrawAvatarSection() const
{
	if (!ImGui::CollapsingHeader(Messages.Avatar_settings()))
		return;

	if (g_gameVals.playerAvatarAddr == NULL && g_gameVals.playerAvatarColAddr == NULL && g_gameVals.playerAvatarAcc1 == NULL && g_gameVals.playerAvatarAcc2 == NULL)
	{
		ImGui::HorizontalSpacing();
		ImGui::TextDisabled(Messages.CONNECT_TO_NETWORK_MODE_FIRST());
	}
	else
	{
		ImGui::HorizontalSpacing(); ImGui::SliderInt(Messages.Avatar(), g_gameVals.playerAvatarAddr, 0, 0x2F);
		ImGui::SameLine(); ImGui::ShowHelpMarker(Messages.Avatar_icon_tooltip());
		ImGui::HorizontalSpacing(); ImGui::SliderInt(Messages.Color(), g_gameVals.playerAvatarColAddr, 0, 0x3);
		ImGui::SameLine(); ImGui::ShowHelpMarker(Messages.Avatar_color_tooltip());
		ImGui::HorizontalSpacing(); ImGui::SliderByte(Messages.Accessory_1(), g_gameVals.playerAvatarAcc1, 0, 0xCF);
		ImGui::SameLine(); ImGui::ShowHelpMarker(Messages.Avatar_accessory1_tooltip());
		ImGui::HorizontalSpacing(); ImGui::SliderByte(Messages.Accessory_2(), g_gameVals.playerAvatarAcc2, 0, 0xCF);
		ImGui::SameLine(); ImGui::ShowHelpMarker(Messages.Avatar_accessory2_tooltip());
	}
}


void MainWindow::DrawFrameHistorySection() const
{
	if (!ImGui::CollapsingHeader(Messages.FrameHistory()))
		return;

	if (!isFrameHistoryEnabledInCurrentState()) {
		ImGui::HorizontalSpacing();
		ImGui::TextDisabled(Messages.YOU_ARE_NOT_IN_A_MATCH_IN_TRAINING_MODE_OR_REPLAY_THEATER());
		return;
	}
	if (g_interfaces.player1.IsCharDataNullPtr() || g_interfaces.player2.IsCharDataNullPtr()) {
		ImGui::HorizontalSpacing();
		ImGui::TextDisabled(Messages.THERE_WAS_AN_ERROR_LOADING_ONE_BOTH_OF_THE_CHARACTERS());
		return;
	}
	//if (g_interfaces.player1.GetData()->charIndex == g_interfaces.player2.GetData()->charIndex) {
	//	ImGui::HorizontalSpacing();
	//	ImGui::TextDisabled("THIS FEATURE CURRENTLY DOES NOT SUPPORT MIRRORS! IF IT ISN'T A MIRROR THERE WAS AN ERROR LOADING ONE OF THE CHARACTERS");
	//	return;
	//}
	FrameHistoryWindow* frameHistWin = m_pWindowContainer->GetWindow<FrameHistoryWindow>(WindowType_FrameHistory);

	ImGui::HorizontalSpacing();
	bool isOpen = Settings::settingsIni.frameHistoryEnabled;
	if (ImGui::Checkbox(Messages.Enable_framehistory_section(), &isOpen))
	{
		Settings::settingsIni.frameHistoryEnabled = isOpen;
		Settings::changeSetting("FrameHistoryEnabled", isOpen ? "1" : "0");
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(Messages.FrameHistory_help());
	if (isOpen)
		frameHistWin->Open();
	else
		frameHistWin->Close();

	ImGui::HorizontalSpacing();
	if (ImGui::Checkbox(Messages.Auto_Reset_Reset_after_each_idle_frame(), &frameHistWin->resetting))
	{
		Settings::settingsIni.frameHistoryAutoReset = frameHistWin->resetting;
		Settings::changeSetting("FrameHistoryAutoReset", frameHistWin->resetting ? "1" : "0");
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(Messages.FrameHistory_auto_reset_help());

	ImGui::HorizontalSpacing();
	if (ImGui::Checkbox(Messages.Count_empty_frames_framehistory(), &frameHistWin->countEmptyFrames))
	{
		Settings::settingsIni.frameHistoryCountEmptyFrames = frameHistWin->countEmptyFrames;
		Settings::changeSetting("FrameHistoryCountEmptyFrames", frameHistWin->countEmptyFrames ? "1" : "0");
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(Messages.FrameHistory_count_empty_frames_help());

	ImGui::HorizontalSpacing();
	if (ImGui::SliderFloat(Messages.Box_width(), &frameHistWin->width, 1., 100.)) {
		Settings::changeSetting("FrameHistoryWidth", std::to_string(frameHistWin->width));
	}
	ImGui::HorizontalSpacing();
	if (ImGui::SliderFloat(Messages.Box_height(), &frameHistWin->height, 1., 100.)) {
		Settings::changeSetting("FrameHistoryHeight", std::to_string(frameHistWin->height));
	}
	ImGui::HorizontalSpacing();
	if (ImGui::SliderFloat(Messages.spacing(), &frameHistWin->spacing, 1., 100.)) {
		Settings::changeSetting("FrameHistorySpacing", std::to_string(frameHistWin->spacing));
	};


}



void MainWindow::DrawFrameAdvantageSection() const
{
	if (!ImGui::CollapsingHeader(Messages.Framedata()))
		return;

	if (!isInMatch())
	{
		ImGui::HorizontalSpacing();
		ImGui::TextDisabled(Messages.YOU_ARE_NOT_IN_MATCH());
		return;
	}
	else if (!(*g_gameVals.pGameMode == GameMode_Training || *g_gameVals.pGameMode == GameMode_ReplayTheater))
	{
		ImGui::HorizontalSpacing();
		ImGui::TextDisabled(Messages.YOU_ARE_NOT_IN_TRAINING_MODE_OR_REPLAY_THEATER());
		return;
	}

	if (!g_gameVals.pEntityList)
		return;

	static bool isFrameAdvantageOpen = false;
	ImGui::HorizontalSpacing();
	ImGui::Checkbox(Messages.Enable_framedata_section(), &isFrameAdvantageOpen);
	ImGui::SameLine();
	ImGui::ShowHelpMarker(Messages.Enable_framedata_tooltip());
	//ImGui::Checkbox("Enable##framedata_section", &isFrameAdvantageOpen);
	ImGui::HorizontalSpacing();
	ImGui::Checkbox(Messages.Advantage_on_stagger_hit(), &idleActionToggles.ukemiStaggerHit);
	ImGui::SameLine();
	ImGui::ShowHelpMarker(Messages.Advantage_stagger_hit_tooltip());

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
	if (!ImGui::CollapsingHeader(Messages.Custom_palettes()))
		return;

	if (!isInMatch())
	{
		ImGui::HorizontalSpacing();
		ImGui::TextDisabled(Messages.YOU_ARE_NOT_IN_MATCH());
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

		if (ImGui::Button(Messages.Palette_editor()))
			m_pWindowContainer->GetWindow(WindowType_PaletteEditor)->ToggleOpen();
		ImGui::SameLine();
		ImGui::ShowHelpMarker(Messages.Palette_editor_tooltip());
	}
}

void MainWindow::DrawHitboxOverlaySection() const
{
	if (!ImGui::CollapsingHeader(Messages.Hitbox_overlay()))
		return;

	if (!isHitboxOverlayEnabledInCurrentState())
	{
		ImGui::HorizontalSpacing();
		ImGui::TextDisabled(Messages.YOU_ARE_NOT_IN_TRAINING_VERSUS_OR_REPLAY());
		return;
	}

	static bool isOpen = false;

	ImGui::HorizontalSpacing();
	bool hitboxOverlayToggled = ImGui::Checkbox(Messages.Enable_hitbox_overlay_section(), &isOpen);
	ImGui::SameLine();
	ImGui::ShowHelpMarker(Messages.Enable_hitbox_overlay_tooltip());
	if (hitboxOverlayToggled)
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
			ImGui::Checkbox(Messages.Player1(), &m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->drawCharacterHitbox[0]);
			ImGui::HoverTooltip(getCharacterNameByIndexA(g_interfaces.player1.GetData()->charIndex).c_str());
			ImGui::SameLine(); ImGui::HorizontalSpacing();
			ImGui::TextUnformatted(g_interfaces.player1.GetData()->currentAction);

			ImGui::HorizontalSpacing();
			ImGui::Checkbox(Messages.Player2(), &m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->drawCharacterHitbox[1]);
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
		ImGui::Checkbox(Messages.Draw_hitbox_hurtbox(),
			&m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->drawHitboxHurtbox);
		ImGui::SameLine();
		ImGui::ShowHelpMarker(Messages.Draw_hitbox_hurtbox_tooltip());
		ImGui::HorizontalSpacing();
		ImGui::Checkbox(Messages.Draw_origin(),
			&m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->drawOriginLine);
		ImGui::SameLine();
		ImGui::ShowHelpMarker(Messages.Origin_point_note());
		ImGui::HorizontalSpacing();
		ImGui::Checkbox(Messages.Draw_collision(),
			&m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->drawCollisionBoxes);
		ImGui::SameLine();
		ImGui::ShowHelpMarker(Messages.Collision_box_note());
		ImGui::HorizontalSpacing();
		ImGui::Checkbox(Messages.Draw_throw_range_boxes(),
			&m_pWindowContainer->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay)->drawRangeCheckBoxes);
		ImGui::SameLine();
		ImGui::ShowHelpMarker(Messages.Throw_range_help());
		ImGui::VerticalSpacing();

		ImGui::HorizontalSpacing();
		ImGui::Checkbox(Messages.Freeze_frame(), &g_gameVals.isFrameFrozen);
		ImGui::SameLine();
		ImGui::ShowHelpMarker(Messages.Freeze_frame_tooltip());
		// No Ctrl check needed any more: HotkeyManager matches modifiers exactly, so a plain
		// "C" freeze binding no longer also fires on the Ctrl+C room-link shortcut.
		if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_FreezeFrame))
			g_gameVals.isFrameFrozen ^= 1;

		if (g_gameVals.pFrameCount)
		{
			ImGui::SameLine();
			ImGui::Text("%d", *g_gameVals.pFrameCount);
			ImGui::SameLine();
			if (ImGui::Button(Messages.Reset()))
			{
				*g_gameVals.pFrameCount = 0;
				g_gameVals.framesToReach = 0;
			}
			ImGui::SameLine();
			ImGui::ShowHelpMarker(Messages.Reset_frame_counter_tooltip());
		}

		if (g_gameVals.isFrameFrozen)
		{
			static int framesToStep = 1;
			ImGui::HorizontalSpacing();
			if (ImGui::Button(Messages.Step_frames()) ||
				HotkeyManager::WasPressedOrRepeated(HotkeyManager::Hotkey_StepFrames))
			{
				g_gameVals.framesToReach = *g_gameVals.pFrameCount + framesToStep;
			}
			ImGui::SameLine();
			ImGui::ShowHelpMarker(Messages.Step_frames_tooltip());

			ImGui::SameLine();
			ImGui::SliderInt("", &framesToStep, 1, 60);
			ImGui::SameLine();
			ImGui::ShowHelpMarker(Messages.Step_frames_count_tooltip());
		}
	}
}

void MainWindow::DrawGameplaySettingSection() const
{
	if (!ImGui::CollapsingHeader(Messages.Gameplay_settings_header()))
		return;

	if (!isInMatch() && !isOnVersusScreen() && !isOnReplayMenuScreen() && !isOnCharacterSelectionScreen())
	{
		ImGui::HorizontalSpacing();
		ImGui::TextDisabled(Messages.YOU_ARE_NOT_IN_MATCH());

		ImGui::HorizontalSpacing();
		ImGui::TextDisabled(Messages.YOU_ARE_NOT_IN_REPLAY_MENU());

		ImGui::HorizontalSpacing();
		ImGui::TextDisabled(Messages.YOU_ARE_NOT_IN_CHARACTER_SELECTION());

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
		ImGui::Checkbox(Messages.Hide_HUD_checkbox(), (bool*)g_gameVals.pIsHUDHidden);
		ImGui::SameLine();
		ImGui::ShowHelpMarker(Messages.Hide_HUD_tooltip());
	}
}

void MainWindow::DrawRankedMatchesSection() const
{
	DrawRankedMatchesMainMenuSection();
}

void MainWindow::DrawControllerSettingSection() const {
	if (!ImGui::CollapsingHeader(Messages.Controller_Settings()))
		return;

	ControllerSettings::DrawSection();
} // DrawControllerSettingSection

void MainWindow::DrawMusicSection() const
{
	if (!ImGui::CollapsingHeader(L("Music").c_str()))
		return;

	// Deliberately just the two doors. Every music control lives in the window it belongs
	// to - rotation and the playlist in the Jukebox, swaps in Music Replacement - so this
	// section stays one line and the main menu keeps its room.
	ImGui::HorizontalSpacing();
	if (ImGui::Button(L("Open Jukebox").c_str())) {
		GetMusicManager().StartCustomMusicDiscovery();
		m_pWindowContainer->GetWindow(WindowType_Jukebox)->ToggleOpen();
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("Choose which songs play during a match and how it moves "
		"between them.").c_str());

	ImGui::SameLine();
	if (ImGui::Button(L("Replace songs...").c_str())) {
		m_pWindowContainer->GetWindow(WindowType_BgmReplacement)->ToggleOpen();
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("Swap any song in the game for one of your own.").c_str());
}

void MainWindow::DrawLinkButtons() const
{
	//ImGui::ButtonUrl("Replay Database", REPLAY_DB_FRONTEND, BTN_SIZE);
	if (*g_gameVals.pGameMode == GameMode_ReplayTheater) {
		if (ImGui::Button(Messages.Toggle_Rewind()))
			m_pWindowContainer->GetWindow(WindowType_ReplayRewind)->ToggleOpen();
	}
	ImGui::ButtonUrl(Messages.Replay_Database(), REPLAY_DB_FRONTEND);
	ImGui::SameLine();
	if (ImGui::Button(Messages.Enable_Disable_Upload())) {
		m_pWindowContainer->GetWindow(WindowType_ReplayDBPopup)->ToggleOpen();
	}


	ImGui::ButtonUrl(Messages.Discord(), MOD_LINK_DISCORD, BTN_SIZE);

	ImGui::SameLine();
	ImGui::ButtonUrl(Messages.Forum(), MOD_LINK_FORUM, BTN_SIZE);

	ImGui::SameLine();
	ImGui::ButtonUrl(Messages.GitHub(), MOD_LINK_GITHUB, BTN_SIZE);

	ImGui::SameLine();
	if (ImGui::Button("Releases##checker", BTN_SIZE))
		m_pWindowContainer->GetWindow(WindowType_ReleaseChecker)->ToggleOpen();
	ImGui::SameLine();
	ImGui::ShowHelpMarker(Messages.Releases_checker_tooltip());

}
