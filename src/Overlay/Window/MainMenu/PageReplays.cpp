#include "MainMenuPages.h"

#include "Core/HotkeyManager.h"
#include "Core/info.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include "Overlay/Window/ReplayExtrasWindow.h"
#include "Overlay/Window/ScrWindow.h"

#include "imgui.h"

namespace MainMenu
{
	void DrawReplaysPage(const PageContext& ctx)
	{
		ScrWindow* scr = ctx.container->GetWindow<ScrWindow>(WindowType_Scr);
		const bool inTheater = g_gameVals.pGameMode && *g_gameVals.pGameMode == GameMode_ReplayTheater;
		// Reachable while the takeover itself is running, not only from the theater: taking
		// one over switches the game to training, and greying the section out at that point
		// hid the only two buttons that get you back out of it.
		const bool takeoverRunning = scr && scr->IsReplayTakeoverActive();

		// The same three control sets as the Replay Extras window, drawn from the same
		// bodies - see ReplayExtrasWindow.h. This is the page you come to when the window is
		// off, or when you want the longer explanations with it.
		bool showExtras = ReplayExtras::IsWindowVisible();
		if (ImGui::Checkbox(L("Show replay extras window").c_str(), &showExtras))
		{
			ReplayExtras::SetWindowVisible(showExtras);
		}
		ImGui::ShowHelpMarkerSameLine(L("A small window that appears over a replay with these same rewind, takeover and capture controls, so you do not have to open this menu while watching.").c_str());

		ImGui::VerticalSpacing(8);

		if (BeginSection(Replays_Rewind, inTheater))
		{
			Hint(L("Step a replay backwards instead of restarting it. The mod keeps checkpoints as the replay plays; rewinding jumps to the nearest one."));
			ReplayExtras::DrawRewindBody(*ctx.container, "menu", false);
		}

		if (BeginSection(Replays_Takeover, inTheater || takeoverRunning))
		{
			Hint(FormatText(L("Stop a replay where it is and play it out yourself, against everything the other side actually did. Hotkey: %s puts you back at the moment you took over.").c_str(),
				HotkeyManager::DisplayString(
					HotkeyManager::GetBinding(HotkeyManager::Hotkey_LoadReplayState)).c_str()));
			ReplayExtras::DrawTakeoverBody(*ctx.container, "menu", false);
		}

		if (BeginSection(Replays_Capture, inTheater))
		{
			Hint(L("Turn a stretch of a replay into a playback file, which you can then load into a recording slot, a playback library, or straight onto a dummy action."));
			ReplayExtras::DrawCaptureBody(*ctx.container, "menu", false);
		}

		if (BeginSection(Replays_Files))
		{
			Hint(L("Load a replay file the game's own theater cannot see, browse the archive, or pull one down from the replay database."));
			if (scr)
				scr->DrawLocalReplaysBody();
		}

		ImGui::VerticalSpacing(8);
		ImGui::Separator();
		ImGui::VerticalSpacing(4);

		Anchor(Replays_Database);
		ImGui::ButtonUrl(Messages.Replay_Database(), REPLAY_DB_FRONTEND);
		ImGui::SameLineOrWrap(ImGui::ButtonWidth(Messages.Enable_Disable_Upload()));
		if (ImGui::Button(Messages.Enable_Disable_Upload()))
		{
			ctx.container->GetWindow(WindowType_ReplayDBPopup)->ToggleOpen();
		}
		ImGui::ShowHelpMarkerSameLine(L("Choose whether your finished matches are uploaded to the community replay database.").c_str());
	}
}
