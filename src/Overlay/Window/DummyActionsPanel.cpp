#include "DummyActionsPanel.h"

#include "Core/HotkeyManager.h"
#include "Core/logger.h"
#include "Core/Localization.h"
#include "Core/NativeFileDialog.h"
#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/utils.h"
#include "Game/Playbacks/DummyActionManager.h"
#include "Game/Playbacks/PlaybackManager.h"
#include "Game/Playbacks/UnlimitedPlaybackManager.h"
#include "Game/Scr/ScrStateNames.h"
#include "Game/TasManager.h"
#include "Game/TrainingSide.h"
#include "Game/gamestates.h"
#include "Overlay/Window/ScrWindow.h"
#include "Overlay/Window/UnlimitedPlaybackWindow.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <Windows.h>

#include <cmath>
#include <map>
#include <set>

#include <algorithm>
#include <string>
#include <vector>

using Manager = DummyActionManager;
using Source = DummyActionManager::Source;
using TriggerType = DummyActionManager::TriggerType;

namespace
{
	const char* kFileDialogToken = "DummyActionsPanel";

	// A playback frame's direction nibble: 5 is neutral, which is also what
	// PlaybackSlot::load_raw_into_slot fills unused frames with.
	const int kNeutralInput = 5;
	// Neutral frames appended after a notation, so a real input is never the final frame.
	const int kNotationTailFrames = 4;

	// Which trigger's source modal is open, for which source, and which of the trigger's
	// actions it is editing (-1 while adding a new one). Trigger_Count means none.
	//
	// Configuration happens on a DRAFT copy of the action, not on the live one. Editing in
	// place meant Cancel had to guess what to undo: a brand new row appeared the moment you
	// picked a source and vanished again if you backed out, and changing the source of an
	// existing row destroyed it if you then cancelled. The draft is committed on Done and
	// thrown away on Cancel, so the list only ever changes when you say so.
	int g_modalTrigger = UnlimitedPlaybackManager::Trigger_Count;
	int g_modalActionIndex = -1;
	Source g_modalSource = Manager::Source_None;
	bool g_modalNeedsOpen = false;
	Manager::Action g_draft;

	// The trigger settings modal edits its TriggerConfig live, because the firing code
	// reads it every frame - so a copy is taken to put back on Cancel.
	UnlimitedPlaybackManager::TriggerConfig g_settingsBackup;
	bool g_settingsBackupValid = false;
	float g_settingsLoopSetup = 0.0f;
	float g_settingsLoopEnding = 0.0f;
	bool g_settingsLoopRestartLab = false;
	int g_settingsLoopRestartMode = 0;
	int g_settingsSelectionMode = 0;

	// Per-modal scratch, only meaningful while that modal is up.
	char g_notationText[256] = "";
	std::string g_notationError;
	int g_notationFrameCount = 0;
	int g_animSelected = -1;
	int g_animCategory = 3;   // 0 normals, 1 specials, 2 common, 3 all
	int g_animDelay = 0;
	char g_animFilter[64] = "";
	int g_pendingCfSlot = 1;
	bool g_pendingCfSlotKeepCopy = false;
	std::string g_cfSlotError;

	// The trigger-settings modal is separate from the source modals: it configures WHEN the
	// trigger fires, not WHAT it plays, so it is reachable whatever the source is.
	int g_settingsTrigger = UnlimitedPlaybackManager::Trigger_Count;
	bool g_settingsNeedsOpen = false;

	// Trigger and action the pending file pick belongs to, since the picker answers on a
	// later frame. Index -1 adds a new action.
	int g_filePickTrigger = UnlimitedPlaybackManager::Trigger_Count;
	int g_filePickIndex = -1;

	// A trigger whose delete needs confirming because it holds several actions.
	int g_confirmClearTrigger = UnlimitedPlaybackManager::Trigger_Count;

	void BeginFilePick(TriggerType trigger, int actionIndex);

	// Centres a modal as it appears and gives it a starting size, leaving it free to be
	// moved and resized afterwards.
	//
	// The size is the point: centring against a pivot needs a known size, and a window with
	// AlwaysAutoResize has none on the frame it appears - which is why an earlier attempt at
	// this landed everything in the top-left. Asking for a size up front makes
	// ImGuiCond_Appearing work, and dropping AlwaysAutoResize is what makes the window
	// resizable at all.
	// How much room the footer needs below the scrolling body: the separator and its
	// spacing, then the button row. Shared with the fit calculation below, because the two
	// disagreeing is what left a permanent scrollbar - the body reserved less than the
	// footer actually used, so the window was always a few pixels over its own height.
	float ModalFooterReserve()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		return (style.ItemSpacing.y * 2.0f) + 2.0f + ImGui::GetFrameHeight();
	}

	// Height of the last body drawn, and whether a modal still wants to be sized to it.
	// Measured rather than guessed: a hand-picked default is either too short for a library
	// list or leaves a slab of dead space above the buttons for a one-field form.
	float g_modalBodyContentHeight = 0.0f;
	bool g_modalFitPending = false;

	// Every modal's size, as the user left it, keyed by its window title. ImGui already keeps
	// each of these windows separate (they have distinct titles), but it only persists them
	// through an ini file, which the mod does not write - so they are kept here for the
	// session and logged, so a size someone settles on can be read back and made the default.
	std::map<std::string, ImVec2> g_modalSizes;
	std::map<std::string, ImVec2> g_modalSizesLogged;
	bool g_modalApplyRememberedSize = false;

	// A clickable label whose hover/press highlight is a faint white wash rather than the
	// theme's blue Header colour, which reads as a selected list row rather than a button.
	//
	// Takes an optional leading glyph drawn in its own colour: the whole thing is one
	// Selectable, with the text painted over it afterwards, so the highlight covers the
	// glyph too.
	bool DrawFaintSelectable(const char* id, const char* glyph, const char* label,
		const ImVec4& glyphColor)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float glyphWidth = glyph ? ImGui::CalcTextSize(glyph).x + 4.0f : 0.0f;
		const float width = glyphWidth + ImGui::CalcTextSize(label).x
			+ (style.FramePadding.x * 2.0f);

		const ImVec2 start = ImGui::GetCursorPos();

		ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.16f));
		const bool pressed = ImGui::Selectable(id, false, 0, ImVec2(width, 0.0f));
		ImGui::PopStyleColor(3);

		// Painted over the Selectable that was just drawn.
		const ImVec2 after = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(start.x + style.FramePadding.x, start.y));
		if (glyph)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, glyphColor);
			ImGui::TextUnformatted(glyph);
			ImGui::PopStyleColor();
			ImGui::SameLine(0.0f, 4.0f);
			ImGui::TextUnformatted(label);
		}
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Text, glyphColor);
			ImGui::TextUnformatted(label);
			ImGui::PopStyleColor();
		}
		ImGui::SetCursorPos(after);
		return pressed;
	}

	// A plain centred confirmation, which does not need the fit-and-remember machinery the
	// configuration modals use.
	void CenterModalOnAppearing()
	{
		const ImVec2 display = ImGui::GetIO().DisplaySize;
		if (display.x <= 0.0f || display.y <= 0.0f)
		{
			return;
		}
		ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
			ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	}

	void RememberModalSize(const char* title)
	{
		const ImVec2 size = ImGui::GetWindowSize();
		if (size.x <= 0.0f || size.y <= 0.0f)
		{
			return;
		}
		g_modalSizes[title] = size;
		// Written through to the saved file, so a size settled on now is the size next
		// session opens at. SetUiSize ignores a value it already holds, so this is not a
		// write per frame.
		Manager::Instance().SetUiSize(title, size.x, size.y);

		// Logged when it changes, so the last line for a modal is the size it was left at.
		const auto logged = g_modalSizesLogged.find(title);
		if (logged == g_modalSizesLogged.end()
			|| fabsf(logged->second.x - size.x) > 1.0f
			|| fabsf(logged->second.y - size.y) > 1.0f)
		{
			g_modalSizesLogged[title] = size;
			LOG(1, "[DummyActions] modal '%s' size %.0fx%.0f\n", title, size.x, size.y);
		}
	}

	void RequestModalFit()
	{
		g_modalFitPending = true;
		// Cleared so a fit can never be applied from the previous modal's measurement.
		g_modalBodyContentHeight = 0.0f;
		// A size the user has already settled on beats fitting to content.
		g_modalApplyRememberedSize = true;
	}

	void BeginCenteredModalLayout(const char* title, float width, float height)
	{
		const ImVec2 display = ImGui::GetIO().DisplaySize;

		// Reopened at whatever size it was last left at, centred once.
		ImVec2 rememberedSize(0.0f, 0.0f);
		bool haveRemembered = false;
		{
			const auto it = g_modalSizes.find(title);
			if (it != g_modalSizes.end())
			{
				rememberedSize = it->second;
				haveRemembered = true;
			}
			else
			{
				float savedW = 0.0f;
				float savedH = 0.0f;
				if (Manager::Instance().GetUiSize(title, &savedW, &savedH))
				{
					rememberedSize = ImVec2(savedW, savedH);
					haveRemembered = true;
				}
			}
		}
		if (g_modalApplyRememberedSize && haveRemembered)
		{
			g_modalApplyRememberedSize = false;
			g_modalFitPending = false;   // a chosen size is not to be second-guessed
			ImGui::SetNextWindowSize(rememberedSize, ImGuiCond_Always);
			if (display.x > 0.0f && display.y > 0.0f)
			{
				ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
					ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			}
			return;
		}
		g_modalApplyRememberedSize = false;

		// Once the body has been measured, snap to exactly what it needs. This happens on the
		// frame after the modal appears, so the provisional size below is never really seen.
		bool fittedThisFrame = false;
		if (g_modalFitPending && g_modalBodyContentHeight > 0.0f)
		{
			g_modalFitPending = false;
			fittedThisFrame = true;
			const ImGuiStyle& style = ImGui::GetStyle();
			float fitted = g_modalBodyContentHeight
				+ ModalFooterReserve()
				+ (style.WindowPadding.y * 2.0f)
				+ ImGui::GetFrameHeight();   // title bar
			const float ceiling = display.y > 0.0f ? display.y * 0.85f : height;
			fitted = (std::min)((std::max)(fitted, 120.0f), ceiling);
			ImGui::SetNextWindowSize(ImVec2(width, fitted), ImGuiCond_Always);
		}
		else
		{
			ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
		}

		if (display.x <= 0.0f || display.y <= 0.0f)
		{
			return;
		}
		// Re-centred on the frame the size changes. Appearing-only centring places the window
		// for the provisional size and then never again, so a modal that grew or shrank to
		// fit its content was left hanging off-centre from where it first appeared.
		ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
			fittedThisFrame ? ImGuiCond_Always : ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	}

	// Starting size per source. The animation picker needs room for a move list beside a
	// framedata panel, and the library needs room for a list plus its add buttons.
	ImVec2 ModalStartSizeFor(Source source)
	{
		switch (source)
		{
		// Measured in game rather than guessed - these are the sizes these modals were
		// actually resized to, read back out of dummy_actions.ini.
		case Manager::Source_Animation: return ImVec2(549.0f, 498.0f);
		case Manager::Source_Notation:  return ImVec2(423.0f, 160.0f);
		case Manager::Source_CfSlot:
		case Manager::Source_Recording: return ImVec2(420.0f, 210.0f);
		default: break;
		}
		return ImVec2(400.0f, 200.0f);
	}

	// Splits a modal into a scrolling body and a footer that cannot scroll away.
	//
	// Without this the Done and Cancel buttons are simply the last thing in the window, so
	// they leave the view the moment the content is taller than the modal - which for a
	// library list is most of the time.
	void BeginModalBody()
	{
		ImGui::BeginChild("##modal_body", ImVec2(0.0f, -ModalFooterReserve()), 0);
	}

	void EndModalBodyAndSeparate()
	{
		// Measured while still inside the child, where the cursor is relative to its content.
		g_modalBodyContentHeight = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
		ImGui::EndChild();
		ImGui::Separator();
	}

	// The gray caption Unlimited Playback's own context menus use, so these read the same.
	void ContextMenuHeader(const char* text)
	{
		ImGui::Separator();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.58f, 0.58f, 1.0f));
		ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 0.86f);
		ImGui::TextUnformatted(text);
		ImGui::PopFont();
		ImGui::PopStyleColor();
	}

	void OpenModal(TriggerType trigger, Source source, int actionIndex)
	{
		g_modalTrigger = trigger;
		g_modalActionIndex = actionIndex;
		g_modalSource = source;
		g_modalNeedsOpen = true;
		RequestModalFit();

		// The draft starts from the live action when one is being edited, so reopening a
		// configured row shows what is set; adding starts clean.
		const Manager::Trigger& slot = Manager::Instance().Get(trigger);
		if (actionIndex >= 0 && static_cast<size_t>(actionIndex) < slot.actions.size()
			&& slot.actions[actionIndex].source == source)
		{
			g_draft = slot.actions[actionIndex];
		}
		else
		{
			g_draft = Manager::Action{};
			g_draft.source = source;
		}

		// Seed the scratch from the draft, so the form shows what is set rather than blank.
		const Manager::Action& action = g_draft;
		switch (source)
		{
		case Manager::Source_Notation:
			strncpy_s(g_notationText, action.notation.c_str(), sizeof(g_notationText) - 1);
			g_notationError.clear();
			g_notationFrameCount = static_cast<int>(action.notationFrames.size());
			break;
		case Manager::Source_Animation:
			g_animSelected = -1;
			g_animDelay = action.animationDelays.empty() ? 0 : action.animationDelays.back();
			g_animFilter[0] = '\0';
			break;
		case Manager::Source_CfSlot:
		case Manager::Source_Recording:
			g_pendingCfSlot = action.cfSlot;
			g_pendingCfSlotKeepCopy = source == Manager::Source_Recording;
			g_cfSlotError.clear();
			break;
		default:
			break;
		}
	}

	void ClearModalState()
	{
		g_modalTrigger = UnlimitedPlaybackManager::Trigger_Count;
		g_modalActionIndex = -1;
		g_modalSource = Manager::Source_None;
		g_draft = Manager::Action{};
	}

	// Commits the draft onto the live table: appended as a new action, or written over the
	// one being edited. The only path by which the trigger list changes from a modal.
	void CommitModal()
	{
		if (g_modalTrigger < UnlimitedPlaybackManager::Trigger_Count)
		{
			const TriggerType trigger = static_cast<TriggerType>(g_modalTrigger);
			if (g_modalActionIndex >= 0)
			{
				Manager::Instance().Replace(trigger, static_cast<size_t>(g_modalActionIndex), g_draft);
			}
			else
			{
				Manager::Instance().Add(trigger, g_draft);
			}
		}
		ClearModalState();
		ImGui::CloseCurrentPopup();
	}

	// Throws the draft away. The live table is left exactly as it was.
	void CancelModal()
	{
		ClearModalState();
		ImGui::CloseCurrentPopup();
	}

	const char* ModalTitleFor(Source source)
	{
		switch (source)
		{
		case Manager::Source_Notation:  return "Input Notation##dummyaction";
		case Manager::Source_File:      return "From File##dummyaction";
		case Manager::Source_Animation: return "Animation##dummyaction";
		case Manager::Source_CfSlot:
		case Manager::Source_Recording: return "CF Recording Slot##dummyaction";
		default: break;
		}
		return "##dummyaction_none";
	}

	void DrawStateDetails(scrState* selected_state)
	{
	    if (!selected_state) {
	        ImGui::TextDisabled("%s", L("Pick a move on the left.").c_str());
	        return;
	    }

	    ImGui::TextUnformatted(ScrStateNames::Display(selected_state->name).c_str());
	    if (ImGui::IsItemHovered()) {
	        ImGui::SetTooltip("%s\nAddr 0x%x", selected_state->name.c_str(), selected_state->addr);
	    }
	    ImGui::Separator();

	    // The numbers people actually look for, on two lines instead of thirteen.
	    ImGui::Text("%s", FormatText(L("%d frames    %d damage    level %d").c_str(),
	        selected_state->frames, selected_state->damage, selected_state->atk_level).c_str());
	    ImGui::Text("%s", FormatText(L("blockstun %d    hitstun %d    hitstop %d").c_str(),
	        selected_state->blockstun, selected_state->hitstun, selected_state->hitstop).c_str());

	    std::string flags;
	    if (selected_state->hit_overhead) { flags += L("overhead") + "  "; }
	    if (selected_state->hit_low) { flags += L("low") + "  "; }
	    if (selected_state->hit_air_unblockable) { flags += L("air unblockable") + "  "; }
	    if (selected_state->fatal_counter) { flags += L("fatal counter") + "  "; }
	    if (!flags.empty()) {
	        ImGui::TextUnformatted(flags.c_str());
	    }

	    if (ImGui::TreeNode(L("Scaling and rating").c_str())) {
	        ImGui::Text("%s", FormatText(L("Starter rating: %d").c_str(), selected_state->starter_rating).c_str());
	        ImGui::Text("%s", FormatText(L("Proration P1: %d    P2: %d").c_str(),
	            selected_state->attack_p1, selected_state->attack_p2).c_str());
	        ImGui::TreePop();
	    }

	    if (ImGui::TreeNode("Frame Breakdown")) {
	        ImGui::ShowHelpMarker("Red numbers are active frames, blue numbers are startup/recovery, black numbers are inactive. \n\nWhite borders are full invul/GP, green borders are partial invul/GP(hover for details). Projectile invul not yet being displayed.\n\n\"Non-deterministic\" frame length means that it is not fixed, landing recovery for example. After a non-deterministic state all values will be +\"n\", representing that would be n frames after the frames in question. They are not wrong, they just can't be statically computed.  \n\nSome are still incorrect, however they should be for the most part pretty obvious, around ~85% are done so far.");
	        auto iter_scr_frames = 1;
	        float window_visible_x2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
	        ImGuiStyle& style = ImGui::GetStyle();
	        int after_non_deterministic = 0;
	        for (auto& frame_activity : selected_state->frame_activity_status) {
	            if (iter_scr_frames > 500) {//needed to limit the amount of drawn frames to keep it from crashing on way too long states(rp based probably/too many branches prob)
	                ImGui::Text("+ Too long to show all"); 
	                break; }


	            auto color = IM_COL32(0, 255, 255, 255);

	            if (frame_activity == FrameActivity::Active || frame_activity == FrameActivity::NonDeterministicAcive) {
	                color = IM_COL32(255, 0, 0, 255);
	            }

	            ImGui::PushStyleColor(ImGuiCol_Text, color);
	            //ImGui::PushStyleColor(ImGuiCol_TextBg, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
	            if (frame_activity == FrameActivity::NonDeterministicAcive || frame_activity == FrameActivity::NonDeterministicInactive) {
	                ImGui::Text("Non Deterministic");// check jin's NmlAtk5B, seems to be wrong, should start hitbox at frame 7 not 8. jn201_03's sprite call says 2 frames, but actually is 3 frames long
	                after_non_deterministic = 1;
	            }
	            else {
	                if (after_non_deterministic) {
	                    ImGui::Text("+%d", after_non_deterministic);
	                    after_non_deterministic++;
	                }
	                else {
	                    ImGui::Text("%d", iter_scr_frames);
	                }
	            }
	            auto invuln_color = IM_COL32(50, 50, 50, 255);
	            if (selected_state->frame_invuln_status.at(iter_scr_frames - 1) == FrameInvuln::All) {
	                invuln_color = IM_COL32(200, 200, 200, 255);
	            }
	            else if (selected_state->frame_invuln_status.at(iter_scr_frames - 1) != FrameInvuln::None) {//its not none and its not full invuln, this is where the permutations come in
	                invuln_color = IM_COL32(100, 200, 100, 255);
	            }
	            ImGui::GetWindowDrawList()->AddRect(ImVec2(ImGui::GetItemRectMin().x - 1.5f, ImGui::GetItemRectMin().y - 1),
	                ImVec2(ImGui::GetItemRectMax().x + 2, ImGui::GetItemRectMax().y + 1),
	                invuln_color);//draws the square around representing invuln
	            ImVec2 mousePos = ImGui::GetMousePos();
	            if ((mousePos.x >= ImGui::GetItemRectMin().x - 1.5f && mousePos.x <= ImGui::GetItemRectMax().x + 2 &&
	                mousePos.y >= ImGui::GetItemRectMin().y - 1 && mousePos.y <= ImGui::GetItemRectMax().y + 1))
	            {
	                ImGui::BeginTooltip();
	                ImGui::PushTextWrapPos(450.0f);
	                ImGui::Text("Invuln/GP: %s", interpret_frame_invuln_enum(selected_state->frame_invuln_status.at(iter_scr_frames - 1)).c_str());
	                ImGui::PopTextWrapPos();
	                ImGui::EndTooltip();
	            }
	            ImGui::PopStyleColor();
	            float last_button_x2 = ImGui::GetItemRectMax().x;
	            float next_button_x2 = last_button_x2 + style.ItemSpacing.x + 10.0f; // Expected position if next button was on same line
	            if (iter_scr_frames < selected_state->frame_activity_status.size() && next_button_x2 < window_visible_x2)
	                ImGui::SameLine();
	            iter_scr_frames++;
	        }

	        for (auto& ea_state_pair : selected_state->frame_EA_effect_pairs) {
	            if (selected_state->frame_EA_effect_pairs.size() > 10) { break;//this is necessary because if you spawn an enourmous amount, the vertices crash. Happened with arakunes "UltimateAntiAirShotOD"
	            }
	            iter_scr_frames = 1;
	            auto frames_before_ptr = &ea_state_pair.first;
	            std::vector<FrameActivity>* frame_activity_status_ptr = &ea_state_pair.second.frame_activity_status;
	            //std::string fstring = "A";
	            if (!std::any_of(frame_activity_status_ptr->begin(), 
	                frame_activity_status_ptr->end(), 
	                [](FrameActivity frame_activity) {
	                    return frame_activity == FrameActivity::Active;})
	                ) {
	                continue;
	            }
	            //if (std::find(frame_activity_status_ptr->begin(), frame_activity_status_ptr->end(), fstring) != frame_activity_status_ptr->end()) {
	           //     continue;
	           // }
	            ImGui::Text("%s", ea_state_pair.second.name.c_str());
	            //will not draw unless there are active frames on the EA state
            
	            std::vector<FrameActivity> temp_vect = {};
	            for (int i = 0; i < *frames_before_ptr; i++) {
	                temp_vect.push_back(FrameActivity::Padding); //adds the padding frames
	            }
	            temp_vect.insert(temp_vect.end(), frame_activity_status_ptr->begin(), frame_activity_status_ptr->end());
	            for (auto& frame_activity : temp_vect) {
	                auto color = IM_COL32(0, 255, 255, 255);
	                if (frame_activity == FrameActivity::Active) {
	                    color = IM_COL32(255, 0, 0, 255);
	                }
	                else if (frame_activity == FrameActivity::Padding) {
	                    color = IM_COL32(0, 0, 0, 255);
	                }

	                ImGui::PushStyleColor(ImGuiCol_Text, color);
	                //ImGui::PushStyleColor(ImGuiCol_TextBg, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
	                ImGui::Text("%d", iter_scr_frames);
	                ImGui::GetWindowDrawList()->AddRect(ImVec2(ImGui::GetItemRectMin().x - 1.5f, ImGui::GetItemRectMin().y - 1),
	                    ImVec2(ImGui::GetItemRectMax().x + 2, ImGui::GetItemRectMax().y + 1),
	                    IM_COL32(50, 50, 50, 255));//draws the square around
	                ImGui::PopStyleColor();
	                float last_button_x2 = ImGui::GetItemRectMax().x;
	                float next_button_x2 = last_button_x2 + style.ItemSpacing.x + 10.0f; // Expected position if next button was on same line
	                if (iter_scr_frames < temp_vect.size() && next_button_x2 < window_visible_x2)
	                    ImGui::SameLine();
	                iter_scr_frames++;
	            }
	        }
	        ImGui::TreePop();
	    }
	    ImGui::Text("Whiff_cancels:", selected_state->fatal_counter);
	    for (std::string name : selected_state->whiff_cancel) {
	        ImGui::Text("    %s", name.c_str());
	    }
	    ImGui::Text("Hit_or_block_cancels(gatlings):", selected_state->fatal_counter);
	    int item_view_len;
	    if (selected_state->hit_or_block_cancel.size() > 5) {
	        item_view_len = 100;
	    }
	    else {
	        item_view_len = selected_state->hit_or_block_cancel.size() * 20;
	    }
	    ImGui::BeginChild("item view", ImVec2(0, item_view_len));
	    for (std::string name : selected_state->hit_or_block_cancel) {
	        ImGui::Text("    %s", name.c_str());
	    }
	    ImGui::EndChild();
	}

	// How each trigger fires, rather than what it plays: this is the config that used to sit
	// in the Playback Library window behind a trigger-type dropdown, where only one trigger's
	// worth of it was reachable at a time.
	void DrawTriggerSettingsModal()
	{
		if (g_settingsTrigger >= UnlimitedPlaybackManager::Trigger_Count)
		{
			return;
		}
		const TriggerType trigger = static_cast<TriggerType>(g_settingsTrigger);
		// Titled per trigger so each one keeps its own remembered size: the loop carries a
		// hotkey, two times, a reset mode and a snapshot button, and the rest carry three
		// fields.
		const std::string titleText = FormatText("%s settings##dummyaction%d",
			L(Manager::TriggerLabel(trigger)).c_str(), static_cast<int>(trigger));
		const char* title = titleText.c_str();

		if (g_settingsNeedsOpen)
		{
			ImGui::OpenPopup(title);
			g_settingsNeedsOpen = false;
		}
		// Sized for the loop, which carries the most controls of any trigger; the others just
		// leave whitespace rather than being cut off.
		BeginCenteredModalLayout(title, 470.0f,
			trigger == UnlimitedPlaybackManager::Trigger_OnLoop ? 560.0f : 330.0f);
		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoScrollbar))
		{
			// Dismissed by clicking away rather than by a button, so nothing reset the state.
			g_settingsTrigger = UnlimitedPlaybackManager::Trigger_Count;
			return;
		}

		UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();
		UnlimitedPlaybackManager::TriggerConfig& config = playback.GetTrigger(trigger);

		RememberModalSize(title);

		BeginModalBody();

		ContextMenuHeader(L(Manager::TriggerLabel(trigger)).c_str());
		ImGui::TextWrapped("%s", FormatText(L("Fires %s.").c_str(),
			L(Manager::TriggerWhen(trigger)).c_str()).c_str());
		ImGui::VerticalSpacing(4);

		// The loop has no cooldown or delay of its own - its pacing is the setup and ending
		// times below.
		if (trigger != UnlimitedPlaybackManager::Trigger_OnLoop)
		{
			// The conditions the game counts toward, so the only ones that can be
			// anticipated: blockstun and hitstun count down, and wakeup and tech count up to
			// the frame the dummy becomes actionable.
			const bool canFireEarly = (trigger == UnlimitedPlaybackManager::Trigger_Gap
				|| trigger == UnlimitedPlaybackManager::Trigger_ThrowTech
				|| trigger == UnlimitedPlaybackManager::Trigger_Wakeup
				|| trigger == UnlimitedPlaybackManager::Trigger_OnHitRecovery);

			ImGui::TextUnformatted(L("Delay").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(110.0f);
			if (ImGui::InputInt("##trigger_delay", &config.delayFrames) && !canFireEarly
				&& config.delayFrames < 0)
			{
				// Only a condition the game counts down to can be anticipated.
				config.delayFrames = 0;
			}
			ImGui::SameLine();
			ImGui::ShowHelpMarker(canFireEarly
				? L("Frames between the condition and the action. Positive waits, for making a reversal late on purpose. NEGATIVE fires early, which is how a reversal's motion gets delivered before the dummy is actionable so the attack lands on the first frame it can - about the length of the motion, so -3 for a 623C. Only wakeup, the block gap, on hit and throw tech can go negative, because those are the conditions the game counts toward and can therefore be anticipated.").c_str()
				: L("Frames to wait after the trigger condition before the action starts, for making a reversal late on purpose.").c_str());
			if (canFireEarly && config.delayFrames < 0)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("%s", FormatText(L("%df early").c_str(), -config.delayFrames).c_str());
			}

			ImGui::TextUnformatted(L("Cooldown").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(110.0f);
			if (ImGui::InputInt("##trigger_cooldown", &config.cooldownFrames) && config.cooldownFrames < 1)
			{
				config.cooldownFrames = 1;
			}
			ImGui::SameLine();
			ImGui::ShowHelpMarker(L("Blocks this trigger from firing again for this many frames after it activates.").c_str());
		}

		// With more than one action, how the trigger chooses between them - the same three
		// orders the Playback Library offered, since a trigger's list is what a library was.
		if (Manager::Instance().Get(trigger).actions.size() > 1)
		{
			ImGui::VerticalSpacing(4);
			ImGui::TextUnformatted(L("Picking order").c_str());
			ImGui::SameLine();
			ImGui::ShowHelpMarker(L("How the trigger chooses between its actions when it fires. Only actions that are on are considered.").c_str());
			int mode = Manager::Instance().Get(trigger).selectionMode;
			const char* modes[] = { L("Random").c_str(), L("Sequential").c_str(), L("Non-repeating Random").c_str() };
			ImGui::SetNextItemWidth(220.0f);
			if (ImGui::Combo("##trigger_picking", &mode, modes, IM_ARRAYSIZE(modes)))
			{
				Manager::Instance().SetSelectionMode(trigger, mode);
			}
		}

		ImGui::VerticalSpacing(4);
		ImGui::Checkbox(L("Mirror for the other side").c_str(), &config.autoMirror);
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Flips left and right when the dummy is on the opposite side from the one this action was written or recorded on, so a forward motion stays forward. Turn it off for something that depends on an absolute direction, like a corner setup.").c_str());

		if (trigger == UnlimitedPlaybackManager::Trigger_KeyPress)
		{
			ImGui::VerticalSpacing(4);
			ImGui::TextUnformatted(L("Hotkey").c_str());
			ImGui::SameLine();
			DrawPlaybackHotkeyBind(playback, HotkeyManager::Hotkey_UnlimitedPlaybackTrigger);
		}

		if (trigger == UnlimitedPlaybackManager::Trigger_OnLoop)
		{
			ImGui::VerticalSpacing(4);
			ImGui::TextUnformatted(L("Start and stop hotkey").c_str());
			ImGui::SameLine();
			DrawPlaybackHotkeyBind(playback, HotkeyManager::Hotkey_UnlimitedPlaybackLoop);

			ImGui::VerticalSpacing(4);
			float setupSeconds = playback.GetLoopSetupSeconds();
			ImGui::TextUnformatted(L("Setup time (s)").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(110.0f);
			if (ImGui::InputFloat("##loop_setup", &setupSeconds, 0.5f))
			{
				playback.SetLoopSetupSeconds(setupSeconds);
			}
			ImGui::SameLine();
			ImGui::ShowHelpMarker(L("How long the loop waits before each run, so you can get into position.").c_str());

			float endingSeconds = playback.GetLoopEndingSeconds();
			ImGui::TextUnformatted(L("Ending time (s)").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(110.0f);
			if (ImGui::InputFloat("##loop_ending", &endingSeconds, 0.5f))
			{
				playback.SetLoopEndingSeconds(endingSeconds);
			}
			ImGui::SameLine();
			ImGui::ShowHelpMarker(L("How long the loop lets the run play out before resetting.").c_str());

			ImGui::VerticalSpacing(4);
			bool restartLab = playback.GetLoopRestartLabState();
			if (ImGui::Checkbox(L("Reset the lab between runs").c_str(), &restartLab))
			{
				playback.SetLoopRestartLabState(restartLab);
			}
			int restartMode = playback.GetLoopRestartMode();
			ImGui::TextUnformatted(L("Reset position").c_str());
			static const char* kResetModes[] = { "Middle", "Left corner", "Right corner", "Where I set it" };
			for (int i = 0; i < 4; ++i)
			{
				if (i) { ImGui::SameLine(); }
				if (ImGui::RadioButton(L(kResetModes[i]).c_str(), restartMode == i))
				{
					playback.SetLoopRestartMode(i);
				}
			}
			if (playback.GetLoopRestartMode() == UnlimitedPlaybackManager::LoopReset_Custom)
			{
				if (ImGui::Button(L("Save the current position").c_str()))
				{
					playback.CaptureLoopCustomSnapshot();
				}
				ImGui::SameLine();
				ImGui::TextDisabled("%s", playback.HasLoopCustomSnapshot()
					? L("saved").c_str() : L("nothing saved yet").c_str());
			}
		}

		EndModalBodyAndSeparate();
		if (ImGui::Button(L("Done").c_str()))
		{
			g_settingsBackupValid = false;
			g_settingsTrigger = UnlimitedPlaybackManager::Trigger_Count;
			// The delay, cooldown and mirror flag live in the saved file too.
			Manager::Instance().Save();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("Cancel").c_str()))
		{
			if (g_settingsBackupValid)
			{
				playback.GetTrigger(trigger) = g_settingsBackup;
				playback.SetLoopSetupSeconds(g_settingsLoopSetup);
				playback.SetLoopEndingSeconds(g_settingsLoopEnding);
				playback.SetLoopRestartLabState(g_settingsLoopRestartLab);
				playback.SetLoopRestartMode(g_settingsLoopRestartMode);
				Manager::Instance().SetSelectionMode(trigger, g_settingsSelectionMode);
				g_settingsBackupValid = false;
			}
			g_settingsTrigger = UnlimitedPlaybackManager::Trigger_Count;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Cancel puts these settings back. A hotkey you rebound stays rebound - that is a global binding, not part of this trigger.").c_str());
		ImGui::EndPopup();
	}

	// ---- source pickers, shared by "+ Add Action" and a trigger row's "+" ----

	// Offers every source the trigger accepts and starts configuring the chosen one as a NEW
	// action of that trigger. Nothing is written until the modal is saved.
	void DrawSourceMenuItems(TriggerType trigger)
	{
		ContextMenuHeader(L("Take its inputs from").c_str());
		static const Source kSources[] = {
			Manager::Source_Notation, Manager::Source_File,
			Manager::Source_Animation, Manager::Source_CfSlot, Manager::Source_Burst,
		};
		for (Source source : kSources)
		{
			// Which sources this trigger takes at all - burst only answers a hit, and
			// animations are not offered on hit or on block. See SourceAllowedForTrigger.
			if (!Manager::SourceAllowedForTrigger(source, trigger))
			{
				continue;
			}
			if (ImGui::MenuItem(L(Manager::SourceLabel(source)).c_str()))
			{
				if (source == Manager::Source_Burst)
				{
					// Nothing to configure, so no modal - the row appears straight away.
					Manager::Action burst;
					burst.source = Manager::Source_Burst;
					Manager::Instance().Add(trigger, burst);
				}
				else if (source == Manager::Source_File)
				{
					// A file has exactly one thing to choose, so a window to host a "pick a
					// file" button is a step for nothing. Straight to the OS picker; the
					// answer commits the row when it arrives.
					BeginFilePick(trigger, -1);
				}
				else
				{
					OpenModal(trigger, source, -1);
				}
			}
		}
	}

	void DrawAddActionButton()
	{
		Manager& manager = Manager::Instance();
		const bool anyConfigured = manager.AssignedCount() > 0;

		// The Selectable is drawn first and the label painted back over it, so its
		// highlight covers the plus as well. Drawing the plus first and the Selectable
		// after left the lit area starting after the plus, cutting it in half.
		if (DrawFaintSelectable("##dummy_add_action_btn", "+", L("Add Action").c_str(),
				ImVec4(0.35f, 0.80f, 0.35f, 1.0f)))
		{
			ImGui::OpenPopup("##dummy_add_action");
		}

		// Only worth offering when there is something to clear. Behind a confirmation
		// because it throws away every trigger and every action in them at once.
		if (anyConfigured)
		{
			ImGui::SameLine(0.0f, 12.0f);
			if (DrawFaintSelectable("##dummy_clear_all_btn", nullptr, L("Clear All").c_str(),
					ImVec4(0.90f, 0.35f, 0.35f, 1.0f)))
			{
				ImGui::OpenPopup(L("Clear every dummy action?").c_str());
			}
		}

		CenterModalOnAppearing();
		if (ImGui::BeginPopupModal(L("Clear every dummy action?").c_str(), nullptr,
				ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted(
				L("This removes every trigger and every action in them.").c_str());
			ImGui::VerticalSpacing(6);
			if (ImGui::Button(L("Clear All").c_str()))
			{
				manager.ClearAll();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button(L("Cancel").c_str()))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopup("##dummy_add_action"))
		{
			ContextMenuHeader(L("Trigger type").c_str());
			// Every trigger, not only the empty ones: adding to a trigger that already has
			// actions is how a set is built, and the count says which ones those are.
			for (TriggerType trigger : Manager::TriggerOrder())
			{
				const size_t count = manager.Get(trigger).actions.size();
				const std::string label = count == 0
					? L(Manager::TriggerLabel(trigger))
					: FormatText("%s  (%d)", L(Manager::TriggerLabel(trigger)).c_str(),
						static_cast<int>(count));
				if (ImGui::BeginMenu(label.c_str()))
				{
					DrawSourceMenuItems(trigger);
					ImGui::EndMenu();
				}
			}
			ImGui::EndPopup();
		}
	}

	// Colours shared by the rows, so on and off read the same everywhere.
	const ImVec4 kDetailColor(0.72f, 0.72f, 0.72f, 1.0f);
	const ImVec4 kDetailOffColor(0.45f, 0.45f, 0.45f, 1.0f);
	const ImVec4 kBrokenColor(0.85f, 0.65f, 0.35f, 1.0f);
	const ImVec4 kFiredColor(0.45f, 0.92f, 0.45f, 1.0f);
	const ImVec4 kDeleteColor(0.90f, 0.35f, 0.35f, 1.0f);
	// How far the action rows sit in from the trigger row, so they read as its contents.
	const float kActionIndent = 26.0f;
	const char* kActionDragPayload = "BBCFIM_DUMMY_ACTIONS";

	// Row selection, per trigger: ctrl-click toggles, shift-click ranges, plain click picks
	// one - the Playback Library's rows had exactly this, and a set of thirty recordings
	// is unmanageable without it.
	std::map<int, std::set<int>> g_selection;
	std::map<int, int> g_selectionAnchor;

	void ApplySelectionClick(std::set<int>& selection, int& anchor, int clicked, bool ctrl, bool shift)
	{
		if (ctrl)
		{
			if (selection.count(clicked)) { selection.erase(clicked); } else { selection.insert(clicked); }
			anchor = clicked;
		}
		else if (shift && anchor >= 0)
		{
			selection.clear();
			for (int k = (std::min)(anchor, clicked); k <= (std::max)(anchor, clicked); ++k) { selection.insert(k); }
		}
		else
		{
			selection.clear();
			selection.insert(clicked);
			anchor = clicked;
		}
	}

	// The indices a row operation applies to: the selection when the row is part of one,
	// otherwise just the row.
	std::vector<size_t> RowGroup(TriggerType trigger, int index)
	{
		const std::set<int>& selection = g_selection[trigger];
		std::vector<size_t> group;
		if (selection.size() > 1 && selection.count(index))
		{
			for (int i : selection) { group.push_back(static_cast<size_t>(i)); }
		}
		else
		{
			group.push_back(static_cast<size_t>(index));
		}
		std::sort(group.begin(), group.end());
		return group;
	}

	// Drag payload: which trigger's rows, and which.
	struct ActionDragPayload
	{
		int trigger = 0;
		int count = 0;
		int indices[64] = {};
	};

	// What a row asks the block to do once the list is no longer being walked.
	struct RowRequest
	{
		std::vector<size_t> remove;
		int editIndex = -1;
		int renameIndex = -1;
		int sendIndex = -1;
		int saveIndex = -1;
		int setIndexOf = -1;
		std::vector<size_t> moveFrom;
		int moveInsertion = -1;
	};

	// Modals raised from a row, drawn at block scope (they cannot live inside the scrolling
	// child). Which trigger they belong to is g_rowModalTrigger; the index is inside.
	int g_rowModalTrigger = UnlimitedPlaybackManager::Trigger_Count;
	int g_rowModalIndex = -1;
	int g_rowModalSlot = 1;
	int g_rowModalNewIndex = 1;
	char g_rowModalName[128] = "";
	std::vector<size_t> g_rowModalRemove;
	enum RowModal { RowModal_None, RowModal_Send, RowModal_Rename, RowModal_SetIndex, RowModal_ConfirmRemove };
	RowModal g_rowModal = RowModal_None;
	bool g_rowModalNeedsOpen = false;

	// The pending "save this action to a playback file" pick.
	const char* kSaveDialogToken = "DummyActionsPanelSave";
	int g_saveTrigger = UnlimitedPlaybackManager::Trigger_Count;
	int g_saveIndex = -1;

	// Which actions carry a frame buffer of their own that can be sent to a slot or written
	// to a file. A CF slot action IS a slot, an animation has no inputs.
	bool ActionHasOwnFrames(const Manager::Action& action)
	{
		return action.source == Manager::Source_Recording || action.source == Manager::Source_Notation
			|| action.source == Manager::Source_File;
	}

	void BeginActionSave(TriggerType trigger, int index)
	{
		if (NativeFileDialog::IsOpen()) { return; }
		const Manager::Trigger& slot = Manager::Instance().Get(trigger);
		if (index < 0 || static_cast<size_t>(index) >= slot.actions.size()) { return; }
		NativeFileDialog::Request request;
		request.save = true;
		request.title = L("Save this action as a playback file");
		request.filters.push_back({ L("Playback file"), "*.playback" });
		request.defaultExtension = "playback";
		std::string name = Manager::ActionSummary(slot.actions[index]);
		for (char& ch : name) { if (strchr("\\/:*?\"<>|", ch)) { ch = '_'; } }
		request.initialPath = UnlimitedPlaybackManager::Instance().GetLibraryFolderPublic() + "\\" + name + ".playback";
		if (NativeFileDialog::Open(kSaveDialogToken, request))
		{
			g_saveTrigger = trigger;
			g_saveIndex = index;
		}
	}

	// One action of a trigger: its switch (when there is more than one), what it is, and
	// the play / edit / remove buttons the Playback Library's rows have - plus that list's
	// selection, drag reordering and right-click menu.
	void DrawActionLine(TriggerType trigger, size_t index, bool multi, bool triggerOn,
		bool inTrainingMatch, RowRequest* request)
	{
		Manager& manager = Manager::Instance();
		const Manager::Trigger& slot = manager.Get(trigger);
		const Manager::Action& action = slot.actions[index];
		const bool actionOn = !multi || action.enabled;
		const bool runnable = Manager::IsActionRunnable(action);
		std::set<int>& selection = g_selection[trigger];
		int& anchor = g_selectionAnchor.emplace(static_cast<int>(trigger), -1).first->second;
		const int i = static_cast<int>(index);

		ImGui::PushID(i);

		if (multi)
		{
			bool on = action.enabled;
			if (ImGui::Checkbox("##action_on", &on))
			{
				// Ticking one of a selection ticks the selection.
				manager.SetActionsEnabled(trigger, RowGroup(trigger, i), on);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", L("Turn this action on or off").c_str());
			}
			ImGui::SameLine(0.0f, 6.0f);
		}
		else
		{
			// A lone action has no switch: it is the trigger's whole content.
			ImGui::TextDisabled("-");
			ImGui::SameLine(0.0f, 6.0f);
		}

		// Buttons pinned to the right; the row's label takes the rest.
		const float buttonsWidth = 78.0f;
		const float buttonsAt = ImGui::GetContentRegionMax().x - buttonsWidth;
		const float labelStart = ImGui::GetCursorPosX();
		const float labelWidth = (std::max)(60.0f, buttonsAt - labelStart - 8.0f);

		std::string detail = L(Manager::SourceLabel(action.source));
		const std::string summary = Manager::ActionSummary(action);
		if (!summary.empty()) { detail += "  -  " + summary; }
		if (multi && !action.enabled) { detail += "  "; detail += L("(off)"); }

		// A Selectable the label is painted over, so the row can be picked and dragged and
		// still show its text in the row's own colour.
		ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
		const ImVec2 rowMin = ImGui::GetCursorScreenPos();
		if (ImGui::Selectable("##row", selection.count(i) != 0, ImGuiSelectableFlags_AllowOverlap,
				ImVec2(labelWidth, 0.0f)))
		{
			ApplySelectionClick(selection, anchor, i, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
		}
		ImGui::PopStyleColor(3);
		const ImVec2 rowMax = ImGui::GetItemRectMax();
		if (ImGui::IsItemHovered() && !runnable)
		{
			ImGui::SetTooltip("%s", L("This action is not set up enough to do anything yet.").c_str());
		}

		// Drag to reorder, within this trigger. A multi-selection drags as one.
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
		{
			ActionDragPayload payload;
			payload.trigger = static_cast<int>(trigger);
			std::vector<size_t> group = RowGroup(trigger, i);
			if (group.size() == 1) { selection.clear(); selection.insert(i); anchor = i; }
			for (size_t k = 0; k < group.size() && payload.count < 64; ++k) { payload.indices[payload.count++] = static_cast<int>(group[k]); }
			ImGui::SetDragDropPayload(kActionDragPayload, &payload, sizeof(payload));
			ImGui::TextUnformatted(payload.count > 1
				? FormatText(L("Dragged %d slots").c_str(), payload.count).c_str()
				: summary.c_str());
			ImGui::EndDragDropSource();
		}
		if (ImGui::BeginDragDropTarget())
		{
			const ImGuiDragDropFlags flags = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
			if (const ImGuiPayload* dropped = ImGui::AcceptDragDropPayload(kActionDragPayload, flags))
			{
				const ActionDragPayload* from = static_cast<const ActionDragPayload*>(dropped->Data);
				if (dropped->DataSize == sizeof(ActionDragPayload) && from->trigger == static_cast<int>(trigger) && from->count > 0)
				{
					const bool after = ImGui::GetIO().MousePos.y >= (rowMin.y + rowMax.y) * 0.5f;
					const float y = after ? rowMax.y : rowMin.y;
					ImGui::GetWindowDrawList()->AddLine(ImVec2(rowMin.x, y), ImVec2(rowMax.x, y), IM_COL32(120, 200, 255, 220), 1.5f);
					if (dropped->IsDelivery())
					{
						request->moveFrom.assign(from->indices, from->indices + from->count);
						request->moveInsertion = i + (after ? 1 : 0);
					}
				}
			}
			ImGui::EndDragDropTarget();
		}

		// The label, over the Selectable.
		{
			ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 0.88f);
			const ImVec4 color = !runnable ? kBrokenColor : (actionOn && triggerOn) ? kDetailColor : kDetailOffColor;
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			const ImVec2 textPos(rowMin.x + 4.0f, rowMin.y + (rowMax.y - rowMin.y - ImGui::GetTextLineHeight()) * 0.5f);
			// Clipped with an ellipsis rather than wrapped, so a row is always one line and
			// the buttons stay on it.
			ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), textPos, ImVec2(rowMax.x - 2.0f, rowMax.y),
				rowMax.x - 2.0f, detail.c_str(), nullptr, nullptr);
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}

		// Right-click: the Playback Library's row menu, for actions.
		if (ImGui::BeginPopupContextItem("action_context"))
		{
			ContextMenuHeader(summary.empty() ? L(Manager::SourceLabel(action.source)).c_str() : summary.c_str());
			const bool hasFrames = ActionHasOwnFrames(action);
			if (ImGui::MenuItem(L("Send to CF Slot").c_str(), nullptr, false, inTrainingMatch && hasFrames))
			{
				request->sendIndex = i;
			}
			if (ImGui::MenuItem(L("Save to File").c_str(), nullptr, false,
					action.source == Manager::Source_Recording || action.source == Manager::Source_Notation))
			{
				request->saveIndex = i;
			}
			if (ImGui::MenuItem(L("Rename").c_str(), nullptr, false, action.source == Manager::Source_Recording))
			{
				request->renameIndex = i;
			}
			ImGui::Separator();
			const std::vector<size_t> group = RowGroup(trigger, i);
			const int groupMin = static_cast<int>(group.front());
			const int groupMax = static_cast<int>(group.back());
			const int count = static_cast<int>(slot.actions.size());
			if (ImGui::MenuItem(L("Move up").c_str(), nullptr, false, count > 1 && groupMin > 0))
			{
				request->moveFrom = group;
				request->moveInsertion = groupMin - 1;
			}
			if (ImGui::MenuItem(L("Move down").c_str(), nullptr, false, count > 1 && groupMax < count - 1))
			{
				request->moveFrom = group;
				request->moveInsertion = groupMax + 2;
			}
			if (ImGui::MenuItem(L("Set index...").c_str(), nullptr, false, count > 1))
			{
				request->setIndexOf = i;
			}
			if (multi)
			{
				ImGui::Separator();
				if (ImGui::MenuItem(L("Turn ALL slots off").c_str())) { manager.SetAllActionsEnabled(trigger, false); }
				if (ImGui::MenuItem(L("Turn ALL slots on").c_str()))  { manager.SetAllActionsEnabled(trigger, true); }
			}
			ImGui::EndPopup();
		}

		// The three buttons.
		ImGui::SameLine(buttonsAt > labelStart + 60.0f ? buttonsAt : 0.0f);
		{
			const bool canPlay = inTrainingMatch && runnable;
			if (!canPlay) { ImGui::BeginDisabled(); }
			if (ImGui::SmallButton(">") && canPlay)
			{
				UnlimitedPlaybackManager::Instance().PlayActionNow(trigger, index);
			}
			if (!canPlay) { ImGui::EndDisabled(); }
			ImGui::HoverTooltipEvenDisabled(inTrainingMatch
				? L("Play now").c_str()
				: L("Play (Disabled due to not being in lab)").c_str());
		}
		ImGui::SameLine();
		{
			const bool editable = action.source != Manager::Source_Burst;
			if (!editable) { ImGui::BeginDisabled(); }
			if (ImGui::SmallButton("/") && editable)
			{
				request->editIndex = i;
			}
			if (!editable) { ImGui::EndDisabled(); }
			ImGui::HoverTooltipEvenDisabled(editable
				? L("Edit").c_str()
				: L("Nothing to configure for a burst").c_str());
		}
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, kDeleteColor);
		if (ImGui::SmallButton("X"))
		{
			request->remove = RowGroup(trigger, i);
		}
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
		{
			const std::vector<size_t> group = RowGroup(trigger, i);
			ImGui::SetTooltip("%s", group.size() > 1
				? FormatText(L("Remove these %d actions").c_str(), static_cast<int>(group.size())).c_str()
				: multi ? L("Remove this action").c_str()
				: L("Remove this action - it is the trigger's last, so the trigger goes with it").c_str());
		}

		ImGui::PopID();
	}

	void OpenRowModal(TriggerType trigger, int index, RowModal which)
	{
		g_rowModalTrigger = trigger;
		g_rowModalIndex = index;
		g_rowModal = which;
		g_rowModalNeedsOpen = true;
		const Manager::Trigger& slot = Manager::Instance().Get(trigger);
		if (index >= 0 && static_cast<size_t>(index) < slot.actions.size())
		{
			strncpy_s(g_rowModalName, slot.actions[index].recordingName.c_str(), sizeof(g_rowModalName) - 1);
			g_rowModalNewIndex = index + 1;
		}
	}

	// The row modals, drawn once at panel scope. Titles are fixed, so one of each exists.
	void DrawRowModals()
	{
		if (g_rowModal == RowModal_None || g_rowModalTrigger >= UnlimitedPlaybackManager::Trigger_Count)
		{
			return;
		}
		Manager& manager = Manager::Instance();
		const TriggerType trigger = static_cast<TriggerType>(g_rowModalTrigger);
		const int count = static_cast<int>(manager.Get(trigger).actions.size());
		const char* title = nullptr;
		switch (g_rowModal)
		{
		case RowModal_Send:          title = "Send to CF Slot##dummyrow"; break;
		case RowModal_Rename:        title = "Rename##dummyrow"; break;
		case RowModal_SetIndex:      title = "Set index##dummyrow"; break;
		case RowModal_ConfirmRemove: title = "Remove actions?##dummyrow"; break;
		default: return;
		}
		if (g_rowModalNeedsOpen)
		{
			ImGui::OpenPopup(title);
			g_rowModalNeedsOpen = false;
		}
		CenterModalOnAppearing();
		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			g_rowModal = RowModal_None;
			return;
		}
		bool close = false;
		switch (g_rowModal)
		{
		case RowModal_Send:
			ImGui::TextUnformatted(L("CF Slot").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(90.0f);
			ImGui::InputInt("##send_slot", &g_rowModalSlot);
			g_rowModalSlot = (std::max)(1, (std::min)(4, g_rowModalSlot));
			ImGui::VerticalSpacing(4);
			if (ImGui::Button(L("Send").c_str()))
			{
				UnlimitedPlaybackManager::Instance().LoadActionIntoSlot(trigger, static_cast<size_t>(g_rowModalIndex), g_rowModalSlot);
				close = true;
			}
			break;
		case RowModal_Rename:
			ImGui::SetNextItemWidth(300.0f);
			ImGui::InputText("##rename", g_rowModalName, IM_ARRAYSIZE(g_rowModalName));
			ImGui::VerticalSpacing(4);
			if (ImGui::Button(L("Done").c_str()))
			{
				manager.SetRecordingName(trigger, static_cast<size_t>(g_rowModalIndex), g_rowModalName);
				close = true;
			}
			break;
		case RowModal_SetIndex:
			ImGui::SetNextItemWidth(110.0f);
			ImGui::InputInt(L("Index").c_str(), &g_rowModalNewIndex);
			g_rowModalNewIndex = (std::max)(1, (std::min)(count, g_rowModalNewIndex));
			ImGui::TextDisabled(L("Valid range: 1-%d").c_str(), count);
			ImGui::VerticalSpacing(4);
			if (ImGui::Button(L("Move").c_str()))
			{
				const int from = g_rowModalIndex;
				const int to = g_rowModalNewIndex - 1;
				if (from >= 0 && from < count && to != from)
				{
					manager.MoveActions(trigger, { static_cast<size_t>(from) }, static_cast<size_t>(to > from ? to + 1 : to));
					g_selection[trigger].clear();
				}
				close = true;
			}
			break;
		case RowModal_ConfirmRemove:
			ImGui::TextUnformatted(FormatText(L("Remove these %d actions?").c_str(),
				static_cast<int>(g_rowModalRemove.size())).c_str());
			ImGui::VerticalSpacing(4);
			if (ImGui::Button(L("Remove").c_str()))
			{
				manager.RemoveActions(trigger, g_rowModalRemove);
				g_selection[trigger].clear();
				close = true;
			}
			break;
		default: break;
		}
		ImGui::SameLine();
		if (ImGui::Button(L("Cancel").c_str())) { close = true; }
		if (close)
		{
			g_rowModal = RowModal_None;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	// A trigger and everything in it: the trigger row (switch, name, timing, its own three
	// buttons), then one indented line per action.
	void DrawTriggerBlock(TriggerType trigger, bool inTrainingMatch)
	{
		Manager& manager = Manager::Instance();
		const Manager::Trigger& slot = manager.Get(trigger);
		if (slot.actions.empty())
		{
			g_selection[trigger].clear();
			return;
		}
		const bool multi = slot.actions.size() > 1;
		const bool triggerOn = slot.enabled;
		// A selection that outlived its rows is dropped.
		{
			std::set<int>& selection = g_selection[trigger];
			for (auto it = selection.begin(); it != selection.end();)
			{
				if (*it < 0 || static_cast<size_t>(*it) >= slot.actions.size()) { it = selection.erase(it); } else { ++it; }
			}
		}

		ImGui::PushID(static_cast<int>(trigger));

		// ---- trigger row ----
		{
			bool on = slot.enabled;
			if (ImGui::Checkbox("##trigger_on", &on))
			{
				manager.SetTriggerEnabled(trigger, on);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", L("Turn this trigger on or off").c_str());
			}
		}
		ImGui::SameLine(0.0f, 6.0f);

		// Everything but the switch fades when the trigger is off, so on and off can be
		// told apart from across the room.
		if (!triggerOn) { ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f); }

		// Flashes green for half a second when the trigger goes off, so you can see THAT it
		// fired and exactly when - a toast tells you the same thing but scrolls away and
		// does not say which row it came from.
		UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();
		const unsigned long long firedMs = playback.GetTriggerLastFiredMs(trigger);
		const bool justFired = firedMs != 0 && (GetTickCount64() - firedMs) < 500;
		if (justFired) { ImGui::PushStyleColor(ImGuiCol_Text, kFiredColor); }
		ImGui::TextUnformatted(L(Manager::TriggerLabel(trigger)).c_str());
		if (justFired) { ImGui::PopStyleColor(); }
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", FormatText(L("Runs %s.").c_str(),
				L(Manager::TriggerWhen(trigger)).c_str()).c_str());
		}

		// The delay, when there is one. Worth stating on the row because it changes what the
		// trigger does and is otherwise two clicks away inside the settings.
		const int delayFrames = playback.GetTrigger(trigger).delayFrames;
		if (delayFrames != 0)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", delayFrames > 0
				? FormatText(L("%df delay").c_str(), delayFrames).c_str()
				: FormatText(L("%df early").c_str(), -delayFrames).c_str());
		}

		// With several actions, how it picks between them.
		if (multi)
		{
			const char* mode = "random";
			switch (slot.selectionMode)
			{
			case UnlimitedPlaybackManager::Selection_Sequential:         mode = "in order"; break;
			case UnlimitedPlaybackManager::Selection_NonRepeatingRandom: mode = "no repeats"; break;
			default: break;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("%s", FormatText("%d, %s",
				static_cast<int>(slot.actions.size()), L(mode).c_str()).c_str());
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", L("How many actions this trigger holds, and how it picks one when it fires. Change the picking order in the trigger's settings.").c_str());
			}
		}

		// The two triggers you press yourself say which key does it, since there is no other
		// way to tell from the row what you bound.
		if (trigger == UnlimitedPlaybackManager::Trigger_KeyPress
			|| trigger == UnlimitedPlaybackManager::Trigger_OnLoop)
		{
			const HotkeyManager::Action bind =
				trigger == UnlimitedPlaybackManager::Trigger_KeyPress
					? HotkeyManager::Hotkey_UnlimitedPlaybackTrigger
					: HotkeyManager::Hotkey_UnlimitedPlaybackLoop;
			const std::string key = HotkeyManager::DisplayString(HotkeyManager::GetBinding(bind));
			ImGui::SameLine();
			ImGui::TextDisabled("%s", FormatText(L("press %s").c_str(),
				key.empty() ? L("nothing").c_str() : key.c_str()).c_str());
		}

		// The loop is the one trigger with a state worth showing at rest: it either is or is
		// not going, and nothing else on the page says so.
		if (trigger == UnlimitedPlaybackManager::Trigger_OnLoop)
		{
			ImGui::SameLine();
			if (playback.IsLoopActive())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, kFiredColor);
				ImGui::TextUnformatted(L("(looping)").c_str());
				ImGui::PopStyleColor();
			}
			else
			{
				ImGui::TextDisabled("%s", L("(stopped)").c_str());
			}
		}

		if (!triggerOn)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", L("(off)").c_str());
		}

		// Buttons pinned to the right of the trigger row: settings, add, remove.
		const float buttonsWidth = 78.0f;
		const float buttonsAt = ImGui::GetContentRegionMax().x - buttonsWidth;
		if (buttonsAt > 200.0f) { ImGui::SameLine(buttonsAt); } else { ImGui::SameLine(); }

		if (ImGui::SmallButton("*"))
		{
			g_settingsTrigger = trigger;
			g_settingsNeedsOpen = true;
			RequestModalFit();

			// Copied so Cancel can put it back: the firing code reads this config every
			// frame, so the modal has nowhere to edit but the live object.
			g_settingsBackup = playback.GetTrigger(trigger);
			g_settingsLoopSetup = playback.GetLoopSetupSeconds();
			g_settingsLoopEnding = playback.GetLoopEndingSeconds();
			g_settingsLoopRestartLab = playback.GetLoopRestartLabState();
			g_settingsLoopRestartMode = playback.GetLoopRestartMode();
			g_settingsSelectionMode = slot.selectionMode;
			g_settingsBackupValid = true;
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", L("Configure when this trigger fires").c_str());
		}
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.80f, 0.35f, 1.0f));
		const bool addPressed = ImGui::SmallButton("+");
		ImGui::PopStyleColor();
		if (addPressed)
		{
			ImGui::OpenPopup("##trigger_add");
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", L("Add another action to this trigger").c_str());
		}
		if (ImGui::BeginPopup("##trigger_add"))
		{
			DrawSourceMenuItems(trigger);
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, kDeleteColor);
		const bool clearPressed = ImGui::SmallButton("X");
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", L("Remove this trigger and everything in it").c_str());
		}

		// ---- action rows ----
		RowRequest request;
		ImGui::Indent(kActionIndent);
		{
			// Past the configured number of rows the list scrolls rather than growing:
			// some people put a great many actions on one trigger, and the page has to
			// stay a page.
			const int maxRows = (std::max)(1, Settings::settingsIni.dummyActionMaxRows);
			const bool scrolls = static_cast<int>(slot.actions.size()) > maxRows;
			if (scrolls)
			{
				const float rowHeight = ImGui::GetFrameHeightWithSpacing();
				ImGui::BeginChild("##trigger_actions",
					ImVec2(0.0f, rowHeight * maxRows + ImGui::GetStyle().ItemSpacing.y),
					false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
			}
			for (size_t i = 0; i < slot.actions.size(); ++i)
			{
				DrawActionLine(trigger, i, multi, triggerOn, inTrainingMatch, &request);
			}
			if (scrolls)
			{
				ImGui::EndChild();
			}
		}
		ImGui::Unindent(kActionIndent);

		if (!triggerOn) { ImGui::PopStyleVar(); }

		// A faint rule under each block, so a list of them reads as blocks rather than a wall.
		ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
		ImGui::Separator();
		ImGui::PopStyleColor();

		// Mutations last, once nothing is reading the list any more.
		if (request.editIndex >= 0 && static_cast<size_t>(request.editIndex) < slot.actions.size())
		{
			const Manager::Action& action = slot.actions[request.editIndex];
			if (action.source == Manager::Source_File)
			{
				BeginFilePick(trigger, request.editIndex);
			}
			else
			{
				OpenModal(trigger, action.source, request.editIndex);
			}
		}
		if (request.sendIndex >= 0)   { OpenRowModal(trigger, request.sendIndex, RowModal_Send); }
		if (request.renameIndex >= 0) { OpenRowModal(trigger, request.renameIndex, RowModal_Rename); }
		if (request.setIndexOf >= 0)  { OpenRowModal(trigger, request.setIndexOf, RowModal_SetIndex); }
		if (request.saveIndex >= 0)   { BeginActionSave(trigger, request.saveIndex); }
		if (!request.moveFrom.empty() && request.moveInsertion >= 0)
		{
			std::vector<size_t> from = request.moveFrom;
			std::sort(from.begin(), from.end());
			size_t insertedAt = 0;
			if (manager.MoveActions(trigger, from, static_cast<size_t>(request.moveInsertion), &insertedAt))
			{
				std::set<int>& selection = g_selection[trigger];
				selection.clear();
				for (size_t k = 0; k < from.size(); ++k) { selection.insert(static_cast<int>(insertedAt + k)); }
				g_selectionAnchor[trigger] = static_cast<int>(insertedAt);
			}
		}
		if (!request.remove.empty())
		{
			if (request.remove.size() > 1)
			{
				g_rowModalRemove = request.remove;
				OpenRowModal(trigger, static_cast<int>(request.remove.front()), RowModal_ConfirmRemove);
			}
			else
			{
				manager.Remove(trigger, request.remove.front());
				g_selection[trigger].clear();
			}
		}
		if (clearPressed)
		{
			if (multi)
			{
				g_confirmClearTrigger = trigger;
				ImGui::OpenPopup(L("Remove this trigger?").c_str());
			}
			else
			{
				manager.Clear(trigger);
			}
		}

		CenterModalOnAppearing();
		if (ImGui::BeginPopupModal(L("Remove this trigger?").c_str(), nullptr,
				ImGuiWindowFlags_AlwaysAutoResize))
		{
			const int count = static_cast<int>(manager.Get(trigger).actions.size());
			ImGui::TextUnformatted(FormatText(L("Remove %s and its %d actions?").c_str(),
				L(Manager::TriggerLabel(trigger)).c_str(), count).c_str());
			ImGui::VerticalSpacing(6);
			if (ImGui::Button(L("Remove").c_str()))
			{
				manager.Clear(trigger);
				g_confirmClearTrigger = UnlimitedPlaybackManager::Trigger_Count;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button(L("Cancel").c_str()))
			{
				g_confirmClearTrigger = UnlimitedPlaybackManager::Trigger_Count;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		ImGui::PopID();
	}

	// ---- the source modals ----

	void DrawNotationModal(TriggerType trigger)
	{
		BeginModalBody();

		ImGui::TextWrapped("%s", L("Type what the dummy should do in numpad notation, one move per frame group. Examples: 5C, 236B, 623C, 41236D.").c_str());
		ImGui::VerticalSpacing(4);

		ImGui::SetNextItemWidth(360.0f);
		if (ImGui::InputText("##notation", g_notationText, IM_ARRAYSIZE(g_notationText)))
		{
			g_notationError.clear();
			g_notationFrameCount = 0;
		}

		// Parsed with the TAS tool's own parser, so the notation the two accept is the same
		// and there is only one place that knows it.
		std::vector<uint16_t> parsed;
		const bool empty = g_notationText[0] == '\0';
		const bool ok = !empty && TasManager::TryParseCommand(g_notationText, &parsed);
		if (!empty && ok)
		{
			g_notationFrameCount = static_cast<int>(parsed.size());
			ImGui::TextDisabled("%s", FormatText(L("%d frame(s)").c_str(), g_notationFrameCount).c_str());

			// A playback frame has room for the four attack buttons and a direction, and
			// nothing else, so the taunt the notation accepts cannot survive the trip.
			bool hasTaunt = false;
			for (uint16_t frame : parsed)
			{
				if (frame & 0x100) { hasTaunt = true; break; }
			}
			if (hasTaunt)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
				ImGui::TextWrapped("%s", L("The taunt button cannot be played back and will be dropped. Everything else in this will work.").c_str());
				ImGui::PopStyleColor();
			}
		}
		else if (!empty)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.45f, 0.45f, 1.0f));
			ImGui::TextWrapped("%s", L("That is not notation this can read. Directions are the numpad 1-9, buttons are A B C D, and ap is the taunt button.").c_str());
			ImGui::PopStyleColor();
		}

		EndModalBodyAndSeparate();
		ImGui::VerticalSpacing(4);
		if (!ok) { ImGui::BeginDisabled(); }
		if (ImGui::Button(L("Done").c_str()) && ok)
		{
			Manager::Action& action = g_draft;
			action.notation = g_notationText;
			// Stored in the RAW slot layout, which is what StartRuntimePlayback wants: two
			// bytes per frame, the input then an aux byte. PlaybackSlot::load_raw_into_slot
			// takes the frame count as size() / 2, so a one-byte-per-frame buffer plays for
			// half its length - a three-token notation lasted a single frame and looked like
			// nothing happening at all.
			//
			// The input byte is direction in the low nibble and buttons in the high one; the
			// aux byte is 0, matching PlaybackSlot::load_into_slot's own expansion. The
			// parser returns 16 bits only because its notation has a taunt bit above the
			// byte, and a slot frame has nowhere to put it.
			action.notationFrames.clear();
			action.notationFrames.reserve((parsed.size() + kNotationTailFrames) * 2);
			for (uint16_t frame : parsed)
			{
				action.notationFrames.push_back(static_cast<char>(frame & 0xFF));
				action.notationFrames.push_back(0);
			}
			// A neutral tail, so the last thing the notation asks for is never also the last
			// frame of the buffer. The game ends playback when the cursor runs past the
			// length, and an input sitting on that boundary is at the mercy of whether it
			// gets applied before the run is torn down - which is what "the C never came
			// out" looked like. Neutral frames change nothing about what is pressed.
			for (int i = 0; i < kNotationTailFrames; ++i)
			{
				action.notationFrames.push_back(static_cast<char>(kNeutralInput));
				action.notationFrames.push_back(0);
			}
			CommitModal();
		}
		if (!ok) { ImGui::EndDisabled(); }
		ImGui::SameLine();
		if (ImGui::Button(L("Cancel").c_str()))
		{
			CancelModal();
		}
	}

	void DrawCfSlotModal(TriggerType trigger)
	{
		BeginModalBody();

		ImGui::TextWrapped("%s", L("Play back one of the game's own four recording slots.").c_str());
		ImGui::VerticalSpacing(4);
		for (int slot = 1; slot <= 4; ++slot)
		{
			if (slot > 1) { ImGui::SameLine(); }
			if (ImGui::RadioButton(FormatText(L("Slot %d").c_str(), slot).c_str(), &g_pendingCfSlot, slot))
			{
				g_cfSlotError.clear();
			}
		}
		ImGui::VerticalSpacing(4);
		ImGui::Checkbox(L("Keep a copy of the recording").c_str(), &g_pendingCfSlotKeepCopy);
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Off: plays whatever the slot holds when the trigger fires, so re-recording the slot changes what the dummy does. On: copies the slot's recording into this action now, so it stays the same however the slot changes - and it travels with the training file.").c_str());
		if (!g_cfSlotError.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.45f, 0.45f, 1.0f));
			ImGui::TextWrapped("%s", g_cfSlotError.c_str());
			ImGui::PopStyleColor();
		}

		EndModalBodyAndSeparate();
		if (ImGui::Button(L("Done").c_str()))
		{
			g_draft.cfSlot = g_pendingCfSlot;
			if (g_pendingCfSlotKeepCopy)
			{
				std::vector<char> frames;
				bool facingLeft = false;
				if (!UnlimitedPlaybackManager::Instance().ReadSlotRecording(g_pendingCfSlot, &frames, &facingLeft)
					|| frames.empty())
				{
					g_cfSlotError = FormatText(L("Slot %d is empty. Record something into it first.").c_str(),
						g_pendingCfSlot);
				}
				else
				{
					g_draft.source = Manager::Source_Recording;
					g_draft.recordingFrames = frames;
					g_draft.recordingFacingLeft = facingLeft;
					if (g_draft.recordingName.empty())
					{
						g_draft.recordingName = FormatText(L("Slot %d").c_str(), g_pendingCfSlot);
					}
					CommitModal();
				}
			}
			else
			{
				g_draft.source = Manager::Source_CfSlot;
				g_draft.recordingFrames.clear();
				CommitModal();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(L("Cancel").c_str()))
		{
			CancelModal();
		}
	}

	void BeginFilePick(TriggerType trigger, int actionIndex)
	{
		if (NativeFileDialog::IsOpen())
		{
			return;
		}
		NativeFileDialog::Request request;
		request.title = L("Choose a playback file");
		request.filters.push_back({ L("Playback file or library"), "*.playback;*.upl" });
		request.filters.push_back({ L("Playback file"), "*.playback" });
		request.filters.push_back({ L("Playback library"), "*.upl" });
		request.defaultExtension = "playback";
		if (NativeFileDialog::Open(kFileDialogToken, request))
		{
			g_filePickTrigger = trigger;
			g_filePickIndex = actionIndex;
		}
	}

	std::string LeafOf(const std::string& path)
	{
		const size_t slash = path.find_last_of("/\\");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	void DrawAnimationModal(TriggerType trigger)
	{
		BeginModalBody();

		Manager::Action& action = g_draft;

		// The move list comes from a parse of the dummy's script that used to happen as a
		// side effect of the old panel drawing itself. Ask for it explicitly, or the modal
		// opens empty for anyone who never visited that panel.
		ScrWindow::EnsureDummyScriptLoadedForUi();
		const std::vector<scrState*>& states = TrainingSide::Dummy().states;

		if (states.empty())
		{
			ImGui::TextWrapped("%s", L("The dummy's move list could not be read. This happens on the training character select screen.").c_str());
			ImGui::VerticalSpacing(4);
			if (ImGui::Button(L("Cancel").c_str())) { CancelModal(); }
			return;
		}

		ImGui::SetNextItemWidth(180.0f);
		ImGui::InputTextWithHint("##anim_filter", L("filter moves").c_str(),
			g_animFilter, IM_ARRAYSIZE(g_animFilter));
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Matches both the readable name and the script's own name, so \"5C\" and \"NmlAtk5C\" both work.").c_str());

		static const char* kCategories[] = { "Normals", "Specials", "Common", "All" };
		for (int i = 0; i < 4; ++i)
		{
			if (i) { ImGui::SameLine(); }
			ImGui::RadioButton(L(kCategories[i]).c_str(), &g_animCategory, i);
		}

		std::vector<int> shown;
		shown.reserve(states.size());
		for (int i = 0; i < static_cast<int>(states.size()); ++i)
		{
			if (!states[i]) { continue; }
			if (g_animCategory != 3)
			{
				const ScrStateNames::Category cat = ScrStateNames::Categorize(states[i]->name);
				const bool wanted =
					(g_animCategory == 0 && cat == ScrStateNames::Category::Normal) ||
					(g_animCategory == 1 && cat == ScrStateNames::Category::Special) ||
					(g_animCategory == 2 && cat == ScrStateNames::Category::Common);
				if (!wanted) { continue; }
			}
			if (!ScrStateNames::Matches(states[i]->name, g_animFilter)) { continue; }
			shown.push_back(i);
		}

		ImGui::BeginChild("##anim_list", ImVec2(190.0f, 260.0f), true);
		if (shown.empty())
		{
			ImGui::TextDisabled("%s", L("nothing matches").c_str());
		}
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(shown.size()));
		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
			{
				const int index = shown[row];
				ImGui::PushID(index);
				if (ImGui::Selectable(ScrStateNames::Display(states[index]->name).c_str(),
					g_animSelected == index))
				{
					g_animSelected = index;
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("%s", states[index]->name.c_str());
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		ImGui::SameLine();
		ImGui::BeginChild("##anim_details", ImVec2(0.0f, 260.0f), true);
		scrState* selected = (g_animSelected >= 0 && g_animSelected < static_cast<int>(states.size()))
			? states[g_animSelected] : nullptr;
		DrawStateDetails(selected);
		ImGui::EndChild();

		// What is already assigned, so building a random set is visible while you build it.
		if (!action.animations.empty())
		{
			std::string current;
			for (size_t i = 0; i < action.animations.size(); ++i)
			{
				if (i) { current += ", "; }
				current += ScrStateNames::Display(action.animations[i]->name);
				const int delay = i < action.animationDelays.size() ? action.animationDelays[i] : 0;
				if (delay) { current += "+" + std::to_string(delay) + "f"; }
			}
			ImGui::TextUnformatted(FormatText(L("Set: %s").c_str(), current.c_str()).c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton(L("Remove all").c_str()))
			{
				action.animations.clear();
				action.animationDelays.clear();
			}
		}

		// Lives here because it is what makes some of Naoto's own script states reachable at
		// all, so it is part of choosing an animation rather than a setting of its own.
		ImGui::Checkbox(L("Naoto: enable EN specials").c_str(), &action.naotoEnSpecials);
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Holds the flag Naoto's EN specials need while this action is armed. Does nothing for any other character.").c_str());

		ImGui::Text("%s", L("Delay").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90.0f);
		if (ImGui::InputInt("##anim_delay", &g_animDelay) && g_animDelay < 0)
		{
			g_animDelay = 0;
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Frames to wait after the trigger before the dummy acts. Stored per move, so each move of a random set can wait a different amount.").c_str());

		ImGui::VerticalSpacing(4);
		if (!selected) { ImGui::BeginDisabled(); }
		if (ImGui::Button(L("Use only this").c_str()) && selected)
		{
			action.animations.assign(1, selected);
			action.animationDelays.assign(1, g_animDelay);
		}
		ImGui::SameLine();
		if (ImGui::Button(L("Add to random set").c_str()) && selected)
		{
			action.animations.push_back(selected);
			action.animationDelays.push_back(g_animDelay);
		}
		if (!selected) { ImGui::EndDisabled(); }
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("With more than one move in the set the dummy picks one at random every time the trigger fires.").c_str());

		// Trying a move is how you find out it was the wrong one. Without this, seeing a move meant
		// assigning it to a trigger, closing the window, and arranging for that trigger to fire -
		// so the picker is where the button belongs, next to the move you are looking at.
		const bool canPlayNow = selected != nullptr && !TrainingSide::Dummy().IsCharDataNullPtr();
		if (!canPlayNow) { ImGui::BeginDisabled(); }
		if (ImGui::Button(L("Play now").c_str()) && canPlayNow)
		{
			UnlimitedPlaybackManager::Instance().PlayAnimationNow(selected);
		}
		if (!canPlayNow) { ImGui::EndDisabled(); }
		ImGui::SameLine();
		ImGui::HoverTooltipEvenDisabled(selected
			? L("Plays the highlighted move on the dummy right now. Nothing is assigned or saved - the delay above is ignored too, so this is just a look at the move.").c_str()
			: L("Pick a move on the left to play it.").c_str());

		EndModalBodyAndSeparate();
		ImGui::VerticalSpacing(4);
		const bool haveAny = !action.animations.empty();
		if (!haveAny) { ImGui::BeginDisabled(); }
		if (ImGui::Button(L("Done").c_str()) && haveAny)
		{
			CommitModal();
		}
		if (!haveAny) { ImGui::EndDisabled(); }
		ImGui::SameLine();
		if (ImGui::Button(L("Cancel").c_str()))
		{
			CancelModal();
		}
	}

	void DrawOpenModal()
	{
		if (g_modalTrigger >= UnlimitedPlaybackManager::Trigger_Count
			|| g_modalSource == Manager::Source_None)
		{
			return;
		}
		const TriggerType trigger = static_cast<TriggerType>(g_modalTrigger);
		const char* title = ModalTitleFor(g_modalSource);

		if (g_modalNeedsOpen)
		{
			ImGui::OpenPopup(title);
			g_modalNeedsOpen = false;
		}
		const ImVec2 startSize = ModalStartSizeFor(g_modalSource);
		BeginCenteredModalLayout(title, startSize.x, startSize.y);
		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoScrollbar))
		{
			// Dismissed by clicking away rather than through a button. That is a cancel,
			// and it has to clear the state, or the row could never be reconfigured: the
			// modal would be gone but still considered open.
			ClearModalState();
			return;
		}

		RememberModalSize(title);

		ContextMenuHeader(FormatText(L("%s - %s").c_str(),
			L(Manager::TriggerLabel(trigger)).c_str(),
			L(Manager::SourceLabel(g_modalSource)).c_str()).c_str());
		ImGui::VerticalSpacing(4);

		switch (g_modalSource)
		{
		case Manager::Source_Notation:  DrawNotationModal(trigger); break;
		case Manager::Source_Animation: DrawAnimationModal(trigger); break;
		case Manager::Source_CfSlot:
		case Manager::Source_Recording: DrawCfSlotModal(trigger); break;
		default: CancelModal(); break;
		}

		ImGui::EndPopup();
	}

	// The picker runs on a worker thread, so its answer arrives on some later frame. Claimed
	// here rather than inside the modal so an answer is not lost if the modal closed first.
	void PumpFileDialog()
	{
		NativeFileDialog::Result result;
		if (!NativeFileDialog::Consume(kFileDialogToken, &result))
		{
			return;
		}
		const int pending = g_filePickTrigger;
		const int index = g_filePickIndex;
		g_filePickTrigger = UnlimitedPlaybackManager::Trigger_Count;
		g_filePickIndex = -1;
		if (!result.accepted || pending >= UnlimitedPlaybackManager::Trigger_Count)
		{
			// Cancelled the picker: the trigger keeps whatever it had, which for a new row
			// is nothing at all.
			return;
		}

		const TriggerType trigger = static_cast<TriggerType>(pending);

		// A Playback Library file is unpacked into one recording action per entry, so a big
		// library built under the old system lands in a trigger in one go.
		std::string lower = result.path;
		for (char& ch : lower) { ch = static_cast<char>(tolower(static_cast<unsigned char>(ch))); }
		if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".upl") == 0)
		{
			int selectionMode = UnlimitedPlaybackManager::Selection_Random;
			const int added = Manager::Instance().AddLibraryFile(trigger, result.path, &selectionMode);
			if (added < 0)
			{
				UnlimitedPlaybackManager::Instance().PushToast(L("That library file could not be read."));
			}
			else
			{
				if (Manager::Instance().Get(trigger).actions.size() == static_cast<size_t>(added))
				{
					// The library was the whole trigger, so its picking order comes along.
					Manager::Instance().SetSelectionMode(trigger, selectionMode);
				}
				UnlimitedPlaybackManager::Instance().PushToast(
					FormatText(L("Added %d recording(s) from the library.").c_str(), added));
			}
			return;
		}

		// Committed straight from the picker's answer - there is no modal to save.
		Manager::Action action;
		action.source = Manager::Source_File;
		action.filePath = result.path;
		action.fileName = LeafOf(result.path);
		if (index >= 0)
		{
			Manager::Instance().Replace(trigger, static_cast<size_t>(index), action);
		}
		else
		{
			Manager::Instance().Add(trigger, action);
		}
	}

	// The "save this action as a playback file" picker's answer.
	void PumpSaveDialog()
	{
		NativeFileDialog::Result result;
		if (!NativeFileDialog::Consume(kSaveDialogToken, &result))
		{
			return;
		}
		const int trigger = g_saveTrigger;
		const int index = g_saveIndex;
		g_saveTrigger = UnlimitedPlaybackManager::Trigger_Count;
		g_saveIndex = -1;
		if (!result.accepted || result.path.empty() || trigger >= UnlimitedPlaybackManager::Trigger_Count)
		{
			return;
		}
		const Manager::Trigger& slot = Manager::Instance().Get(static_cast<TriggerType>(trigger));
		if (index < 0 || static_cast<size_t>(index) >= slot.actions.size())
		{
			return;
		}
		const Manager::Action& action = slot.actions[index];
		std::vector<char> raw;
		bool facingLeft = false;
		if (action.source == Manager::Source_Recording) { raw = action.recordingFrames; facingLeft = action.recordingFacingLeft; }
		else if (action.source == Manager::Source_Notation) { raw = action.notationFrames; facingLeft = false; }
		if (raw.empty())
		{
			UnlimitedPlaybackManager::Instance().PushToast(L("That action has no inputs to save."));
			return;
		}
		// Same on-disk layout as every other playback file: one facing byte, one byte per frame.
		const bool ok = PlaybackManager::save_playback_to_path(result.path,
			PlaybackManager::raw_to_trimmed(raw), facingLeft ? 1 : 0);
		UnlimitedPlaybackManager::Instance().PushToast(ok
			? FormatText(L("Saved to %s").c_str(), result.path.c_str())
			: FormatText(L("Could not write %s").c_str(), result.path.c_str()));
	}
}

void DummyActionsPanel::OnDummyScriptReloaded()
{
	Manager::Instance().InvalidateAnimations();
}

void DummyActionsPanel::Draw()
{
	PumpFileDialog();
	PumpSaveDialog();

	Manager& manager = Manager::Instance();
	const bool inTrainingMatch =
		g_gameVals.pGameMode && g_gameVals.pGameState &&
		(*g_gameVals.pGameMode == GameMode_Training) &&
		(*g_gameVals.pGameState == GameState_InMatch) &&
		(GetGameSceneStatus() >= GameSceneStatus_Running) &&
		!TrainingSide::Dummy().IsCharDataNullPtr();

	// Above the rows: it is the thing you reach for on an empty list, and it should not walk
	// down the page as rows are added.
	DrawAddActionButton();

	if (manager.AssignedCount() == 0)
	{
		ImGui::TextDisabled("%s", L("The dummy is not set to do anything yet.").c_str());
	}
	else
	{
		ImGui::VerticalSpacing(2);
	}
	for (TriggerType trigger : Manager::TriggerOrder())
	{
		DrawTriggerBlock(trigger, inTrainingMatch);
	}

	DrawOpenModal();
	DrawTriggerSettingsModal();
	DrawRowModals();
}
