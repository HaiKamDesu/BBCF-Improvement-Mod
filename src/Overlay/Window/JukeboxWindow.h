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
	// Track whose volume popup is open, and the value being edited in it. m_volumeSavedSeen
	// is the stored value the draft was seeded from, so the draft re-seeds if that moves
	// underneath it - which is what an apply from the music replacement window does.
	int   m_volumePopupTrackId = -1;
	float m_volumeDraft = 0.0f;
	float m_volumeSavedSeen = 0.0f;
	// Track whose reconversion we are waiting on, so it can be reloaded if it is the one
	// playing. -1 when nothing is pending.
	int   m_restartAfterVolumeTrackId = -1;

	void DrawControls();
	void DrawTrackList();
	void DrawImportButton();
	void PollImportDialog();
	// The volume button and popup for one row. Custom songs and BGM replacements keep their
	// stored level in different places - a custom song's here, a replacement's in
	// BgmReplacementManager alongside the browser that also edits it - so the popup reads
	// and writes through whichever owns this id. Both bake the level into the converted
	// .pac, so both apply by re-converting.
	void DrawTrackVolume(int trackId);
	void PollRestartAfterVolume();
	void DrawCurrentTrackInfo();

	// Search/filter
	char m_searchBuffer[256] = {};
	int m_filterValue = 34;  // For address hunter filter

	// Scroll position for track list
	ImGuiID m_trackListID = 0;
};
