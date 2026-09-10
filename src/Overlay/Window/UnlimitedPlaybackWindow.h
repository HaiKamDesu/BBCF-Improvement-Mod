#pragma once

#include "Game/Playbacks/UnlimitedPlaybackManager.h"

#include "Core/HotkeyManager.h"

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

// The keybind row used by the trigger settings, shared so the dummy-action rows bind the
// same hotkey the same way rather than growing their own copy.
void DrawPlaybackHotkeyBind(UnlimitedPlaybackManager& mgr, HotkeyManager::Action action);

// The real library UI, shared with the dummy-action rows so a trigger's library is
// configured through the surface people already know rather than a second, lesser copy.
// Both draw against whichever library the manager is pointed at - see
// UnlimitedPlaybackManager::SetEditTarget.
// listHeight 0 fills the space available (what the window wants); a positive value fixes
// the entry list's height, which is what lets a modal size itself to its content.
void DrawPlaybackLibraryEntriesAndAdd(float listHeight = 0.0f);
// The popups the panel above raises. Call at window/modal scope, never inside a child.
void DrawPlaybackLibraryPopups();
void DrawPlaybackPickingOrder();
