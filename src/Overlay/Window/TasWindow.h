#pragma once

#include "Overlay/Window/IWindow.h"

#include <cstddef>
#include <string>
#include <vector>

class WindowContainer;
class TasManager;

class TasWindow : public IWindow {
public:
    TasWindow(const std::string& windowTitle, bool windowClosable,
        WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0)
        : IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer) {}

    ~TasWindow() override = default;

public:
    void Update() override;

protected:
    void BeforeDraw() override;
    void Draw() override;

private:
    // The window reads top to bottom in the order the tool is actually used: what state
    // am I in, what am I starting from, what have I built, what am I adding, play it back.
    void DrawInactiveState(TasManager& manager);
    void DrawStatusStrip(TasManager& manager) const;
    void DrawBaseStateSection(TasManager& manager);
    void DrawTimeline(TasManager& manager);
    void DrawComposer(TasManager& manager);
    void DrawPlaybackSection(TasManager& manager);
    void DrawFooter(TasManager& manager);

    // Occasional work, kept behind buttons so it does not compete with the editor.
    void DrawMovieFilePopup(TasManager& manager);
    void DrawHelpPopup() const;
    // Committing on top of existing frames throws the rest of the movie away, so it asks
    // first. This is the only destructive action in the editor.
    void DrawInsertWarningPopup(TasManager& manager);
    void CommitTypedInput(TasManager& manager, int frameCount);

    // Number of frames the text currently in the field would produce, or -1 if the
    // notation is invalid. Empty counts as a single neutral frame.
    static int ParsedFrameCount(const char* text);

    WindowContainer* m_pWindowContainer = nullptr;

    char m_p1Input[256] = "5";
    char m_p2Input[256] = "5";
    int m_frameCount = 1;

    bool m_includeInitialConditions = true;

    // Scrubbing is applied on release: seeking re-simulates, so reacting to every drag
    // position would start a run per mouse-move.
    int m_scrubTarget = 0;
    bool m_scrubActive = false;
    int m_pendingCommitFrames = 0;

    // Drives the timeline's follow-the-playhead scrolling: only scroll when the playhead
    // actually moved, so the user can scrub the strip by hand without it snapping back.
    size_t m_lastPlayhead = 0;
    bool m_hasLastPlayhead = false;
};
