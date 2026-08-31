#include "MainMenuPages.h"

#include "Core/HotkeyManager.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include "Overlay/Window/FrameAdvantage/FrameAdvantage.h"
#include "Overlay/Window/FrameHistory/FrameHistoryWindow.h"
#include "Overlay/Window/HitboxOverlay.h"
#include "Overlay/Window/ScrWindow.h"

#include "imgui.h"

namespace MainMenu
{
	namespace
	{
		void DrawHitboxes(const PageContext& ctx)
		{
			if (!isHitboxOverlayEnabledInCurrentState())
			{
				Unavailable(Messages.YOU_ARE_NOT_IN_TRAINING_VERSUS_OR_REPLAY());
				return;
			}

			HitboxOverlay* overlay = ctx.container->GetWindow<HitboxOverlay>(WindowType_HitboxOverlay);
			static bool isOpen = false;

			ImGui::HorizontalSpacing();
			const bool toggled = ImGui::Checkbox(Messages.Enable_hitbox_overlay_section(), &isOpen);
			ImGui::ShowHelpMarkerSameLine(Messages.Enable_hitbox_overlay_tooltip());
			if (toggled)
			{
				if (isOpen)
				{
					overlay->Open();
				}
				else
				{
					g_gameVals.isFrameFrozen = false;
					overlay->Close();
				}
			}

			if (!isOpen)
				return;

			ImGui::VerticalSpacing(10);

			if (!g_interfaces.player1.IsCharDataNullPtr() && !g_interfaces.player2.IsCharDataNullPtr())
			{
				ImGui::HorizontalSpacing();
				ImGui::Checkbox(Messages.Player1(), &overlay->drawCharacterHitbox[0]);
				ImGui::HoverTooltip(getCharacterNameByIndexA(g_interfaces.player1.GetData()->charIndex).c_str());
				ImGui::SameLine(); ImGui::HorizontalSpacing();
				ImGui::TextUnformatted(g_interfaces.player1.GetData()->currentAction);

				ImGui::HorizontalSpacing();
				ImGui::Checkbox(Messages.Player2(), &overlay->drawCharacterHitbox[1]);
				ImGui::HoverTooltip(getCharacterNameByIndexA(g_interfaces.player2.GetData()->charIndex).c_str());
				ImGui::SameLine(); ImGui::HorizontalSpacing();
				ImGui::TextUnformatted(g_interfaces.player2.GetData()->currentAction);
			}

			ImGui::VerticalSpacing(10);

			ImGui::HorizontalSpacing();
			overlay->DrawRectThicknessSlider();
			ImGui::HorizontalSpacing();
			overlay->DrawRectFillTransparencySlider();

			ImGui::HorizontalSpacing();
			ImGui::Checkbox(Messages.Draw_hitbox_hurtbox(), &overlay->drawHitboxHurtbox);
			ImGui::ShowHelpMarkerSameLine(Messages.Draw_hitbox_hurtbox_tooltip());

			ImGui::HorizontalSpacing();
			ImGui::Checkbox(Messages.Draw_origin(), &overlay->drawOriginLine);
			ImGui::ShowHelpMarkerSameLine(Messages.Origin_point_note());

			ImGui::HorizontalSpacing();
			ImGui::Checkbox(Messages.Draw_collision(), &overlay->drawCollisionBoxes);
			ImGui::ShowHelpMarkerSameLine(Messages.Collision_box_note());

			ImGui::HorizontalSpacing();
			ImGui::Checkbox(Messages.Draw_throw_range_boxes(), &overlay->drawRangeCheckBoxes);
			ImGui::ShowHelpMarkerSameLine(Messages.Throw_range_help());
		}

		// Freezing and stepping used to be buried at the bottom of the hitbox section, which
		// is why so few people knew the mod could pause a match at all. It is its own thing:
		// it works anywhere the hitbox overlay does, with or without boxes drawn.
		void DrawFreezeAndStep()
		{
			if (!isHitboxOverlayEnabledInCurrentState())
			{
				Unavailable(Messages.YOU_ARE_NOT_IN_TRAINING_VERSUS_OR_REPLAY());
				return;
			}

			ImGui::HorizontalSpacing();
			ImGui::Checkbox(Messages.Freeze_frame(), &g_gameVals.isFrameFrozen);
			ImGui::ShowHelpMarkerSameLine(Messages.Freeze_frame_tooltip());

			// No Ctrl check needed any more: HotkeyManager matches modifiers exactly, so a
			// plain "C" freeze binding no longer also fires on the Ctrl+C room-link shortcut.
			if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_FreezeFrame))
				g_gameVals.isFrameFrozen ^= 1;

			if (g_gameVals.pFrameCount)
			{
				ImGui::SameLineOrWrap(ImGui::CalcTextSize("000000").x);
				ImGui::Text("%d", *g_gameVals.pFrameCount);
				ImGui::SameLineOrWrap(ImGui::ButtonWidth(Messages.Reset()));
				if (ImGui::Button(Messages.Reset()))
				{
					*g_gameVals.pFrameCount = 0;
					g_gameVals.framesToReach = 0;
				}
				ImGui::ShowHelpMarkerSameLine(Messages.Reset_frame_counter_tooltip());
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
				ImGui::ShowHelpMarkerSameLine(Messages.Step_frames_tooltip());

				ImGui::SameLineOrWrap(160.0f);
				ImGui::SetNextItemWidth(160.0f);
				ImGui::SliderInt("##framestostep", &framesToStep, 1, 60);
				ImGui::ShowHelpMarkerSameLine(Messages.Step_frames_count_tooltip());
			}

			Hint(FormatText(L("Hotkeys: %s pauses, %s steps forward.").c_str(),
				HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_FreezeFrame)).c_str(),
				HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_StepFrames)).c_str()));
		}

		void DrawFrameAdvantage(const PageContext& ctx)
		{
			if (!isInMatch())
			{
				Unavailable(Messages.YOU_ARE_NOT_IN_MATCH());
				return;
			}
			if (!(*g_gameVals.pGameMode == GameMode_Training || *g_gameVals.pGameMode == GameMode_ReplayTheater))
			{
				Unavailable(Messages.YOU_ARE_NOT_IN_TRAINING_MODE_OR_REPLAY_THEATER());
				return;
			}
			if (!g_gameVals.pEntityList)
				return;

			static bool isFrameAdvantageOpen = false;
			ImGui::HorizontalSpacing();
			ImGui::Checkbox(Messages.Enable_framedata_section(), &isFrameAdvantageOpen);
			ImGui::ShowHelpMarkerSameLine(Messages.Enable_framedata_tooltip());

			ImGui::HorizontalSpacing();
			ImGui::Checkbox(Messages.Advantage_on_stagger_hit(), &idleActionToggles.ukemiStaggerHit);
			ImGui::ShowHelpMarkerSameLine(Messages.Advantage_stagger_hit_tooltip());

			if (isFrameAdvantageOpen)
				ctx.container->GetWindow(WindowType_FrameAdvantage)->Open();
			else
				ctx.container->GetWindow(WindowType_FrameAdvantage)->Close();
		}

		void DrawFrameHistory(const PageContext& ctx)
		{
			if (!isFrameHistoryEnabledInCurrentState())
			{
				Unavailable(Messages.YOU_ARE_NOT_IN_A_MATCH_IN_TRAINING_MODE_OR_REPLAY_THEATER());
				return;
			}
			if (g_interfaces.player1.IsCharDataNullPtr() || g_interfaces.player2.IsCharDataNullPtr())
			{
				Unavailable(Messages.THERE_WAS_AN_ERROR_LOADING_ONE_BOTH_OF_THE_CHARACTERS());
				return;
			}

			FrameHistoryWindow* frameHistWin = ctx.container->GetWindow<FrameHistoryWindow>(WindowType_FrameHistory);

			ImGui::HorizontalSpacing();
			bool isOpen = Settings::settingsIni.frameHistoryEnabled;
			if (ImGui::Checkbox(Messages.Enable_framehistory_section(), &isOpen))
			{
				Settings::settingsIni.frameHistoryEnabled = isOpen;
				Settings::changeSetting("FrameHistoryEnabled", isOpen ? "1" : "0");
			}
			ImGui::ShowHelpMarkerSameLine(Messages.FrameHistory_help());
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
			ImGui::ShowHelpMarkerSameLine(Messages.FrameHistory_auto_reset_help());

			ImGui::HorizontalSpacing();
			if (ImGui::Checkbox(Messages.Count_empty_frames_framehistory(), &frameHistWin->countEmptyFrames))
			{
				Settings::settingsIni.frameHistoryCountEmptyFrames = frameHistWin->countEmptyFrames;
				Settings::changeSetting("FrameHistoryCountEmptyFrames", frameHistWin->countEmptyFrames ? "1" : "0");
			}
			ImGui::ShowHelpMarkerSameLine(Messages.FrameHistory_count_empty_frames_help());

			ImGui::VerticalSpacing(6);
			ImGui::HorizontalSpacing();
			if (ImGui::TreeNode(L("Size and spacing").c_str()))
			{
				if (ImGui::SliderFloat(Messages.Box_width(), &frameHistWin->width, 1., 100.))
					Settings::changeSetting("FrameHistoryWidth", std::to_string(frameHistWin->width));
				if (ImGui::SliderFloat(Messages.Box_height(), &frameHistWin->height, 1., 100.))
					Settings::changeSetting("FrameHistoryHeight", std::to_string(frameHistWin->height));
				if (ImGui::SliderFloat(Messages.spacing(), &frameHistWin->spacing, 1., 100.))
					Settings::changeSetting("FrameHistorySpacing", std::to_string(frameHistWin->spacing));
				ImGui::TreePop();
			}
		}
	}

	void DrawOverlaysPage(const PageContext& ctx)
	{
		ScrWindow* scr = ctx.container->GetWindow<ScrWindow>(WindowType_Scr);

		const bool overlaysUsable = isHitboxOverlayEnabledInCurrentState();
		const bool frameToolsUsable = isInMatch() && g_gameVals.pGameMode
			&& (*g_gameVals.pGameMode == GameMode_Training || *g_gameVals.pGameMode == GameMode_ReplayTheater);

		if (BeginSection(Overlays_Hitboxes, overlaysUsable))
			DrawHitboxes(ctx);

		GroupLabel(Overlays_FrameStep, overlaysUsable);
		DrawFreezeAndStep();

		ImGui::VerticalSpacing(4);
		GroupLabel(Overlays_FrameAdvantage, frameToolsUsable);
		DrawFrameAdvantage(ctx);

		ImGui::VerticalSpacing(4);
		GroupLabel(Overlays_FrameHistory, isFrameHistoryEnabledInCurrentState());
		DrawFrameHistory(ctx);

		ImGui::VerticalSpacing(8);
		ImGui::Separator();
		ImGui::VerticalSpacing(4);

		// Three buttons that each open a window. They do not need a heading between them.
		if (scr)
		{
			Anchor(Overlays_InputDisplay);
			scr->DrawInputBufferButton();
			ImGui::SameLineOrWrap(ImGui::ButtonWidth("Combo Data"));
			Anchor(Overlays_ComboData);
			scr->DrawComboDataButton();
		}
	}
}
