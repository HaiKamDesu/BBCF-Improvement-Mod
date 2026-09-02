#pragma once

#include "Overlay/Window/IWindow.h"

#include <string>

class WindowContainer;

// Small standalone front-end for the live P1 recorder. Recording state and movie are
// owned by StandaloneTasRecorder, not TasManager; this window only provides the UI.
class TasRecordingWindow : public IWindow
{
public:
    TasRecordingWindow(const std::string& windowTitle, bool windowClosable,
        WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0)
        : IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer) {}

    ~TasRecordingWindow() override = default;

    // Polls the asynchronous save dialog even if the user closes the window while it is open.
    void Update() override;

protected:
    void BeforeDraw() override;
    void Draw() override;

private:
    WindowContainer* m_pWindowContainer = nullptr;
};