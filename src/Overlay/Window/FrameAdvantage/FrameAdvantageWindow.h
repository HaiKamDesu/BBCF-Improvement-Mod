#pragma once

#include "Overlay/Window/IWindow.h"
#include "Core/interfaces.h"
#include "Game/CharData.h"
#include "Core/interfaces.h"
#include <imgui.h>

class FrameAdvantageWindow : public IWindow {
public:



	FrameAdvantageWindow(const std::string& windowTitle, bool windowClosable, 
		ImGuiWindowFlags windowFlags)
		: IWindow(windowTitle, windowClosable, windowFlags){

	}


	// Opened by a checkbox on a mod-menu page, which only runs while that page is drawn, so
	// leaving training never closes this. Both of these gate on there actually being a match
	// to report on instead.
	void Update() override;
	bool WantsMouseCursor() const override;

protected:

	void Draw() override;

private:
	static bool HasMatchToReportOn();

};