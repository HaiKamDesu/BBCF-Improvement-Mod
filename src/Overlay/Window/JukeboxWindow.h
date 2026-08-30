#pragma once
#include "IWindow.h"
#include "Audio/MusicManager.h"

class JukeboxWindow : public IWindow
{
public:
	JukeboxWindow(const std::string& windowTitle, bool windowClosable, ImGuiWindowFlags windowFlags = 0);
	~JukeboxWindow() override = default;

protected:
	void BeforeDraw() override;
	void Draw() override;

private:
	void DrawControls();
	void DrawTrackList();
	void DrawCurrentTrackInfo();

	// Search/filter
	char m_searchBuffer[256] = {};
	int m_filterValue = 34;  // For address hunter filter

	// Scroll position for track list
	ImGuiID m_trackListID = 0;
};
