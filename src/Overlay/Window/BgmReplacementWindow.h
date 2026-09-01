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
	void StartGainPreview(int tableIndex);

	void OpenPickerFor(int tableIndex);
	void PollPicker();

	char m_search[64] = {};
	bool m_onlyReplaced = false;
	int  m_categoryFilter = 0;      // 0 = all
	int  m_pendingPickIndex = -1;   // track whose picker is open
	int  m_gainEditIndex = -1;      // track whose gain draft m_gainDraft holds
	int  m_previewIndex = -1;       // track currently auditioning, or -1
	AudioDecode::GainSpec m_gainDraft;
	std::string m_lastMessage;
	std::vector<std::string> m_categories;
};
