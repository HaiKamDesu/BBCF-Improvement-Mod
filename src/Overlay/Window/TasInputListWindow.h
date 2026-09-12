#pragma once

#include "Overlay/Widget/TasDocument.h"
#include "Overlay/Widget/TasFrameListView.h"
#include "Overlay/Window/IWindow.h"

#include <string>

class WindowContainer;

/*
	The TAS movie as an editable list, one row per frame.

	Vertical is the shape that fits a combo, and it lives in its own window so it can be made
	as tall as the screen and parked beside the editor. All of the editing lives in
	TasFrameListView, which the editor window also hosts inline when it is editing a
	recording slot - the two used to be separate editors with separate bugs.
*/
class TasInputListWindow : public IWindow {
public:
    TasInputListWindow(const std::string& windowTitle, bool windowClosable,
        WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0)
        : IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer) {}

    ~TasInputListWindow() override = default;

public:
    void Update() override;

protected:
    void BeforeDraw() override;
    void Draw() override;

private:
    WindowContainer* m_pWindowContainer = nullptr;
    TasMovieDocument m_document;
    TasFrameListView m_list;
};
