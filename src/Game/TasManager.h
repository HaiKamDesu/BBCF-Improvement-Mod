#pragma once

#include "Game/SnapshotApparatus/SnapshotApparatus.h"

#include <cstdint>
#include <string>
#include <vector>

struct TasFrameInput {
    uint16_t p1 = 5;
    uint16_t p2 = 5;
};

enum class TasRunState {
    Idle,
    PausedAtMovieFrame,
    PresentationLeadIn,
    PlayingMovie,
    PresentationLeadOut,
    ReplayingMovie
};

class TasManager {
public:
    static TasManager& Instance();

    bool IsActive() const { return m_active; }
    bool HasBaseSnapshot() const { return m_snapshotOwner != nullptr && m_snapshotOwner->snapshot_count > 0 && m_snapshotSize > 0; }
    bool IsInTrainingMatch() const;
    // --- live recording ----------------------------------------------------------------
    // Hands one player back to whoever is holding the controller and samples what they do,
    // one packed input per game frame. Nothing is written to the movie: the capture comes
    // back as notation for the composer's text field, so a played sequence becomes an
    // ordinary typed command the user can edit and commit (or throw away) as usual.
    // The other player is held to opponentFrames (its own text field, already parsed, and
    // neutral once it runs out) because that is exactly what a commit will replay for it.
    // Holding it to anything else - the movie's current contents included - would record
    // the capture against an opponent it never plays back against.
    bool StartLiveRecording(int player, std::vector<uint16_t> opponentFrames);
    void StopLiveRecording();
    bool IsLiveRecording() const { return m_liveRecording; }
    // 0 or 1 while recording, -1 otherwise.
    int GetLiveRecordingPlayer() const { return m_liveRecording ? m_liveRecordingPlayer : -1; }
    size_t GetLiveRecordingFrameCount() const { return m_liveRecordingFrames.size(); }
    // Capped so a recording left running cannot outgrow the composer's text field.
    static size_t GetLiveRecordingFrameLimit();
    // The finished capture as numpad notation, empty if nothing was recorded. Truncates on
    // a frame boundary if it will not fit in maxChars (and says so in the status), so the
    // caller always gets something the composer can parse. Clears the capture.
    std::string TakeLiveRecordingNotation(size_t maxChars);

    bool IsRecording() const { return false; }
    bool HasRecording() const { return !m_movie.empty(); }
    bool IsPlaying() const {
        return m_runState == TasRunState::PresentationLeadIn ||
            m_runState == TasRunState::PlayingMovie ||
            m_runState == TasRunState::PresentationLeadOut;
    }
    bool IsPlaybackRunning() const {
        return IsPlaying() || m_runState == TasRunState::ReplayingMovie;
    }
    bool IsEditingRecording() const { return m_runState == TasRunState::PausedAtMovieFrame && !m_movie.empty(); }
    bool IsPlaybackUiHidden() const { return m_playbackUiHidden; }
    size_t GetRecordedFrameCount() const { return m_movie.size(); }

    void Enter();
    void Exit();
    void Update();

    void StartPlayback(bool presentationMode);
    void StopPlayback(bool completed = false);
    void EditAndAdvanceFrames(int count);
    void ResetParsedInputs();
    void ResetMovie();
    // --- structural edits -------------------------------------------------------------
    // The movie is just a list, so it can be edited anywhere, not only at the playhead.
    // Editing at or after the playhead costs nothing; editing behind it re-seeks so what is
    // on screen still matches the list. All of them are undoable and refuse to run mid-run.
    bool InsertNeutralFrames(size_t index, size_t count);
    bool DeleteFrames(size_t index, size_t count);
    // outNewIndex receives where the block ended up, so a caller can keep it selected.
    bool MoveFrames(size_t fromIndex, size_t count, size_t toIndex, size_t* outNewIndex = nullptr);
    bool SetFrameInput(size_t index, TasFrameInput input);
    bool DuplicateFrames(size_t index, size_t count);

    bool CanUndo() const { return !m_undoStack.empty(); }
    bool CanRedo() const { return !m_redoStack.empty(); }
    bool Undo();
    bool Redo();

    bool ExportMovie(const std::string& path, bool includeInitialConditions);
    bool ImportMovie(const std::string& path);

    bool SaveBaseSnapshot();
    bool LoadBaseSnapshot();
    // How many keyframes are currently held, for the UI to show why seeking is fast or slow.
    int GetKeyframeCount() const;
    bool SetInputText(const std::string& p1, const std::string& p2);
    bool AdvanceOneFrame();
    bool AdvanceFrames(int count);
    void ResumeGame();

    // Moves the match to a frame of the movie without changing it: reloads the base state
    // and re-simulates the stored input up to that point. This is how the transport bar
    // scrubs. Nothing is deleted until the user actually commits new input there.
    bool SeekToFrame(size_t targetFrame);
    // Seek relative to the playhead. Negative goes back. Forward past the end of the movie
    // extends it with neutral frames rather than stopping, so stepping forward always does
    // something - overshooting the end by 5 simply appends 5 idle frames.
    bool SeekRelative(int delta);
    // Move forward `count` frames, appending neutral frames if that runs off the end.
    // Never destroys anything: frames between here and the target are replayed as they are.
    bool AdvanceOrExtend(int count);
    // True while a seek or a commit is still re-simulating its way to the target.
    bool IsSeeking() const { return m_runState == TasRunState::ReplayingMovie; }
    size_t GetRunTarget() const { return m_runTarget; }

    // Parses one player's numpad-notation command into one packed input per frame,
    // without touching manager state, so the UI can validate as the user types.
    static bool TryParseCommand(const std::string& text, std::vector<uint16_t>* outFrames);
    // Renders one packed input back into numpad notation ("5", "3C", "2AB").
    static std::string FormatInput(uint16_t packed);

    // Committed movie frames, for drawing the timeline. Out-of-range reads give neutral.
    TasFrameInput GetMovieFrame(size_t index) const;
    // Frames waiting in the parsed command queue, and how many have been consumed.
    size_t GetQueuedFrameCount() const { return m_commandFrames.size(); }
    size_t GetQueueCursor() const { return m_commandCursor; }
    TasRunState GetRunState() const { return m_runState; }

    TasFrameInput GetCurrentPlaybackInput() const;
    TasFrameInput GetLastRecordedInput() const;
    TasFrameInput GetCurrentInput() const;
    TasFrameInput GetCommandInput() const;
    TasFrameInput GetMovieInput() const;

    bool IsAutoLoadAfterPlayback() const { return m_autoLoadAfterPlayback; }
    void SetAutoLoadAfterPlayback(bool enabled) { m_autoLoadAfterPlayback = enabled; }
    unsigned int GetRerecordCount() const { return m_rerecordCount; }

    const std::string& GetP1Text() const { return m_p1Text; }
    const std::string& GetP2Text() const { return m_p2Text; }
    void SetP1Text(const std::string& value) { m_p1Text = value; m_inputsParsed = false; }
    void SetP2Text(const std::string& value) { m_p2Text = value; m_inputsParsed = false; }

    const std::string& GetError() const { return m_error; }
    const std::string& GetStatus() const { return m_status; }
    unsigned int GetBaseFrame() const { return m_baseFrame; }
    unsigned int GetCurrentFrame() const;
    size_t GetCursor() const { return m_playhead; }
    size_t GetFrameCount() const { return m_movie.size(); }

private:
    TasManager() = default;
    ~TasManager();
    TasManager(const TasManager&) = delete;
    TasManager& operator=(const TasManager&) = delete;

    bool ParseInputs();
    // Snapshot the movie before a structural edit so it can be stepped back through.
    void PushUndoState();
    // Bring the match back in line with the movie after frames at or before the playhead
    // changed underneath it.
    void ResyncAfterEdit(size_t firstChangedFrame);
    bool CanEditMovie() const;
    bool BeginMovieRun(TasRunState state, size_t target);
    void ScheduleMovieFrame();
    void FinishMovieRun(bool completed);
    void ClearInputOverride();
    void ClearSnapshot();
    // Keyframes are savestates taken part-way through the movie so a seek can restart from
    // the nearest one instead of replaying from the base every time. They live in the slots
    // this manager reserved; there is no per-frame state, which would need 10 MiB a frame.
    void ClearKeyframes();
    void CaptureKeyframeIfDue();
    void InvalidateKeyframesAfter(size_t frame);
    // Best starting point for a seek: the latest keyframe at or before the target.
    // Returns -1 when the base state is the best we have.
    int FindKeyframeFor(size_t targetFrame) const;
    void SetError(const char* message);
    void StartMovieFrames();
    void StartPresentationLeadOut();
    void ScheduleNeutralFrame();

    void SampleLiveRecordingFrame();

    bool m_active = false;
    bool m_liveRecording = false;
    int m_liveRecordingPlayer = 0;
    // Where the playhead sat when recording began: the match runs on while the user plays,
    // so stopping seeks back here and leaves the editor exactly where they left it.
    size_t m_liveRecordingReturnFrame = 0;
    bool m_liveRecordingHasFrame = false;
    unsigned int m_liveRecordingLastFrame = 0;
    std::vector<uint16_t> m_liveRecordingFrames;
    std::vector<uint16_t> m_liveRecordingOpponentFrames;
    bool m_p2KeyboardOverrideWasEnabled = false;
    bool m_frameHistoryOpenedByTas = false;
    SnapshotApparatus* m_snapshotOwner = nullptr;

    // Logical slot 0 of the reserved range holds the base state; 1..N hold keyframes.
    struct Keyframe {
        size_t movieFrame = 0;
        bool valid = false;
    };
    std::vector<Keyframe> m_keyframes;
    size_t m_nextKeyframeSlot = 0;
    Snapshot* m_snapshotBuffer = nullptr;
    int m_snapshotSize = 0;
    unsigned int m_baseFrame = 0;
    unsigned int m_rerecordCount = 0;

    TasRunState m_runState = TasRunState::Idle;
    std::vector<TasFrameInput> m_movie;

    // A movie is a few bytes a frame, so undo just keeps whole copies rather than a diff.
    struct MovieState {
        std::vector<TasFrameInput> movie;
        size_t playhead = 0;
    };
    std::vector<MovieState> m_undoStack;
    std::vector<MovieState> m_redoStack;
    size_t m_playhead = 0;
    size_t m_runTarget = 0;
    unsigned int m_presentationFramesRemaining = 0;
    bool m_presentationMode = false;
    TasFrameInput m_scheduledInput{};
    bool m_hasScheduledInput = false;
    unsigned int m_lastScheduledFrame = 0;

    bool m_inputsParsed = false;
    std::vector<TasFrameInput> m_commandFrames;
    size_t m_commandCursor = 0;
    bool m_autoLoadAfterPlayback = false;
    bool m_playbackUiHidden = false;
    bool m_frameHistoryWasOpenBeforePlayback = false;
    std::string m_p1Text = "5";
    std::string m_p2Text = "5";
    std::string m_error;
    std::string m_status;
};
