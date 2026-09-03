#include "JukeboxWindow.h"
#include "Core/logger.h"
#include "Core/Settings.h"
#include "Core/HotkeyManager.h"
#include "Core/Localization.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Logger/ImGuiLogger.h"
#include "Overlay/NotificationBar/NotificationBar.h"
#include "Audio/AudioDecode.h"
#include "Audio/BgmReplacementManager.h"
#include "Core/NativeFileDialog.h"
#include "Core/utils.h"

#include <Windows.h>

#include <algorithm>
#include <iomanip>
#include <chrono>
#include <cstdio>
#include <string>

namespace
{
	const char* const kJukeboxDialogOwner = "jukebox_window";
	const char* const kCustomDirRel = "data/Sound/BGM/custom";

	// Centres a row of buttons whose combined width is known, so a dialog's actions sit
	// under the middle of its text rather than hugging the left edge.
	void CenterNextButtonsRow(float totalWidth)
	{
		const float offset = (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - totalWidth) * 0.5f);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
	}

	std::string FormatDb(float value)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%+.1f dB", value);
		return buf;
	}

	std::string FileNameOf(const std::string& path)
	{
		const size_t slash = path.find_last_of("\\/");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}
}

JukeboxWindow::JukeboxWindow(const std::string& windowTitle, bool windowClosable, ImGuiWindowFlags windowFlags)
	: IWindow(windowTitle, windowClosable, windowFlags)
{
}

void JukeboxWindow::BeforeDraw() {
	// Sensible default size on first appearance (the window is no longer
	// auto-resized; the track list fills the remaining space and scrolls).
	ImGui::SetNextWindowSize(ImVec2(440, 640), ImGuiCond_FirstUseEver);
}

void JukeboxWindow::Draw() {
	MusicManager& musicManager = GetMusicManager();
	musicManager.StartCustomMusicDiscovery();
	musicManager.PollCustomMusicDiscovery();
	// Both of these can rewrite the track list, so they run before anything walks it:
	// DrawTrackList holds pointers into it for the length of one frame.
	musicManager.SyncReplacementTracks();
	PollImportDialog();
	PollRestartAfterVolume();

	DrawControls();
	ImGui::SameLine();
	DrawImportButton();
	ImGui::Separator();
	DrawCurrentTrackInfo();
	ImGui::Separator();
	DrawTrackList();
	DrawDeleteTrackModal();
}

void JukeboxWindow::DrawControls() {
	MusicManager& musicManager = GetMusicManager();

	ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", L("Jukebox").c_str());

	ImGui::Spacing();

	if (musicManager.IsCustomMusicLoading()) {
		std::string status = musicManager.GetCustomMusicStatus();
		ImGui::Text("%s", L("Loading custom music...").c_str());
		ImGui::ProgressBar(musicManager.GetCustomMusicProgress(), ImVec2(-1, 0), "");
		ImGui::TextDisabledWrapped("%s", status.c_str());
		ImGui::Spacing();
	} else if (musicManager.HasStartedCustomMusicDiscovery()) {
		std::string status = musicManager.GetCustomMusicStatus();
		ImGui::TextDisabledWrapped("%s", status.c_str());
		ImGui::Spacing();
	}

	// Enable checkbox
	bool enabled = musicManager.IsEnabled();
	if (ImGui::Checkbox(L("Enable Music Rotation").c_str(), &enabled)) {
		musicManager.SetEnabled(enabled);
		musicManager.SavePreferences();
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("Enable/disable automatic music rotation in-game").c_str());

	ImGui::Spacing();

	// Rotation mode (Random and Shuffle are merged: both play in shuffled order)
	ImGui::Text("%s", L("Rotation Mode:").c_str());
	ImGui::SameLine();
	const std::string rotationHelp =
		L("Sequential: play tracks in order") + "\n" +
		L("Shuffle: play tracks in a shuffled order, no repeats until all have played");
	ImGui::ShowHelpMarker(rotationHelp.c_str());

	int modeInt = (musicManager.GetRotationMode() == MusicRotationMode::Sequential) ? 0 : 1;
	const std::string rotationItems = L("Sequential") + '\0' + L("Shuffle") + '\0';
	if (ImGui::Combo("##RotationMode", &modeInt, rotationItems.c_str())) {
		musicManager.SetRotationMode(modeInt == 0 ? MusicRotationMode::Sequential : MusicRotationMode::Shuffle);
		musicManager.SavePreferences();
	}

	ImGui::Text("%s", L("VS/Online Rematch:").c_str());
	ImGui::SameLine();
	const std::string rematchHelp =
		L("Character Select Track: use the song selected at Character Select") + "\n" +
		L("Resume Last Playlist Track: restart the last song played by the Jukebox") + "\n" +
		L("Play Next Playlist Track: advance from the last Jukebox song using the selected rotation mode; if none has played yet, advance from the Character Select song") + "\n\n" +
		L("Only applies to local VS and Online rematches. The first match always uses the Character Select track.");
	ImGui::ShowHelpMarker(rematchHelp.c_str());

	int rematchMode = static_cast<int>(musicManager.GetRematchTrackMode());
	const std::string rematchItems = L("Character Select Track") + '\0' +
		L("Resume Last Playlist Track") + '\0' + L("Play Next Playlist Track") + '\0';
	if (ImGui::Combo("##RematchTrackMode", &rematchMode, rematchItems.c_str())) {
		musicManager.SetRematchTrackMode(static_cast<RematchTrackMode>(rematchMode));
		musicManager.SavePreferences();
	}

	// Repeat settings
	ImGui::HorizontalSpacing();
	bool repeatSingle = musicManager.IsRepeatSingle();
	if (ImGui::Checkbox(L("Repeat Single").c_str(), &repeatSingle)) {
		musicManager.SetRepeatSingle(repeatSingle);
		musicManager.SavePreferences();
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("Repeat the current track instead of playing a new one").c_str());

	ImGui::Spacing();

	// Play button — advance per the rotation mode (Sequential / Shuffle).
	if (ImGui::Button(L("Play Next >|").c_str())) {
		musicManager.PlayNextTrack();
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltipWrapped(L("Play the next track (per the rotation mode)").c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("%s: %s", L("Shortcut").c_str(),
		HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_JukeboxNextTrack)).c_str());

	ImGui::Spacing();

	// Search bar
	ImGui::InputText(L("Search Tracks").c_str(), m_searchBuffer, sizeof(m_searchBuffer),
		ImGuiInputTextFlags_AutoSelectAll);

	ImGui::Spacing();

	// Enable/Disable all
	if (ImGui::Button(L("Enable All").c_str())) {
		for (const auto& track : musicManager.GetAllTracks()) {
			if (!musicManager.IsTrackEnabled(track.id)) {
				musicManager.ToggleTrackEnabled(track.id);
			}
		}
		musicManager.SavePreferences();
	}
	ImGui::SameLine();
	if (ImGui::Button(L("Disable All").c_str())) {
		for (const auto& track : musicManager.GetAllTracks()) {
			if (musicManager.IsTrackEnabled(track.id)) {
				musicManager.ToggleTrackEnabled(track.id);
			}
		}
		musicManager.SavePreferences();
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("Enable/Disable all tracks at once").c_str());

	ImGui::Spacing();

    // Reset button
    if (ImGui::Button(L("Reset Preferences").c_str())) {
        musicManager.ResetPreferences();
    }
    ImGui::SameLine();
    ImGui::ShowHelpMarker(L("Reset all settings and re-enable all tracks").c_str());
}

// Copies a chosen audio file into the custom folder and rescans. Adding music otherwise
// meant finding data/Sound/BGM/custom yourself and restarting the game.
void JukeboxWindow::DrawImportButton() {
	MusicManager& musicManager = GetMusicManager();

	const bool busy = NativeFileDialog::IsOpen() || musicManager.IsCustomMusicLoading();
	ImGui::BeginDisabled(busy);
	if (ImGui::Button(L("Add music...").c_str())) {
		NativeFileDialog::Request request;
		request.title = "Add music to the Jukebox";
		request.filters.push_back({ AudioDecode::FilterDescription(), AudioDecode::FilterPattern() });
		NativeFileDialog::Open(kJukeboxDialogOwner, request);
	}
	ImGui::EndDisabled();

	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltipWrapped(
			L("Copies a song into the Jukebox's custom folder and converts it. Accepts "
			  "every format the mod can read, not just MP3.").c_str());
	}
}

void JukeboxWindow::PollImportDialog() {
	NativeFileDialog::Result result;
	if (!NativeFileDialog::Consume(kJukeboxDialogOwner, &result))
		return;
	if (!result.accepted || result.path.empty())
		return;

	const std::string fileName = FileNameOf(result.path);
	const std::string destDir = GamePath(kCustomDirRel);
	CreateDirectoryW(utf8_to_utf16(destDir).c_str(), NULL);
	const std::string destPath = destDir + "\\" + fileName;

	// The source file has to stay somewhere the scan can find it on later launches, which
	// is what the custom folder is; copying rather than referencing is what makes the
	// track survive the user moving or deleting the original.
	// Wide copy: the chosen path and the name it keeps can both be outside the system
	// codepage, and an ANSI copy would silently mangle them.
	if (CopyFileW(utf8_to_utf16(result.path).c_str(), utf8_to_utf16(destPath).c_str(), FALSE) == 0) {
		const DWORD error = GetLastError();
		if (g_imGuiLogger)
			g_imGuiLogger->Log("[error] Could not copy '%s' into the custom folder (error %lu)\n",
				fileName.c_str(), error);
		if (g_notificationBar)
			g_notificationBar->AddNotification(("Could not add " + fileName).c_str());
		return;
	}

	if (g_imGuiLogger)
		g_imGuiLogger->Log("[system] Added '%s' to the Jukebox's custom folder\n", fileName.c_str());
	if (g_notificationBar)
		g_notificationBar->AddNotification(("Converting " + fileName + "...").c_str());

	GetMusicManager().RescanCustomMusic();
}

// Reload a track whose volume was just changed, if it is the one playing. The volume is
// baked into the converted file, so until the game reads the new one it keeps playing at
// the old level with nothing to show that anything happened.
void JukeboxWindow::PollRestartAfterVolume() {
	if (m_restartAfterVolumeTrackId < 0)
		return;

	MusicManager& musicManager = GetMusicManager();
	const int tableIndex = MusicManager::GetReplacementTableIndex(m_restartAfterVolumeTrackId);
	if (tableIndex >= 0) {
		if (GetBgmReplacements().GetState(tableIndex) == BgmReplacementState::Converting)
			return; // still reconverting
	} else if (musicManager.IsCustomMusicLoading()) {
		return; // still reconverting
	}

	const int trackId = m_restartAfterVolumeTrackId;
	m_restartAfterVolumeTrackId = -1;

	// Only if it is actually playing; reloading anything else would interrupt the music
	// for no reason.
	if (musicManager.GetCurrentTrackId() != trackId)
		return;

	musicManager.PlayTrack(trackId, true);
}

// A small button per row opening a volume popup. A slider on every row would swamp a list
// that is mostly there for choosing what to play.
//
// Both kinds of row bake the level into the converted .pac, because the game plays that file
// through its own XACT engine where the mod has no volume control - so both apply by
// re-converting, and neither costs anything until Apply. What differs is only where the
// stored level lives: a custom song's here, a replacement's in BgmReplacementManager, which
// is the same value the music replacement window edits. Reading and writing through the
// owner is what keeps the two windows showing one number rather than two copies of it.
void JukeboxWindow::DrawTrackVolume(int trackId) {
	MusicManager& musicManager = GetMusicManager();
	BgmReplacementManager& replacements = GetBgmReplacements();

	const int tableIndex = MusicManager::GetReplacementTableIndex(trackId);
	const bool isReplacement = tableIndex >= 0;

	const float saved = isReplacement
		? replacements.GetGainDb(tableIndex)
		: musicManager.GetCustomTrackVolumeDb(trackId);

	const std::string popupId = "##vol" + std::to_string(trackId);

	char label[48];
	if (saved == 0.0f)
		snprintf(label, sizeof(label), "%s%s", L("vol").c_str(), popupId.c_str());
	else
		snprintf(label, sizeof(label), "%+.0f dB%s", saved, popupId.c_str());

	if (ImGui::SmallButton(label))
		ImGui::OpenPopup(popupId.c_str());

	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Set how loud this song is. The file is converted again when you apply it.").c_str());

	if (ImGui::BeginPopup(popupId.c_str())) {
		// The draft only exists while this popup is open, and re-seeds if the stored value
		// moves underneath it - an apply from the music replacement window, or this one's
		// own reconversion landing.
		if (m_volumePopupTrackId != trackId || m_volumeSavedSeen != saved) {
			m_volumePopupTrackId = trackId;
			m_volumeDraft = saved;
			m_volumeSavedSeen = saved;
		}

		// If the song carries a loudness tag it has already been levelled by it, and this
		// slider is an adjustment on top rather than the whole story. Saying so is the
		// difference between "0 dB means untouched" and "0 dB means whatever the tag said".
		float tagGain = 0.0f;
		const bool hasTag = isReplacement
			? replacements.GetTagGainDb(tableIndex, tagGain)
			: musicManager.GetCustomTrackTagGainDb(trackId, tagGain);
		if (hasTag) {
			char note[160];
			snprintf(note, sizeof(note), "%s %+.1f dB", L("Levelled by the song's own tag:").c_str(), tagGain);
			ImGui::TextDisabled("%s", note);
		}

		ImGui::SetNextItemWidth(200.0f);
		ImGui::SliderFloat(L("Adjust").c_str(), &m_volumeDraft, -40.0f, 40.0f, "%+.1f dB");

		// How much of that is free. Up to the song's own headroom nothing is altered at all;
		// past it the loud parts have to be held down to fit, and the song is being squashed
		// rather than turned up. Only measured for replacements so far.
		if (isReplacement) {
			const float headroom = replacements.GetHeadroomDb(tableIndex);
			if (headroom > -200.0f) {
				if (m_volumeDraft > headroom) {
					ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.35f, 1.0f), "%s",
						(L("Above") + " " + FormatDb(headroom) + " " + L("this song gets squashed to fit.")).c_str());
				} else {
					ImGui::TextDisabled("%s", (L("Untouched up to") + " " + FormatDb(headroom)).c_str());
				}
			}
		}

		const bool busy = isReplacement
			? replacements.GetState(tableIndex) == BgmReplacementState::Converting
			: musicManager.IsCustomMusicLoading();

		ImGui::BeginDisabled(busy || m_volumeDraft == saved);
		if (ImGui::Button(L("Apply volume").c_str())) {
			if (isReplacement)
				replacements.SetGainDb(tableIndex, m_volumeDraft);
			else
				musicManager.SetCustomTrackVolumeDb(trackId, m_volumeDraft);
			// The rebuild is asynchronous, and the game is still holding the old file. If
			// this is what is playing, it has to be reloaded once the new one exists.
			m_restartAfterVolumeTrackId = trackId;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();

		if (busy) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s", L("converting...").c_str());
		} else if (m_volumeDraft != saved) {
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.35f, 1.0f), "%s", L("not applied yet").c_str());
		}

		ImGui::EndPopup();
	} else if (m_volumePopupTrackId == trackId) {
		m_volumePopupTrackId = -1;
	}
}

void JukeboxWindow::DrawCurrentTrackInfo() {
	MusicManager& musicManager = GetMusicManager();

	const MusicTrack* currentTrack = musicManager.GetCurrentTrack();
	int currentTrackId = musicManager.GetCurrentTrackId();
	bool inMatch = musicManager.ShouldShowPlayback();

	ImGui::Text("%s", L("Current Track:").c_str());
	ImGui::SameLine();
	if (!inMatch) {
		// The jukebox only drives music during a match.
		ImGui::TextDisabled("%s", L("None (enter a match)").c_str());
	} else if (currentTrack) {
		ImGui::TextColored(ImVec4(0, 1, 1, 1), "%s (ID: %d)", currentTrack->name.c_str(), currentTrack->id);
	} else if (currentTrackId >= 0) {
		const char* filename = MusicManager::GetBgmFilename(currentTrackId);
		if (filename) {
			ImGui::TextDisabled("%s.pac (ID: %d)", filename, currentTrackId);
		} else {
			ImGui::TextDisabled("ID: %d (Unknown track)", currentTrackId);
		}
	} else {
		ImGui::TextDisabled("%s", L("None").c_str());
	}

	// Song timer — frozen at 00:00 / 00:00 when not in a match.
	ImGui::Text("%s", L("Time:").c_str());
	ImGui::SameLine();
	if (!inMatch) {
		ImGui::TextColored(ImVec4(0, 1, 0, 1), "00:00");
		ImGui::SameLine();
		ImGui::TextDisabled("/ 00:00");
	} else {
		ImGui::TextColored(ImVec4(0, 1, 0, 1), "%s", musicManager.GetSongTimeString().c_str());

		int totalFrames = musicManager.GetRotationThresholdFrames();
		if (totalFrames > 0) {
			int totalSec = totalFrames / 60;
			int totalMins = totalSec / 60;
			int totalSecs = totalSec % 60;
			ImGui::SameLine();
			ImGui::TextDisabled("/ %02d:%02d", totalMins, totalSecs);

			// Progress bar
			float progress = (float)musicManager.GetSongPlaybackFrames() / (float)totalFrames;
			if (progress > 1.0f) progress = 1.0f;
			ImGui::ProgressBar(progress, ImVec2(-1, 0), "");
		}
	}

	ImGui::Text("%s: %d / %d", L("Enabled Tracks").c_str(),
		(int)musicManager.GetEnabledTracks().size(),
		(int)musicManager.GetAllTracks().size());
}

// Category colors
static const ImVec4 COLOR_BTL  = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);   // Red - Battle themes
static const ImVec4 COLOR_VS   = ImVec4(0.4f, 0.6f, 1.0f, 1.0f);   // Blue - Versus themes
static const ImVec4 COLOR_BOSS = ImVec4(0.8f, 0.4f, 1.0f, 1.0f);   // Purple - Boss themes
static const ImVec4 COLOR_SYS  = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);   // Yellow - System themes
static const ImVec4 COLOR_OLD  = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);   // Gray - Legacy/old versions
static const ImVec4 COLOR_STORY = ImVec4(0.4f, 1.0f, 0.6f, 1.0f);  // Light Green - Story themes
static const ImVec4 COLOR_ASTRAL = ImVec4(1.0f, 0.6f, 0.2f, 1.0f); // Orange - Astral Finish themes
static const ImVec4 COLOR_CUSTOM = ImVec4(0.2f, 0.8f, 1.0f, 1.0f); // Cyan/Teal - Custom user tracks
static const ImVec4 COLOR_REPLACEMENT = ImVec4(0.6f, 1.0f, 0.4f, 1.0f); // Green - BGM replacements

static ImVec4 GetCategoryColor(const std::string& category) {
	if (category == "btl") return COLOR_BTL;
	if (category == "vs") return COLOR_VS;
	if (category == "boss") return COLOR_BOSS;
	if (category == "sys" || category == "sysex") return COLOR_SYS;
	if (category == "old") return COLOR_OLD;
	if (category == "story") return COLOR_STORY;
	if (category == "astral") return COLOR_ASTRAL;
	if (category == "custom") return COLOR_CUSTOM;
	if (category == "replacements") return COLOR_REPLACEMENT;
	return ImVec4(1, 1, 1, 1);
}

// Only a row the user put there can be deleted: a native track is one of the game's own
// files. A replacement is removed through Unassign, which puts the table pointer back as
// well as deleting the converted file - which is why the confirmation says so.
void JukeboxWindow::DrawTrackContextMenu(const MusicTrack& track) {
	const bool ownedByUser = MusicManager::IsCustomTrackId(track.id) ||
		MusicManager::IsReplacementTrackId(track.id);
	if (!ownedByUser) {
		return;
	}

	const std::string contextId = "##ctx" + std::to_string(track.id);
	if (ImGui::BeginPopupContextItem(contextId.c_str())) {
		if (ImGui::MenuItem(L("Delete track").c_str())) {
			m_deleteRequestTrackId = track.id;
			m_deleteRequestName = track.name;
			m_deleteModalQueued = true;
		}
		ImGui::EndPopup();
	}
}

void JukeboxWindow::DrawDeleteTrackModal() {
	// A translated caption over a stable id: ImGui hashes only what follows the ###, so
	// changing language cannot orphan a popup that is already open.
	const std::string modalTitle = L("Delete track") + "###jukebox_delete_track";

	if (m_deleteModalQueued) {
		m_deleteModalQueued = false;
		// Positioned and sized on the frame it opens, which is the frame BeginPopupModal
		// below first begins the window. DisplaySize rather than the viewport, because the
		// mod overrides it when scaling the overlay and that is the space windows live in.
		const ImVec2 displayCenter = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
			ImGui::GetIO().DisplaySize.y * 0.5f);
		ImGui::SetNextWindowPos(displayCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
		ImGui::OpenPopup(modalTitle.c_str());
	}
	if (m_deleteRequestTrackId < 0) {
		return;
	}
	// Escape dismisses a modal without going through either button, so the request is
	// dropped once the popup is gone rather than left pointing at a track that may not
	// even be in the list any more.
	if (!ImGui::IsPopupOpen(modalTitle.c_str())) {
		m_deleteRequestTrackId = -1;
		m_deleteRequestName.clear();
		return;
	}

	if (!ImGui::BeginPopupModal(modalTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		return;
	}

	const int trackId = m_deleteRequestTrackId;
	const int tableIndex = MusicManager::GetReplacementTableIndex(trackId);
	const bool isReplacement = tableIndex >= 0;

	ImGui::TextWrapped("%s", L("Remove this track from the Jukebox?").c_str());
	ImGui::Spacing();

	// Coloured like its row in the list, so it is obvious which of the two kinds this is
	// before reading the explanation under it.
	ImGui::PushStyleColor(ImGuiCol_Text, GetCategoryColor(isReplacement ? "replacements" : "custom"));
	ImGui::TextWrapped("%s", m_deleteRequestName.c_str());
	ImGui::PopStyleColor();

	ImGui::Spacing();
	if (isReplacement) {
		ImGui::TextWrapped("%s", L("This removes the replacement itself, so the track it stands in for goes back to the original song everywhere - the music replacement window included. The song you picked is not touched; only the converted copy is deleted.").c_str());
	} else {
		ImGui::TextWrapped("%s", L("The imported song and its converted copy are both deleted from the Jukebox's custom folder. This cannot be undone.").c_str());
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	const float buttonWidth = 130.0f;
	CenterNextButtonsRow(buttonWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x);

	// Tinted red: it is the destructive half of the choice, and a confirmation whose two
	// buttons look identical is one people click through without reading.
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.18f, 0.18f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.24f, 0.24f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.30f, 0.30f, 1.0f));
	const bool confirmed = ImGui::Button(L("Delete track").c_str(), ImVec2(buttonWidth, 0.0f));
	ImGui::PopStyleColor(3);

	if (confirmed) {
		bool ok = true;
		if (isReplacement) {
			GetBgmReplacements().Unassign(tableIndex);
		} else {
			ok = GetMusicManager().DeleteCustomTrack(trackId);
		}
		if (g_notificationBar) {
			g_notificationBar->AddNotification(ok
				? ("Deleted " + m_deleteRequestName).c_str()
				: ("Could not delete " + m_deleteRequestName).c_str());
		}
		m_deleteRequestTrackId = -1;
		m_deleteRequestName.clear();
		ImGui::CloseCurrentPopup();
	}

	ImGui::SameLine();
	if (ImGui::Button(L("Cancel").c_str(), ImVec2(buttonWidth, 0.0f))) {
		m_deleteRequestTrackId = -1;
		m_deleteRequestName.clear();
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}

void JukeboxWindow::DrawTrackList() {
	MusicManager& musicManager = GetMusicManager();

	const auto& allTracks = musicManager.GetAllTracks();

	// Fill the remaining vertical space so only this track container scrolls
	// (the window itself has NoScrollbar — no second scrollbar).
	float childHeight = ImGui::GetContentRegionAvail().y;
	if (childHeight < 100.0f) childHeight = 100.0f;

	ImGui::BeginChild("##TrackList", ImVec2(0, childHeight), true);

	// Build filtered track list
	std::vector<const MusicTrack*> filteredTracks;
	std::string searchStr(m_searchBuffer);
	std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::tolower);

	for (const auto& track : allTracks) {
		if (searchStr.empty()) {
			filteredTracks.push_back(&track);
		} else {
			std::string trackName = track.name;
			std::transform(trackName.begin(), trackName.end(), trackName.begin(), ::tolower);
			if (trackName.find(searchStr) != std::string::npos ||
				(std::to_string(track.id)).find(searchStr) != std::string::npos) {
				filteredTracks.push_back(&track);
			}
		}
	}

	// Render tracks grouped by category
	std::string lastCategory;
	bool categoryOpen = true;
	for (const MusicTrack* track : filteredTracks) {
	// Draw category header with a "check all" toggle for the whole category
		if (track->category != lastCategory) {
			lastCategory = track->category;
			ImVec4 catColor = GetCategoryColor(lastCategory);
			ImGui::Separator();
			int catState = musicManager.GetCategoryEnabledState(lastCategory);
			bool catAllOn = (catState == 1);
			if (ImGui::Checkbox(("##CatAll_" + lastCategory).c_str(), &catAllOn)) {
				musicManager.SetCategoryEnabled(lastCategory, catAllOn);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltipWrapped(catState == -1
					? L("Mixed: click to enable ALL tracks in this category").c_str()
					: L("Enable/disable ALL tracks in this category").c_str());
			}
			ImGui::SameLine();

			// Forced open/closed from our own saved state every frame rather than left to
			// ImGui's, which does not survive the session. The return value is still the
			// user's click, so a toggle is caught and written back below.
			// While searching, every category is forced open: a hit hidden inside a
			// collapsed category makes the search look broken, and rows shown under a
			// shut header look like a bug. The saved state is left alone, so clearing
			// the search puts the collapsed ones back.
			const bool searching = !searchStr.empty();
			const bool wasExpanded = musicManager.IsCategoryExpanded(lastCategory);
			ImGui::SetNextItemOpen(searching || wasExpanded, ImGuiCond_Always);
			ImGui::PushStyleColor(ImGuiCol_Text, catColor);
			char header[96];
			snprintf(header, sizeof(header), "[ %s ]##Cat_%s",
				lastCategory.c_str(), lastCategory.c_str());
			categoryOpen = ImGui::CollapsingHeader(header);
			ImGui::PopStyleColor();
			if (!searching && categoryOpen != wasExpanded) {
				musicManager.SetCategoryExpanded(lastCategory, categoryOpen);
			}
		}

		if (!categoryOpen) {
			continue;
		}

		bool enabled = musicManager.IsTrackEnabled(track->id);
		const MusicTrack* currentTrack = musicManager.GetCurrentTrack();
		bool isCurrent = (currentTrack && currentTrack->id == track->id);

		if (isCurrent) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 0, 1));
		}

		if (ImGui::Checkbox(("##Track" + std::to_string(track->id)).c_str(), &enabled)) {
			bool currentState = musicManager.IsTrackEnabled(track->id);
			if (currentState != enabled) {
				musicManager.ToggleTrackEnabled(track->id);
			}
		}

		ImGui::SameLine();

		// Custom songs and replacements both get a volume of their own. Native tracks are
		// already balanced against each other by the game, so there is nothing to fix there.
		const bool isReplacement = MusicManager::IsReplacementTrackId(track->id);
		const bool hasVolume = MusicManager::IsCustomTrackId(track->id) || isReplacement;

		// A Selectable spans the rest of the line by default, which puts it underneath
		// anything drawn after it on the same row - that is why the volume button could
		// not be clicked, the row was swallowing the press. Reserve the space instead.
		const float volumeWidth = hasVolume ? 70.0f : 0.0f;
		const float selectableWidth = (std::max)(1.0f, ImGui::GetContentRegionAvail().x - volumeWidth);

		if (ImGui::Selectable((std::to_string(track->id) + ": " + track->name + "##Sel" + std::to_string(track->id)).c_str(),
			isCurrent, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap,
			ImVec2(selectableWidth, 0.0f)) && ImGui::IsMouseDoubleClicked(0)) {
			musicManager.PlayTrack(track->id);
		}

		if (ImGui::IsItemHovered() && isReplacement) {
			ImGui::SetTooltipWrapped(L("Plays the replacement itself. The track it stands in for is still listed separately and still plays the original. Its volume is the same setting the music replacement window shows.").c_str());
		}

		// Attached to the Selectable, so it is the row that answers a right-click. It has
		// to come after the hover check above: the MenuItem drawn inside the popup becomes
		// the last item, and IsItemHovered would then be asking about that instead.
		DrawTrackContextMenu(*track);

		if (isCurrent) {
			ImGui::PopStyleColor();
		}

		if (hasVolume) {
			ImGui::SameLine();
			DrawTrackVolume(track->id);
		}
	}

	ImGui::EndChild();
}
