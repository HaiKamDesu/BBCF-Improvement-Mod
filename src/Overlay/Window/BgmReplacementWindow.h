#pragma once
#include "IWindow.h"
#include "Audio/AudioDecode.h"

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

	void OpenPickerFor(int tableIndex);
	void PollPicker();

	char m_search[64] = {};
	bool m_onlyReplaced = false;
	int  m_categoryFilter = 0;      // 0 = all
	int  m_pendingPickIndex = -1;   // track whose picker is open
	int  m_gainEditIndex = -1;      // track whose gain draft m_gainDraft holds
	float m_gainDraft = 0.0f;
	// Track whose rebuild we are waiting on, so the game can be told to play it again
	// once the new file exists. -1 when nothing is pending.
	int  m_restartAfterApplyIndex = -1;
	std::string m_lastMessage;
	std::vector<std::string> m_categories;
};
