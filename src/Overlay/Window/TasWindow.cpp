#include "TasWindow.h"

#include "Core/HotkeyManager.h"
#include "Core/Localization.h"
#include "Core/NativeFileDialog.h"
#include "Core/interfaces.h"
#include "Game/TasManager.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/WindowContainer/WindowContainer.h"

#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace {

// One timeline cell holds a single frame's notation, at most "9ABCD".
constexpr float kCellWidth = 44.0f;
constexpr float kCellGap = 3.0f;

const ImVec4 kColOk(0.40f, 0.82f, 0.48f, 1.00f);
const ImVec4 kColWarn(1.00f, 0.76f, 0.30f, 1.00f);
const ImVec4 kColError(1.00f, 0.40f, 0.40f, 1.00f);
const ImVec4 kColIdle(0.60f, 0.64f, 0.70f, 1.00f);
const ImVec4 kColLive(0.35f, 0.70f, 1.00f, 1.00f);
// Frames already played read as settled; frames with a button on them are what the eye
// should catch when scanning a combo, so those get the one saturated colour in the strip.
const ImVec4 kColPlayed(0.52f, 0.55f, 0.60f, 1.00f);
const ImVec4 kColPending(0.82f, 0.84f, 0.88f, 1.00f);
const ImVec4 kColAttack(1.00f, 0.78f, 0.36f, 1.00f);

// Width a button needs for its label, so a row can work out whether the next one still fits.
float ButtonWidth(const std::string& label, float minimum = 0.0f) {
    const float text = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    return (std::max)(text, minimum);
}

float HelpMarkerWidth() {
    return ImGui::CalcTextSize("(?)").x;
}

// Continue the current row when the next item still fits, otherwise start a new one. ImGui
// has no flow layout, so without this every control row runs off the edge as soon as the
// window is made narrow.
void FlowSameLine(float nextWidth) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rowRight = ImGui::GetItemRectMax().x + style.ItemSpacing.x + nextWidth;
    const float contentRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (rowRight <= contentRight) {
        ImGui::SameLine();
    }
}

// TextDisabled does not wrap, and most of the explanatory copy in this window is a sentence.
void TextDisabledWrapped(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void TextColoredWrapped(const ImVec4& colour, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// A help marker that wraps onto its own line rather than pushing the row over the edge.
void FlowHelpMarker(const std::string& text) {
    FlowSameLine(HelpMarkerWidth());
    ImGui::ShowHelpMarker(text.c_str());
}

void SectionHeader(const std::string& label) {
    ImGui::VerticalSpacing(4);
    ImGui::TextDisabled("%s", label.c_str());
    ImGui::Separator();
    ImGui::VerticalSpacing(2);
}

void StatusDot(const ImVec4& colour) {
    const float radius = ImGui::GetFontSize() * 0.25f;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 centre(cursor.x + radius, cursor.y + ImGui::GetTextLineHeight() * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(centre, radius, ImGui::ColorConvertFloat4ToU32(colour));
    ImGui::Dummy(ImVec2(radius * 2.0f + 4.0f, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0f, 6.0f);
}


std::string NextTasFileName() {
    for (unsigned int number = 1;; ++number) {
        char name[64]{};
        std::snprintf(name, sizeof(name), "tas_movie_%u.txt", number);
        if (GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES) return name;
    }
}

constexpr const char* kFileDialogOwner = "tas_window";
constexpr int kFileDialogSaveMovie = 0;
constexpr int kFileDialogLoadMovie = 1;

// "1 frames" reads as a bug, and Spanish has the same problem, so the singular is its
// own string rather than a formatted count.
std::string FrameCountLabel(int count) {
    if (count == 1) {
        return L("1 frame");
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), Messages.u_frames(), static_cast<unsigned int>((std::max)(count, 0)));
    return buffer;
}

// An empty field means "no input this frame" rather than an error, which is what a user
// typing a P2 command and leaving P1 alone expects.
const char* TextOrNeutral(const char* text) {
    return (text && text[0]) ? text : "5";
}

} // namespace

int TasWindow::ParsedFrameCount(const char* text) {
    std::vector<uint16_t> frames;
    if (!TasManager::TryParseCommand(TextOrNeutral(text), &frames)) {
        return -1;
    }
    return static_cast<int>(frames.size());
}


void TasWindow::Update() {
    TasManager& manager = TasManager::Instance();
    if (!manager.IsActive()) {
        if (IsOpen()) {
            manager.Exit();
        }
        return;
    }

    if (manager.IsPlaying() && manager.IsPlaybackUiHidden()) {
        if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_TasParse)) {
            manager.StopPlayback();
        }
        return;
    }
    if (!IsOpen()) {
        manager.Exit();
        return;
    }

    // Undo/redo are fixed shortcuts rather than rebindable hotkeys: they mean the same
    // thing everywhere, and they apply while either TAS window has focus. Suppressed while
    // a text field has the keyboard so typing in a command box is not hijacked.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
            if (io.KeyShift) {
                manager.Redo();
            } else {
                manager.Undo();
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
            manager.Redo();
        }
    }

    if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_TasParse)) {
        manager.SetInputText(TextOrNeutral(m_p1Input), TextOrNeutral(m_p2Input));
    }
    if (!manager.IsPlaybackRunning() && HotkeyManager::WasPressed(HotkeyManager::Hotkey_TasRewind)) {
        manager.SeekRelative(-m_frameCount);
    }
    if (!manager.IsPlaybackRunning() && HotkeyManager::WasPressed(HotkeyManager::Hotkey_TasAdvance)) {
        // Only steps forward through what already exists; adding frames is a click, so the
        // hotkey can never silently overwrite the tail of a combo.
        if (manager.GetCursor() < manager.GetFrameCount()) {
            manager.SeekRelative(m_frameCount);
        } else {
            manager.AdvanceFrames(m_frameCount);
        }
    }
    IWindow::Update();
}

void TasWindow::BeforeDraw() {
    // Wide enough for a useful run of timeline cells without the window resizing itself
    // every time the movie grows.
    ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
}

void TasWindow::Draw() {
    TasManager& manager = TasManager::Instance();

    // The picker outlives the click that opened it, and the popup it was opened from may
    // already be gone, so the answer is collected here rather than next to the button.
    NativeFileDialog::Result fileResult;
    if (NativeFileDialog::Consume(kFileDialogOwner, &fileResult) && fileResult.accepted) {
        if (fileResult.contextId == kFileDialogSaveMovie) {
            manager.ExportMovie(fileResult.path, m_includeInitialConditions);
        } else if (fileResult.contextId == kFileDialogLoadMovie) {
            manager.ImportMovie(fileResult.path);
        }
    }

    if (!manager.IsActive()) {
        DrawInactiveState(manager);
        return;
    }

    DrawStatusStrip(manager);
    DrawBaseStateSection(manager);

    // Nothing downstream of the base state can do anything without one, so rather than
    // letting the user click buttons that only produce an error, the whole editor is
    // disabled until the situation has been captured.
    const bool ready = manager.HasBaseSnapshot();
    ImGui::BeginDisabled(!ready);
    DrawTimeline(manager);
    DrawComposer(manager);
    DrawPlaybackSection(manager);
    ImGui::EndDisabled();

    DrawFooter(manager);

    DrawInsertWarningPopup(manager);
    DrawMovieFilePopup(manager);
    DrawHelpPopup();
}

void TasWindow::DrawInactiveState(TasManager& manager) {
    if (IsOpen()) {
        Close();
    }
    ImGui::TextUnformatted(L("TAS mode is not active.").c_str());
    if (ImGui::Button(L("Enter TAS mode").c_str())) {
        manager.Enter();
    }
    if (!manager.GetError().empty()) {
        TextColoredWrapped(kColError, manager.GetError());
    }
}

void TasWindow::DrawStatusStrip(TasManager& manager) const {
    const TasRunState state = manager.GetRunState();

    ImVec4 colour = kColIdle;
    std::string label = L("Editing");
    if (manager.IsPlaying()) {
        colour = kColLive;
        label = manager.IsPlaybackUiHidden() ? L("Presentation") : L("Preview");
    } else if (state == TasRunState::ReplayingMovie) {
        colour = kColLive;
        label = L("Replaying");
    } else if (!manager.HasBaseSnapshot()) {
        colour = kColWarn;
        label = L("No base state");
    }

    char frameText[64];
    std::snprintf(frameText, sizeof(frameText), Messages.Frame_u(), manager.GetCurrentFrame());

    std::string summary = label;
    summary += "  |  ";
    summary += frameText;
    summary += "  |  ";
    summary += FrameCountLabel(static_cast<int>(manager.GetFrameCount()));
    if (manager.GetRerecordCount() > 0) {
        char rerecordText[64];
        std::snprintf(rerecordText, sizeof(rerecordText), Messages.Rerecords_u(), manager.GetRerecordCount());
        summary += "  |  ";
        summary += rerecordText;
    }

    StatusDot(colour);
    TextColoredWrapped(colour, summary);
}

void TasWindow::DrawBaseStateSection(TasManager& manager) {
    SectionHeader(L("Base state"));

    if (!manager.HasBaseSnapshot()) {
        TextColoredWrapped(kColWarn, L("Set the match up the way you want the combo to start, then save a base state."));
        ImGui::VerticalSpacing(2);
        if (ImGui::Button(L("Save base state").c_str())) {
            manager.SaveBaseSnapshot();
        }
        FlowHelpMarker(L("Captures positions, health, meter and everything else as the starting point of the combo. Every playback and every rewind returns here, so save it before you type any input."));
        return;
    }

    ImGui::Text(Messages.Saved_at_frame_u(), manager.GetBaseFrame());
    FlowHelpMarker(L("Every playback and every rewind restores this moment."));
    FlowSameLine(ButtonWidth(L("Re-save")));
    if (ImGui::Button(L("Re-save").c_str())) {
        manager.SaveBaseSnapshot();
    }
    FlowHelpMarker(L("Replaces the base state with the current moment. The movie you have recorded is kept, but it will no longer line up with the new starting point."));
    FlowSameLine(ButtonWidth(L("Restore")));
    if (ImGui::Button(L("Restore").c_str())) {
        manager.LoadBaseSnapshot();
    }
    FlowHelpMarker(L("Jumps the match back to the base state without touching the movie."));
}

void TasWindow::DrawTimeline(TasManager& manager) {
    SectionHeader(L("Timeline"));

    const size_t count = manager.GetFrameCount();
    const size_t playhead = manager.GetCursor();
    const bool seeking = manager.IsSeeking();
    const bool playing = manager.IsPlaying();
    const bool busy = seeking || playing;

    // Transport. Symbols rather than words: this row is read as a set of controls, and
    // every one of them carries a tooltip. Centred, and it wraps if the window is narrow.
    const float stepWidth = ButtonWidth("<<", 30.0f);
    const float playWidth = (std::max)(ButtonWidth(L("Play")), ButtonWidth(L("Stop")) );
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float rowWidth = stepWidth * 6.0f + playWidth + spacing * 6.0f;
    // Centre on the content area rather than the window, so padding and a scrollbar do not
    // pull the row off-centre. If it does not fit it stays left-aligned and wraps instead.
    const float available = ImGui::GetContentRegionAvail().x;
    if (rowWidth <= available) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - rowWidth) * 0.5f);
    }

    const ImVec2 stepSize(stepWidth, 0.0f);

    ImGui::BeginDisabled(busy || playhead == 0);
    if (ImGui::Button("|<", stepSize)) {
        manager.SeekToFrame(0);
    }
    ImGui::EndDisabled();
    ImGui::HoverTooltip(L("Jump to the start").c_str());

    const struct { const char* glyph; int delta; const char* tip; } kBack[] = {
        { "<<", -10, "Back 10 frames" },
        { "<",   -1, "Back 1 frame" },
    };
    for (const auto& step : kBack) {
        FlowSameLine(stepWidth);
        ImGui::BeginDisabled(busy || playhead == 0);
        if (ImGui::Button(step.glyph, stepSize)) {
            manager.SeekRelative(step.delta);
        }
        ImGui::EndDisabled();
        ImGui::HoverTooltip(L(step.tip).c_str());
    }

    FlowSameLine(playWidth);
    if (playing) {
        if (ImGui::Button(L("Stop").c_str(), ImVec2(playWidth, 0.0f))) {
            manager.StopPlayback();
        }
        ImGui::HoverTooltip(L("Stop playback").c_str());
    } else {
        ImGui::BeginDisabled(count == 0 || seeking);
        if (ImGui::Button(L("Play").c_str(), ImVec2(playWidth, 0.0f))) {
            manager.StartPlayback(false);
        }
        ImGui::EndDisabled();
        ImGui::HoverTooltip(L("Play from the current frame to the end, then freeze there. At the end of the combo it plays from the start instead.").c_str());
    }

    // Forward is never disabled: running past the end appends that many neutral frames,
    // so stepping forward at the end of the combo is just another way to wait.
    const struct { const char* glyph; int delta; const char* tip; } kForward[] = {
        { ">",    1, "Forward 1 frame. Past the end of the combo this adds an idle frame." },
        { ">>",  10, "Forward 10 frames. Past the end of the combo this adds idle frames." },
    };
    for (const auto& step : kForward) {
        FlowSameLine(stepWidth);
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(step.glyph, stepSize)) {
            manager.SeekRelative(step.delta);
        }
        ImGui::EndDisabled();
        ImGui::HoverTooltip(L(step.tip).c_str());
    }

    FlowSameLine(stepWidth);
    ImGui::BeginDisabled(busy || playhead >= count);
    if (ImGui::Button(">|", stepSize)) {
        manager.AdvanceOrExtend(static_cast<int>(count - playhead));
    }
    ImGui::EndDisabled();
    ImGui::HoverTooltip(L("Jump to the end").c_str());

    // Scrub bar. Released rather than live, because every drag position would otherwise
    // kick off a fresh re-simulation.
    ImGui::VerticalSpacing(2);
    int scrubFrame = static_cast<int>(playhead);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::BeginDisabled(busy || count == 0);
    if (ImGui::SliderInt("##tas_scrub", &scrubFrame, 0, static_cast<int>(count), "%d")) {
        m_scrubActive = true;
        m_scrubTarget = scrubFrame;
    }
    if (m_scrubActive && ImGui::IsItemDeactivatedAfterEdit()) {
        manager.SeekToFrame(static_cast<size_t>((std::max)(m_scrubTarget, 0)));
        m_scrubActive = false;
    }
    ImGui::EndDisabled();
    ImGui::HoverTooltip(L("Drag to move through the combo. Release to jump there.").c_str());

    ImGui::VerticalSpacing(2);
    if (seeking) {
        ImGui::TextColored(kColLive, Messages.Seeking_u_u(),
            static_cast<unsigned int>(playhead), static_cast<unsigned int>(manager.GetRunTarget()));
    } else {
        ImGui::Text(Messages.Frame_u_of_u(),
            static_cast<unsigned int>(playhead), static_cast<unsigned int>(count));
    }

    FlowSameLine(ButtonWidth(L("Input list")));
    if (ImGui::Button(L("Input list").c_str())) {
        m_pWindowContainer->GetWindow(WindowType_TasInputList)->ToggleOpen();
    }
    FlowHelpMarker(L("Opens the whole movie as a vertical list, one row per frame. It is a separate window so you can make it tall and put it wherever you like."));

    TextDisabledWrapped(L("Moving around never deletes anything. Frames are only replaced when you commit new input."));
    FlowHelpMarker(L("Seeking reloads a savestate and replays the stored input from there. Keyframes are taken every 60 frames so a nearby jump only replays a little; jumping far back replays from the start, which takes about a second per 60 frames."));
}

void TasWindow::DrawComposer(TasManager& manager) {
    SectionHeader(L("Add input"));

    const bool busy = manager.IsPlaybackRunning();
    const float badgeWidth = ImGui::CalcTextSize(L("1 frame").c_str()).x + 60.0f;

    ImGui::BeginDisabled(busy);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("P1");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-badgeWidth);
    ImGui::InputTextWithHint("##tas_p1", L("numpad notation, e.g. 623C").c_str(), m_p1Input, sizeof(m_p1Input));
    const int p1Frames = ParsedFrameCount(m_p1Input);
    ImGui::SameLine();
    if (p1Frames < 0) {
        ImGui::TextColored(kColError, "%s", L("invalid").c_str());
    } else {
        ImGui::TextColored(kColOk, "%s", FrameCountLabel(p1Frames).c_str());
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("P2");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-badgeWidth);
    ImGui::InputTextWithHint("##tas_p2", L("leave empty for neutral").c_str(), m_p2Input, sizeof(m_p2Input));
    const int p2Frames = ParsedFrameCount(m_p2Input);
    ImGui::SameLine();
    if (p2Frames < 0) {
        ImGui::TextColored(kColError, "%s", L("invalid").c_str());
    } else {
        ImGui::TextColored(kColOk, "%s", FrameCountLabel(p2Frames).c_str());
    }

    TextDisabledWrapped(L("Numpad notation: 5 is neutral, 623C is three frames. One digit is one frame."));
    FlowHelpMarker(L("Directions use the numpad layout, 7 8 9 over 4 5 6 over 1 2 3, with 5 as neutral. Letters A B C D attach to the direction right before them, so 623C is 6, then 2, then 3 with C held. Press Help for the full reference."));

    ImGui::VerticalSpacing(3);

    const bool valid = p1Frames >= 0 && p2Frames >= 0;
    const int commitFrames = (std::max)(p1Frames, p2Frames);

    ImGui::BeginDisabled(!valid || commitFrames <= 0);
    char commitLabel[160];
    if (commitFrames == 1) {
        std::snprintf(commitLabel, sizeof(commitLabel), "%s", L("Commit 1 frame").c_str());
    } else {
        std::snprintf(commitLabel, sizeof(commitLabel), Messages.Commit_u_frames(),
            static_cast<unsigned int>((std::max)(commitFrames, 0)));
    }
    if (ImGui::Button(commitLabel, ImVec2(150.0f, 0.0f))) {
        if (manager.GetCursor() < manager.GetFrameCount()) {
            m_pendingCommitFrames = commitFrames;
            ImGui::OpenPopup("##tas_insert_warning");
        } else {
            CommitTypedInput(manager, commitFrames);
        }
    }
    ImGui::EndDisabled();
    FlowHelpMarker(L("Appends the typed input to the timeline and plays those frames. This is the main action: type, commit, repeat."));

    FlowSameLine(90.0f);
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputInt("##tas_frame_count", &m_frameCount);
    if (m_frameCount < 1) {
        m_frameCount = 1;
    }
    FlowSameLine(ButtonWidth(L("Advance")));
    if (ImGui::Button(L("Advance").c_str())) {
        if (manager.GetCursor() < manager.GetFrameCount()) {
            m_pendingCommitFrames = m_frameCount;
            ImGui::OpenPopup("##tas_insert_warning");
        } else {
            manager.AdvanceFrames(m_frameCount);
        }
    }
    FlowHelpMarker(L("Adds this many frames using whatever is left of the typed input, then neutral. Use it to wait between moves. To move around without changing anything, use the transport buttons above."));

    const size_t queued = manager.GetQueuedFrameCount();
    const size_t consumed = manager.GetQueueCursor();
    if (queued > consumed) {
        ImGui::TextDisabled(Messages.Typed_but_not_committed_yet_u(), static_cast<unsigned int>(queued - consumed));
    }

    ImGui::EndDisabled();
}

void TasWindow::DrawPlaybackSection(TasManager& manager) {
    SectionHeader(L("Playback"));

    const bool hasMovie = manager.GetFrameCount() > 0;
    const bool busy = manager.IsPlaybackRunning();

    if (manager.IsPlaying()) {
        if (ImGui::Button(L("Stop playback").c_str(), ImVec2(150.0f, 0.0f))) {
            manager.StopPlayback();
        }
    } else {
        ImGui::BeginDisabled(!hasMovie || busy);
        if (ImGui::Button(L("Preview").c_str(), ImVec2(150.0f, 0.0f))) {
            manager.StartPlayback(false);
        }
        ImGui::EndDisabled();
        FlowHelpMarker(L("Restores the base state, plays the whole combo, then freezes on the last frame so you can keep adding to it."));

        FlowSameLine(ButtonWidth(L("Presentation")));
        ImGui::BeginDisabled(!hasMovie || busy);
        if (ImGui::Button(L("Presentation").c_str())) {
            manager.StartPlayback(true);
        }
        ImGui::EndDisabled();
        FlowHelpMarker(L("For recording video: hides this window, holds neutral for 60 frames, plays the combo, then holds neutral for 240 frames. Press the TAS parse hotkey to stop it early."));
    }

    FlowSameLine(ButtonWidth(L("Resume game")));
    if (ImGui::Button(L("Resume game").c_str())) {
        manager.ResumeGame();
    }
    FlowHelpMarker(L("Unfreezes the match so you can move around normally. The movie is kept."));

    FlowSameLine(ButtonWidth(L("Reset movie")));
    ImGui::BeginDisabled(!hasMovie || busy);
    if (ImGui::Button(L("Reset movie").c_str())) {
        manager.ResetMovie();
    }
    ImGui::EndDisabled();
    FlowHelpMarker(L("Deletes every recorded frame and returns to the base state. The base state itself is kept."));

    bool autoLoad = manager.IsAutoLoadAfterPlayback();
    if (ImGui::Checkbox(L("Return to base state when playback finishes").c_str(), &autoLoad)) {
        manager.SetAutoLoadAfterPlayback(autoLoad);
    }
    FlowHelpMarker(L("After a preview ends, jump straight back to the start of the combo instead of staying on the final frame."));
}

void TasWindow::DrawFooter(TasManager& manager) {
    ImGui::VerticalSpacing(4);
    ImGui::Separator();

    if (ImGui::Button(L("Movie file").c_str())) {
        ImGui::OpenPopup("##tas_movie_file");
    }
    FlowHelpMarker(L("Save the combo to a .txt file in the game folder, or load one back."));
    FlowSameLine(ButtonWidth(L("Help")));
    if (ImGui::Button(L("Help").c_str())) {
        ImGui::OpenPopup("##tas_help");
    }
    FlowSameLine(ButtonWidth(L("Exit TAS mode")));
    if (ImGui::Button(L("Exit TAS mode").c_str())) {
        manager.Exit();
        Close();
        return;
    }

    if (!manager.GetError().empty()) {
        TextColoredWrapped(kColError, manager.GetError());
    } else if (!manager.GetStatus().empty()) {
        TextDisabledWrapped(manager.GetStatus());
    }
}

void TasWindow::CommitTypedInput(TasManager& manager, int frameCount) {
    manager.SetInputText(TextOrNeutral(m_p1Input), TextOrNeutral(m_p2Input));
    manager.EditAndAdvanceFrames(frameCount);
}

void TasWindow::DrawInsertWarningPopup(TasManager& manager) {
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("##tas_insert_warning", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }

    const size_t playhead = manager.GetCursor();
    const size_t count = manager.GetFrameCount();
    const unsigned int discarded = static_cast<unsigned int>(count > playhead ? count - playhead : 0);

    ImGui::TextUnformatted(L("Overwrite the rest of the combo?").c_str());
    ImGui::Separator();
    ImGui::VerticalSpacing(3);
    ImGui::TextWrapped(Messages.You_are_at_frame_u_of_u_Committing_here_replaces_the_u_frames_after_it(),
        static_cast<unsigned int>(playhead), static_cast<unsigned int>(count), discarded);
    ImGui::VerticalSpacing(3);
    TextDisabledWrapped(L("This is how re-recording works: the new input becomes the end of the combo."));
    ImGui::VerticalSpacing(5);

    if (ImGui::Button(L("Overwrite").c_str(), ImVec2(150.0f, 0.0f))) {
        CommitTypedInput(manager, m_pendingCommitFrames);
        m_pendingCommitFrames = 0;
        ImGui::CloseCurrentPopup();
    }
    FlowSameLine(ButtonWidth(L("Cancel")));
    if (ImGui::Button(L("Cancel").c_str(), ImVec2(120.0f, 0.0f))) {
        m_pendingCommitFrames = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void TasWindow::DrawMovieFilePopup(TasManager& manager) {
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("##tas_movie_file", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }

    ImGui::TextUnformatted(L("Movie file").c_str());
    ImGui::Separator();
    ImGui::VerticalSpacing(3);

    TextDisabledWrapped(L("Movies are plain text. They store the inputs only, not the base state."));
    ImGui::VerticalSpacing(4);

    ImGui::Checkbox(L("Include initial conditions").c_str(), &m_includeInitialConditions);
    FlowHelpMarker(L("Writes the characters and starting positions into the file as a comment, so you can tell later what the combo was built on."));

    ImGui::VerticalSpacing(4);

    const bool dialogBusy = NativeFileDialog::IsOpen();
    ImGui::BeginDisabled(dialogBusy || manager.GetFrameCount() == 0);
    if (ImGui::Button(L("Save to file").c_str(), ImVec2(150.0f, 0.0f))) {
        NativeFileDialog::Request request;
        request.save = true;
        request.title = L("Save TAS movie");
        request.filters.push_back({ L("TAS Movie (*.txt)"), "*.txt" });
        request.defaultExtension = "txt";
        request.initialPath = NextTasFileName();
        request.contextId = kFileDialogSaveMovie;
        NativeFileDialog::Open(kFileDialogOwner, request);
    }
    ImGui::EndDisabled();

    FlowSameLine(ButtonWidth(L("Load from file")));
    ImGui::BeginDisabled(dialogBusy || manager.IsPlaybackRunning());
    if (ImGui::Button(L("Load from file").c_str(), ImVec2(150.0f, 0.0f))) {
        NativeFileDialog::Request request;
        request.title = L("Load TAS movie");
        request.filters.push_back({ L("TAS Movie (*.txt)"), "*.txt" });
        request.defaultExtension = "txt";
        request.contextId = kFileDialogLoadMovie;
        NativeFileDialog::Open(kFileDialogOwner, request);
    }
    ImGui::EndDisabled();

    if (dialogBusy) {
        TextDisabledWrapped(L("Waiting for the file picker..."));
    }

    ImGui::VerticalSpacing(4);
    TextColoredWrapped(kColWarn, L("A movie file does not contain the base state. Save a matching one after loading."));

    ImGui::VerticalSpacing(6);
    ImGui::Separator();
    ImGui::VerticalSpacing(4);
    if (ImGui::Button(L("Close").c_str(), ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void TasWindow::DrawHelpPopup() const {
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("##tas_help", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }

    ImGui::TextUnformatted(L("How the TAS editor works").c_str());
    ImGui::Separator();
    ImGui::VerticalSpacing(4);

    ImGui::TextDisabled("%s", L("Notation").c_str());
    ImGui::TextWrapped("%s", L("Directions follow the numpad: 7 8 9 on the top row, 4 5 6 in the middle, 1 2 3 on the bottom, and 5 for neutral. Each digit is one frame, so 66 holds forward for two frames and 656 is a dash.").c_str());
    ImGui::TextWrapped("%s", L("A B C D attach to the direction immediately before them: 623C is three frames, the last of them 3 with C held. Spaces, commas and dashes are ignored, so 236 - 236C reads the same as 236236C.").c_str());

    ImGui::VerticalSpacing(4);
    ImGui::TextDisabled("%s", L("Building a combo").c_str());
    ImGui::TextWrapped("%s", L("1. Set the match up the way the combo should start and save a base state.").c_str());
    ImGui::TextWrapped("%s", L("2. Type a command and press Commit. The frames are added to the timeline and played.").c_str());
    ImGui::TextWrapped("%s", L("3. Repeat. Use Advance to wait a number of frames without pressing anything.").c_str());
    ImGui::TextWrapped("%s", L("4. If a frame was wrong, press Rewind or click that frame in the timeline, then commit the replacement.").c_str());
    ImGui::TextWrapped("%s", L("Rewinding deletes every frame after the point you go back to. That is what makes it a re-record rather than an undo.").c_str());

    ImGui::VerticalSpacing(4);
    ImGui::TextDisabled("%s", L("Hotkeys").c_str());
    ImGui::TextWrapped("%s", L("The three TAS hotkeys can be rebound in the Hotkeys section of the Settings.ini window. They do nothing while you are typing in a text field.").c_str());
    ImGui::BulletText("%s", L("Parse: reads the text fields into the queue.").c_str());
    ImGui::BulletText("%s", L("Step forward: same as Advance.").c_str());
    ImGui::BulletText("%s", L("Step back: same as Rewind.").c_str());

    ImGui::VerticalSpacing(6);
    ImGui::Separator();
    ImGui::VerticalSpacing(4);
    if (ImGui::Button(L("Close").c_str(), ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
