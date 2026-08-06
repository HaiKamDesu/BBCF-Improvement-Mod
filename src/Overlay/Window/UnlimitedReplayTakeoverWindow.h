#pragma once

#include "IWindow.h"
class WindowContainer;

class UnlimitedReplayTakeoverWindow : public IWindow {
public:
    UnlimitedReplayTakeoverWindow(const std::string& windowTitle, bool windowClosable,
        WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0)
        : IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer) {}

    ~UnlimitedReplayTakeoverWindow() override = default;

protected:
    void BeforeDraw() override;
    void Draw() override;

private:
    WindowContainer* m_pWindowContainer = nullptr;
};

// Draws the setup-delay countdown popup regardless of whether the Unlimited Replay Takeover
// window is open. The delay itself (UnlimitedReplayTakeoverManager's real-time setup gate) elapses
// unconditionally, so the indicator that explains it must be visible too - otherwise closing the
// window leaves the delay silent (and, being real-time-ms based rather than frame-counted, its
// timing can already look flaky even when it IS visible).
void DrawUnlimitedReplayTakeoverSetupDelayIndicatorStandalone();
