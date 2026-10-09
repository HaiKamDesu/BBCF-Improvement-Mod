#include "BgmReplacementWindow.h"

#include "MusicListStyle.h"

#include "Audio/BgmReplacementManager.h"
#include "Audio/MusicManager.h"
#include "Audio/MusicPackage.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/NativeFileDialog.h"
#include "Overlay/imgui_utils.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace
{
	const char* kSwapFileToken = "BgmReplacementWindow";
	const char* kAddSongsToken = "BgmReplacementWindow_add";
	const char* kImportPackToken = "BgmReplacementWindow_import";
	const char* kExportPackToken = "BgmReplacementWindow_export";

	const ImVec4 kAmber(1.0f, 0.80f, 0.35f, 1.0f);

	using LibrarySong = BgmReplacementManager::LibrarySong;

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
		case BgmReplacementState::Off:         return ImVec4(0.62f, 0.64f, 0.70f, 1.0f); // dim
		default:                               return ImVec4(0.70f, 0.72f, 0.78f, 1.0f);
		}
	}

	std::string ToLower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), ::tolower);
		return s;
	}

	bool SamePath(const std::string& a, const std::string& b)
	{
		return !a.empty() && _stricmp(a.c_str(), b.c_str()) == 0;
	}

	std::string PackFilterDescription()
	{
		return L("Improvement Mod music replacement file") + " (*." + MusicPackage::kReplacementExtension + ")";
	}

	// The game's table lists its tracks in its own order, where a category such as "sys"
	// comes back several times. Grouped here once per category, in order of first
	// appearance, so each category is one header (and one ImGui ID).
	struct CategoryGroup
	{
		std::string category;
		std::vector<const BgmReplaceableTrack*> tracks;
	};

	template <typename Keep>
	std::vector<CategoryGroup> GroupByCategory(const std::vector<BgmReplaceableTrack>& tracks, Keep keep)
	{
		std::vector<CategoryGroup> groups;
		for (const BgmReplaceableTrack& track : tracks)
		{
			if (!keep(track))
				continue;
			auto it = std::find_if(groups.begin(), groups.end(),
				[&](const CategoryGroup& g) { return g.category == track.category; });
			if (it == groups.end())
			{
				groups.push_back({ track.category, {} });
				it = groups.end() - 1;
			}
			it->tracks.push_back(&track);
		}
		return groups;
	}

	// A game track that a song can be put on: installed, actually loaded by the game, and
	// not the song itself when the song is a game track.
	bool CanReplace(const BgmReplaceableTrack& track, const LibrarySong& song)
	{
		if (track.originalDir.empty() || !track.everUsed)
			return false;
		if (song.vanillaTrack >= 0)
		{
			const BgmReplaceableTrack* self = GetBgmReplacements().FindTrack(song.vanillaTrack);
			if (self && self->fileName == track.fileName)
				return false;
		}
		return true;
	}

	// Puts a song on a track. A song already on other tracks brings its volume along, so a
	// song levelled once sounds the same everywhere it plays.
	void PutSongOn(const LibrarySong& song, int tableIndex)
	{
		BgmReplacementManager& mgr = GetBgmReplacements();
		if (song.vanillaTrack >= 0 || song.replaces.empty())
		{
			mgr.AssignSong(tableIndex, song.path);
			return;
		}
		const float gain = mgr.GetGainDb(song.replaces.front());
		mgr.Assign(tableIndex, song.path, &gain);
	}

	// When a replacement is actually heard, in plain words. The swap is written long before
	// the game next loads the file, and saying which of the two it is answers "why didn't
	// it change?" before it is asked.
	const char* TimingNote(const BgmReplaceableTrack& track, bool live)
	{
		switch (track.timing)
		{
		case BgmLoadTiming::NextMatch:
			return live ? "Loads with each match." : "Starts playing from your next match.";
		case BgmLoadTiming::NextScreen:
			return live ? "Loads each time this screen comes up."
			            : "Starts playing the next time this screen's music loads.";
		case BgmLoadTiming::GameRestart:
			return live ? "The game loads this one at startup, so it is already in use."
			            : "The game only loads this one while it starts up, so restart the "
			              "game to hear it. The swap itself is saved and done.";
		}
		return nullptr;
	}
}

void BgmReplacementWindow::BeforeDraw()
{
	ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
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

	PollDialogs();

	// The next of several songs added together gets its turn once the picker is closed.
	if (!m_pickerShowing && !m_openTrackPicker)
	{
		if (!m_addQueue.empty())
		{
			OpenTrackPicker(m_addQueue.front());
			m_addQueue.erase(m_addQueue.begin());
		}
		else
		{
			m_addQueueTotal = 0;   // the batch is over; a later pick is a song of its own
		}
	}
	// The list only holds songs that replace something. A song keeps its row while its
	// tracks are being chosen, or while it waits for that turn.
	{
		std::vector<std::string> keep = m_addQueue;
		if (m_pickerShowing || m_openTrackPicker)
			keep.push_back(m_pickerSong);
		mgr.PruneLibrary(keep);
	}

	DrawToolbar();
	DrawStatus();
	DrawSongList();

	DrawTrackPickerModal();
	DrawSoundtrackModal();
	DrawImportChoiceModal();
}

// ---------------------------------------------------------------------------------------
// Toolbar and status
// ---------------------------------------------------------------------------------------

void BgmReplacementWindow::DrawToolbar()
{
	BgmReplacementManager& mgr = GetBgmReplacements();
	const bool dialogBusy = NativeFileDialog::IsOpen();

	// Where the music comes from: your own files, or one of the game's tracks.
	if (ImGui::Button((L("Add music") + "  v##add").c_str()))
		ImGui::OpenPopup("##add_menu");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Bring in songs, then choose which of the game's tracks each one replaces. One song can replace as many tracks as you like.").c_str());
	MusicListStyle::AnchorNextPopupBelowLastItem();
	bool addOpen = false;
	{
		MusicListStyle::OpaquePopup opaque;
		addOpen = ImGui::BeginPopup("##add_menu");
	}
	if (addOpen)
	{
		if (ImGui::MenuItem(L("From files...").c_str(), nullptr, false, !dialogBusy))
			OpenAddSongs();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltipWrapped(L("Pick as many songs as you like at once.").c_str());
		if (ImGui::MenuItem(L("From the game's soundtrack...").c_str()))
		{
			m_soundtrackSearch[0] = '\0';
			m_openSoundtrack = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltipWrapped(L("Use one of the game's own tracks in place of others.").c_str());
		ImGui::EndPopup();
	}

	// Search fills what the menu button leaves of the row.
	const float moreWidth = ImGui::GetFrameHeight() * 1.6f;
	const float rightWidth = moreWidth + ImGui::HelpMarkerWidth() + ImGui::GetStyle().ItemSpacing.x * 2.0f;
	ImGui::SameLine();
	ImGui::SetNextItemWidth((std::max)(80.0f, ImGui::GetContentRegionAvail().x - rightWidth));
	ImGui::InputTextWithHint("##search", L("Search").c_str(), m_search, sizeof(m_search));

	ImGui::SameLine();
	if (ImGui::Button("...##more", ImVec2(moreWidth, 0.0f)))
		ImGui::OpenPopup("##more_menu");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Music replacement files, and undoing everything.").c_str());
	MusicListStyle::AnchorNextPopupBelowLastItem();
	bool moreOpen = false;
	{
		MusicListStyle::OpaquePopup opaque;
		moreOpen = ImGui::BeginPopup("##more_menu");
	}
	if (moreOpen)
	{
		const bool packBusy = mgr.IsPackBusy();
		if (ImGui::MenuItem(L("Import music file...").c_str(), nullptr, false, !dialogBusy && !packBusy))
		{
			NativeFileDialog::Request request;
			request.title = L("Import a music replacement file");
			request.filters.push_back({ PackFilterDescription(), std::string("*.") + MusicPackage::kReplacementExtension });
			NativeFileDialog::Open(kImportPackToken, request);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltipWrapped(L("Load a music replacement file someone made, or one you exported before.").c_str());
		if (ImGui::MenuItem(L("Export music file...").c_str(), nullptr, false,
			!dialogBusy && !packBusy && (mgr.GetAssignedCount() > 0 || !mgr.GetLibrary().empty())))
		{
			NativeFileDialog::Request request;
			request.save = true;
			request.title = L("Export your music replacements");
			request.filters.push_back({ PackFilterDescription(), std::string("*.") + MusicPackage::kReplacementExtension });
			request.defaultExtension = MusicPackage::kReplacementExtension;
			request.initialPath = std::string("My music.") + MusicPackage::kReplacementExtension;
			NativeFileDialog::Open(kExportPackToken, request);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltipWrapped(L("Saves everything here into one file - every song, what it replaces, its volume and whether it is on - so it works on any PC.").c_str());
		ImGui::Separator();
		const std::vector<int> rebuildable = mgr.GetRebuildableTracks();
		const std::string rebuildLabel = L("Rebuild the tracks that need it") + " (" + std::to_string(rebuildable.size()) + ")";
		if (ImGui::MenuItem(rebuildLabel.c_str(), nullptr, false, !rebuildable.empty()))
		{
			for (int index : rebuildable)
				mgr.Retry(index);
			m_lastMessage = L("Rebuilding") + " " + std::to_string(rebuildable.size()) + " " + L("track(s)...");
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltipWrapped(L("Converts again every replacement marked as needing a rebuild or as failed, from the song it was set to.").c_str());
		if (ImGui::MenuItem(L("Put every track back to normal").c_str(), nullptr, false, mgr.GetAssignedCount() > 0))
		{
			mgr.UnassignAll();
			m_lastMessage = L("Every track is back to normal.");
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltipWrapped(L("Puts every track back to the original, deletes the converted files and empties the list.").c_str());
		ImGui::EndPopup();
	}

	ImGui::SameLine();
	ImGui::ShowHelpMarker((L("Swaps take effect when a match STARTS.") + "\n" +
		L("If you are in a match right now, leave and start a new one. This includes Astral Finish music, which is loaded up front with the rest of the match, not when the Astral happens.") + "\n\n" +
		L("Click what a song replaces to choose its tracks. Untick a song to hear the originals again without losing it. Open a song with its arrow to set each track on its own, and right-click for previews and more.")).c_str());
}

// Only what is happening right now; nothing at all when nothing is.
void BgmReplacementWindow::DrawStatus()
{
	BgmReplacementManager& mgr = GetBgmReplacements();

	std::string packMessage;
	bool packOk = true;
	if (mgr.ConsumePackMessage(packMessage, packOk))
	{
		m_packMessage = packMessage;
		m_packMessageOk = packOk;
	}

	bool exporting = false;
	int packDone = 0, packTotal = 0;
	float packFraction = 0.0f;
	if (mgr.GetPackProgress(exporting, packDone, packTotal, packFraction))
	{
		std::string label = L(exporting ? "Saving the music file's songs" : "Unpacking the music file's songs");
		if (packTotal > 0)
			label += ": " + std::to_string((std::min)(packDone + 1, packTotal)) + " / " + std::to_string(packTotal);
		ImGui::TextColoredWrapped(kAmber, "%s", label.c_str());
		char percent[16];
		snprintf(percent, sizeof(percent), "%d%%", (int)(packFraction * 100.0f + 0.5f));
		ImGui::ProgressBar(packFraction, ImVec2(-1, 0.0f), percent);
	}
	else if (!m_packMessage.empty())
	{
		ImGui::TextColoredWrapped(m_packMessageOk ? StateColor(BgmReplacementState::Active) : kAmber,
			"%s", m_packMessage.c_str());
	}

	if (mgr.IsBusy())
	{
		const int converting = mgr.GetConvertingIndex();
		const BgmReplaceableTrack* track = converting >= 0 ? mgr.FindTrack(converting) : nullptr;
		std::string text = track ? L("Converting for") + " " + track->displayName + "..." : L("Converting...");
		int done = 0, total = 0;
		mgr.GetBatchCounts(done, total);
		if (total > 1)
			text += "  " + std::to_string((std::min)(done + 1, total)) + " / " + std::to_string(total);
		ImGui::TextColored(StateColor(BgmReplacementState::Converting), "%s", text.c_str());
		ImGui::ProgressBar(mgr.GetBatchProgress(), ImVec2(-1, total > 1 ? 0.0f : 3.0f), total > 1 ? nullptr : "");
	}
	else if (!m_lastMessage.empty())
	{
		ImGui::TextDisabledWrapped("%s", m_lastMessage.c_str());
	}
}

// ---------------------------------------------------------------------------------------
// The list: one row per song
// ---------------------------------------------------------------------------------------

void BgmReplacementWindow::DrawSongList()
{
	BgmReplacementManager& mgr = GetBgmReplacements();
	const std::vector<LibrarySong> songs = mgr.GetLibrary();
	const std::string needle = ToLower(m_search);

	float height = ImGui::GetContentRegionAvail().y;
	if (height < 120.0f) height = 120.0f;
	ImGui::BeginChild("##BgmSongList", ImVec2(0, height), true);

	if (songs.empty())
	{
		ImGui::TextDisabledWrapped("%s", L("No music yet. Use Add music to bring in songs, then choose which of the game's tracks each one replaces.").c_str());
		ImGui::EndChild();
		return;
	}

	int shown = 0;
	for (const LibrarySong& song : songs)
	{
		if (!needle.empty())
		{
			// A song shows when its own name matches or any track it replaces does.
			bool match = ToLower(song.name).find(needle) != std::string::npos;
			for (size_t i = 0; !match && i < song.replaces.size(); ++i)
			{
				const BgmReplaceableTrack* track = mgr.FindTrack(song.replaces[i]);
				match = track && ToLower(track->displayName + " " + track->baseName).find(needle) != std::string::npos;
			}
			if (!match)
				continue;
		}
		DrawSongRow(song);
		++shown;
	}
	if (shown == 0)
		ImGui::TextDisabled("%s", L("Nothing matches that search.").c_str());

	ImGui::EndChild();
}

void BgmReplacementWindow::DrawSongRow(const LibrarySong& song)
{
	BgmReplacementManager& mgr = GetBgmReplacements();
	ImGui::PushID(song.path.c_str());

	// On / off for every track this song replaces. Unticking keeps the song set: the
	// originals play until it is ticked again.
	int on = 0;
	for (int index : song.replaces)
		if (mgr.IsEnabled(index)) ++on;
	const int count = (int)song.replaces.size();
	const int checkState = count == 0 ? 0 : on == count ? 1 : on == 0 ? 0 : -1;
	bool checked = checkState == 1;
	ImGui::BeginDisabled(count == 0);
	ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, checkState == -1);
	if (ImGui::Checkbox("##on", &checked))
	{
		const bool to = checkState == -1 ? true : checked;
		for (int index : song.replaces)
			mgr.SetEnabled(index, to);
	}
	ImGui::PopItemFlag();
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltipWrapped(count > 0
			? L("Untick to hear the originals again. The song stays set, ready to tick back on.").c_str()
			: L("Choose what this song replaces on the right.").c_str());
	ImGui::SameLine(0.0f, 2.0f);

	// The arrow that opens the row to show each track on its own.
	const bool searching = m_search[0] != '\0';
	bool& expanded = m_expanded[song.path];
	const bool open = count > 0 && (expanded || searching);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::BeginDisabled(count == 0);
	if (ImGui::ArrowButton("##open", open ? ImGuiDir_Down : ImGuiDir_Right))
		expanded = !expanded;
	ImGui::EndDisabled();
	ImGui::PopStyleColor();
	if (count > 0 && ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Show each track on its own, with its own switch and volume.").c_str());
	ImGui::SameLine();

	// Name, then what it replaces and the volume in fixed columns on the right, so the
	// columns line up down the list.
	const bool hasVolume = count > 0 && song.vanillaTrack < 0;
	const float volumeWidth = ImGui::GetFontSize() * 4.0f;
	const float avail = ImGui::GetContentRegionAvail().x;
	const float replacesWidth = (std::max)(90.0f, (std::min)(avail * 0.4f, 300.0f));
	const float nameWidth = (std::max)(1.0f, avail - replacesWidth - volumeWidth - ImGui::GetStyle().ItemSpacing.x * 2.0f);

	ImGui::PushStyleColor(ImGuiCol_Text, song.exists ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
		: StateColor(BgmReplacementState::Failed));
	const std::string name = song.exists ? song.name : song.name + "  (" + L("file not found") + ")";
	if (ImGui::Selectable((name + "##name").c_str(), false,
		ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(nameWidth, 0.0f)) &&
		ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
	{
		OpenTrackPicker(song.path);
	}
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
	{
		std::string tip = song.vanillaTrack >= 0 ? L("One of the game's own tracks.") : song.path;
		tip += "\n" + L("Double-click to choose what it replaces. Right-click for more.");
		ImGui::SetTooltipWrapped(tip.c_str());
	}
	DrawSongContextMenu(song);

	ImGui::SameLine();
	DrawReplacesButton(song, replacesWidth);
	if (hasVolume)
	{
		ImGui::SameLine();
		DrawVolumeButton(song.replaces, "song:" + song.path);
	}

	if (open)
	{
		const float indent = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.x;
		ImGui::Indent(indent);
		for (int index : song.replaces)
		{
			if (const BgmReplaceableTrack* track = mgr.FindTrack(index))
				DrawReplacedTrackRow(*track);
		}
		ImGui::Unindent(indent);
	}

	ImGui::PopID();
}

void BgmReplacementWindow::DrawReplacesButton(const LibrarySong& song, float width)
{
	BgmReplacementManager& mgr = GetBgmReplacements();

	// What it replaces, coloured by how those replacements are doing: something wrong
	// wins, then something still converting, then all switched off.
	std::string label;
	ImVec4 color = StateColor(BgmReplacementState::Active);
	if (song.replaces.empty())
	{
		label = L("Choose tracks...");
		color = kAmber;
	}
	else
	{
		const BgmReplaceableTrack* first = mgr.FindTrack(song.replaces.front());
		label = first ? first->displayName : "?";
		if (song.replaces.size() > 1)
			label += "  +" + std::to_string(song.replaces.size() - 1);
		int missing = 0, failed = 0;
		bool converting = false, anyOn = false;
		for (int index : song.replaces)
		{
			const BgmReplacementState state = mgr.GetState(index);
			if (state == BgmReplacementState::Missing) ++missing;
			if (state == BgmReplacementState::Failed || state == BgmReplacementState::Unavailable) ++failed;
			converting = converting || state == BgmReplacementState::Converting;
			anyOn = anyOn || mgr.IsEnabled(index);
		}
		if (failed > 0 || missing > 0)
		{
			// Said in words, and in front where a narrow column cannot cut it off: a bare
			// "!" told nobody what was wrong or what to do.
			label = L(failed > 0 ? "Failed:" : "Needs rebuilding:") + " " + label;
			color = StateColor(BgmReplacementState::Failed);
		}
		else if (converting)
			color = StateColor(BgmReplacementState::Converting);
		else if (!anyOn)
			color = StateColor(BgmReplacementState::Off);
	}

	if (MusicListStyle::DropdownButton(label.c_str(), "##replaces", width, color))
		OpenTrackPicker(song.path);
	if (ImGui::IsItemHovered())
	{
		std::string tip = L("Replaces:");
		if (song.replaces.empty())
			tip += " " + L("nothing yet");
		bool anyProblem = false;
		for (int index : song.replaces)
		{
			const BgmReplaceableTrack* track = mgr.FindTrack(index);
			if (!track)
				continue;
			tip += "\n  " + track->displayName;
			const BgmReplacementState state = mgr.GetState(index);
			if (state == BgmReplacementState::Missing || state == BgmReplacementState::Failed ||
				state == BgmReplacementState::Unavailable)
			{
				anyProblem = true;
				tip += "  -  " + L(state == BgmReplacementState::Missing ? "needs rebuilding" : "failed") + ": " +
					mgr.GetError(index);
			}
		}
		if (anyProblem)
			tip += "\n\n" + L("Those tracks play the original music until they are fixed. Right-click the song and choose Rebuild, or use ... > Rebuild the tracks that need it.");
		tip += "\n\n" + L("Click to choose the tracks.");
		ImGui::SetTooltipWrapped(tip.c_str());
	}
}

void BgmReplacementWindow::DrawSongContextMenu(const LibrarySong& song)
{
	bool open = false;
	{
		MusicListStyle::OpaquePopup opaque;
		open = ImGui::BeginPopupContextItem("##song_menu");
	}
	if (!open)
		return;

	BgmReplacementManager& mgr = GetBgmReplacements();
	ImGui::TextDisabled("%s", song.name.c_str());
	ImGui::Separator();

	if (ImGui::MenuItem(L("Choose tracks...").c_str(), nullptr, false, song.exists))
		OpenTrackPicker(song.path);

	// A preview plays through the game's own audio, which only runs inside a match. A
	// file has to be converted before the game can play it, so it is heard through one of
	// the tracks it replaces.
	const bool inMatch = MusicManager::GetInstance().IsInMatch();
	std::string previewPath, previewCue;
	if (song.vanillaTrack >= 0)
	{
		previewPath = mgr.GetOriginalPreviewPath(song.vanillaTrack);
		previewCue = mgr.FindTrack(song.vanillaTrack)->baseName;
	}
	else
	{
		for (int index : song.replaces)
		{
			const BgmReplacementState state = mgr.GetState(index);
			const std::string path = mgr.GetReplacementPreviewPath(index);
			if ((state == BgmReplacementState::Active || state == BgmReplacementState::Off) && !path.empty())
			{
				previewPath = path;
				previewCue = mgr.FindTrack(index)->baseName;
				break;
			}
		}
	}
	if (ImGui::MenuItem(L("Play this song").c_str(), nullptr, false, inMatch && !previewPath.empty()))
		MusicManager::GetInstance().PreviewPac(previewPath, previewCue);
	if (!inMatch)
		ImGui::TextDisabled("%s", L("Start a match to hear previews.").c_str());
	else if (previewPath.empty())
		ImGui::TextDisabled("%s", L("Put it on a track first to hear it.").c_str());

	if (!song.replaces.empty())
	{
		ImGui::Separator();
		bool converting = false;
		for (int index : song.replaces)
			converting = converting || mgr.GetState(index) == BgmReplacementState::Converting;
		if (ImGui::MenuItem(L("Rebuild").c_str(), nullptr, false, !converting))
		{
			for (int index : song.replaces)
				mgr.Retry(index);
			m_lastMessage = L("Rebuilding") + " " + song.name + "...";
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltipWrapped(L("Build its replacements again from the same song.").c_str());
	}
	if (song.vanillaTrack < 0 &&
		ImGui::MenuItem(L("Use a different file...").c_str(), nullptr, false, !NativeFileDialog::IsOpen()))
		OpenSwapFile(song.path);
	if (song.vanillaTrack < 0 && ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Swap this song for another file, keeping the tracks it replaces, their volume and their switches. Handy when the file was moved.").c_str());

	ImGui::Separator();
	if (ImGui::MenuItem(L("Remove from the list").c_str()))
	{
		mgr.RemoveFromLibrary(song.path);
		m_lastMessage = song.name + " " + L("was removed.");
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Takes the song out of the list and puts back the original of every track it replaced. The file itself is not deleted.").c_str());
	ImGui::EndPopup();
}

// One track under an opened song: its own switch, volume and right-click menu.
void BgmReplacementWindow::DrawReplacedTrackRow(const BgmReplaceableTrack& track)
{
	BgmReplacementManager& mgr = GetBgmReplacements();
	const int tableIndex = track.tableIndex;
	const BgmReplacementState state = mgr.GetState(tableIndex);

	ImGui::PushID(tableIndex);

	bool on = mgr.IsEnabled(tableIndex);
	if (ImGui::Checkbox("##on", &on))
		mgr.SetEnabled(tableIndex, on);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Untick to hear the original of this track again.").c_str());
	ImGui::SameLine();

	const bool hasVolume = !mgr.IsPacSource(tableIndex);
	const float volumeWidth = ImGui::GetFontSize() * 4.0f;
	const float nameWidth = (std::max)(1.0f, ImGui::GetContentRegionAvail().x -
		(hasVolume ? volumeWidth + ImGui::GetStyle().ItemSpacing.x : 0.0f));

	ImGui::PushStyleColor(ImGuiCol_Text, StateColor(state));
	std::string label = track.displayName;
	if (state == BgmReplacementState::Converting)
		label += "  (" + L("converting...") + ")";
	else if (state == BgmReplacementState::Missing)
		label = L("Needs rebuilding:") + " " + label;
	else if (state == BgmReplacementState::Failed)
		label = L("Failed:") + " " + label;
	ImGui::Selectable((label + "##name").c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(nameWidth, 0.0f));
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
	{
		std::string tip = track.displayName + "  (" + track.baseName + ")";
		if (!track.usageNote.empty())
			tip += "\n" + L(track.usageNote);
		if (state == BgmReplacementState::Active)
		{
			if (const char* when = TimingNote(track, mgr.IsLiveNow(tableIndex)))
				tip += "\n" + L(when);
		}
		else if (state == BgmReplacementState::Missing || state == BgmReplacementState::Failed ||
			state == BgmReplacementState::Unavailable)
			tip += "\n" + mgr.GetError(tableIndex) + "\n" +
				L("The original plays until this is fixed. Right-click and choose Rebuild.");
		else if (state == BgmReplacementState::Off)
			tip += "\n" + L("Switched off: the original plays.");
		ImGui::SetTooltipWrapped(tip.c_str());
	}
	DrawTrackContextMenu(track);

	if (hasVolume)
	{
		ImGui::SameLine();
		DrawVolumeButton({ tableIndex }, "track:" + std::to_string(tableIndex));
	}

	// A swap that only lands on a restart says so right on the row: the one case where
	// "set" and "heard" are minutes apart.
	if (state == BgmReplacementState::Active && !mgr.IsLiveNow(tableIndex) && track.timing == BgmLoadTiming::GameRestart)
	{
		ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
		ImGui::TextColoredWrapped(kAmber, "%s", L("Restart the game to hear this one.").c_str());
		ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
	}

	ImGui::PopID();
}

void BgmReplacementWindow::DrawTrackContextMenu(const BgmReplaceableTrack& track)
{
	bool open = false;
	{
		MusicListStyle::OpaquePopup opaque;
		open = ImGui::BeginPopupContextItem("##track_menu");
	}
	if (!open)
		return;

	BgmReplacementManager& mgr = GetBgmReplacements();
	const int tableIndex = track.tableIndex;
	const BgmReplacementState state = mgr.GetState(tableIndex);

	ImGui::TextDisabled("%s", track.displayName.c_str());
	ImGui::Separator();

	// Previews play through the game's own audio, which only runs inside a match.
	const bool inMatch = MusicManager::GetInstance().IsInMatch();
	const std::string originalPath = mgr.GetOriginalPreviewPath(tableIndex);
	const std::string yoursPath = mgr.GetReplacementPreviewPath(tableIndex);
	if (ImGui::MenuItem(L("Play original").c_str(), nullptr, false, inMatch && !originalPath.empty()))
		MusicManager::GetInstance().PreviewPac(originalPath, track.baseName);
	if (ImGui::MenuItem(L("Play yours").c_str(), nullptr, false, inMatch && !yoursPath.empty() &&
		(state == BgmReplacementState::Active || state == BgmReplacementState::Off)))
		MusicManager::GetInstance().PreviewPac(yoursPath, track.baseName);
	if (!inMatch)
		ImGui::TextDisabled("%s", L("Start a match to hear previews.").c_str());

	ImGui::Separator();
	if (ImGui::MenuItem(L("Rebuild").c_str(), nullptr, false, state != BgmReplacementState::Converting))
	{
		mgr.Retry(tableIndex);
		m_lastMessage = L("Rebuilding") + " " + track.displayName + "...";
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Build this replacement again from the same source.").c_str());
	if (ImGui::MenuItem(L("Put the original back").c_str()))
	{
		mgr.Unassign(tableIndex);
		m_lastMessage = track.displayName + " " + L("is back to normal.");
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Put the original back and delete the converted file.").c_str());
	if (mgr.IsPacSource(tableIndex))
		ImGui::TextDisabled("%s", L("Copied as-is, so it plays at its original volume.").c_str());
	ImGui::EndPopup();
}

// Volume is baked into the converted .pac: the game plays that file through its own XACT
// engine and the mod has no volume control there. So moving the slider costs nothing until
// Apply, which re-converts. On a song's row it sets every track the song replaces.
void BgmReplacementWindow::DrawVolumeButton(const std::vector<int>& tableIndices, const std::string& id)
{
	if (tableIndices.empty())
		return;
	BgmReplacementManager& mgr = GetBgmReplacements();
	const int first = tableIndices.front();
	const float saved = mgr.GetGainDb(first);
	bool mixed = false;
	for (int index : tableIndices)
		mixed = mixed || mgr.GetGainDb(index) != saved;

	if (MusicListStyle::VolumeButton(saved, "vol"))
		ImGui::OpenPopup("##volume");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(mixed
			? L("Its tracks are at different volumes. Applying here sets them all to the same one.").c_str()
			: L("Set how loud this song is. The file is converted again when you apply it.").c_str());

	MusicListStyle::AnchorNextPopupBelowLastItem();
	MusicListStyle::OpaquePopup opaque;
	if (!ImGui::BeginPopup("##volume"))
	{
		if (m_volumeFor == id)
			m_volumeFor.clear();
		return;
	}
	if (m_volumeFor != id || m_volumeSavedSeen != saved)
	{
		m_volumeFor = id;
		m_volumeDraft = saved;
		m_volumeSavedSeen = saved;
	}

	// If the song carries a loudness tag it has already been levelled by it, and this
	// slider adjusts from there rather than from the raw file.
	float tagGain = 0.0f;
	if (mgr.GetTagGainDb(first, tagGain))
		ImGui::TextDisabled("%s %+.1f dB", L("Levelled by the song's own tag:").c_str(), tagGain);

	// Ctrl+click to type an exact value.
	ImGui::SetNextItemWidth(200.0f);
	ImGui::SliderFloat(L("Adjust").c_str(), &m_volumeDraft, -40.0f, 40.0f, "%+.1f dB");

	// Up to the song's own headroom nothing is altered at all; past it the loud parts have
	// to be held down to fit, and the song is being squashed rather than turned up.
	const float headroom = mgr.GetHeadroomDb(first);
	if (headroom > -200.0f)
	{
		if (m_volumeDraft > headroom)
			ImGui::TextColored(kAmber, "%s", (L("Above") + " " + FormatDb(headroom) + " " + L("this song gets squashed to fit.")).c_str());
		else
			ImGui::TextDisabled("%s", (L("Untouched up to") + " " + FormatDb(headroom)).c_str());
	}

	bool busy = false;
	for (int index : tableIndices)
		busy = busy || mgr.GetState(index) == BgmReplacementState::Converting;
	ImGui::BeginDisabled(busy || (m_volumeDraft == saved && !mixed));
	if (ImGui::Button(L("Apply volume").c_str()))
	{
		for (int index : tableIndices)
			mgr.SetGainDb(index, m_volumeDraft);
		m_lastMessage = L("Reconverting at the new volume") + "...";
		// The game is holding the old .pac. If this track is being previewed, play it again
		// once the rebuild lands, or the new volume is not heard at all.
		m_restartAfterApplyIndex = first;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	if (busy)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("%s", L("converting...").c_str());
	}
	else if (m_volumeDraft != saved)
	{
		ImGui::SameLine();
		ImGui::TextColored(kAmber, "%s", L("not applied yet").c_str());
	}
	ImGui::EndPopup();
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

// ---------------------------------------------------------------------------------------
// Modals
// ---------------------------------------------------------------------------------------

// Choosing which of the game's tracks a song replaces: the soundtrack grouped like the
// Jukebox, a tick on every track the song plays on. Ticking converts right away; a track
// another song already had is handed over to this one, and the row says whose it was.
void BgmReplacementWindow::DrawTrackPickerModal()
{
	BgmReplacementManager& mgr = GetBgmReplacements();
	const std::string popupId = L("Choose tracks") + "##track_picker";

	if (m_openTrackPicker)
	{
		ImGui::OpenPopup(popupId.c_str());
		m_openTrackPicker = false;
		m_pickerSearch[0] = '\0';
	}

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowSize(ImVec2((std::min)(600.0f, display.x * 0.9f), (std::min)(600.0f, display.y * 0.85f)), ImGuiCond_Appearing);
	MusicListStyle::OpaquePopup opaque;
	m_pickerShowing = ImGui::BeginPopupModal(popupId.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings);
	if (!m_pickerShowing)
		return;

	LibrarySong song;
	bool found = false;
	for (const LibrarySong& candidate : mgr.GetLibrary())
	{
		if (SamePath(candidate.path, m_pickerSong))
		{
			song = candidate;
			found = true;
			break;
		}
	}
	if (!found)
	{
		// The song went away underneath the modal; nothing left to choose for.
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}

	if (m_addQueueTotal > 1)
		ImGui::TextDisabled("%s", (L("Song") + " " + std::to_string(m_addQueueTotal - (int)m_addQueue.size()) +
			" / " + std::to_string(m_addQueueTotal)).c_str());
	ImGui::TextWrapped("%s", (L("Tracks that play") + " " + song.name + " " + L("instead:")).c_str());
	if (!song.exists)
		ImGui::TextColoredWrapped(StateColor(BgmReplacementState::Failed), "%s",
			L("This song's file is gone, so it can't be put on new tracks. Right-click it and use a different file.").c_str());
	ImGui::Spacing();
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputTextWithHint("##picker_search", L("Search").c_str(), m_pickerSearch, sizeof(m_pickerSearch));
	ImGui::Spacing();

	const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	ImGui::BeginChild("##picker_list", ImVec2(0, -footer), true);

	const std::string needle = ToLower(m_pickerSearch);
	const bool searching = !needle.empty();
	const auto isMine = [&](int tableIndex) { return SamePath(mgr.GetSourcePath(tableIndex), song.path); };
	const std::vector<CategoryGroup> groups = GroupByCategory(mgr.GetTracks(), [&](const BgmReplaceableTrack& track) {
		if (!track.everUsed)
			return false; // the game never reads it: nothing to replace, nothing to show
		return !searching || ToLower(track.displayName + " " + track.baseName).find(needle) != std::string::npos;
	});

	for (const CategoryGroup& group : groups)
	{
		int eligible = 0, mine = 0;
		for (const BgmReplaceableTrack* track : group.tracks)
		{
			if (!CanReplace(*track, song))
				continue;
			++eligible;
			if (isMine(track->tableIndex)) ++mine;
		}
		const int checkState = mine == 0 ? 0 : mine == eligible ? 1 : -1;
		const bool collapsed = m_pickerCollapsed[group.category];
		const MusicListStyle::HeaderResult header = MusicListStyle::CategoryHeader(group.category, checkState,
			song.exists && eligible > 0, searching || !collapsed,
			L("Put this song on every track in this group, or take it off them all").c_str());
		if (header.checkboxClicked)
		{
			for (const BgmReplaceableTrack* track : group.tracks)
			{
				if (!CanReplace(*track, song))
					continue;
				if (header.checkTo && !isMine(track->tableIndex))
					PutSongOn(song, track->tableIndex);
				else if (!header.checkTo && isMine(track->tableIndex))
					mgr.Unassign(track->tableIndex);
			}
			// The song's own list of tracks changed; later rows read it fresh next frame.
			song.replaces.clear();
			for (const BgmReplaceableTrack& t : mgr.GetTracks())
				if (isMine(t.tableIndex)) song.replaces.push_back(t.tableIndex);
		}
		if (!searching)
			m_pickerCollapsed[group.category] = !header.open;
		if (!header.open)
			continue;

		for (const BgmReplaceableTrack* track : group.tracks)
		{
			ImGui::PushID(track->tableIndex);
			const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
			const bool canReplace = CanReplace(*track, song);
			bool checked = isMine(track->tableIndex);
			ImGui::BeginDisabled(!canReplace || (!song.exists && !checked));
			if (ImGui::Checkbox((track->displayName + "##pick").c_str(), &checked))
			{
				if (checked)
				{
					PutSongOn(song, track->tableIndex);
					song.replaces.push_back(track->tableIndex);
				}
				else
				{
					mgr.Unassign(track->tableIndex);
				}
			}
			ImGui::EndDisabled();
			const float labelRight = ImGui::GetItemRectMax().x;

			// Whose the track is now, when it is another song's.
			std::string other;
			const std::string source = mgr.GetSourcePath(track->tableIndex);
			if (!source.empty() && !isMine(track->tableIndex))
				other = mgr.GetSourceName(track->tableIndex);

			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
			{
				std::string tip = track->displayName + "  (" + track->baseName + ")";
				if (!track->usageNote.empty())
					tip += "\n" + L(track->usageNote);
				if (track->originalDir.empty())
					tip += "\n" + L("This track isn't installed on your copy of the game.");
				else if (!canReplace)
					tip += "\n" + L("A song can't replace itself.");
				if (!other.empty())
				{
					tip += "\n" + L("Now plays:") + " " + other;
					if (canReplace)
						tip += "\n" + L("Ticking it hands it to this song.");
				}
				ImGui::SetTooltipWrapped(tip.c_str());
			}
			if (!other.empty())
			{
				// Right-aligned, and cut short with "..." when a long song name would run
				// into the track's own; the tooltip has it in full.
				const std::string note = L("now:") + " " + other;
				const float noteWidth = ImGui::CalcTextSize(note.c_str()).x;
				const float left = (std::max)(labelRight + ImGui::GetStyle().ItemSpacing.x * 2.0f, right - noteWidth);
				if (right - left > ImGui::GetFontSize() * 3.0f)
				{
					const float y = ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y;
					ImDrawList* draw = ImGui::GetWindowDrawList();
					draw->PushClipRect(ImVec2(left, y), ImVec2(right, y + ImGui::GetFontSize()), true);
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					ImGui::RenderTextEllipsis(draw, ImVec2(left, y), ImVec2(right, y + ImGui::GetFontSize()), right,
						note.c_str(), nullptr, nullptr);
					ImGui::PopStyleColor();
					draw->PopClipRect();
				}
			}
			ImGui::PopID();
		}
	}
	if (groups.empty())
		ImGui::TextDisabled("%s", L("No tracks match.").c_str());

	ImGui::EndChild();

	const bool more = !m_addQueue.empty();
	if (ImGui::Button(L(more ? "Next song" : "Done").c_str(), ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();
	ImGui::SameLine();
	if (song.replaces.empty())
		ImGui::TextColored(kAmber, "%s", L("Nothing ticked: this song leaves the list when you close this.").c_str());
	else
		ImGui::TextDisabled("%s", (std::to_string(song.replaces.size()) + " " + L("track(s) ticked")).c_str());

	ImGui::EndPopup();
}

// Bringing in one of the game's own tracks, to play it in place of others. Picking one
// adds it to the list and goes straight on to choosing what it replaces.
void BgmReplacementWindow::DrawSoundtrackModal()
{
	BgmReplacementManager& mgr = GetBgmReplacements();
	const std::string popupId = L("Add from the game's soundtrack") + "##soundtrack";

	if (m_openSoundtrack)
	{
		ImGui::OpenPopup(popupId.c_str());
		m_openSoundtrack = false;
	}

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowSize(ImVec2((std::min)(560.0f, display.x * 0.9f), (std::min)(560.0f, display.y * 0.85f)), ImGuiCond_Appearing);
	bool open = false;
	{
		MusicListStyle::OpaquePopup opaque;
		open = ImGui::BeginPopupModal(popupId.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings);
	}
	if (!open)
		return;

	ImGui::TextWrapped("%s", L("Choose a track to add. You choose what it replaces next.").c_str());
	ImGui::Spacing();
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputTextWithHint("##soundtrack_search", L("Search").c_str(), m_soundtrackSearch, sizeof(m_soundtrackSearch));
	ImGui::Spacing();

	const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	ImGui::BeginChild("##soundtrack_list", ImVec2(0, -footer), true);

	const std::vector<LibrarySong> songs = mgr.GetLibrary();
	const std::string needle = ToLower(m_soundtrackSearch);
	const bool searching = !needle.empty();
	// Only tracks whose file is actually on disk can donate their audio.
	const std::vector<CategoryGroup> groups = GroupByCategory(mgr.GetTracks(), [&](const BgmReplaceableTrack& track) {
		if (track.originalDir.empty() || !track.everUsed)
			return false;
		return !searching || ToLower(track.displayName + " " + track.baseName).find(needle) != std::string::npos;
	});

	std::string picked;
	for (const CategoryGroup& group : groups)
	{
		const bool collapsed = m_soundtrackCollapsed[group.category];
		const bool headerOpen = MusicListStyle::CategoryHeaderPlain(group.category, searching || !collapsed);
		if (!searching)
			m_soundtrackCollapsed[group.category] = !headerOpen;
		if (!headerOpen)
			continue;

		ImGui::Indent();
		for (const BgmReplaceableTrack* track : group.tracks)
		{
			bool listed = false;
			for (const LibrarySong& song : songs)
				listed = listed || song.vanillaTrack == track->tableIndex;
			ImGui::PushID(track->tableIndex);
			if (ImGui::Selectable(track->displayName.c_str()))
				picked = mgr.AddVanillaToLibrary(track->tableIndex);
			if (listed)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(%s)", L("in your list").c_str());
			}
			ImGui::PopID();
		}
		ImGui::Unindent();
	}
	if (groups.empty())
		ImGui::TextDisabled("%s", L("No tracks match.").c_str());

	ImGui::EndChild();

	if (ImGui::Button(L("Cancel").c_str(), ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();

	if (!picked.empty())
	{
		ImGui::CloseCurrentPopup();
		m_addQueueTotal = 1;
		OpenTrackPicker(picked);
	}
	ImGui::EndPopup();
}

void BgmReplacementWindow::DrawImportChoiceModal()
{
	BgmReplacementManager& mgr = GetBgmReplacements();
	const std::string popupId = L("Import music replacements") + "##import_choice";
	if (m_openImportChoice)
	{
		ImGui::OpenPopup(popupId.c_str());
		m_openImportChoice = false;
	}
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2((std::min)(520.0f, display.x * 0.9f), 0.0f), ImGuiCond_Always);
	MusicListStyle::OpaquePopup opaque;
	if (!ImGui::BeginPopupModal(popupId.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::TextWrapped("%s", MusicPackage::FileNameOf(m_importPackPath).c_str());
	ImGui::Spacing();
	ImGui::TextWrapped("%s", L("Keep the songs you have now and add these on top? The tracks this file sets are changed either way.").c_str());
	ImGui::Spacing();
	const auto start = [&](bool replace) {
		if (mgr.ImportPack(m_importPackPath, replace))
			m_packMessage.clear();
		m_importPackPath.clear();
		ImGui::CloseCurrentPopup();
	};
	if (ImGui::Button(L("Add on top").c_str()))
		start(false);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Your other replacements stay as they are.").c_str());
	const std::string replaceLabel = L("Replace everything");
	ImGui::SameLineOrWrap(ImGui::ButtonWidth(replaceLabel.c_str()));
	if (ImGui::Button(replaceLabel.c_str()))
		start(true);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Every song goes back to the original first, and your song list is cleared, so you get exactly what is in the file. Export first if you want to keep your current setup.").c_str());
	const std::string cancelLabel = L("Cancel");
	ImGui::SameLineOrWrap(ImGui::ButtonWidth(cancelLabel.c_str()));
	if (ImGui::Button(cancelLabel.c_str()))
	{
		m_importPackPath.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------
// File pickers
// ---------------------------------------------------------------------------------------

void BgmReplacementWindow::OpenTrackPicker(const std::string& songPath)
{
	m_pickerSong = songPath;
	m_openTrackPicker = true;
}

void BgmReplacementWindow::OpenAddSongs()
{
	NativeFileDialog::Request request;
	request.title = L("Add songs");
	request.filters.push_back({ AudioDecode::FilterDescription(), AudioDecode::FilterPattern() });
	request.allowMultiple = true;
	NativeFileDialog::Open(kAddSongsToken, request);
}

void BgmReplacementWindow::OpenSwapFile(const std::string& songPath)
{
	if (NativeFileDialog::IsOpen())
		return;
	NativeFileDialog::Request request;
	request.title = L("Choose the file to use instead of") + " " + MusicPackage::FileNameOf(songPath);
	request.filters.push_back({ AudioDecode::FilterDescription(), AudioDecode::FilterPattern() });
	request.defaultExtension = "mp3";
	if (NativeFileDialog::Open(kSwapFileToken, request))
		m_swapFrom = songPath;
}

void BgmReplacementWindow::PollDialogs()
{
	BgmReplacementManager& mgr = GetBgmReplacements();
	NativeFileDialog::Result result;

	// A song swapped for another file keeps everything it had: its tracks, each one's
	// volume and whether it was on, and its place in the list.
	if (NativeFileDialog::Consume(kSwapFileToken, &result))
	{
		const std::string from = m_swapFrom;
		m_swapFrom.clear();
		if (result.accepted && !result.path.empty() && !SamePath(from, result.path))
		{
			std::vector<int> replaces;
			for (const LibrarySong& song : mgr.GetLibrary())
				if (SamePath(song.path, from)) replaces = song.replaces;
			mgr.AddToLibrary({ result.path });
			for (int index : replaces)
			{
				const float gain = mgr.GetGainDb(index);
				const bool enabled = mgr.IsEnabled(index);
				mgr.Assign(index, result.path, &gain);
				if (!enabled)
					mgr.SetEnabled(index, false);
			}
			mgr.RemoveFromLibrary(from);
			m_expanded[result.path] = m_expanded[from];
			m_lastMessage = MusicPackage::FileNameOf(result.path) + " " + L("now plays in place of") + " " +
				MusicPackage::FileNameOf(from) + ".";
		}
	}

	if (NativeFileDialog::Consume(kAddSongsToken, &result) && result.accepted && !result.paths.empty())
	{
		mgr.AddToLibrary(result.paths);
		// Straight on to what each one replaces, one song after another.
		m_addQueue = result.paths;
		m_addQueueTotal = (int)result.paths.size();
		OpenTrackPicker(m_addQueue.front());
		m_addQueue.erase(m_addQueue.begin());
	}

	if (NativeFileDialog::Consume(kImportPackToken, &result) && result.accepted && !result.path.empty())
	{
		m_importPackPath = result.path;
		m_openImportChoice = true;
	}
	if (NativeFileDialog::Consume(kExportPackToken, &result) && result.accepted && !result.path.empty())
	{
		if (mgr.ExportPack(result.path))
			m_packMessage.clear();
	}
}
