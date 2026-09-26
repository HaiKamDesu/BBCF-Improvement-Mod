#pragma once

#include "Overlay/Window/IWindow.h"

#include <cstdint>
#include <imgui.h>

// Net-color palette (0-8) shared with other windows that need to render the
// same colored square the game shows for this player/rank (see kNetColors in
// NetworkSquareColorWindow.cpp for the palette).
ImVec4 GetNetColorVec4(uint8_t value);

class NetworkSquareColorWindow : public IWindow
{
public:
	NetworkSquareColorWindow(const std::string& windowTitle, bool windowClosable, ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags)
	{
	}

protected:
	void BeforeDraw() override;
	void Draw() override;
};

void DrawNetworkSquareColorProgressStandalone();

// For the "Steam uploads" dialog: read the live square colour/counter, and draw
// the same square the window draws but for supplied values (a saved snapshot).
bool CaptureLocalNetworkSquareValues(uint8_t* outColor, uint8_t* outCounter);
void DrawNetworkSquareColorForValues(uint8_t color, uint8_t counter, float alpha);
