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
#include "MusicListStyle.h"
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
	const char* const kPlaylistSaveOwner = "jukebox_playlist_save";
	const char* const kPlaylistLoadOwner = "jukebox_playlist_load";

	std::string PlaylistFilterDescription()
	{
		return L("Improvement Mod Jukebox playlist") + " (*." + MusicPackage::kPlaylistExtension + ")";
	}
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
	PollPlaylistDialogs();
	musicManager.PollPlaylistJobs();
	PollRestartAfterVolume();
	DrawConfirmLoadModal();

	DrawCurrentTrackInfo();
	ImGui::Spacing();
	DrawControls();
	DrawTrackList();
	DrawDeleteTrackModal();
}

// The everyday controls on two rows - rotation on/off with its options behind one button,
// then adding music, playlists and the search - and nothing else. Every setting the window
// used to lay out in full is one click away in the Options popup.
void JukeboxWindow::DrawControls() {
	MusicManager& musicManager = GetMusicManager();

	bool enabled = musicManager.IsEnabled();
	if (ImGui::Checkbox(L("Enable Music Rotation").c_str(), &enabled)) {
		musicManager.SetEnabled(enabled);
		musicManager.SavePreferences();
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("Enable/disable automatic music rotation in-game").c_str());

	const std::string optionsLabel = L("Options");
	ImGui::SameLine((std::max)(ImGui::GetCursorPosX(),
		ImGui::GetContentRegionMax().x - ImGui::ButtonWidth(optionsLabel.c_str())));
	if (ImGui::Button(optionsLabel.c_str()))
		ImGui::OpenPopup("##jukebox_options");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Rotation mode, what plays when a match starts and on a rematch, repeat, and resetting everything.").c_str());
	MusicListStyle::AnchorNextPopupBelowLastItem();
	DrawOptionsPopup();

	// Row two: add music, playlists, search.
	DrawImportButton();
	ImGui::SameLine();
	const std::string playlistLabel = L("Playlist");
	if (ImGui::Button((playlistLabel + "  v").c_str()))
		ImGui::OpenPopup("##jukebox_playlists");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("A playlist file holds all of your music: the ticked tracks, the settings above, your custom songs and your music replacements, the song files included. Save one per mood and swap between them any time, or share it as a music pack.").c_str());
	MusicListStyle::AnchorNextPopupBelowLastItem();
	DrawPlaylistMenu();

	char ticked[64];
	snprintf(ticked, sizeof(ticked), "%d / %d", (int)musicManager.GetEnabledTracks().size(),
		(int)musicManager.GetAllTracks().size());
	const float tickedWidth = ImGui::CalcTextSize(ticked).x;
	ImGui::SameLine();
	ImGui::SetNextItemWidth((std::max)(80.0f, ImGui::GetContentRegionAvail().x - tickedWidth - ImGui::GetStyle().ItemSpacing.x));
	ImGui::InputTextWithHint("##search", L("Search Tracks").c_str(), m_searchBuffer, sizeof(m_searchBuffer),
		ImGuiInputTextFlags_AutoSelectAll);
	ImGui::SameLine();
	ImGui::TextDisabled("%s", ticked);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Ticked tracks out of all tracks. Only ticked tracks play in rotation.").c_str());

	// Only while something is actually happening.
	if (musicManager.IsCustomMusicLoading()) {
		ImGui::TextDisabled("%s", L("Loading custom music...").c_str());
		ImGui::ProgressBar(musicManager.GetCustomMusicProgress(), ImVec2(-1, 3.0f), "");
	}
	std::string message;
	bool ok = true;
	if (musicManager.ConsumePlaylistMessage(message, ok)) {
		m_playlistMessage = message;
		m_playlistMessageOk = ok;
	}
	// A playlist can carry hundreds of megabytes of songs, all of which may need
	// converting, so a load shows each stage and how far it has got.
	const MusicManager::PlaylistProgress progress = musicManager.GetPlaylistProgress();
	if (progress.stage != MusicManager::PlaylistProgress::None) {
		const char* stage = "Working on the playlist";
		switch (progress.stage) {
		case MusicManager::PlaylistProgress::Saving:       stage = "Saving the playlist's songs"; break;
		case MusicManager::PlaylistProgress::Unpacking:    stage = "Unpacking the playlist's songs"; break;
		case MusicManager::PlaylistProgress::CustomSongs:  stage = "Converting custom songs"; break;
		case MusicManager::PlaylistProgress::Replacements: stage = "Converting music replacements"; break;
		default: break;
		}
		std::string label = L(stage);
		if (progress.total > 0)
			label += ": " + std::to_string((std::min)(progress.done + 1, progress.total)) + " / " + std::to_string(progress.total);
		ImGui::TextColoredWrapped(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%s", label.c_str());
		char percent[16];
		snprintf(percent, sizeof(percent), "%d%%", (int)(progress.fraction * 100.0f + 0.5f));
		ImGui::ProgressBar(progress.fraction, ImVec2(-1, 0.0f), percent);
	}
	else if (musicManager.IsPlaylistBusy())
		ImGui::TextColoredWrapped(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%s", L("Working on the playlist, please wait...").c_str());
	else if (!m_playlistMessage.empty())
		ImGui::TextColoredWrapped(m_playlistMessageOk ? ImVec4(0.35f, 0.90f, 0.65f, 1.0f) : ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
			"%s", m_playlistMessage.c_str());
}

void JukeboxWindow::DrawOptionsPopup() {
	MusicListStyle::OpaquePopup opaque;
	if (!ImGui::BeginPopup("##jukebox_options"))
		return;
	MusicManager& musicManager = GetMusicManager();
	// Wide enough for the longest answer, so no choice is cut off.
	float longest = 0.0f;
	for (const char* key : { "Play the track picked at Character Select", "Play the next track in the playlist",
		"Play the last track the Jukebox played" })
		longest = (std::max)(longest, ImGui::CalcTextSize(L(key).c_str()).x);
	const float comboWidth = longest + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 3.0f;

	// Rotation mode (Random and Shuffle are merged: both play in shuffled order)
	ImGui::Text("%s", L("Rotation Mode:").c_str());
	ImGui::SameLine();
	const std::string rotationHelp =
		L("Sequential: play tracks in order") + "\n" +
		L("Shuffle: play tracks in a shuffled order, no repeats until all have played");
	ImGui::ShowHelpMarker(rotationHelp.c_str());
	int modeInt = (musicManager.GetRotationMode() == MusicRotationMode::Sequential) ? 0 : 1;
	const std::string rotationItems = L("Sequential") + '\0' + L("Shuffle") + '\0';
	ImGui::SetNextItemWidth(comboWidth);
	if (ImGui::Combo("##RotationMode", &modeInt, rotationItems.c_str())) {
		musicManager.SetRotationMode(modeInt == 0 ? MusicRotationMode::Sequential : MusicRotationMode::Shuffle);
		musicManager.SavePreferences();
	}

	// Which song a match opens on. Two questions, not one: people want the game's own
	// pick for the first match of a set and the playlist from the rematch on, or the
	// playlist from the very first match, and one combined control could never say which
	// of those it was doing. The wording is the whole point here - each option says what
	// you will hear, not which internal mode it selects.
	//
	// Both are dead while rotation is off: MusicManager::Update returns before it can
	// apply anything, so they grey out and say why instead of quietly doing nothing.
	const bool rotationOn = musicManager.IsEnabled();
	const std::string choiceItems =
		L("Play the track picked at Character Select") + '\0' +
		L("Play the next track in the playlist") + '\0' +
		L("Play the last track the Jukebox played") + '\0';

	ImGui::Spacing();
	ImGui::BeginDisabled(!rotationOn);
	ImGui::Text("%s", L("When a match starts:").c_str());
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("What plays when you enter a match - Training, local VS or Online. Leave it on the Character Select track to keep the song the game picked; pick a playlist option to hear only the tracks you have ticked below.").c_str());
	int matchStartChoice = static_cast<int>(musicManager.GetMatchStartTrackChoice());
	ImGui::SetNextItemWidth(comboWidth);
	if (ImGui::Combo("##MatchStartTrack", &matchStartChoice, choiceItems.c_str())) {
		musicManager.SetMatchStartTrackChoice(static_cast<MatchTrackChoice>(matchStartChoice));
		musicManager.SavePreferences();
	}

	ImGui::Text("%s", L("When you rematch:").c_str());
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("What plays on the next match of a set in local VS and Online, after the victory screen. Training never gets here - it uses the setting above.").c_str());
	int rematchChoice = static_cast<int>(musicManager.GetRematchTrackChoice());
	ImGui::SetNextItemWidth(comboWidth);
	if (ImGui::Combo("##RematchTrack", &rematchChoice, choiceItems.c_str())) {
		musicManager.SetRematchTrackChoice(static_cast<MatchTrackChoice>(rematchChoice));
		musicManager.SavePreferences();
	}
	ImGui::EndDisabled();
	if (!rotationOn) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + comboWidth);
		ImGui::TextDisabledWrapped("%s", L("Turn on \"Enable Music Rotation\" above for these two to do anything.").c_str());
		ImGui::PopTextWrapPos();
	}

	ImGui::Spacing();
	bool repeatSingle = musicManager.IsRepeatSingle();
	if (ImGui::Checkbox(L("Repeat Single").c_str(), &repeatSingle)) {
		musicManager.SetRepeatSingle(repeatSingle);
		musicManager.SavePreferences();
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("Repeat the current track instead of playing a new one").c_str());

	ImGui::Separator();
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
	if (ImGui::Button(L("Reset Preferences").c_str())) {
		musicManager.ResetPreferences();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Reset all settings and re-enable all tracks").c_str());

	ImGui::TextDisabled("%s: %s %s, %s %s", L("Shortcuts").c_str(),
		HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_JukeboxNextTrack)).c_str(),
		L("next track").c_str(),
		HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_ToggleJukebox)).c_str(),
		L("this window").c_str());
	ImGui::EndPopup();
}

// Copies chosen audio files into the custom folder and rescans. Adding music otherwise
// meant finding data/Sound/BGM/custom yourself and restarting the game.
void JukeboxWindow::DrawImportButton() {
	MusicManager& musicManager = GetMusicManager();

	const bool busy = NativeFileDialog::IsOpen() || musicManager.IsCustomMusicLoading();
	ImGui::BeginDisabled(busy);
	if (ImGui::Button(L("Add music...").c_str())) {
		NativeFileDialog::Request request;
		request.title = "Add music to the Jukebox";
		request.filters.push_back({ AudioDecode::FilterDescription(), AudioDecode::FilterPattern() });
		request.allowMultiple = true;
		NativeFileDialog::Open(kJukeboxDialogOwner, request);
	}
	ImGui::EndDisabled();

	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltipWrapped((L("Copies songs into the Jukebox's custom folder and converts them. Accepts "
			"every format the mod can read, not just MP3.") + "\n\n" + musicManager.GetCustomMusicStatus()).c_str());
	}
}



void JukeboxWindow::PollImportDialog() {
	NativeFileDialog::Result result;
	if (!NativeFileDialog::Consume(kJukeboxDialogOwner, &result))
		return;
	if (!result.accepted || result.paths.empty())
		return;

	const std::string destDir = GamePath(kCustomDirRel);
	CreateDirectoryW(utf8_to_utf16(destDir).c_str(), NULL);
	int added = 0;
	for (const std::string& path : result.paths) {
	const std::string fileName = FileNameOf(path);
	const std::string destPath = destDir + "\\" + fileName;

	// The source file has to stay somewhere the scan can find it on later launches, which
	// is what the custom folder is; copying rather than referencing is what makes the
	// track survive the user moving or deleting the original.
	// Wide copy: the chosen path and the name it keeps can both be outside the system
	// codepage, and an ANSI copy would silently mangle them.
	if (CopyFileW(utf8_to_utf16(path).c_str(), utf8_to_utf16(destPath).c_str(), FALSE) == 0) {
		const DWORD error = GetLastError();
		if (g_imGuiLogger)
			g_imGuiLogger->Log("[error] Could not copy '%s' into the custom folder (error %lu)\n",
				fileName.c_str(), error);
		if (g_notificationBar)
			g_notificationBar->AddNotification(("Could not add " + fileName).c_str());
		continue;
	}

	if (g_imGuiLogger)
		g_imGuiLogger->Log("[system] Added '%s' to the Jukebox's custom folder\n", fileName.c_str());
	++added;
	}
	if (added == 0)
		return;
	if (g_notificationBar)
		g_notificationBar->AddNotification(("Converting " + std::to_string(added) + " song(s)...").c_str());

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

	if (musicManager.IsInMatch())
		musicManager.PlayTrack(trackId, true, "volume-change-reload");
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

	if (MusicListStyle::VolumeButton(saved, popupId.c_str()))
		ImGui::OpenPopup(popupId.c_str());

	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Set how loud this song is. The file is converted again when you apply it.").c_str());

	MusicListStyle::AnchorNextPopupBelowLastItem();
	MusicListStyle::OpaquePopup opaque;
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

// Now playing: the track, its time and a thin progress bar, with Next beside it. Outside a
// match the Jukebox drives nothing, so it says that instead of showing a stale track.
void JukeboxWindow::DrawCurrentTrackInfo() {
	MusicManager& musicManager = GetMusicManager();

	const MusicTrack* currentTrack = musicManager.GetCurrentTrack();
	const int currentTrackId = musicManager.GetCurrentTrackId();
	const bool inMatch = musicManager.ShouldShowPlayback();

	const std::string nextLabel = L("Play Next >|");
	const float nextWidth = ImGui::ButtonWidth(nextLabel.c_str());
	ImGui::BeginGroup();
	ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - nextWidth - ImGui::GetStyle().ItemSpacing.x);
	if (!inMatch) {
		ImGui::TextDisabledWrapped("%s", L("Nothing playing - the Jukebox plays during matches.").c_str());
	} else if (currentTrack) {
		ImGui::PushStyleColor(ImGuiCol_Text, MusicListStyle::CategoryColor(currentTrack->category));
		ImGui::TextWrapped("%s", currentTrack->name.c_str());
		ImGui::PopStyleColor();
	} else if (currentTrackId >= 0) {
		const char* filename = MusicManager::GetBgmFilename(currentTrackId);
		ImGui::TextDisabledWrapped("%s", filename ? (std::string(filename) + ".pac").c_str() : L("Unknown track").c_str());
	} else {
		ImGui::TextDisabled("%s", L("None").c_str());
	}
	ImGui::PopTextWrapPos();

	if (inMatch) {
		const int totalFrames = musicManager.GetRotationThresholdFrames();
		const int totalSec = totalFrames > 0 ? totalFrames / 60 : 0;
		ImGui::TextDisabled("%s / %02d:%02d", musicManager.GetSongTimeString().c_str(), totalSec / 60, totalSec % 60);
	}
	ImGui::EndGroup();

	ImGui::SameLine((std::max)(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - nextWidth));
	ImGui::BeginDisabled(!musicManager.IsInMatch());
	if (ImGui::Button(nextLabel.c_str())) {
		musicManager.PlayNextTrack();
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltipWrapped((L("Play the next track (per the rotation mode)") + "  (" +
			HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_JukeboxNextTrack)) + ")").c_str());
	}

	if (inMatch) {
		const int totalFrames = musicManager.GetRotationThresholdFrames();
		float progress = totalFrames > 0 ? (float)musicManager.GetSongPlaybackFrames() / (float)totalFrames : 0.0f;
		if (progress > 1.0f) progress = 1.0f;
		ImGui::ProgressBar(progress, ImVec2(-1, 3.0f), "");
	}
}

// Shared with the music replacement window, so both lists use the same colours.
static ImVec4 GetCategoryColor(const std::string& category) {
	return MusicListStyle::CategoryColor(category);
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

	MusicListStyle::OpaquePopup opaque;
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
			const int catState = musicManager.GetCategoryEnabledState(lastCategory);

			// Forced open/closed from our own saved state every frame rather than left to
			// ImGui's, which does not survive the session. The return value is still the
			// user's click, so a toggle is caught and written back below.
			// While searching, every category is forced open: a hit hidden inside a
			// collapsed category makes the search look broken, and rows shown under a
			// shut header look like a bug. The saved state is left alone, so clearing
			// the search puts the collapsed ones back.
			const bool searching = !searchStr.empty();
			const bool wasExpanded = musicManager.IsCategoryExpanded(lastCategory);
			ImGui::Spacing();
			const MusicListStyle::HeaderResult header = MusicListStyle::CategoryHeader(lastCategory, catState, true,
				searching || wasExpanded, L("Enable/disable ALL tracks in this category").c_str());
			if (header.checkboxClicked) {
				musicManager.SetCategoryEnabled(lastCategory, header.checkTo);
			}
			categoryOpen = header.open;
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

		if (ImGui::Selectable((track->name + "##Sel" + std::to_string(track->id)).c_str(),
			isCurrent, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap,
			ImVec2(selectableWidth, 0.0f)) && ImGui::IsMouseDoubleClicked(0)) {
			// Inside a match only: playing a track anywhere else takes the game's BGM slot
			// over and breaks the next character select (no cursor, no input).
			if (musicManager.IsInMatch())
				musicManager.PlayTrack(track->id, false, "user-pick");
		}

		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
			// The id is what search also matches; the list itself stays names only.
			std::string tip = track->name + "  (ID " + std::to_string(track->id) + ")\n" +
				(musicManager.IsInMatch() ? L("Double-click to play it now.") : L("Double-click it during a match to play it."));
			if (isReplacement)
				tip += "\n" + L("Plays the replacement itself. The track it stands in for is still listed separately and still plays the original. Its volume is the same setting the music replacement window shows.");
			ImGui::SetTooltipWrapped(tip.c_str());
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

void JukeboxWindow::DrawPlaylistMenu() {
	MusicListStyle::OpaquePopup opaque;
	if (!ImGui::BeginPopup("##jukebox_playlists"))
		return;
	MusicManager& musicManager = GetMusicManager();
	const bool busy = NativeFileDialog::IsOpen() || musicManager.IsPlaylistBusy();

	if (ImGui::MenuItem(L("Save playlist...").c_str(), nullptr, false, !busy)) {
		NativeFileDialog::Request request;
		request.save = true;
		request.title = L("Save this Jukebox playlist");
		request.filters.push_back({ PlaylistFilterDescription(), std::string("*.") + MusicPackage::kPlaylistExtension });
		request.defaultExtension = MusicPackage::kPlaylistExtension;
		request.initialPath = std::string("My playlist.") + MusicPackage::kPlaylistExtension;
		NativeFileDialog::Open(kPlaylistSaveOwner, request);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Saves your whole music setup into one file: ticked tracks, every option, custom songs and music replacements, song files included.").c_str());
	if (ImGui::MenuItem(L("Load playlist...").c_str(), nullptr, false, !busy)) {
		NativeFileDialog::Request request;
		request.title = L("Load a Jukebox playlist");
		request.filters.push_back({ PlaylistFilterDescription(), std::string("*.") + MusicPackage::kPlaylistExtension });
		NativeFileDialog::Open(kPlaylistLoadOwner, request);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Replaces your whole music setup - Jukebox and music replacements - with the one in the file. You are asked first.").c_str());

	// The recent ones, for swapping with one click.
	const std::vector<std::string> recent = musicManager.GetRecentPlaylists();
	if (!recent.empty()) {
		ImGui::Separator();
		ImGui::TextDisabled("%s", L("Recent").c_str());
		for (const std::string& path : recent) {
			ImGui::PushID(path.c_str());
			const bool exists = GetFileAttributesW(utf8_to_utf16(path).c_str()) != INVALID_FILE_ATTRIBUTES;
			const std::string name = MusicPackage::StemOf(path);
			if (ImGui::MenuItem(exists ? name.c_str() : (name + "  (" + L("file not found") + ")").c_str(), nullptr, false, !busy)) {
				if (exists) {
					AskToLoadPlaylist(path);
				} else {
					musicManager.ForgetRecentPlaylist(path);
				}
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltipWrapped(exists ? path.c_str() : L("This file was moved or deleted. Click to take it off the list.").c_str());
			ImGui::PopID();
		}
	}
	ImGui::EndPopup();
}

void JukeboxWindow::PollPlaylistDialogs() {
	MusicManager& musicManager = GetMusicManager();
	NativeFileDialog::Result result;
	if (NativeFileDialog::Consume(kPlaylistSaveOwner, &result)) {
		if (result.accepted && !result.path.empty() && musicManager.ExportPlaylist(result.path))
			m_playlistMessage.clear();
		else if (m_savingBeforeLoad) {
			// Saving was cancelled: back to the question rather than dropping it.
			m_savingBeforeLoad = false;
			m_openConfirmLoad = true;
		}
	}
	if (NativeFileDialog::Consume(kPlaylistLoadOwner, &result) && result.accepted && !result.path.empty())
		AskToLoadPlaylist(result.path);
	// The save of the current setup has finished: ask about the load again.
	if (m_savingBeforeLoad && !musicManager.IsPlaylistBusy()) {
		m_savingBeforeLoad = false;
		m_openConfirmLoad = true;
	}
}

void JukeboxWindow::AskToLoadPlaylist(const std::string& path) {
	m_confirmLoadPath = path;
	m_openConfirmLoad = true;
}

void JukeboxWindow::DrawConfirmLoadModal() {
	MusicManager& musicManager = GetMusicManager();
	const std::string popupId = L("Load this playlist?") + "##confirm_load";
	if (m_openConfirmLoad) {
		ImGui::OpenPopup(popupId.c_str());
		m_openConfirmLoad = false;
	}
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2((std::min)(560.0f, display.x * 0.9f), 0.0f), ImGuiCond_Always);
	MusicListStyle::OpaquePopup opaque;
	if (!ImGui::BeginPopupModal(popupId.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
		return;

	const ImVec4 warn(1.0f, 0.75f, 0.30f, 1.0f);
	ImGui::TextWrapped("%s", MusicPackage::FileNameOf(m_confirmLoadPath).c_str());
	ImGui::Spacing();
	ImGui::TextColoredWrapped(warn, "%s", L("This replaces your WHOLE music setup with the one in the file:").c_str());
	ImGui::Bullet(); ImGui::TextWrapped("%s", L("Which Jukebox tracks are ticked, and every Jukebox option.").c_str());
	const int replaced = BgmReplacementManager::GetInstance().GetAssignedCount();
	std::string replacements = L("Every music replacement. The ones you have now are removed and their converted files deleted.");
	if (replaced > 0)
		replacements += " (" + L("You have") + " " + std::to_string(replaced) + " " + L("replaced track(s) now.") + ")";
	ImGui::Bullet(); ImGui::TextWrapped("%s", replacements.c_str());
	ImGui::Bullet(); ImGui::TextWrapped("%s", L("Its custom songs are added to your custom songs folder. The ones you have are kept.").c_str());
	ImGui::Spacing();
	ImGui::TextColoredWrapped(warn, "%s", L("This can't be undone. If you might want your current setup back, save it as a playlist first.").c_str());
	ImGui::Spacing();

	const bool busy = NativeFileDialog::IsOpen() || musicManager.IsPlaylistBusy();
	ImGui::BeginDisabled(busy);
	const std::string saveLabel = L("Save mine first...");
	if (ImGui::Button(saveLabel.c_str())) {
		NativeFileDialog::Request request;
		request.save = true;
		request.title = L("Save this Jukebox playlist");
		request.filters.push_back({ PlaylistFilterDescription(), std::string("*.") + MusicPackage::kPlaylistExtension });
		request.defaultExtension = MusicPackage::kPlaylistExtension;
		request.initialPath = std::string("My setup before ") + MusicPackage::StemOf(m_confirmLoadPath) + "." + MusicPackage::kPlaylistExtension;
		if (NativeFileDialog::Open(kPlaylistSaveOwner, request))
			m_savingBeforeLoad = true;
		ImGui::CloseCurrentPopup();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltipWrapped(L("Saves your current setup as a playlist, then asks you again.").c_str());
	const std::string loadLabel = L("Replace everything");
	ImGui::SameLineOrWrap(ImGui::ButtonWidth(loadLabel.c_str()));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.30f, 0.20f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.38f, 0.25f, 1.0f));
	if (ImGui::Button(loadLabel.c_str())) {
		if (musicManager.ImportPlaylist(m_confirmLoadPath))
			m_playlistMessage.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::PopStyleColor(2);
	ImGui::EndDisabled();
	const std::string cancelLabel = L("Cancel");
	ImGui::SameLineOrWrap(ImGui::ButtonWidth(cancelLabel.c_str()));
	if (ImGui::Button(cancelLabel.c_str()))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}
