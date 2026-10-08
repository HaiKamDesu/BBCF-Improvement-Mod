#pragma once
#include "IWindow.h"
#include "Audio/AudioDecode.h"
#include "Audio/BgmReplacementManager.h"

#include <map>
#include <string>
#include <vector>

// The music replacement window: the songs you brought in, one row each, with the game
// tracks each one replaces. A song can stand in for any number of tracks, so the list is
// as long as your set and not the whole soundtrack. A row opens to show its tracks one by
// one (each with its own switch, volume and right-click menu); volume, previews and
// rebuilding live behind buttons and menus so the everyday view is just the songs.
class BgmReplacementWindow : public IWindow
{
public:
	BgmReplacementWindow(const std::string& windowTitle, bool windowClosable,
		ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags) {}
	~BgmReplacementWindow() override = default;

protected:
	void BeforeDraw() override;
	void Draw() override;

private:
	void DrawToolbar();
	void DrawStatus();
	void DrawSongList();
	void DrawSongRow(const BgmReplacementManager::LibrarySong& song);
	// The "what it replaces" button on a song's row.
	void DrawReplacesButton(const BgmReplacementManager::LibrarySong& song, float width);
	void DrawSongContextMenu(const BgmReplacementManager::LibrarySong& song);
	// One game track a song replaces, under its opened row.
	void DrawReplacedTrackRow(const BgmReplaceableTrack& track);
	void DrawTrackContextMenu(const BgmReplaceableTrack& track);
	// Volume for one or more replacements at once; `id` keys the popup's draft.
	void DrawVolumeButton(const std::vector<int>& tableIndices, const std::string& id);
	void PollRestartAfterApply();

	void DrawTrackPickerModal();
	void DrawSoundtrackModal();
	void DrawImportChoiceModal();

	void OpenTrackPicker(const std::string& songPath);
	void OpenAddSongs();
	void OpenSwapFile(const std::string& songPath);
	void PollDialogs();

	char m_search[64] = {};
	std::map<std::string, bool> m_expanded;   // song path -> row opened, this session

	// The song whose tracks are being chosen, and the one-shot that opens the picker.
	// Opening is deferred to window level because the menus that trigger it live in the
	// scrolling list, and a modal opened from in there would be parented to it.
	std::string m_pickerSong;
	bool m_openTrackPicker = false;
	char m_pickerSearch[64] = {};
	std::map<std::string, bool> m_pickerCollapsed;   // category -> collapsed

	bool m_openSoundtrack = false;
	char m_soundtrackSearch[64] = {};
	std::map<std::string, bool> m_soundtrackCollapsed;

	std::string m_swapFrom;            // song whose file is being replaced by another
	std::string m_importPackPath;      // picked file waiting on "add on top / replace"
	bool m_openImportChoice = false;

	// The volume popup's draft, for the one button whose popup is open. savedSeen is what
	// the manager held when it was taken, so it re-seeds if that moves underneath it.
	std::string m_volumeFor;
	float m_volumeDraft = 0.0f;
	float m_volumeSavedSeen = 0.0f;
	// Track whose rebuild we are waiting on, so the game can be told to play it again once
	// the new file exists. -1 when nothing is pending.
	int  m_restartAfterApplyIndex = -1;

	std::string m_lastMessage;
	std::string m_packMessage;         // how the last export/import went
	bool m_packMessageOk = true;
};
