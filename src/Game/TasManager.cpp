#include "TasManager.h"

#include "Core/ControllerOverrideManager.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/logger.h"
#include "Game/gamestates.h"
#include "Game/CharData.h"
#include "Game/SnapshotApparatus/SnapshotSlotPool.h"
#include "Game/characters.h"
#include "Hooks/hooks_battle_input.h"
#include "Overlay/Window/FrameHistory/FrameHistoryWindow.h"
#include "Overlay/WindowManager.h"

#include <Windows.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
namespace {
// One savestate is 10.06 MiB and the game's ring holds ten, so a state per frame is not on
// the table (a single second of frames would be 600 MB in a 2 GB address space). Keyframes
// every kKeyframeInterval frames make a nearby seek cheap; anything further back replays
// from the base state, which is correct either way, just slower.
constexpr unsigned int kKeyframeInterval = 60;
constexpr int kTasBaseSlot = 0;
constexpr int kTasSlotRequest = 5; // base + four keyframes
// Deep enough to walk back a bad editing session, small enough to stay trivial in memory:
// a 10,000 frame movie is 40 KB, so this ceiling is a couple of megabytes at worst.
constexpr size_t kUndoDepth = 64;

// TAS Movie length is not artificially capped.
constexpr unsigned int kPresentationLeadInFrames = 60;
constexpr unsigned int kPresentationLeadOutFrames = 240;
constexpr const char* kTasMovieHeaderV1 = "BBCF_TAS_MOVIE_V1";
constexpr const char* kTasMovieHeaderV2 = "BBCF_TAS_MOVIE_V2";

// Matching the packed values used by hooks_battle_input.h (taunt = 256).
constexpr uint16_t kInputButtonTaunt = 256;

uint16_t ButtonValue(char button) {
    switch (button) {
    case 'A': return 16;
    case 'B': return 32;
    case 'C': return 64;
    case 'D': return 128;
    default: return 0;
    }
}

std::string HumanInput(uint16_t packed) {
    return TasManager::FormatInput(packed);
}

bool ParseHumanInput(const std::string& text, uint16_t* result) {
    if (!result) return false;
    uint16_t value = 0;
    bool sawDirection = false;
    for (size_t i = 0; i < text.size(); ) {
        const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
        if (ch >= '1' && ch <= '9') {
            if (sawDirection) return false;
            value = static_cast<uint16_t>(ch - '0');
            sawDirection = true;
            ++i;
        } else if (ch == 'A' && i + 1 < text.size() &&
                   static_cast<char>(std::toupper(static_cast<unsigned char>(text[i + 1]))) == 'P') {
            // "ap" = taunt button. Checked before the single 'A' branch so 5ap does not
            // get parsed as "5A" followed by an invalid 'p'.
            if (!sawDirection || (value & kInputButtonTaunt)) return false;
            value = static_cast<uint16_t>(value + kInputButtonTaunt);
            i += 2;
        } else if (ch == 'A' || ch == 'B' || ch == 'C' || ch == 'D') {
            if (!sawDirection || (value & ButtonValue(ch))) return false;
            value = static_cast<uint16_t>(value + ButtonValue(ch));
            ++i;
        } else if (ch != ' ' && ch != '\t') {
            return false;
        } else {
            ++i;
        }
    }
    if (!sawDirection) return false;
    *result = value;
    return true;
}
}

TasManager& TasManager::Instance() {
    static TasManager instance;
    return instance;
}

TasManager::~TasManager() {
    ClearInputOverride();
    ClearSnapshot();
}

bool TasManager::IsInTrainingMatch() const {
    return g_gameVals.pGameMode && g_gameVals.pGameState &&
        *g_gameVals.pGameMode == GameMode_Training &&
        *g_gameVals.pGameState == GameState_InMatch &&
        !g_interfaces.player1.IsCharDataNullPtr() &&
        !g_interfaces.player2.IsCharDataNullPtr();
}

void TasManager::SetError(const char* message) {
    m_error = message ? message : "Unknown error.";
    m_status.clear();
}

void TasManager::ClearInputOverride() {
    ClearBattleInputOverride(0);
    ClearBattleInputOverride(1);
    m_hasScheduledInput = false;
    m_scheduledInput = TasFrameInput{};
}

int TasManager::GetKeyframeCount() const {
    int count = 0;
    for (const Keyframe& keyframe : m_keyframes) {
        if (keyframe.valid) {
            ++count;
        }
    }
    return count;
}

void TasManager::InvalidateKeyframesAfter(size_t frame) {
    for (Keyframe& keyframe : m_keyframes) {
        if (keyframe.valid && keyframe.movieFrame > frame) {
            keyframe.valid = false;
        }
    }
}

void TasManager::ClearKeyframes() {
    // One logical slot goes to the base state; the rest become the keyframe ring.
    m_keyframes.assign(kTasSlotRequest > 1 ? kTasSlotRequest - 1 : 0, Keyframe{});
    m_nextKeyframeSlot = 0;
}

int TasManager::FindKeyframeFor(size_t targetFrame) const {
    int best = -1;
    size_t bestFrame = 0;
    for (size_t i = 0; i < m_keyframes.size(); ++i) {
        const Keyframe& keyframe = m_keyframes[i];
        if (!keyframe.valid || keyframe.movieFrame > targetFrame) {
            continue;
        }
        if (best == -1 || keyframe.movieFrame > bestFrame) {
            best = static_cast<int>(i);
            bestFrame = keyframe.movieFrame;
        }
    }
    return best;
}

void TasManager::CaptureKeyframeIfDue() {
    if (m_keyframes.empty() || !m_snapshotOwner || m_playhead == 0) {
        return;
    }
    if (m_playhead % kKeyframeInterval != 0) {
        return;
    }
    // Already covered by an existing keyframe at this exact frame.
    for (const Keyframe& keyframe : m_keyframes) {
        if (keyframe.valid && keyframe.movieFrame == m_playhead) {
            return;
        }
    }

    const size_t slot = m_nextKeyframeSlot % m_keyframes.size();
    if (!m_snapshotOwner->save_snapshot_index(static_cast<int>(slot) + 1)) {
        LOG(1, "[TAS] keyframe capture failed at frame %u\n", static_cast<unsigned int>(m_playhead));
        return;
    }
    m_keyframes[slot].movieFrame = m_playhead;
    m_keyframes[slot].valid = true;
    m_nextKeyframeSlot = slot + 1;
    LOG(1, "[TAS] keyframe slot=%u movieFrame=%u\n",
        static_cast<unsigned int>(slot), static_cast<unsigned int>(m_playhead));
}

void TasManager::ClearSnapshot() {
    // Dropping the apparatus releases its reserved slots, so every keyframe held in them
    // is gone too. Covers import, exit and any base-state reset.
    ClearKeyframes();
    delete m_snapshotBuffer;
    m_snapshotBuffer = nullptr;
    m_snapshotSize = 0;
    delete m_snapshotOwner;
    m_snapshotOwner = nullptr;
    m_baseFrame = 0;
}

void TasManager::Enter() {
    if (m_active) {
        return;
    }
    if (!IsInTrainingMatch()) {
        SetError(L("TAS mode is available only during a training match.").c_str());
        return;
    }
    // Every TAS input goes through OverrideBattleInputPacked, which only reaches the game
    // via the BattleInputWrite hook. That hook is skipped when controller hooks are
    // disabled, so without it playback would run and silently do nothing.
    if (!IsBattleInputHookInstalled()) {
        SetError(L("TAS mode needs the controller hooks. Enable EnableControllerHooks in settings.ini and restart the game.").c_str());
        return;
    }

    m_active = true;
    m_runState = TasRunState::Idle;
    m_playhead = 0;
    m_runTarget = 0;
    m_presentationFramesRemaining = 0;
    m_presentationMode = false;
    m_lastScheduledFrame = 0;
    m_movie.clear();
    m_commandFrames.clear();
    m_commandCursor = 0;
    m_inputsParsed = false;
    m_rerecordCount = 0;
    m_error.clear();
    m_status.clear();

    auto& overrides = ControllerOverrideManager::GetInstance();
    m_p2KeyboardOverrideWasEnabled = overrides.IsMultipleKeyboardOverrideEnabled();
    if (m_p2KeyboardOverrideWasEnabled) {
        overrides.SetMultipleKeyboardOverrideEnabled(false);
    }
    LOG(1, "[TAS] entered movie mode frame=%u\n", GetCurrentFrame());

    auto* frameHistory = WindowManager::GetInstance().GetWindowContainer()
        ->GetWindow<FrameHistoryWindow>(WindowType_FrameHistory);
    if (frameHistory && !frameHistory->IsOpen()) {
        frameHistory->Open();
        m_frameHistoryOpenedByTas = true;
    }
}

void TasManager::Exit() {
    ClearInputOverride();
    if (m_p2KeyboardOverrideWasEnabled) {
        ControllerOverrideManager::GetInstance().SetMultipleKeyboardOverrideEnabled(true);
    }
    m_p2KeyboardOverrideWasEnabled = false;

    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
    if (m_frameHistoryOpenedByTas) {
        auto* frameHistory = WindowManager::GetInstance().GetWindowContainer()
            ->GetWindow<FrameHistoryWindow>(WindowType_FrameHistory);
        if (frameHistory) {
            frameHistory->Close();
        }
    }

    m_frameHistoryOpenedByTas = false;
    m_playbackUiHidden = false;
    m_frameHistoryWasOpenBeforePlayback = false;
    m_runState = TasRunState::Idle;
    m_active = false;
    m_playhead = 0;
    m_runTarget = 0;
    m_presentationFramesRemaining = 0;
    m_presentationMode = false;
    m_lastScheduledFrame = 0;
    m_movie.clear();
    m_commandFrames.clear();
    m_commandCursor = 0;
    m_inputsParsed = false;
    m_error.clear();
    m_status.clear();
    ClearSnapshot();
}

void TasManager::ScheduleMovieFrame() {
    if (m_playhead >= m_movie.size()) {
        ClearInputOverride();
        return;
    }
    m_scheduledInput = m_movie[m_playhead];
    m_hasScheduledInput = true;
    OverrideBattleInputPacked(0, m_scheduledInput.p1, 1);
    OverrideBattleInputPacked(1, m_scheduledInput.p2, 1);
}

void TasManager::ScheduleNeutralFrame() {
    m_scheduledInput = TasFrameInput{};
    m_hasScheduledInput = true;
    OverrideBattleInputPacked(0, m_scheduledInput.p1, 1);
    OverrideBattleInputPacked(1, m_scheduledInput.p2, 1);
}

bool TasManager::BeginMovieRun(TasRunState state, size_t target) {
    if (!m_active || !g_gameVals.pFrameCount || target > m_movie.size()) {
        return false;
    }
    m_runState = state;
    m_runTarget = target;
    m_lastScheduledFrame = *g_gameVals.pFrameCount;
    LOG(1, "[TAS] run state=%d playhead=%u target=%u movie=%u frame=%u\n",
        static_cast<int>(state), static_cast<unsigned int>(m_playhead),
        static_cast<unsigned int>(target), static_cast<unsigned int>(m_movie.size()),
        m_lastScheduledFrame);
    if (m_playhead >= m_runTarget) {
        FinishMovieRun(false);
        return true;
    }
    ScheduleMovieFrame();
    g_gameVals.isFrameFrozen = true;
    g_gameVals.framesToReach = m_lastScheduledFrame + 1;
    return true;
}

void TasManager::FinishMovieRun(bool completed) {
    ClearInputOverride();
    m_runTarget = m_playhead;
    m_runState = completed ? TasRunState::Idle : TasRunState::PausedAtMovieFrame;
    LOG(1, "[TAS] run finished completed=%d playhead=%u movie=%u frame=%u\n",
        completed ? 1 : 0, static_cast<unsigned int>(m_playhead),
        static_cast<unsigned int>(m_movie.size()), GetCurrentFrame());
    g_gameVals.isFrameFrozen = !completed;
    g_gameVals.framesToReach = completed ? 0 : GetCurrentFrame();

    if (completed && m_autoLoadAfterPlayback && HasBaseSnapshot()) {
        LoadBaseSnapshot();
        m_playhead = 0;
        m_runState = TasRunState::PausedAtMovieFrame;
    }
}

void TasManager::Update() {
    if (m_active && !IsInTrainingMatch()) {
        Exit();
        return;
    }
    if (!m_active || !g_gameVals.pFrameCount) {
        return;
    }

    // GetFrameCounter invokes us immediately before incrementing the game's frame count.
    // The input hooks have therefore finished the current frame, and currentFrame + 1
    // is the exact movie position after this callback returns.
    const unsigned int currentFrame = *g_gameVals.pFrameCount;
    if (m_runState == TasRunState::PresentationLeadIn ||
        m_runState == TasRunState::PresentationLeadOut) {
        if (m_presentationFramesRemaining > 0) {
            --m_presentationFramesRemaining;
        }
        if (m_presentationFramesRemaining == 0) {
            if (m_runState == TasRunState::PresentationLeadIn) {
                StartMovieFrames();
            } else {
                StopPlayback(true);
            }
            return;
        }
        ScheduleNeutralFrame();
        g_gameVals.isFrameFrozen = false;
        g_gameVals.framesToReach = 0;
        return;
    }

    if (m_runState != TasRunState::PlayingMovie && m_runState != TasRunState::ReplayingMovie) {
        return;
    }

    ++m_playhead;
    m_lastScheduledFrame = currentFrame + 1;
    CaptureKeyframeIfDue();
    if (m_playhead >= m_runTarget || m_playhead >= m_movie.size()) {
        const bool completed = m_runState == TasRunState::PlayingMovie && m_playhead >= m_movie.size();
        if (completed) {
            if (m_presentationMode) {
                StartPresentationLeadOut();
            } else {
                // Preview is an editing pass. Freeze on the resulting state so
                // the next frame can be authored without losing the combo.
                StopPlayback(false);
                m_status = L("Preview finished and paused at the movie end.");
            }
        } else {
            FinishMovieRun(false);
        }
        return;
    }

    ScheduleMovieFrame();
    if (m_runState == TasRunState::PlayingMovie) {
        g_gameVals.isFrameFrozen = false;
        g_gameVals.framesToReach = 0;
    } else {
        g_gameVals.isFrameFrozen = true;
        g_gameVals.framesToReach = currentFrame + 2;
    }
}

void TasManager::StartMovieFrames() {
    m_presentationFramesRemaining = 0;
    m_runState = TasRunState::PlayingMovie;
    m_runTarget = m_movie.size();
    m_lastScheduledFrame = GetCurrentFrame();
    ScheduleMovieFrame();
    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
    LOG(1, "[TAS] continuous playback started playhead=%u target=%u movie=%u frame=%u\n",
        static_cast<unsigned int>(m_playhead), static_cast<unsigned int>(m_runTarget),
        static_cast<unsigned int>(m_movie.size()), m_lastScheduledFrame);
}

void TasManager::StartPresentationLeadOut() {
    ClearInputOverride();
    m_runState = TasRunState::PresentationLeadOut;
    m_presentationFramesRemaining = kPresentationLeadOutFrames;
    ScheduleNeutralFrame();
    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
    LOG(1, "[TAS] presentation lead-out started frames=%u\n", kPresentationLeadOutFrames);
}

void TasManager::StartPlayback(bool presentationMode) {
    if (!m_active || m_movie.empty()) {
        SetError(L("Edit movie input first.").c_str());
        return;
    }

    // Preview picks up from wherever the playhead is, so you can watch just the tail of a
    // combo you are working on. Presentation is a recording of the whole thing, and playing
    // from the very end has nowhere to go, so both of those restart from the base state.
    const bool restartFromBase = presentationMode || m_playhead >= m_movie.size();
    if (restartFromBase) {
        if (!LoadBaseSnapshot()) {
            return;
        }
        m_playhead = 0;
    }
    m_presentationMode = presentationMode;

    m_playbackUiHidden = presentationMode;
    auto* frameHistory = WindowManager::GetInstance().GetWindowContainer()
        ->GetWindow<FrameHistoryWindow>(WindowType_FrameHistory);
    m_frameHistoryWasOpenBeforePlayback = frameHistory && frameHistory->IsOpen();
    if (frameHistory) {
        frameHistory->Close();
    }

    if (presentationMode) {
        ClearInputOverride();
        m_runState = TasRunState::PresentationLeadIn;
        m_runTarget = m_movie.size();
        m_presentationFramesRemaining = kPresentationLeadInFrames;
        ScheduleNeutralFrame();
        g_gameVals.isFrameFrozen = false;
        g_gameVals.framesToReach = 0;
        LOG(1, "[TAS] presentation lead-in started frames=%u movie=%u\n",
            kPresentationLeadInFrames, static_cast<unsigned int>(m_movie.size()));
    } else {
        StartMovieFrames();
    }
    m_error.clear();
}

void TasManager::StopPlayback(bool completed) {
    m_playbackUiHidden = false;
    m_presentationFramesRemaining = 0;
    m_presentationMode = false;
    auto* frameHistory = WindowManager::GetInstance().GetWindowContainer()
        ->GetWindow<FrameHistoryWindow>(WindowType_FrameHistory);
    if (frameHistory && m_frameHistoryWasOpenBeforePlayback) {
        frameHistory->Open();
    }
    m_frameHistoryWasOpenBeforePlayback = false;
    FinishMovieRun(completed);
}

void TasManager::EditAndAdvanceFrames(int count) {
    if (IsPlaybackRunning()) {
        SetError(L("Cannot advance frames during playback.").c_str());
        return;
    }
    if (!m_active || !HasBaseSnapshot() || count <= 0) {
        SetError(L("Save a base state before editing movie input.").c_str());
        return;
    }
    if (!m_inputsParsed && !ParseInputs()) {
        return;
    }

    const size_t frameCount = static_cast<size_t>(count);
    if (frameCount > (std::numeric_limits<size_t>::max)() - m_playhead) {
        SetError(L("Movie frame count exceeds the addressable size limit.").c_str());
        return;
    }
    PushUndoState();
    const size_t end = m_playhead + frameCount;
    if (m_playhead < m_movie.size()) {
        // This is the destructive edit the UI warns about: everything after the playhead
        // is replaced, which is exactly what makes it a rerecord rather than an undo.
        LOG(1, "[TAS] overwrite at frame %u: movie %u -> %u frames\n",
            static_cast<unsigned int>(m_playhead),
            static_cast<unsigned int>(m_movie.size()),
            static_cast<unsigned int>(end));
        m_movie.resize(m_playhead);
        InvalidateKeyframesAfter(m_playhead);
        ++m_rerecordCount;
    }
    if (m_movie.size() < end) {
        m_movie.resize(end, TasFrameInput{});
    }
    for (size_t i = m_playhead; i < end; ++i) {
        if (m_commandCursor < m_commandFrames.size()) {
            m_movie[i] = m_commandFrames[m_commandCursor++];
        } else {
            m_movie[i] = TasFrameInput{};
        }
    }
    BeginMovieRun(TasRunState::ReplayingMovie, end);
    m_error.clear();
}

bool TasManager::CanEditMovie() const {
    return m_active && !IsPlaybackRunning();
}

void TasManager::PushUndoState() {
    m_undoStack.push_back(MovieState{ m_movie, m_playhead });
    if (m_undoStack.size() > kUndoDepth) {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_redoStack.clear();
}

void TasManager::ResyncAfterEdit(size_t firstChangedFrame) {
    InvalidateKeyframesAfter(firstChangedFrame);

    if (m_playhead > m_movie.size()) {
        m_playhead = m_movie.size();
    }
    // Frames the match has already played were rewritten, so the state on screen was produced
    // by input that no longer exists. Replay to the same position from the stored movie.
    if (firstChangedFrame < m_playhead && HasBaseSnapshot()) {
        SeekToFrame(m_playhead);
    }
}

bool TasManager::InsertNeutralFrames(size_t index, size_t count) {
    if (!CanEditMovie() || count == 0) {
        return false;
    }
    if (index > m_movie.size()) {
        index = m_movie.size();
    }
    PushUndoState();
    m_movie.insert(m_movie.begin() + static_cast<ptrdiff_t>(index), count, TasFrameInput{});
    if (index <= m_playhead) {
        m_playhead += count;
    }
    LOG(1, "[TAS] insert %u neutral frame(s) at %u -> %u frames\n",
        static_cast<unsigned int>(count), static_cast<unsigned int>(index),
        static_cast<unsigned int>(m_movie.size()));
    ResyncAfterEdit(index);
    m_error.clear();
    return true;
}

bool TasManager::DeleteFrames(size_t index, size_t count) {
    if (!CanEditMovie() || count == 0 || index >= m_movie.size()) {
        return false;
    }
    count = (std::min)(count, m_movie.size() - index);
    PushUndoState();
    m_movie.erase(m_movie.begin() + static_cast<ptrdiff_t>(index),
        m_movie.begin() + static_cast<ptrdiff_t>(index + count));
    if (m_playhead > index) {
        m_playhead -= (std::min)(count, m_playhead - index);
    }
    LOG(1, "[TAS] delete %u frame(s) at %u -> %u frames\n",
        static_cast<unsigned int>(count), static_cast<unsigned int>(index),
        static_cast<unsigned int>(m_movie.size()));
    ResyncAfterEdit(index);
    m_error.clear();
    return true;
}

bool TasManager::MoveFrames(size_t fromIndex, size_t count, size_t toIndex, size_t* outNewIndex) {
    if (!CanEditMovie() || count == 0 || fromIndex >= m_movie.size()) {
        return false;
    }
    count = (std::min)(count, m_movie.size() - fromIndex);
    if (toIndex > m_movie.size()) {
        toIndex = m_movie.size();
    }
    // Dropping inside the block being moved, or exactly where it already is, is a no-op.
    if (toIndex >= fromIndex && toIndex <= fromIndex + count) {
        return false;
    }

    PushUndoState();
    const std::vector<TasFrameInput> block(m_movie.begin() + static_cast<ptrdiff_t>(fromIndex),
        m_movie.begin() + static_cast<ptrdiff_t>(fromIndex + count));
    m_movie.erase(m_movie.begin() + static_cast<ptrdiff_t>(fromIndex),
        m_movie.begin() + static_cast<ptrdiff_t>(fromIndex + count));

    const size_t insertAt = toIndex > fromIndex ? toIndex - count : toIndex;
    m_movie.insert(m_movie.begin() + static_cast<ptrdiff_t>(insertAt), block.begin(), block.end());

    if (outNewIndex) {
        *outNewIndex = insertAt;
    }
    LOG(1, "[TAS] move %u frame(s) from %u to %u\n",
        static_cast<unsigned int>(count), static_cast<unsigned int>(fromIndex),
        static_cast<unsigned int>(insertAt));
    ResyncAfterEdit((std::min)(fromIndex, insertAt));
    m_error.clear();
    return true;
}

bool TasManager::SetFrameInput(size_t index, TasFrameInput input) {
    if (!CanEditMovie() || index >= m_movie.size()) {
        return false;
    }
    PushUndoState();
    m_movie[index] = input;
    ResyncAfterEdit(index);
    m_error.clear();
    return true;
}

bool TasManager::DuplicateFrames(size_t index, size_t count) {
    if (!CanEditMovie() || count == 0 || index >= m_movie.size()) {
        return false;
    }
    count = (std::min)(count, m_movie.size() - index);
    PushUndoState();
    const std::vector<TasFrameInput> block(m_movie.begin() + static_cast<ptrdiff_t>(index),
        m_movie.begin() + static_cast<ptrdiff_t>(index + count));
    m_movie.insert(m_movie.begin() + static_cast<ptrdiff_t>(index + count), block.begin(), block.end());
    ResyncAfterEdit(index + count);
    m_error.clear();
    return true;
}

bool TasManager::Undo() {
    if (!CanEditMovie() || m_undoStack.empty()) {
        return false;
    }
    m_redoStack.push_back(MovieState{ m_movie, m_playhead });
    const MovieState state = m_undoStack.back();
    m_undoStack.pop_back();
    m_movie = state.movie;
    m_playhead = (std::min)(state.playhead, m_movie.size());
    ClearKeyframes();
    if (HasBaseSnapshot()) {
        SeekToFrame(m_playhead);
    }
    m_status = L("Undone.");
    return true;
}

bool TasManager::Redo() {
    if (!CanEditMovie() || m_redoStack.empty()) {
        return false;
    }
    m_undoStack.push_back(MovieState{ m_movie, m_playhead });
    const MovieState state = m_redoStack.back();
    m_redoStack.pop_back();
    m_movie = state.movie;
    m_playhead = (std::min)(state.playhead, m_movie.size());
    ClearKeyframes();
    if (HasBaseSnapshot()) {
        SeekToFrame(m_playhead);
    }
    m_status = L("Redone.");
    return true;
}

void TasManager::ResetParsedInputs() {
    if (!m_active) {
        return;
    }
    if (!m_inputsParsed && !ParseInputs()) {
        return;
    }

    for (auto& frame : m_commandFrames) {
        frame = TasFrameInput{};
    }
    m_commandCursor = 0;
    m_error.clear();
    m_status = L("Parsed inputs reset to neutral.");
}

void TasManager::ResetMovie() {
    if (!m_active) {
        return;
    }
    StopPlayback(false);
    if (HasBaseSnapshot()) {
        LoadBaseSnapshot();
    }
    ClearInputOverride();
    m_movie.clear();
    ClearKeyframes();
    m_undoStack.clear();
    m_redoStack.clear();
    m_playhead = 0;
    m_runTarget = 0;
    m_commandCursor = 0;
    m_runState = TasRunState::Idle;
    m_error.clear();
    m_status = L("Movie reset.");
}

bool TasManager::AdvanceOneFrame() {
    return AdvanceFrames(1);
}

bool TasManager::AdvanceFrames(int count) {
    if (count <= 0) {
        return true;
    }
    if (IsPlaybackRunning()) {
        SetError(L("Cannot advance frames during playback.").c_str());
        return false;
    }
    EditAndAdvanceFrames(count);
    return m_error.empty();
}

bool TasManager::SeekToFrame(size_t targetFrame) {
    if (IsPlaying()) {
        SetError(L("Cannot seek during playback.").c_str());
        return false;
    }
    if (!m_active || !HasBaseSnapshot()) {
        SetError(L("Save a base state before seeking.").c_str());
        return false;
    }
    if (targetFrame > m_movie.size()) {
        targetFrame = m_movie.size();
    }
    const int keyframe = FindKeyframeFor(targetFrame);
    if (keyframe >= 0 && m_keyframes[keyframe].movieFrame > 0) {
        // Restart from the nearest keyframe and re-simulate only the remainder.
        if (m_snapshotOwner->load_snapshot_index(keyframe + 1)) {
            ClearInputOverride();
            m_playhead = m_keyframes[keyframe].movieFrame;
            m_runTarget = m_playhead;
            m_lastScheduledFrame = GetCurrentFrame();
            g_gameVals.isFrameFrozen = true;
            g_gameVals.framesToReach = GetCurrentFrame();
            if (m_playhead == targetFrame) {
                m_runState = TasRunState::PausedAtMovieFrame;
                m_status = L("Seek complete.");
                return true;
            }
            m_status = L("Seeking...");
            return BeginMovieRun(TasRunState::ReplayingMovie, targetFrame);
        }
        LOG(1, "[TAS] keyframe load failed, falling back to the base state\n");
    }

    if (!LoadBaseSnapshot()) {
        return false;
    }

    // LoadBaseSnapshot leaves the playhead at 0, which is already the answer for a seek
    // to the start; anything further is re-simulated from the stored input.
    if (targetFrame == 0) {
        ClearInputOverride();
        m_runTarget = 0;
        m_runState = m_movie.empty() ? TasRunState::Idle : TasRunState::PausedAtMovieFrame;
        m_status = L("At the start of the movie.");
        return true;
    }

    m_status = L("Seeking...");
    return BeginMovieRun(TasRunState::ReplayingMovie, targetFrame);
}

bool TasManager::SeekRelative(int delta) {
    if (delta == 0) {
        return true;
    }
    if (delta > 0) {
        return AdvanceOrExtend(delta);
    }

    const size_t amount = static_cast<size_t>(-static_cast<long long>(delta));
    const size_t target = amount >= m_playhead ? 0 : m_playhead - amount;
    if (target == m_playhead) {
        return true;
    }
    return SeekToFrame(target);
}

bool TasManager::AdvanceOrExtend(int count) {
    if (count <= 0) {
        return true;
    }
    if (IsPlaybackRunning()) {
        SetError(L("Cannot seek during playback.").c_str());
        return false;
    }
    if (!m_active || !HasBaseSnapshot()) {
        SetError(L("Save a base state before seeking.").c_str());
        return false;
    }

    const size_t target = m_playhead + static_cast<size_t>(count);
    if (target > m_movie.size()) {
        // Running off the end is not an error: the combo simply gets that many idle frames,
        // which is what waiting looks like in a movie.
        m_movie.resize(target, TasFrameInput{});
    }

    // Forward movement never needs a reload - the match is already at m_playhead, so the
    // frames in between are simply played out from here.
    m_status.clear();
    return BeginMovieRun(TasRunState::ReplayingMovie, target);
}

void TasManager::ResumeGame() {
    ClearInputOverride();
    m_presentationFramesRemaining = 0;
    m_presentationMode = false;
    m_runTarget = m_playhead;
    m_runState = m_movie.empty() ? TasRunState::Idle : TasRunState::PausedAtMovieFrame;
    g_gameVals.isFrameFrozen = false;
    g_gameVals.framesToReach = 0;
}

bool TasManager::SaveBaseSnapshot() {
    if (!m_active || !IsInTrainingMatch()) {
        SetError(L("Enter a training match and enable TAS mode first.").c_str());
        return false;
    }
    if (!m_snapshotOwner || !m_snapshotOwner->check_if_valid(
        g_interfaces.player1.GetData(), g_interfaces.player2.GetData())) {
        delete m_snapshotOwner;
        m_snapshotOwner = new SnapshotApparatus();
        m_snapshotOwner->ReserveSlots("tas_editor", kTasSlotRequest);
        ClearKeyframes();
    }
    if (!m_snapshotOwner->save_snapshot_index(kTasBaseSlot)) {
        SetError(L("Base-state save failed.").c_str());
        return false;
    }

    m_snapshotSize = m_snapshotOwner->get_last_saved_snapshot_size();
    ClearKeyframes();
    m_baseFrame = GetCurrentFrame();
    m_playhead = 0;
    m_runTarget = 0;
    m_presentationFramesRemaining = 0;
    m_presentationMode = false;
    ClearInputOverride();
    g_gameVals.isFrameFrozen = true;
    g_gameVals.framesToReach = m_baseFrame;
    m_error.clear();
    return true;
}

bool TasManager::LoadBaseSnapshot() {
    if (!m_active || !IsInTrainingMatch() || !HasBaseSnapshot()) {
        SetError(L("No native base state is available.").c_str());
        return false;
    }
    ClearInputOverride();
    m_presentationFramesRemaining = 0;
    if (!m_snapshotOwner->load_snapshot_index(kTasBaseSlot)) {
        SetError(L("Native base-state load failed.").c_str());
        return false;
    }

    m_playhead = 0;
    m_runTarget = 0;
    m_lastScheduledFrame = GetCurrentFrame();
    g_gameVals.isFrameFrozen = true;
    g_gameVals.framesToReach = GetCurrentFrame();
    m_error.clear();
    return true;
}

bool TasManager::ExportMovie(const std::string& path, bool includeInitialConditions) {
    if (m_movie.empty() || path.empty()) {
        SetError(m_movie.empty() ? "There is no movie to export." : "Choose an export filename.");
        return false;
    }
    std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc);
    if (!output) {
        SetError(L("Could not open the TAS file for writing.").c_str());
        return false;
    }
    output << kTasMovieHeaderV2 << '\n';
    output << "frames " << m_movie.size() << '\n';
    if (includeInitialConditions) {
        output << "initial_conditions\n";
        output << "p1_character " << (g_interfaces.player1.GetData() ? getCharacterNameByIndexA(g_interfaces.player1.GetData()->charIndex) : "Unknown") << '\n';
        output << "p2_character " << (g_interfaces.player2.GetData() ? getCharacterNameByIndexA(g_interfaces.player2.GetData()->charIndex) : "Unknown") << '\n';
        output << "base_frame " << m_baseFrame << '\n';
        output << "cursor " << m_playhead << '\n';
        output << "base_snapshot " << (HasBaseSnapshot() ? "available_current_process_only" : "not_saved") << '\n';
        output << "end_initial_conditions\n";
    }
    output << "# Inputs use numpad notation: 7 8 9 / 4 5 6 / 1 2 3; suffixes A B C D are buttons, ap is the taunt button.\n";
    for (size_t i = 0; i < m_movie.size(); ++i) {
        output << i << " | P1=" << HumanInput(m_movie[i].p1)
               << " | P2=" << HumanInput(m_movie[i].p2) << '\n';
    }
    if (!output) {
        SetError(L("Failed while writing the TAS file.").c_str());
        return false;
    }
    m_error.clear();
    {
        char formatted[512];
        std::snprintf(formatted, sizeof(formatted), Messages.Exported_s(), path.c_str());
        m_status = formatted;
    }
    LOG(1, "[TAS] exported movie path=%s frames=%u\n", path.c_str(), static_cast<unsigned int>(m_movie.size()));
    return true;
}

bool TasManager::ImportMovie(const std::string& path) {
    std::ifstream input(path.c_str());
    if (!input) {
        SetError(L("Could not open the selected TAS file.").c_str());
        return false;
    }
    std::string header;
    if (!std::getline(input, header)) {
        SetError(L("The TAS file is empty.").c_str());
        return false;
    }
    if (!header.empty() && header.back() == '\r') header.pop_back();
    std::string framesLabel;
    size_t declaredCount = 0;
    if (header == kTasMovieHeaderV1) {
        if (!(input >> framesLabel >> declaredCount) || framesLabel != "frames" || declaredCount == 0) {
            SetError(L("The V1 TAS file has an invalid frame count.").c_str());
            return false;
        }
    } else if (header == kTasMovieHeaderV2) {
        if (!(input >> framesLabel >> declaredCount) || framesLabel != "frames" || declaredCount == 0) {
            SetError(L("The V2 TAS file has an invalid frame count.").c_str());
            return false;
        }
        std::string line;
        std::getline(input, line);
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            if (line == "initial_conditions") {
                while (std::getline(input, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line == "end_initial_conditions") break;
                }
                continue;
            }
            break;
        }
        if (input && !line.empty() && line[0] != '#') {
            std::vector<TasFrameInput> imported;
            for (size_t expected = 0; expected < declaredCount; ++expected) {
                if (expected != 0 && !std::getline(input, line)) {
                    SetError(L("The V2 TAS file is missing frame data.").c_str());
                    return false;
                }
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t firstBar = line.find(" | P1=");
                const size_t secondBar = line.find(" | P2=", firstBar == std::string::npos ? 0 : firstBar + 6);
                if (firstBar == std::string::npos || secondBar == std::string::npos) {
                    SetError(L("The V2 TAS file contains an invalid frame.").c_str());
                    return false;
                }
                std::istringstream indexStream(line.substr(0, firstBar));
                size_t index = 0;
                if (!(indexStream >> index) || index != expected) {
                    SetError(L("The V2 TAS file contains an invalid frame index.").c_str());
                    return false;
                }
                const std::string p1Text = line.substr(firstBar + 6, secondBar - (firstBar + 6));
                const std::string p2Text = line.substr(secondBar + 6);
                uint16_t p1 = 0, p2 = 0;
                if (!ParseHumanInput(p1Text, &p1) || !ParseHumanInput(p2Text, &p2)) {
                    SetError(L("The V2 TAS file contains an invalid input.").c_str());
                    return false;
                }
                imported.push_back(TasFrameInput{p1, p2});
            }
            if (imported.size() != declaredCount) {
                SetError(L("The V2 TAS file is missing frame data.").c_str());
                return false;
            }
            ClearInputOverride(); ClearSnapshot(); m_movie.swap(imported);
            m_playhead = 0; m_runTarget = 0; m_presentationFramesRemaining = 0;
            m_presentationMode = false; m_runState = TasRunState::Idle;
            m_error.clear(); {
        char formatted[512];
        std::snprintf(formatted, sizeof(formatted),
            Messages.Imported_s_Save_a_matching_base_state_before_playback(), path.c_str());
        m_status = formatted;
    }
            return true;
        }
        SetError(L("The V2 TAS file contains no frame data.").c_str());
        return false;
    } else {
        SetError(L("The TAS file has an unknown format.").c_str());
        return false;
    }
    std::vector<TasFrameInput> imported;
    imported.reserve(declaredCount);
    for (size_t expectedIndex = 0; expectedIndex < declaredCount; ++expectedIndex) {
        size_t index = 0; unsigned int p1 = 0, p2 = 0;
        if (!(input >> index >> p1 >> p2) || index != expectedIndex || p1 > UINT16_MAX || p2 > UINT16_MAX ||
            (p1 & 0xF) < 1 || (p1 & 0xF) > 9 || (p2 & 0xF) < 1 || (p2 & 0xF) > 9) {
            SetError(L("The V1 TAS file contains an invalid frame.").c_str()); return false;
        }
        imported.push_back(TasFrameInput{static_cast<uint16_t>(p1), static_cast<uint16_t>(p2)});
    }
    ClearInputOverride(); ClearSnapshot(); m_movie.swap(imported);
    m_playhead = 0; m_runTarget = 0; m_presentationFramesRemaining = 0;
    m_presentationMode = false; m_runState = TasRunState::Idle;
    m_error.clear(); {
        char formatted[512];
        std::snprintf(formatted, sizeof(formatted),
            Messages.Imported_s_Save_a_matching_base_state_before_playback(), path.c_str());
        m_status = formatted;
    }
    LOG(1, "[TAS] imported movie path=%s frames=%u\n", path.c_str(), static_cast<unsigned int>(m_movie.size()));
    return true;
}

bool TasManager::TryParseCommand(const std::string& text, std::vector<uint16_t>* out) {
    if (!out) {
        return false;
    }
    out->clear();
    uint16_t pendingButtons = 0;
    bool sawDirection = false;
    for (size_t i = 0; i < text.size(); ) {
        const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
        if (ch >= '1' && ch <= '9') {
            if (pendingButtons && !out->empty()) {
                out->back() = static_cast<uint16_t>(out->back() + pendingButtons);
                pendingButtons = 0;
            }
            out->push_back(static_cast<uint16_t>(ch - '0'));
            sawDirection = true;
            ++i;
        } else if (ch == 'A' && i + 1 < text.size() &&
                   static_cast<char>(std::toupper(static_cast<unsigned char>(text[i + 1]))) == 'P') {
            // "ap" = taunt button attached to the current frame. Checked before the single
            // 'A' branch so 5ap does not get parsed as "5A" followed by an invalid 'p'.
            if (!sawDirection || out->empty() || (pendingButtons & kInputButtonTaunt)) {
                return false;
            }
            pendingButtons = static_cast<uint16_t>(pendingButtons + kInputButtonTaunt);
            i += 2;
        } else if (ch == 'A' || ch == 'B' || ch == 'C' || ch == 'D') {
            if (!sawDirection || out->empty()) {
                return false;
            }
            pendingButtons = static_cast<uint16_t>(pendingButtons + ButtonValue(ch));
            ++i;
        } else if (ch != ' ' && ch != ',' && ch != '-') {
            return false;
        } else {
            ++i;
        }
    }
    if (pendingButtons && !out->empty()) {
        out->back() = static_cast<uint16_t>(out->back() + pendingButtons);
    }
    return !out->empty();
}

bool TasManager::ParseInputs() {
    std::vector<uint16_t> p1;
    std::vector<uint16_t> p2;
    if (!TryParseCommand(m_p1Text, &p1)) {
        SetError(L("Invalid P1 input. Use examples such as 5C, 28D, 623C, 656, or 5ap.").c_str());
        return false;
    }
    if (!TryParseCommand(m_p2Text, &p2)) {
        SetError(L("Invalid P2 input. Use examples such as 5C, 28D, 623C, 656, or 5ap.").c_str());
        return false;
    }

    const size_t frameCount = p1.size() > p2.size() ? p1.size() : p2.size();
    m_commandFrames.assign(frameCount, TasFrameInput{});
    for (size_t i = 0; i < frameCount; ++i) {
        m_commandFrames[i].p1 = i < p1.size() ? p1[i] : 5;
        m_commandFrames[i].p2 = i < p2.size() ? p2[i] : 5;
    }
    m_commandCursor = 0;
    m_inputsParsed = true;
    m_error.clear();
    return true;
}

bool TasManager::SetInputText(const std::string& p1, const std::string& p2) {
    m_p1Text = p1;
    m_p2Text = p2;
    return ParseInputs();
}

std::string TasManager::FormatInput(uint16_t packed) {
    std::string result(1, static_cast<char>('0' + (packed & 0x0F)));
    if (packed & 16) result += 'A';
    if (packed & 32) result += 'B';
    if (packed & 64) result += 'C';
    if (packed & 128) result += 'D';
    if (packed & kInputButtonTaunt) result += "ap";
    return result;
}

TasFrameInput TasManager::GetMovieFrame(size_t index) const {
    return index < m_movie.size() ? m_movie[index] : TasFrameInput{};
}

TasFrameInput TasManager::GetCommandInput() const {
    if (m_commandCursor >= m_commandFrames.size()) {
        return TasFrameInput{};
    }
    return m_commandFrames[m_commandCursor];
}

TasFrameInput TasManager::GetMovieInput() const {
    return m_playhead < m_movie.size() ? m_movie[m_playhead] : TasFrameInput{};
}

TasFrameInput TasManager::GetCurrentInput() const {
    return IsEditingRecording() || IsPlaying() || m_runState == TasRunState::ReplayingMovie
        ? GetMovieInput() : GetCommandInput();
}

TasFrameInput TasManager::GetCurrentPlaybackInput() const {
    return m_hasScheduledInput ? m_scheduledInput : GetMovieInput();
}

TasFrameInput TasManager::GetLastRecordedInput() const {
    return m_movie.empty() ? TasFrameInput{} : m_movie.back();
}

unsigned int TasManager::GetCurrentFrame() const {
    return g_gameVals.pFrameCount ? *g_gameVals.pFrameCount : 0;
}
