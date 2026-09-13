#include "ReplayExtrasWindow.h"

#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Game/SnapshotApparatus/SnapshotApparatus.h"
#include "Game/Playbacks/UnlimitedPlaybackManager.h"
#include "Game/gamestates.h"
#include "Overlay/Window/ScrWindow.h"
#include "Overlay/imgui_utils.h"

#include "imgui_internal.h"

#include <cstdio>

namespace
{
	bool g_windowVisible = true;

	bool InReplayMatch()
	{
		return g_gameVals.pGameMode && g_gameVals.pGameState &&
			*g_gameVals.pGameMode == GameMode_ReplayTheater &&
			*g_gameVals.pGameState == GameState_InMatch;
	}

	bool IsSearchingForRanked()
	{
		return *(GetBbcfBaseAdress() + 0x8F7758) != 0;
	}

	void* ReplayExtras_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
	{
		return strcmp(name, "Layout") == 0 ? (void*)1 : nullptr;
	}

	void ReplayExtras_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
	{
		int value = 0;
		if (sscanf_s(line, "Window=%d", &value) == 1) { g_windowVisible = value != 0; return; }
	}

	void ReplayExtras_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
	{
		buf->appendf("[%s][Layout]\n", handler->TypeName);
		buf->appendf("Window=%d\n\n", g_windowVisible ? 1 : 0);
	}
}

namespace ReplayExtras
{
	bool IsWindowVisible()
	{
		return g_windowVisible;
	}

	void SetWindowVisible(bool visible)
	{
		if (g_windowVisible == visible)
		{
			return;
		}
		g_windowVisible = visible;
		ImGui::MarkIniSettingsDirty();
	}

	void RegisterLayoutSettings()
	{
		// AddSettingsHandler asserts on a duplicate type, and init can run again after a
		// device loss.
		if (ImGui::FindSettingsHandler("BBCFIMReplayExtras"))
		{
			return;
		}

		ImGuiSettingsHandler handler;
		handler.TypeName = "BBCFIMReplayExtras";
		handler.TypeHash = ImHashStr("BBCFIMReplayExtras");
		handler.ReadOpenFn = ReplayExtras_ReadOpen;
		handler.ReadLineFn = ReplayExtras_ReadLine;
		handler.WriteAllFn = ReplayExtras_WriteAll;
		ImGui::AddSettingsHandler(&handler);
	}

	Mode CurrentMode(WindowContainer& container)
	{
		if (UnlimitedPlaybackManager::Instance().IsReplayRecording())
		{
			return Mode::Capture;
		}
		ScrWindow* scr = container.GetWindow<ScrWindow>(WindowType_Scr);
		if (scr && scr->IsReplayTakeoverActive())
		{
			return Mode::Takeover;
		}
		return Mode::Idle;
	}

	void DrawRewindBody(WindowContainer& container, const char* idScope, bool compact)
	{
		(void)container;
		(void)idScope;

		const bool searching = IsSearchingForRanked();
		const bool ready = !searching && InReplayMatch() && g_interfaces.pReplayRewindManager &&
			!g_interfaces.player1.IsCharDataNullPtr() && !g_interfaces.player2.IsCharDataNullPtr();

		const char* why = searching
			? Messages.Ranked_search_warning()
			: Messages.Only_works_during_a_running_replay();

		if (!compact && !ready)
		{
			ImGui::TextWrapped("%s", why);
			return;
		}

		ImGui::BeginDisabled(!ready);
		if (ImGui::Button(Messages.Rewind()) && ready)
		{
			g_interfaces.pReplayRewindManager->rewind_to_nearest();
		}
		ImGui::EndDisabled();

		if (compact)
		{
			ImGui::HoverTooltipEvenDisabled(ready ? Messages.Replay_rewind_help_tooltip() : why);
			ImGui::ShowHelpMarkerSameLine(Messages.Replay_rewind_help_tooltip());
		}
		else
		{
			ImGui::ShowHelpMarkerSameLine(Messages.Replay_rewind_help_tooltip());
			ImGui::SameLineOrWrap(ImGui::CalcTextSize(Messages.Rewind_Interval()).x);
			ImGui::TextUnformatted(Messages.Rewind_Interval());
			ImGui::ShowHelpMarkerSameLine(Messages.Rewind_interval_help_tooltip());
		}

		// How far back one press goes. Three radios rather than a number box: these are the
		// only three answers anyone wants, and they fit on the same row as the button.
		static int unusedStep = 60;
		int* step = ready ? &g_interfaces.pReplayRewindManager->FRAME_STEP : &unusedStep;

		ImGui::BeginDisabled(!ready);
		ImGui::SameLine();
		ImGui::RadioButton(Messages._1s(), step, 60);
		ImGui::SameLine();
		ImGui::RadioButton(Messages._3s(), step, 180);
		ImGui::SameLine();
		ImGui::RadioButton(Messages._9s(), step, 540);
		ImGui::EndDisabled();

		if (compact)
		{
			ImGui::ShowHelpMarkerSameLine(Messages.Rewind_interval_help_tooltip());
		}
	}

	bool PauseHudApplies()
	{
		// Exactly the condition the patch itself is gated on - the game has to be in Replay
		// Theater for the option to change anything. That rules out a takeover, which keeps
		// this window open but switches the game to training.
		return InReplayMatch();
	}

	void DrawPauseHudBody(WindowContainer& container, const char* idScope, bool compact)
	{
		(void)container;
		(void)idScope;

		// Read live rather than cached in a static: the same setting is on the Settings
		// window's Replays page, and a cached copy would leave the two showing different
		// answers until a restart.
		bool keep = Settings::settingsIni.showHudWhenReplayPaused;
		if (ImGui::CheckboxWrapped(Messages.Keep_input_display_on_replay_pause(), &keep))
		{
			Settings::settingsIni.showHudWhenReplayPaused = keep;
			Settings::changeSetting("ShowHudWhenReplayPaused", keep ? "1" : "0");
		}

		// L() rather than a generated Messages accessor: the accessor name is the string, and
		// for a sentence this long that is an unreadable 200-character identifier.
		const std::string& why = L("Pausing a replay normally makes the game hide its input display - the button columns down both sides and the two stick-and-button panels. Turn this on to leave them on screen. Pausing still pauses.");

		if (compact)
		{
			ImGui::HoverTooltip(why.c_str());
		}
		ImGui::ShowHelpMarkerSameLine(why.c_str());
	}

	void DrawTakeoverBody(WindowContainer& container, const char* idScope, bool compact)
	{
		ScrWindow* scr = container.GetWindow<ScrWindow>(WindowType_Scr);
		if (!scr)
		{
			return;
		}
		scr->DrawReplayTakeoverBody(idScope, compact);
	}

	void DrawCaptureBody(WindowContainer& container, const char* idScope, bool compact)
	{
		ScrWindow* scr = container.GetWindow<ScrWindow>(WindowType_Scr);
		if (!scr)
		{
			return;
		}
		scr->DrawReplayPlaybackCaptureBody(idScope, compact);
	}
}

// Kept for diagnostics: nothing calls these now that the window is three fixed rows with no
// room for a debug tree, but they are the only written-down way to walk the entity list and
// to find the rewind checkpoint nearest the current frame.
unsigned int ReplayExtrasWindow::count_entities(bool unk_status2) {
    if (!g_interfaces.player1.IsCharDataNullPtr() && !g_interfaces.player2.IsCharDataNullPtr()) {
        std::vector<int*> entities{};
        std::vector<CharData**> entities_char_data{};
        for (int i = 0; i < 252; i++) {
            entities.push_back((g_gameVals.pEntityList + i));
        }
        for (auto entity_ptr : entities) {
            entities_char_data.push_back((CharData**)entity_ptr);
        }
        auto r = 0;
        for (auto entity : entities_char_data) {
            if ((*entity)->unknownStatus1 != NULL) {
                if (unk_status2) {
                    if ((*entity)->unknown_status2 != NULL && (*entity)->unknown_status2 == 2) {
                        r++;
                    }
                }
                else {
                    r++;
                }
            }
        }
        return r;
    }
    return 0;
}

std::vector<int> ReplayExtrasWindow::find_nearest_checkpoint(std::vector<unsigned int> frameCount) {
    //returns a vector with the fist being the nearest pos in the checkpoints for a backwards and the second for the fwd, -1 if not available
    auto fc = *g_gameVals.pFrameCount;
    int nearest_back = 9999999;
    int nearest_back_pos = -1;
    int nearest_fwd = 9999999;
    int nearest_fwd_pos = -1;
    int i = 0;
    if (frameCount.size() == 0) {
        static_DAT_of_PTR_on_load_4* DAT_on_load_4_addr = (static_DAT_of_PTR_on_load_4*)(GetBbcfBaseAdress() + 0x612718);
        SnapshotManager* snap_manager = 0;
        snap_manager = DAT_on_load_4_addr->ptr_snapshot_manager_mine;
        auto stru = snap_manager->_saved_states_related_struct;
        frameCount = std::vector<unsigned int>{};
        for (int i = 0; (i < 10) && (stru[i]._framecount != 0); i++) {
            frameCount.push_back(stru[i]._framecount);
        }
    }
    for (auto frameCheckpoint : frameCount) {
        if ((fc - frameCheckpoint < fc - nearest_back && fc - frameCheckpoint >60)) {
            nearest_back = frameCheckpoint;
            nearest_back_pos = i;
        }
        if ((frameCheckpoint - fc < nearest_fwd - fc && frameCheckpoint - fc >60)) {
            nearest_fwd = frameCheckpoint;
            nearest_fwd_pos = i;
        }
        i++;
    }
    if (frameCount.empty()) {
        return std::vector<int>{-1, -1};
    }
    if (frameCount.size() < nearest_back_pos + 1) {
        nearest_fwd_pos = -1;
    }
    return std::vector<int>{nearest_back_pos, nearest_fwd_pos};
}

void ReplayExtrasWindow::Update()
{
	ScrWindow* scr = m_pWindowContainer ? m_pWindowContainer->GetWindow<ScrWindow>(WindowType_Scr) : nullptr;
	const bool takeoverRunning = scr && scr->IsReplayTakeoverActive();

	// While a replay is on screen, and while a takeover taken from one is still running -
	// that switches the game to training, and the window holds the only way back out.
	const bool applicable = g_gameVals.pGameState && *g_gameVals.pGameState == GameState_InMatch &&
		(InReplayMatch() || takeoverRunning);

	if (!applicable || !ReplayExtras::IsWindowVisible())
	{
		// Not the user closing it: either there is nothing to control, or it was switched
		// off from the mod menu. Neither should ask a question.
		if (IsOpen())
		{
			Close();
		}
		m_askClose = false;
		return;
	}

	if (!IsOpen())
	{
		Open();
	}

	IWindow::Update();

	// IWindow::Update hands m_windowOpen to ImGui's close button, so a close that happened
	// inside it is the user pressing the X. Put it back and ask.
	if (!IsOpen())
	{
		Open();
		m_askClose = true;
	}
}

void ReplayExtrasWindow::BeforeDraw()
{
	// Rebuilt per frame rather than at registration, so switching language renames it
	// straight away. Everything after ### is the identity, so ImGui still sees one window.
	m_windowTitle = L("Replay Extras") + "###ReplayExtras";

	// Fixed, not resizable. Every mode is laid out as exactly three rows, so there is one
	// right size for this window and letting the user drag it to a wrong one only produces
	// wrapped rows and empty space. Measured from the font rather than hard-coded in pixels,
	// because the menu size setting scales it.
	// Four rows while the replay-pause option is on offer, three otherwise. The height still
	// does not change under the mouse: the row appears and disappears with the game mode, not
	// with anything the window itself does.
	const float rowCount = ReplayExtras::PauseHudApplies() ? 4.0f : 3.0f;

	const ImGuiStyle& style = ImGui::GetStyle();
	const float rows = ImGui::GetFrameHeight() * rowCount + style.ItemSpacing.y * (rowCount - 1.0f);
	const float titleBar = ImGui::GetFrameHeight();
	ImGui::SetNextWindowSize(
		ImVec2(ImGui::GetFontSize() * 30.0f, titleBar + style.WindowPadding.y * 2.0f + rows),
		ImGuiCond_Always);
}

void ReplayExtrasWindow::Draw()
{
	if (!m_pWindowContainer)
	{
		return;
	}

	// Three rows for the controls, whatever mode this is in, so they never move out from
	// under the mouse. No headings and no collapsing: a heading over a single row spends that
	// row saying what the button next to it already says.
	switch (ReplayExtras::CurrentMode(*m_pWindowContainer))
	{
	case ReplayExtras::Mode::Takeover:
		// Three rows of its own: what mode you are in, what you can do, and the hotkey.
		// Rewinding and capturing both need the replay, which you are no longer watching.
		ReplayExtras::DrawTakeoverBody(*m_pWindowContainer, "extras", true);
		break;

	case ReplayExtras::Mode::Capture:
		// Two rows, plus rewind. Taking over is not offered: it would end the replay the
		// capture is reading from.
		ReplayExtras::DrawCaptureBody(*m_pWindowContainer, "extras", true);
		ReplayExtras::DrawRewindBody(*m_pWindowContainer, "extras", true);
		break;

	default:
		ReplayExtras::DrawRewindBody(*m_pWindowContainer, "extras", true);
		ReplayExtras::DrawTakeoverBody(*m_pWindowContainer, "extras", true);
		ReplayExtras::DrawCaptureBody(*m_pWindowContainer, "extras", true);
		break;
	}

	// Last, in every mode that has it, so the three controls people reach for keep the
	// positions they have always had. This one is set once and then left alone.
	if (ReplayExtras::PauseHudApplies())
	{
		ReplayExtras::DrawPauseHudBody(*m_pWindowContainer, "extras", true);
	}

	DrawCloseConfirm();
}

void ReplayExtrasWindow::DrawCloseConfirm()
{
	const std::string title = L("Close Replay Extras?") + std::string("##replay_extras_close");

	if (m_askClose)
	{
		ImGui::OpenPopup(title.c_str());
		m_askClose = false;
	}

	// Explicit size, then centre. Centring is a position plus a (0.5, 0.5) pivot, and ImGui
	// can only apply a pivot once it knows how big the window is - an AlwaysAutoResize popup
	// has measured nothing on the frame it appears, so the pivot lands its top-left corner
	// at the centre of the screen and ImGuiCond_Appearing never corrects it afterwards. That
	// is the "modal spawns at the top middle" bug. See docs/ImGuiModalCentering.md.
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowSize(ImVec2(430.0f, 165.0f), ImGuiCond_Appearing);
	if (display.x > 0.0f && display.y > 0.0f)
	{
		ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
			ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	}

	if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize))
	{
		return;
	}

	ImGui::TextWrapped("%s", L("Close the Replay Extras window? Rewind, takeover and capture all live in here.").c_str());
	ImGui::VerticalSpacing(2);
	ImGui::TextDisabledWrapped("%s", L("You can bring it back from the mod menu's Replays page, where the same controls also live.").c_str());
	ImGui::VerticalSpacing(6);

	if (ImGui::Button(L("Close it").c_str(), ImVec2(120, 0)))
	{
		// Closes the popup before the window it belongs to goes away.
		ImGui::CloseCurrentPopup();
		ReplayExtras::SetWindowVisible(false);
	}
	ImGui::SameLine();
	if (ImGui::Button(L("Keep it open").c_str(), ImVec2(120, 0)))
	{
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}
