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
    bool ExportMovie(const std::string& path, bool includeInitialConditions);
    bool ImportMovie(const std::string& path);

    bool SaveBaseSnapshot();
    bool LoadBaseSnapshot();
    bool SetInputText(const std::string& p1, const std::string& p2);
    bool AdvanceOneFrame();
    bool AdvanceFrames(int count);
    void ResumeGame();
    bool RewindFrames(int count);

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
    bool ParseOne(const std::string& text, std::vector<uint16_t>* out) const;
    bool BeginMovieRun(TasRunState state, size_t target);
    void ScheduleMovieFrame();
    void FinishMovieRun(bool completed);
    void ClearInputOverride();
    void ClearSnapshot();
    void SetError(const char* message);
    void StartMovieFrames();
    void StartPresentationLeadOut();
    void ScheduleNeutralFrame();

    bool m_active = false;
    bool m_p2KeyboardOverrideWasEnabled = false;
    bool m_frameHistoryOpenedByTas = false;
    SnapshotApparatus* m_snapshotOwner = nullptr;
    Snapshot* m_snapshotBuffer = nullptr;
    int m_snapshotSize = 0;
    unsigned int m_baseFrame = 0;
    unsigned int m_rerecordCount = 0;

    TasRunState m_runState = TasRunState::Idle;
    std::vector<TasFrameInput> m_movie;
    size_t m_playhead = 0;
    size_t m_runTarget = 0;
    unsigned int m_presentationFramesRemaining = 0;
    bool m_presentationMode = false;
    bool m_truncateOnReplayFinish = false;
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
