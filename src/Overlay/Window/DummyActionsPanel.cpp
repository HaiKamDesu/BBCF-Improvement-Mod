#include "DummyActionsPanel.h"

#include "Core/HotkeyManager.h"
#include "Core/logger.h"
#include "Core/Localization.h"
#include "Core/NativeFileDialog.h"
#include "Core/interfaces.h"
#include "Core/utils.h"
#include "Game/Playbacks/DummyActionManager.h"
#include "Game/Playbacks/UnlimitedPlaybackManager.h"
#include "Game/Scr/ScrStateNames.h"
#include "Game/TasManager.h"
#include "Game/gamestates.h"
#include "Overlay/Window/ScrWindow.h"
#include "Overlay/Window/UnlimitedPlaybackWindow.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"

#include <Windows.h>

#include <cmath>
#include <map>

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

	// Which trigger's source modal is open, and for which source. Trigger_Count means none.
	//
	// Configuration happens on a DRAFT copy of the action, not on the live one. Editing in
	// place meant Cancel had to guess what to undo: a brand new row appeared the moment you
	// picked a source and vanished again if you backed out, and changing the source of an
	// existing row destroyed it if you then cancelled. The draft is committed on Done and
	// thrown away on Cancel, so the list only ever changes when you say so.
	int g_modalTrigger = UnlimitedPlaybackManager::Trigger_Count;
	Source g_modalSource = Manager::Source_None;
	bool g_modalNeedsOpen = false;
	Manager::Action g_draft;

	// Cancel has to undo more than the draft.
	//
	// The library editor works on the trigger's live library on purpose - it is the same
	// panel the Playback Library window uses, and it has to be, or the two drift apart. So
	// the library is copied on the way in and put back if you cancel. Saving or loading a
	// FILE is not undone, because that already happened on disk.
	UnlimitedPlaybackManager::PlaybackLibrary g_libraryBackup;
	bool g_libraryBackupValid = false;

	// The trigger settings modal edits its TriggerConfig live too, for the same reason: the
	// firing code reads it every frame.
	UnlimitedPlaybackManager::TriggerConfig g_settingsBackup;
	bool g_settingsBackupValid = false;
	float g_settingsLoopSetup = 0.0f;
	float g_settingsLoopEnding = 0.0f;
	bool g_settingsLoopRestartLab = false;
	int g_settingsLoopRestartMode = 0;

	// Per-modal scratch, only meaningful while that modal is up.
	char g_notationText[256] = "";
	std::string g_notationError;
	int g_notationFrameCount = 0;
	int g_animSelected = -1;
	int g_animCategory = 3;   // 0 normals, 1 specials, 2 common, 3 all
	int g_animDelay = 0;
	char g_animFilter[64] = "";
	int g_pendingCfSlot = 1;
	int g_librarySelected = -1;
	int g_librarySelectionMode = UnlimitedPlaybackManager::Selection_Random;

	// The trigger-settings modal is separate from the source modals: it configures WHEN the
	// trigger fires, not WHAT it plays, so it is reachable whatever the source is.
	int g_settingsTrigger = UnlimitedPlaybackManager::Trigger_Count;
	bool g_settingsNeedsOpen = false;

	// Trigger the pending file pick belongs to, since the picker answers on a later frame.
	int g_filePickTrigger = UnlimitedPlaybackManager::Trigger_Count;

	void BeginFilePick(TriggerType trigger);

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
		case Manager::Source_Library:   return ImVec2(607.0f, 504.0f);
		case Manager::Source_Notation:  return ImVec2(423.0f, 160.0f);
		case Manager::Source_CfSlot:    return ImVec2(305.0f, 146.0f);
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

	void OpenModal(TriggerType trigger, Source source)
	{
		g_modalTrigger = trigger;
		g_modalSource = source;
		g_modalNeedsOpen = true;
		RequestModalFit();

		// The draft starts from the live action when the source is unchanged, so reopening a
		// configured row shows what is set; changing source starts clean, since none of the
		// old payload means anything to the new one.
		const Manager::Action& live = Manager::Instance().Get(trigger);
		if (live.source == source)
		{
			g_draft = live;
		}
		else
		{
			g_draft = Manager::Action{};
			g_draft.source = source;
		}

		// A library is edited in place, so take a copy to restore if this is cancelled.
		g_libraryBackupValid = false;
		if (source == Manager::Source_Library)
		{
			if (const UnlimitedPlaybackManager::PlaybackLibrary* live =
					UnlimitedPlaybackManager::Instance().MutableLibraryForTrigger(trigger))
			{
				g_libraryBackup = *live;
				g_libraryBackupValid = true;
			}
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
			g_pendingCfSlot = action.cfSlot;
			break;
		case Manager::Source_Library:
			g_librarySelected = -1;
			g_librarySelectionMode = action.selectionMode;
			break;
		default:
			break;
		}
	}

	void ClearModalState()
	{
		g_modalTrigger = UnlimitedPlaybackManager::Trigger_Count;
		g_modalSource = Manager::Source_None;
		g_draft = Manager::Action{};
		g_libraryBackup = UnlimitedPlaybackManager::PlaybackLibrary{};
		g_libraryBackupValid = false;
	}

	// Commits the draft onto the live action. The only path by which the trigger list
	// changes.
	void CommitModal()
	{
		if (g_modalTrigger < UnlimitedPlaybackManager::Trigger_Count)
		{
			Manager::Instance().Set(static_cast<TriggerType>(g_modalTrigger), g_draft);
		}
		ClearModalState();
		ImGui::CloseCurrentPopup();
	}

	// Throws the draft away. The live action is left exactly as it was, whether that is a
	// fully configured row or no row at all.
	// Puts the library back the way it was found. Without this, entries added, deleted,
	// reordered or unticked while the modal was open stayed applied after Cancel.
	void RevertLibraryIfBackedUp()
	{
		if (!g_libraryBackupValid || g_modalTrigger >= UnlimitedPlaybackManager::Trigger_Count)
		{
			return;
		}
		UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();
		if (UnlimitedPlaybackManager::PlaybackLibrary* live =
				playback.MutableLibraryForTrigger(static_cast<TriggerType>(g_modalTrigger)))
		{
			*live = g_libraryBackup;
		}
	}

	void CancelModal()
	{
		RevertLibraryIfBackedUp();
		ClearModalState();
		ImGui::CloseCurrentPopup();
	}

	const char* ModalTitleFor(Source source)
	{
		switch (source)
		{
		case Manager::Source_Notation:  return "Input Notation##dummyaction";
		case Manager::Source_Library:   return "Playback Library##dummyaction";
		case Manager::Source_File:      return "From File##dummyaction";
		case Manager::Source_Animation: return "Animation##dummyaction";
		case Manager::Source_CfSlot:    return "CF Recording Slot##dummyaction";
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
			// The three conditions the game counts toward, so the only three that can be
			// anticipated: blockstun counts down, and wakeup and tech count up to the frame
			// the dummy becomes actionable.
			const bool canFireEarly = (trigger == UnlimitedPlaybackManager::Trigger_Gap
				|| trigger == UnlimitedPlaybackManager::Trigger_ThrowTech
				|| trigger == UnlimitedPlaybackManager::Trigger_Wakeup);

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
				? L("Frames between the condition and the action. Positive waits, for making a reversal late on purpose. NEGATIVE fires early, which is how a reversal's motion gets delivered before the dummy is actionable so the attack lands on the first frame it can - about the length of the motion, so -3 for a 623C. Only wakeup, the block gap and throw tech can go negative, because those are the conditions the game counts toward and can therefore be anticipated.").c_str()
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
				g_settingsBackupValid = false;
			}
			g_settingsTrigger = UnlimitedPlaybackManager::Trigger_Count;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Cancel puts these settings back. A hotkey you rebound stays rebound - that is a global binding, not part of this trigger.").c_str());
		ImGui::EndPopup();
	}

	// ---- source pickers, shared by "+ Add Action" and a row's "Change..." ----

	void DrawSourceMenuItems(TriggerType trigger)
	{
		ContextMenuHeader(L("Take its inputs from").c_str());
		static const Source kSources[] = {
			Manager::Source_Notation, Manager::Source_Library, Manager::Source_File,
			Manager::Source_Animation, Manager::Source_CfSlot, Manager::Source_Burst,
		};
		for (Source source : kSources)
		{
			// Bursting only means anything as an answer to being hit.
			if (source == Manager::Source_Burst
				&& trigger != UnlimitedPlaybackManager::Trigger_OnHit)
			{
				continue;
			}
			if (ImGui::MenuItem(L(Manager::SourceLabel(source)).c_str()))
			{
				// Nothing is written to the trigger yet - the row appears when the choice is
				// saved, not when a source is picked.
				if (source == Manager::Source_Burst)
				{
					// Nothing to configure, so no modal - the row appears straight away.
					Manager::Action burst;
					burst.source = Manager::Source_Burst;
					Manager::Instance().Set(trigger, burst);
				}
				else if (source == Manager::Source_File)
				{
					// A file has exactly one thing to choose, so a window to host a "pick a
					// file" button is a step for nothing. Straight to the OS picker; the
					// answer commits the row when it arrives.
					BeginFilePick(trigger);
				}
				else
				{
					OpenModal(trigger, source);
				}
			}
		}
	}

	void DrawAddActionButton()
	{
		Manager& manager = Manager::Instance();
		const bool anyConfigured = manager.AssignedCount() > 0;

		if (manager.HasFreeTrigger())
		{
			// The Selectable is drawn first and the label painted back over it, so its
			// highlight covers the plus as well. Drawing the plus first and the Selectable
			// after left the lit area starting after the plus, cutting it in half.
			if (DrawFaintSelectable("##dummy_add_action_btn", "+", L("Add Action").c_str(),
					ImVec4(0.35f, 0.80f, 0.35f, 1.0f)))
			{
				ImGui::OpenPopup("##dummy_add_action");
			}
		}

		// Only worth offering when there is something to clear. Behind a confirmation
		// because it throws away every row and their libraries at once.
		if (anyConfigured)
		{
			if (manager.HasFreeTrigger())
			{
				ImGui::SameLine(0.0f, 12.0f);
			}
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
				L("This removes every trigger and the libraries they were using.").c_str());
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

		if (!manager.HasFreeTrigger())
		{
			// Every trigger is armed, so there is nothing left to add.
			return;
		}

		if (ImGui::BeginPopup("##dummy_add_action"))
		{
			ContextMenuHeader(L("Trigger type").c_str());
			for (TriggerType trigger : Manager::TriggerOrder())
			{
				if (manager.HasAction(trigger))
				{
					continue;
				}
				if (ImGui::BeginMenu(L(Manager::TriggerLabel(trigger)).c_str()))
				{
					DrawSourceMenuItems(trigger);
					ImGui::EndMenu();
				}
			}
			ImGui::EndPopup();
		}
	}

	void DrawActionRow(TriggerType trigger)
	{
		Manager& manager = Manager::Instance();
		const Manager::Action& action = manager.Get(trigger);

		ImGui::PushID(static_cast<int>(trigger));

		// Two lines rather than three columns. The summary can be anything from "623C" to a
		// list of animations with per-move delays, and squeezing that between a label and a
		// row of buttons meant it either collided with them or had nowhere to wrap. Giving
		// it its own line under the trigger name lets it wrap to the full width, and keeps
		// the buttons on a fixed baseline whatever the summary says.
		// Configures the trigger itself - when it fires - as opposed to Set.../Change...,
		// which are about what it plays.
		if (ImGui::SmallButton("*"))
		{
			g_settingsTrigger = trigger;
			g_settingsNeedsOpen = true;
			RequestModalFit();

			// Copied so Cancel can put it back: the firing code reads this config every
			// frame, so the modal has nowhere to edit but the live object.
			UnlimitedPlaybackManager& snapshotOf = UnlimitedPlaybackManager::Instance();
			g_settingsBackup = snapshotOf.GetTrigger(trigger);
			g_settingsLoopSetup = snapshotOf.GetLoopSetupSeconds();
			g_settingsLoopEnding = snapshotOf.GetLoopEndingSeconds();
			g_settingsLoopRestartLab = snapshotOf.GetLoopRestartLabState();
			g_settingsLoopRestartMode = snapshotOf.GetLoopRestartMode();
			g_settingsBackupValid = true;
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", L("Configure when this trigger fires").c_str());
		}

		ImGui::SameLine();

		// Flashes green for half a second when the trigger goes off, so you can see THAT it
		// fired and exactly when - a toast tells you the same thing but scrolls away and
		// does not say which row it came from.
		UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();
		const unsigned long long firedMs = playback.GetTriggerLastFiredMs(trigger);
		const bool justFired = firedMs != 0 && (GetTickCount64() - firedMs) < 500;
		if (justFired)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.92f, 0.45f, 1.0f));
		}
		ImGui::TextUnformatted(L(Manager::TriggerLabel(trigger)).c_str());
		if (justFired)
		{
			ImGui::PopStyleColor();
		}

		// The delay, when there is one. Worth stating on the row because it changes what the
		// trigger does and is otherwise two clicks away inside the gear.
		const int delayFrames = playback.GetTrigger(trigger).delayFrames;
		if (delayFrames != 0)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", delayFrames > 0
				? FormatText(L("%df delay").c_str(), delayFrames).c_str()
				: FormatText(L("%df early").c_str(), -delayFrames).c_str());
		}

		// The loop is the one trigger with a state worth showing at rest: it either is or is
		// not going, and nothing else on the page says so.
		if (trigger == UnlimitedPlaybackManager::Trigger_OnLoop)
		{
			ImGui::SameLine();
			if (playback.IsLoopActive())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.92f, 0.45f, 1.0f));
				ImGui::TextUnformatted(L("(looping)").c_str());
				ImGui::PopStyleColor();
			}
			else
			{
				ImGui::TextDisabled("%s", L("(stopped)").c_str());
			}
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", FormatText(L("Runs %s.").c_str(),
				L(Manager::TriggerWhen(trigger)).c_str()).c_str());
		}

		// Buttons pinned to the right of the first line.
		const float buttons = 190.0f;
		const float buttonsAt = ImGui::GetContentRegionMax().x - buttons;
		if (buttonsAt > 160.0f) { ImGui::SameLine(buttonsAt); } else { ImGui::SameLine(); }

		if (ImGui::SmallButton(L("Edit").c_str()))
		{
			OpenModal(trigger, action.source);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", L("Reopen this action's configuration").c_str());
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(L("Change Source").c_str()))
		{
			ImGui::OpenPopup("##row_change");
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", L("Take this trigger's inputs from somewhere else").c_str());
		}
		if (ImGui::BeginPopup("##row_change"))
		{
			DrawSourceMenuItems(trigger);
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.35f, 0.35f, 1.0f));
		const bool cleared = ImGui::SmallButton("X");
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", L("Clear").c_str());
		}

		// Second line: what it will do, in the same quiet grey and smaller type the context
		// menu captions use, indented under the trigger it belongs to.
		ImGui::Indent(20.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.58f, 0.58f, 1.0f));
		ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 0.86f);
		std::string detail = L(Manager::SourceLabel(action.source));
		const std::string summary = manager.Summary(trigger);
		if (!summary.empty())
		{
			detail += "  -  " + summary;
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
			detail += "  -  ";
			detail += FormatText(L("press %s").c_str(), key.empty() ? L("nothing").c_str() : key.c_str());
		}
		// Wrapped short of the buttons above, so a long summary never runs under them.
		ImGui::PushTextWrapPos((std::max)(200.0f, ImGui::GetContentRegionMax().x - 30.0f));
		if (manager.IsRunnable(trigger))
		{
			ImGui::TextUnformatted(detail.c_str());
		}
		else
		{
			ImGui::PopStyleColor();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.65f, 0.35f, 1.0f));
			ImGui::TextUnformatted(detail.c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::PopFont();
		ImGui::PopStyleColor();
		ImGui::Unindent(20.0f);

		// A faint rule under each row, so a list of them reads as rows rather than a wall.
		ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
		ImGui::Separator();
		ImGui::PopStyleColor();

		if (cleared)
		{
			manager.Clear(trigger);
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
			ImGui::RadioButton(FormatText(L("Slot %d").c_str(), slot).c_str(), &g_pendingCfSlot, slot);
		}
		EndModalBodyAndSeparate();
		if (ImGui::Button(L("Done").c_str()))
		{
			g_draft.cfSlot = g_pendingCfSlot;
			CommitModal();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("Cancel").c_str()))
		{
			CancelModal();
		}
	}

	void BeginFilePick(TriggerType trigger)
	{
		if (NativeFileDialog::IsOpen())
		{
			return;
		}
		NativeFileDialog::Request request;
		request.title = L("Choose a playback file");
		request.filters.push_back({ L("Playback file (*.bin)"), "*.bin" });
		if (NativeFileDialog::Open(kFileDialogToken, request))
		{
			g_filePickTrigger = trigger;
		}
	}


	// Libraries are the files Unlimited Playback already writes: self-contained, with every
	// entry's playback bytes embedded, so one of them is portable on its own.
	// Win32 rather than <filesystem>: this project builds at the toolset's default language
	// level, where <filesystem> is not available, and the rest of the mod enumerates this way.
	std::vector<std::string> ListLibraryFiles()
	{
		std::vector<std::string> found;
		const std::string folder = UnlimitedPlaybackManager::Instance().GetLibraryFolderPublic();
		if (folder.empty())
		{
			return found;
		}
		WIN32_FIND_DATAA findData = {};
		const HANDLE handle = FindFirstFileA((folder + "\\*").c_str(), &findData);
		if (handle == INVALID_HANDLE_VALUE)
		{
			return found;
		}
		do
		{
			if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { continue; }
			found.push_back(folder + "\\" + findData.cFileName);
		} while (FindNextFileA(handle, &findData));
		FindClose(handle);
		std::sort(found.begin(), found.end());
		return found;
	}

	// Leaf name without its extension, for showing a path as a library name.
	std::string StemOf(const std::string& path)
	{
		const size_t slash = path.find_last_of("/\\");
		std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
		const size_t dot = leaf.find_last_of('.');
		return dot == std::string::npos ? leaf : leaf.substr(0, dot);
	}

	std::string LeafOf(const std::string& path)
	{
		const size_t slash = path.find_last_of("/\\");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	void DrawLibraryModal(TriggerType trigger)
	{
		Manager::Action& action = g_draft;
		UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();

		// This trigger's own library, loaded from whatever file was last saved or loaded into
		// it. There is no library picker here on purpose: libraries are files, and loading or
		// saving one is what the buttons in the panel below already do.
		// This trigger's OWN library, not the shared working set. Pointing at the working
		// set whenever no file had been picked yet meant configuring one trigger edited
		// every other trigger's list at the same time.
		playback.SetEditTarget(playback.MutableLibraryForTrigger(trigger));

		BeginModalBody();

		// Picking order first: it is the one thing that is about this trigger rather than
		// about the collection, so it reads as the setting and the list as its contents.
		DrawPlaybackPickingOrder();
		ImGui::VerticalSpacing(4);
		ImGui::Separator();
		ImGui::VerticalSpacing(4);

		// The real library UI - same list, context menus, ordering, enable checkboxes and
		// add buttons as the Playback Library window.
		// A named height rather than "fill": the modal fits itself to its content, and
		// content that expands to fill whatever it is given would never settle on a size.
		DrawPlaybackLibraryEntriesAndAdd(300.0f);

		EndModalBodyAndSeparate();

		// At modal scope, outside the scrolling body: these are the popups the buttons above
		// raise, and a nested modal has to be begun at this level to be usable.
		DrawPlaybackLibraryPopups();

		action.selectionMode = playback.GetSelectionMode();
		if (const UnlimitedPlaybackManager::PlaybackLibrary* edited = playback.GetEditTarget())
		{
			// Remember whatever the panel loaded or saved, so the trigger keeps pointing at
			// the file the user just worked with.
			action.libraryPath = edited->path;
			action.libraryName = StemOf(edited->path);
		}

		// Back to the working set before anything else draws, so the Playback Library window
		// is never left editing a trigger's library behind the user's back.
		playback.SetEditTarget(nullptr);

		if (ImGui::Button(L("Done").c_str()))
		{
			CommitModal();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("Cancel").c_str()))
		{
			CancelModal();
		}
	}

	void DrawAnimationModal(TriggerType trigger)
	{
		BeginModalBody();

		Manager::Action& action = g_draft;

		// The move list comes from a parse of the dummy's script that used to happen as a
		// side effect of the old panel drawing itself. Ask for it explicitly, or the modal
		// opens empty for anyone who never visited that panel.
		ScrWindow::EnsureDummyScriptLoadedForUi();
		const std::vector<scrState*>& states = g_interfaces.player2.states;

		if (states.empty())
		{
			ImGui::TextWrapped("%s", L("The dummy's move list could not be read. This happens on the training character select screen, and in mirror matches.").c_str());
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
			// Dismissed by clicking away rather than through a button. That is a cancel, so
			// it reverts exactly what Cancel reverts - and it has to clear the state, or the
			// row could never be reconfigured: the modal would be gone but still considered
			// open.
			RevertLibraryIfBackedUp();
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
		case Manager::Source_Library:   DrawLibraryModal(trigger); break;
		case Manager::Source_Animation: DrawAnimationModal(trigger); break;
		case Manager::Source_CfSlot:    DrawCfSlotModal(trigger); break;
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
		g_filePickTrigger = UnlimitedPlaybackManager::Trigger_Count;
		if (!result.accepted || pending >= UnlimitedPlaybackManager::Trigger_Count)
		{
			// Cancelled the picker: the trigger keeps whatever it had, which for a new row
			// is nothing at all.
			return;
		}

		// Committed straight from the picker's answer - there is no modal to save.
		Manager::Action action;
		action.source = Manager::Source_File;
		action.filePath = result.path;
		action.fileName = LeafOf(result.path);
		Manager::Instance().Set(static_cast<TriggerType>(pending), action);
	}
}

void DummyActionsPanel::OnDummyScriptReloaded()
{
	Manager::Instance().InvalidateAnimations();
}

void DummyActionsPanel::Draw()
{
	PumpFileDialog();

	Manager& manager = Manager::Instance();

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
		if (manager.HasAction(trigger))
		{
			DrawActionRow(trigger);
		}
	}

	DrawOpenModal();
	DrawTriggerSettingsModal();
}
