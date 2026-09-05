#pragma once
#include "IWindow.h"
#include "Audio/AudioDecode.h"

#include <map>
#include <string>
#include <vector>

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
	void DrawSummary();
	void DrawFilters();
	void DrawTrackList();
	void DrawTrackRow(int tableIndex);
	void DrawGainRow(int tableIndex);
	void PollRestartAfterApply();

	// The "Replace.../Change..." button is a menu, not a direct action: a replacement can
	// come from a file on disk or from another shipped track.
	void DrawSourceMenuButton(int tableIndex, bool firstTime);
	void DrawVanillaPickerModal();

	void OpenPickerFor(int tableIndex);
	void PollPicker();

	char m_search[64] = {};
	bool m_onlyReplaced = false;
	int  m_categoryFilter = 0;      // 0 = all
	int  m_pendingPickIndex = -1;   // track whose picker is open
	// Track the vanilla-OST modal is choosing a stand-in for, and the one-shot that opens
	// it. Opening is deferred to the end of the frame because the menu that triggers it is
	// drawn inside the scrolling track list, and a modal has to be opened at window level.
	int  m_vanillaPickFor = -1;
	bool m_openVanillaModal = false;
	char m_vanillaSearch[64] = {};
	// One draft per track, not one shared draft: every assigned track draws its own
	// slider each frame, so a single draft was overwritten by the next row drawn and the
	// value snapped back the moment the mouse was released.
	struct GainDraft
	{
		float value = 0.0f;
		float savedSeen = 0.0f;   // what the manager held when the draft was taken
	};
	std::map<int, GainDraft> m_gainDrafts;
	// Track whose rebuild we are waiting on, so the game can be told to play it again
	// once the new file exists. -1 when nothing is pending.
	int  m_restartAfterApplyIndex = -1;
	std::string m_lastMessage;
	std::vector<std::string> m_categories;
};
