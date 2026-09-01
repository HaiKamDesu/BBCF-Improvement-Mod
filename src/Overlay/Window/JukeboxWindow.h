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
	// Track whose volume popup is open, and the value being edited in it.
	int   m_volumePopupTrackId = -1;
	float m_volumeDraft = 0.0f;
	// Track whose reconversion we are waiting on, so it can be reloaded if it is the one
	// playing. -1 when nothing is pending.
	int   m_restartAfterVolumeTrackId = -1;

	void DrawControls();
	void DrawTrackList();
	void DrawImportButton();
	void PollImportDialog();
	void DrawCustomTrackVolume(int trackId);
	void PollRestartAfterVolume();
	void DrawCurrentTrackInfo();

	// Search/filter
	char m_searchBuffer[256] = {};
	int m_filterValue = 34;  // For address hunter filter

	// Scroll position for track list
	ImGuiID m_trackListID = 0;
};
