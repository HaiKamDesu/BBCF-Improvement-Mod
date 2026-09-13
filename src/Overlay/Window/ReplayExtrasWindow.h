#pragma once
#include "IWindow.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include <vector>

/*
	Everything you want while a replay is on screen: rewind it, take it over, or capture a
	stretch of it as a playback.

	All three used to be collapsing headers buried on the mod menu's Replays page, with the
	rewind bar additionally existing as its own title-bar-less window you toggled with a
	button called "Toggle Rewind". Nobody found any of it.

	The control sets live in the ReplayExtras namespace, which is the only place they are
	written, and two hosts draw them: the small ReplayExtrasWindow that sits on screen while
	you watch, and the mod menu's Replays page. Both are live at once, so each host passes an
	id scope - two modals with one ImGui id in the same frame is one modal with a fight over
	it.
*/
namespace ReplayExtras
{
	// What the window is currently for. Everything the user can do while taking a replay
	// over is different from everything they can do while watching one, so rather than
	// greying half the controls out it shows the ones that apply.
	enum class Mode
	{
		Idle,      // watching a replay: rewind, take over, capture
		Takeover,  // inside a takeover: restart, back to the replay, settings
		Capture,   // recording: stop and save, cancel - taking over is not offered
	};

	Mode CurrentMode(WindowContainer& container);

	// Whether the standalone window should be on screen. This is the single source of truth:
	// the checkbox on the Replays page reads and writes it, and so does the window's own
	// close button, which is how closing one unticks the other.
	bool IsWindowVisible();
	void SetWindowVisible(bool visible);

	// It lives in ImGui's own menus.ini, next to the window positions - it is the same kind
	// of "where things were" state, and does not belong in settings.ini as a row to edit by
	// hand. Must run before the first frame, which is when ImGui parses that file.
	void RegisterLayoutSettings();

	// The three control sets. idScope makes the popups inside them unique per host.
	//
	// compact is the window's contract: exactly one row, no headings, no explanatory
	// sentences, no (?) markers, and controls that stay put and grey out rather than being
	// replaced by a line of text. The window is three of these rows and nothing else, which
	// is the whole reason it can sit over a replay you are trying to watch.
	void DrawRewindBody(WindowContainer& container, const char* idScope, bool compact);
	void DrawTakeoverBody(WindowContainer& container, const char* idScope, bool compact);
	void DrawCaptureBody(WindowContainer& container, const char* idScope, bool compact);

	// A fourth row, drawn only while it can do anything: the option that keeps the game's
	// input display on screen when you pause the replay.
	//
	// Unlike the three above it is hidden rather than greyed out when it does not apply,
	// because the thing it does not apply to is a takeover - which switches the game to
	// training, where replay pausing does not exist at all. A greyed-out row there would be
	// permanently greyed out and would only take up a row of a window sitting over a match.
	bool PauseHudApplies();
	void DrawPauseHudBody(WindowContainer& container, const char* idScope, bool compact);
}

class ReplayExtrasWindow : public IWindow
{
public:
	ReplayExtrasWindow(const std::string& windowTitle, bool windowClosable,
		WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer) {}
	~ReplayExtrasWindow() override = default;

	unsigned int count_entities(bool unk_status2);
	std::vector<int> find_nearest_checkpoint(std::vector<unsigned int>);

	void Update() override;

protected:
	void BeforeDraw() override;
	void Draw() override;

	WindowContainer* m_pWindowContainer = nullptr;

private:
	void DrawCloseConfirm();

	// Set on the frame ImGui's own close button took the window away. Honouring it straight
	// away would make a feature vanish with no hint that it can come back, so it asks first.
	bool m_askClose = false;
};
