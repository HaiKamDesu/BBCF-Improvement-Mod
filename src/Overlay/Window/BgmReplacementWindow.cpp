#include "BgmReplacementWindow.h"

#include "Audio/BgmReplacementManager.h"
#include "Audio/MusicManager.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/NativeFileDialog.h"
#include "Overlay/imgui_utils.h"

#include <algorithm>
#include <vector>

namespace
{
	const char* kPickerToken = "BgmReplacementWindow";

	std::string FormatDb(float value)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%+.1f dB", value);
		return buf;
	}

	// One colour per state, so a glance down the list tells you what is going on without
	// reading any of it.
	ImVec4 StateColor(BgmReplacementState state)
	{
		switch (state)
		{
		case BgmReplacementState::Active:      return ImVec4(0.35f, 0.90f, 0.65f, 1.0f); // green
		case BgmReplacementState::Converting:  return ImVec4(1.00f, 0.85f, 0.35f, 1.0f); // amber
		case BgmReplacementState::Missing:     return ImVec4(1.00f, 0.60f, 0.30f, 1.0f); // orange
		case BgmReplacementState::Failed:      return ImVec4(1.00f, 0.45f, 0.45f, 1.0f); // red
		case BgmReplacementState::Unavailable: return ImVec4(0.55f, 0.55f, 0.55f, 1.0f); // grey
		default:                               return ImVec4(0.70f, 0.72f, 0.78f, 1.0f);
		}
	}

	const char* StateGlyph(BgmReplacementState state)
	{
		switch (state)
		{
		case BgmReplacementState::Active:      return "[*]";
		case BgmReplacementState::Converting:  return "[~]";
		case BgmReplacementState::Missing:     return "[!]";
		case BgmReplacementState::Failed:      return "[x]";
		case BgmReplacementState::Unavailable: return "[-]";
		default:                               return "[ ]";
		}
	}

	std::string ToLower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), ::tolower);
		return s;
	}

	// Category order for the filter dropdown, built from what's actually present.
	std::vector<std::string> CollectCategories()
	{
		std::vector<std::string> cats;
		for (const auto& t : GetBgmReplacements().GetTracks())
		{
			if (std::find(cats.begin(), cats.end(), t.category) == cats.end())
				cats.push_back(t.category);
		}
		return cats;
	}
}

void BgmReplacementWindow::BeforeDraw()
{
	ImGui::SetNextWindowSize(ImVec2(620, 620), ImGuiCond_FirstUseEver);
}

void BgmReplacementWindow::Draw()
{
	BgmReplacementManager& mgr = GetBgmReplacements();
	PollRestartAfterApply();

	if (!mgr.IsInitialized())
	{
		ImGui::TextColoredWrapped(ImVec4(1, 0.45f, 0.45f, 1), "%s",
			L("Music replacement is unavailable - the game's audio table could not be read.").c_str());
		return;
	}

	PollPicker();

	if (m_categories.empty())
		m_categories = CollectCategories();

	DrawSummary();
	ImGui::Separator();
	DrawFilters();
	DrawTrackList();
}

void BgmReplacementWindow::DrawSummary()
{
	BgmReplacementManager& mgr = GetBgmReplacements();

	ImGui::TextWrapped("%s", L("Swap any song in the game for one of your own. Nothing in your "
		"game folder is overwritten, and every swap can be undone.").c_str());
	ImGui::Spacing();

	const int active = mgr.GetActiveCount();
	if (active > 0)
		ImGui::TextColored(StateColor(BgmReplacementState::Active), "%s: %d",
			L("Songs replaced").c_str(), active);
	else
		ImGui::TextDisabled("%s", L("No songs replaced yet.").c_str());

	if (mgr.GetAssignedCount() > 0)
	{
		ImGui::SameLine();
		if (ImGui::Button(L("Undo everything").c_str()))
		{
			mgr.UnassignAll();
			m_lastMessage = L("Every song is back to normal.");
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltipWrapped(L("Puts every song back to the original and deletes the "
				"converted files.").c_str());
	}

	// While anything is converting, say exactly what and how much is left.
	if (mgr.IsBusy())
	{
		ImGui::Spacing();
		const int converting = mgr.GetConvertingIndex();
		const BgmReplaceableTrack* track = converting >= 0 ? mgr.FindTrack(converting) : nullptr;
		ImGui::TextColored(StateColor(BgmReplacementState::Converting), "%s", track
			? (L("Converting for") + " " + track->displayName + "...").c_str()
			: L("Converting...").c_str());
		ImGui::ProgressBar(mgr.GetBatchProgress(), ImVec2(-1, 0), "");
		const int queued = mgr.GetQueueLength();
		if (queued > 1)
			ImGui::TextDisabled("%s: %d", L("Still to do").c_str(), queued - 1);
	}

	if (!m_lastMessage.empty())
	{
		ImGui::Spacing();
		ImGui::TextDisabledWrapped("%s", m_lastMessage.c_str());
	}

	// The game reads a song's filename only when it loads it, and it loads a match's music
	// - Astral Heat included - when the match starts. A swap made mid-match therefore does
	// nothing until the next one. This is the single most confusing thing about the
	// feature, so it gets a coloured line rather than grey small print.
	ImGui::Spacing();
	ImGui::TextColoredWrapped(StateColor(BgmReplacementState::Converting), "%s",
		L("Swaps take effect when a match STARTS.").c_str());
	ImGui::TextDisabledWrapped("%s", L("If you are in a match right now, leave and start a new one. "
		"This includes Astral Finish music, which is loaded up front with the rest of the "
		"match, not when the Astral happens.").c_str());
}

void BgmReplacementWindow::DrawFilters()
{
	ImGui::PushItemWidth(180);
	ImGui::InputText(L("Search").c_str(), m_search, sizeof(m_search));
	ImGui::PopItemWidth();

	ImGui::SameLine();
	ImGui::Checkbox(L("Only replaced").c_str(), &m_onlyReplaced);

	const std::vector<std::string>& cats = m_categories;
	std::string preview = (m_categoryFilter == 0 || m_categoryFilter > (int)cats.size())
		? L("All groups") : cats[m_categoryFilter - 1];

	ImGui::SameLine();
	ImGui::PushItemWidth(120);
	if (ImGui::BeginCombo("##BgmCategory", preview.c_str()))
	{
		if (ImGui::Selectable(L("All groups").c_str(), m_categoryFilter == 0))
			m_categoryFilter = 0;
		for (int i = 0; i < (int)cats.size(); ++i)
		{
			if (ImGui::Selectable(cats[i].c_str(), m_categoryFilter == i + 1))
				m_categoryFilter = i + 1;
		}
		ImGui::EndCombo();
	}
	ImGui::PopItemWidth();
	ImGui::Spacing();
}

void BgmReplacementWindow::DrawTrackList()
{
	BgmReplacementManager& mgr = GetBgmReplacements();

	const std::vector<std::string>& cats = m_categories;
	const std::string wantedCat = (m_categoryFilter > 0 && m_categoryFilter <= (int)cats.size())
		? cats[m_categoryFilter - 1] : std::string();

	const std::string needle = ToLower(m_search);

	float height = ImGui::GetContentRegionAvail().y;
	if (height < 120.0f) height = 120.0f;
	ImGui::BeginChild("##BgmReplaceList", ImVec2(0, height), true);

	std::string lastCategory;
	int shown = 0;
	for (const auto& track : mgr.GetTracks())
	{
		if (!wantedCat.empty() && track.category != wantedCat)
			continue;
		if (m_onlyReplaced && mgr.GetState(track.tableIndex) == BgmReplacementState::Original)
			continue;
		if (!needle.empty())
		{
			const std::string hay = ToLower(track.displayName + " " + track.baseName);
			if (hay.find(needle) == std::string::npos)
				continue;
		}

		if (track.category != lastCategory)
		{
			lastCategory = track.category;
			if (shown > 0) ImGui::Spacing();
			ImGui::TextDisabled("%s", lastCategory.c_str());
			ImGui::Separator();
		}

		DrawTrackRow(track.tableIndex);
		++shown;
	}

	if (shown == 0)
		ImGui::TextDisabled("%s", L("Nothing matches that search.").c_str());

	ImGui::EndChild();
}

void BgmReplacementWindow::DrawTrackRow(int tableIndex)
{
	BgmReplacementManager& mgr = GetBgmReplacements();
	const BgmReplaceableTrack* track = mgr.FindTrack(tableIndex);
	if (!track)
		return;

	const BgmReplacementState state = mgr.GetState(tableIndex);
	const ImVec4 color = StateColor(state);

	ImGui::PushID(tableIndex);

	ImGui::TextColored(color, "%s", StateGlyph(state));
	ImGui::SameLine();
	ImGui::TextColored(color, "%s", track->displayName.c_str());

	// Second line: what is actually going on with this track, in plain words.
	ImGui::Indent(22.0f);
	switch (state)
	{
	case BgmReplacementState::Original:
		if (m_pendingPickIndex == tableIndex)
			ImGui::TextColored(StateColor(BgmReplacementState::Converting), "%s",
				L("Choosing a file...").c_str());
		else
			ImGui::TextDisabled("%s", L("Original game music").c_str());
		break;
	case BgmReplacementState::Converting:
		ImGui::TextColored(color, "%s", L("Converting your song...").c_str());
		break;
	case BgmReplacementState::Active:
	{
		// "Now playing your song" was not always true, and that is the whole reason people
		// swap a track and ask why nothing changed. A replacement is written correctly long
		// before the game next loads that file, so say which it is.
		const bool live = mgr.IsLiveNow(tableIndex);
		ImGui::TextColoredWrapped(color, "%s: %s",
			(live ? L("Now playing your song") : L("Your song is set")).c_str(),
			mgr.GetSourceName(tableIndex).c_str());

		const char* when = nullptr;
		switch (track->timing)
		{
		case BgmLoadTiming::NextMatch:
			when = live ? "Loads with each match." : "Starts playing from your next match.";
			break;
		case BgmLoadTiming::NextScreen:
			when = live ? "Loads each time this screen comes up."
			            : "Starts playing the next time this screen's music loads.";
			break;
		case BgmLoadTiming::GameRestart:
			when = live ? "The game loads this one at startup, so it is already in use."
			            : "The game only loads this one while it starts up, so restart the "
			              "game to hear it. The swap itself is saved and done.";
			break;
		}
		if (when != nullptr)
		{
			if (live)
				ImGui::TextDisabledWrapped("%s", L(when).c_str());
			else
				ImGui::TextColoredWrapped(StateColor(BgmReplacementState::Converting), "%s",
					L(when).c_str());
		}
		break;
	}
	case BgmReplacementState::Missing:
	case BgmReplacementState::Failed:
		ImGui::TextColoredWrapped(color, "%s", mgr.GetError(tableIndex).c_str());
		break;
	case BgmReplacementState::Unavailable:
		ImGui::TextDisabledWrapped("%s", track->everUsed
			? L("This track isn't installed on your copy of the game.").c_str()
			: L("The game never plays this file, so it can't be replaced.").c_str());
		break;
	}

	// Where a track is heard is not always obvious - Astral Heat music in particular only
	// uses one of three sets, chosen by a sound option - so say it right on the row.
	if (!track->usageNote.empty())
		ImGui::TextDisabledWrapped("%s", L(track->usageNote).c_str());

	// Buttons. Preview only works inside a match, where the audio engine is live, so it is
	// disabled rather than silently doing nothing.
	const bool inMatch = MusicManager::GetInstance().IsInMatch();

	auto previewButton = [&](const char* label, const std::string& relPath, const char* tip)
	{
		const bool can = inMatch && !relPath.empty();
		if (!can) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
		if (ImGui::Button(label) && can)
			MusicManager::GetInstance().PreviewPac(relPath, track->baseName);
		if (!can) ImGui::PopStyleVar();
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltipWrapped(!inMatch
				? L("Start a match to hear previews.").c_str()
				: (relPath.empty() ? L("Nothing to preview for this track.").c_str() : tip));
		}
	};

	if (state == BgmReplacementState::Unavailable)
	{
		ImGui::Unindent(22.0f);
		ImGui::PopID();
		return;
	}

	previewButton(L("Play original").c_str(), mgr.GetOriginalPreviewPath(tableIndex),
		L("Hear the game's own version of this track.").c_str());

	if (state == BgmReplacementState::Active)
	{
		ImGui::SameLine();
		previewButton(L("Play yours").c_str(), mgr.GetReplacementPreviewPath(tableIndex),
			L("Hear the song you put in its place.").c_str());
	}

	ImGui::SameLine();
	if (state == BgmReplacementState::Original)
	{
		if (ImGui::Button(L("Replace...").c_str()))
			OpenPickerFor(tableIndex);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltipWrapped(L("Pick a song to play instead of this track.").c_str());
	}
	else if (state == BgmReplacementState::Converting)
	{
		ImGui::TextDisabled("%s", L("please wait").c_str());
	}
	else
	{
		if (ImGui::Button(L("Change...").c_str()))
			OpenPickerFor(tableIndex);

		if (state == BgmReplacementState::Missing || state == BgmReplacementState::Failed)
		{
			ImGui::SameLine();
			if (ImGui::Button(L("Rebuild").c_str()))
			{
				mgr.Retry(tableIndex);
				m_lastMessage = L("Rebuilding") + " " + track->displayName + "...";
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltipWrapped(L("Convert the same MP3 again.").c_str());
		}

		ImGui::SameLine();
		if (ImGui::Button(L("Undo").c_str()))
		{
			mgr.Unassign(tableIndex);
			m_lastMessage = track->displayName + " " + L("is back to normal.");
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltipWrapped(L("Put the original back and delete the converted file.").c_str());
	}

	if (state == BgmReplacementState::Active || state == BgmReplacementState::Missing ||
		state == BgmReplacementState::Failed)
	{
		DrawGainRow(tableIndex);
	}

	ImGui::Unindent(22.0f);
	ImGui::Spacing();
	ImGui::PopID();
}

// Gain is baked into the converted .pac: the game plays that file through its own XACT
// engine and the mod has no volume control there. So changing the slider costs nothing
// until Apply, which re-converts.
void BgmReplacementWindow::DrawGainRow(int tableIndex)
{
	BgmReplacementManager& mgr = GetBgmReplacements();
	const float saved = mgr.GetGainDb(tableIndex);

	// The draft is per-row. It is seeded from the saved value and left alone after that,
	// so dragging is not fought by the other rows; it re-seeds only when the saved value
	// itself moves underneath it, which is what an Apply or a new file does.
	GainDraft& draft = m_gainDrafts[tableIndex];
	if (draft.savedSeen != saved)
	{
		draft.value = saved;
		draft.savedSeen = saved;
	}

	ImGui::Spacing();

	// If the song carries a loudness tag it has already been levelled by it, and this
	// slider adjusts from there rather than from the raw file.
	float tagGain = 0.0f;
	if (mgr.GetTagGainDb(tableIndex, tagGain))
	{
		char note[160];
		snprintf(note, sizeof(note), "%s %+.1f dB",
			L("Levelled by the song's own tag:").c_str(), tagGain);
		ImGui::TextDisabled("%s", note);
	}

	// Ctrl+click to type an exact value.
	ImGui::SetNextItemWidth(180.0f);
	ImGui::SliderFloat(L("Adjust").c_str(), &draft.value, -40.0f, 40.0f, "%+.1f dB");

	// How much of that is free. Up to the song's own headroom nothing is altered at all;
	// past it the loud parts have to be held down to fit, and the song is being squashed
	// rather than turned up. Saying so is the difference between a slider that sounds
	// broken and one whose limits are understood.
	const float headroom = mgr.GetHeadroomDb(tableIndex);
	if (headroom > -200.0f)
	{
		if (draft.value > headroom)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.35f, 1.0f), "%s",
				(L("Above") + " " + FormatDb(headroom) + " " + L("this song gets squashed to fit.")).c_str());
		}
		else
		{
			ImGui::TextDisabled("%s",
				(L("Untouched up to") + " " + FormatDb(headroom)).c_str());
		}
	}

	const bool dirty = (draft.value != saved);
	if (dirty)
	{
		if (ImGui::Button(L("Apply volume").c_str()))
		{
			mgr.SetGainDb(tableIndex, draft.value);
			m_lastMessage = L("Reconverting at the new volume") + "...";

			// The game is holding the old .pac. If this track is being previewed, play it
			// again once the rebuild lands, or the new volume is not heard at all.
			m_restartAfterApplyIndex = tableIndex;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltipWrapped(L("Converts the song again so the game plays it at this volume.").c_str());

		// Say so plainly. Without this the slider looks like it did something, and the
		// song keeps playing at the old volume with no indication why.
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.35f, 1.0f), "%s", L("not applied yet").c_str());
	}
}

// A rebuild writes a new .pac, but the game is still holding the one it already loaded,
// so a track that is playing keeps playing at the old volume. Once the rebuild lands, ask
// the game to load it again - the same thing "Play yours" does - so the change is audible
// straight away instead of only after the track next starts.
void BgmReplacementWindow::PollRestartAfterApply()
{
	if (m_restartAfterApplyIndex < 0)
		return;

	BgmReplacementManager& mgr = GetBgmReplacements();
	const BgmReplacementState state = mgr.GetState(m_restartAfterApplyIndex);
	if (state == BgmReplacementState::Converting)
		return; // still rebuilding

	const int tableIndex = m_restartAfterApplyIndex;
	m_restartAfterApplyIndex = -1;

	// The "reconverting" note has served its purpose either way.
	if (m_lastMessage == L("Reconverting at the new volume") + "...")
		m_lastMessage.clear();

	if (state != BgmReplacementState::Active)
		return;

	const BgmReplaceableTrack* track = mgr.FindTrack(tableIndex);
	if (!track)
		return;

	const std::string relPath = mgr.GetReplacementPreviewPath(tableIndex);
	if (relPath.empty())
		return;

	// Only in a match: this is the game's own playback path and it has nowhere to play
	// otherwise. Outside one, the next time the track starts is soon enough.
	if (!MusicManager::GetInstance().IsInMatch())
		return;

	MusicManager::GetInstance().PreviewPac(relPath, track->baseName);
	m_lastMessage = track->displayName + " " + L("is playing at the new volume.");
}

void BgmReplacementWindow::OpenPickerFor(int tableIndex)
{
	if (NativeFileDialog::IsOpen())
		return;

	const BgmReplaceableTrack* track = GetBgmReplacements().FindTrack(tableIndex);
	if (!track)
		return;

	NativeFileDialog::Request request;
	request.title = L("Choose a song to play instead of") + " " + track->displayName;
	request.filters.push_back({ AudioDecode::FilterDescription(), AudioDecode::FilterPattern() });
	request.defaultExtension = "mp3";
	request.contextId = tableIndex;

	if (NativeFileDialog::Open(kPickerToken, request))
		m_pendingPickIndex = tableIndex;
}

void BgmReplacementWindow::PollPicker()
{
	NativeFileDialog::Result result;
	if (!NativeFileDialog::Consume(kPickerToken, &result))
		return;

	m_pendingPickIndex = -1;
	if (!result.accepted || result.path.empty())
		return;

	BgmReplacementManager& mgr = GetBgmReplacements();
	const BgmReplaceableTrack* track = mgr.FindTrack(result.contextId);
	mgr.Assign(result.contextId, result.path);
	if (track)
		m_lastMessage = L("Converting your song for") + " " + track->displayName + "...";
}
