#pragma once

#include "Overlay/Window/IWindow.h"
#include "Core/interfaces.h"
#include "Core/Settings.h"
#include "Game/CharData.h"
#include "FrameHistory.h"

#include <imgui.h>

class FrameHistoryWindow : public IWindow {
public:
	float width = 12.;
	float height = 20.;
	float spacing = 6.;
	int last_frame = 0;

	bool resetting = true;
	bool countEmptyFrames = false;
	int maxHistoryFrames = HISTORY_DEPTH_DEFAULT;
	FrameHistory history;

	FrameHistoryWindow(const std::string& windowTitle, bool windowClosable,
		ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar)
		: IWindow(windowTitle, windowClosable, windowFlags) {
			width = g_modVals.frame_history_width;
			height = g_modVals.frame_history_height;
			spacing = g_modVals.frame_history_spacing;
			resetting = g_modVals.frame_history_auto_reset;
			countEmptyFrames = Settings::settingsIni.frameHistoryCountEmptyFrames;
			if (Settings::settingsIni.frameHistoryEnabled)
				Open();
		}

	void Update() override;

	// Only while it is genuinely on screen. Update() stops drawing outside a training or
	// replay match but never closes itself, so IsOpen() stays true for the whole session -
	// and this window self-opens from settings.ini, so that used to mean a cursor from the
	// title screen onwards.
	bool WantsMouseCursor() const override;
protected:
	void BeforeDraw() override;
	void Draw() override;
	void AfterDraw() override;
	bool hasWorldTimeMoved();
};
