#include "JukeboxWindow.h"
#include "Core/logger.h"
#include "Core/Settings.h"
#include "Core/HotkeyManager.h"
#include "Core/Localization.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Logger/ImGuiLogger.h"
#include "Overlay/NotificationBar/NotificationBar.h"
#include "Audio/AudioDecode.h"
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
	if (musicManager.IsCustomMusicLoading())
		return; // still reconverting

	const int trackId = m_restartAfterVolumeTrackId;
	m_restartAfterVolumeTrackId = -1;

	// Only if it is actually playing; reloading anything else would interrupt the music
	// for no reason.
	if (musicManager.GetCurrentTrackId() != trackId)
		return;

	musicManager.PlayTrack(trackId, true);
}

// A small button per custom track opening a volume popup. A slider on every row would
// swamp a list that is mostly there for choosing what to play.
void JukeboxWindow::DrawCustomTrackVolume(int trackId) {
	MusicManager& musicManager = GetMusicManager();

	const std::string popupId = "##vol" + std::to_string(trackId);
	const float current = musicManager.GetCustomTrackVolumeDb(trackId);

	char label[48];
	if (current == 0.0f)
		snprintf(label, sizeof(label), "%s%s", L("vol").c_str(), popupId.c_str());
	else
		snprintf(label, sizeof(label), "%+.0f dB%s", current, popupId.c_str());

	if (ImGui::SmallButton(label))
		ImGui::OpenPopup(popupId.c_str());

	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Set how loud this song is. The file is converted again when you apply it.").c_str());

	if (ImGui::BeginPopup(popupId.c_str())) {
		// The draft only exists while this popup is open.
		if (m_volumePopupTrackId != trackId) {
			m_volumePopupTrackId = trackId;
			m_volumeDraft = current;
		}

		// If the song carries a loudness tag it has already been levelled by it, and this
		// slider is an adjustment on top rather than the whole story. Saying so is the
		// difference between "0 dB means untouched" and "0 dB means whatever the tag said".
		float tagGain = 0.0f;
		if (musicManager.GetCustomTrackTagGainDb(trackId, tagGain)) {
			char note[128];
			snprintf(note, sizeof(note), "%s %+.1f dB", L("Levelled by the song's own tag:").c_str(), tagGain);
			ImGui::TextDisabled("%s", note);
		}

		ImGui::SetNextItemWidth(200.0f);
		ImGui::SliderFloat(L("Adjust").c_str(), &m_volumeDraft, -40.0f, 40.0f, "%+.1f dB");

		const bool busy = musicManager.IsCustomMusicLoading();
		ImGui::BeginDisabled(busy || m_volumeDraft == current);
		if (ImGui::Button(L("Apply volume").c_str())) {
			musicManager.SetCustomTrackVolumeDb(trackId, m_volumeDraft);
			// The rebuild is asynchronous, and the game is still holding the old file. If
			// this is what is playing, it has to be reloaded once the new one exists.
			m_restartAfterVolumeTrackId = trackId;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();

		if (m_volumeDraft != current) {
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
					? "Mixed: click to enable ALL tracks in this category"
					: "Enable/disable ALL tracks in this category");
			}
			ImGui::SameLine();
			ImGui::TextColored(catColor, "[ %s ]", lastCategory.c_str());
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

		// Custom tracks get a volume of their own. Native ones are already balanced
		// against each other by the game, so there is nothing to fix there - and a
		// replacement's gain belongs to the replacement browser, which owns the
		// reconversion that applying it needs, so it is deliberately not offered twice.
		const bool isCustom = MusicManager::IsCustomTrackId(track->id);
		const bool isReplacement = MusicManager::IsReplacementTrackId(track->id);

		// A Selectable spans the rest of the line by default, which puts it underneath
		// anything drawn after it on the same row - that is why the volume button could
		// not be clicked, the row was swallowing the press. Reserve the space instead.
		const float volumeWidth = isCustom ? 70.0f : 0.0f;
		const float selectableWidth = (std::max)(1.0f, ImGui::GetContentRegionAvail().x - volumeWidth);

		if (ImGui::Selectable((std::to_string(track->id) + ": " + track->name + "##Sel" + std::to_string(track->id)).c_str(),
			isCurrent, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap,
			ImVec2(selectableWidth, 0.0f)) && ImGui::IsMouseDoubleClicked(0)) {
			musicManager.PlayTrack(track->id);
		}

		if (ImGui::IsItemHovered() && isReplacement) {
			ImGui::SetTooltipWrapped(L("Plays the replacement itself. The track it stands in for is still listed separately and still plays the original. Set its volume in the music replacement window.").c_str());
		}

		if (isCurrent) {
			ImGui::PopStyleColor();
		}

		if (isCustom) {
			ImGui::SameLine();
			DrawCustomTrackVolume(track->id);
		}
	}

	ImGui::EndChild();
}
