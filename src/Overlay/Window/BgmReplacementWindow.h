#pragma once
#include "IWindow.h"

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

	void OpenPickerFor(int tableIndex);
	void PollPicker();

	char m_search[64] = {};
	bool m_onlyReplaced = false;
	int  m_categoryFilter = 0;      // 0 = all
	int  m_pendingPickIndex = -1;   // track whose picker is open
	std::string m_lastMessage;
	std::vector<std::string> m_categories;
};
