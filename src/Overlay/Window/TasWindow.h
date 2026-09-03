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
    // One player's row: label, notation field, Record/Stop, frame-count badge. Returns the
    // frame count the field currently parses to, or -1 if the notation is invalid.
    int DrawComposerRow(TasManager& manager, int player, const char* label, const char* id,
        const std::string& hint, char* buffer, size_t bufferSize, float badgeWidth);
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

    // Sized for a live capture rather than a typed command: one token per recorded frame,
    // up to TasManager::GetLiveRecordingFrameLimit() of them.
    char m_p1Input[16384] = "5";
    char m_p2Input[16384] = "5";
    int m_frameCount = 1;
    // Which row started the running capture, so the result is collected however the
    // recording ended - Stop, the frame limit, or the match going away - rather than only
    // when the user is the one who ended it.
    int m_recordingPlayer = -1;

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
