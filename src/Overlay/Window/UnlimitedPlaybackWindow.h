#pragma once

#include "IWindow.h"
class WindowContainer;

class UnlimitedPlaybackWindow : public IWindow {
public:
    UnlimitedPlaybackWindow(const std::string& windowTitle, bool windowClosable,
        WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0)
        : IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer) {}

    ~UnlimitedPlaybackWindow() override = default;

protected:
    void BeforeDraw() override;
    void Draw() override;

private:
    WindowContainer* m_pWindowContainer = nullptr;
};

// Draws the loop-restart-position banner and the loop-setup countdown popup regardless of
// whether the Unlimited Playback window is open. The setup freeze itself (UnlimitedPlaybackManager::
// Tick(), driven unconditionally from hooks_bbcf.cpp) runs every frame no matter what, so the
// indicator that explains it must too - otherwise closing the window leaves the freeze silent.
void DrawUnlimitedPlaybackLoopSetupIndicatorStandalone();
