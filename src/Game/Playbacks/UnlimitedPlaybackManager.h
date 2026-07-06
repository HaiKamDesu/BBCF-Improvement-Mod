#pragma once

#include "Game/Playbacks/PlaybackManager.h"
#include "Game/Playbacks/CompatibilityManager.h"
#include "Game/SnapshotApparatus/SnapshotApparatus.h"

#include <array>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

class UnlimitedPlaybackManager {
public:
    enum Mode {
        Mode_Default = 0,
        Mode_Unlimited = 1,
    };

    enum SelectionMode {
        Selection_Random = 0,
        Selection_Sequential = 1,
        Selection_NonRepeatingRandom = 2,
    };

    enum TriggerType {
        Trigger_Wakeup = 0,
        Trigger_Gap,
        Trigger_OnBlock,
        Trigger_OnHit,
        Trigger_ThrowTech,
        Trigger_KeyPress,
        Trigger_OnLoop,
        Trigger_Count,
    };

    enum LoopResetMode {
        LoopReset_Middle = 0,
        LoopReset_Left = 1,
        LoopReset_Right = 2,
        LoopReset_Custom = 3,
    };

    struct TriggerConfig {
        bool enabled = false;
        int cooldownFrames = 1;
        int keyCode = 0;
        int lastTriggeredFrame = -999999;
    };

    struct PlaybackEntry {
        std::string id;
        std::string name;
        std::string relativePath;
        bool enabled = true;
        float weight = 1.0f;
        std::array<bool, Trigger_Count> triggerEnabled = { true, true, true, true, true, true, true };
    };

    struct CachedPlayback {
        bool loaded = false;
        bool facingLeft = false;
        std::vector<char> frames;
    };

    struct ToastMessage {
        std::string key;
        std::string text;
        unsigned long long createdAtMs = 0;
        unsigned long long durationMs = 5000;
        bool sticky = false;
    };

    static UnlimitedPlaybackManager& Instance();

    void InitializeIfNeeded();
    void Tick();
    // Called from a hook point that runs BEFORE the game reads this tick's active-slot playback
    // input (earlier in the frame than Tick()/GetFrameCounter). Only handles the parts of
    // starting playback that have no real-time game-state dependency (i.e. "Play Now"), so that
    // the recorded input takes effect on the same tick as the request rather than one tick late.
    void RunPreTick();
    void OnMatchInit();
    void ForceResetTriggers(const char* toastText = nullptr);
    void OnMatchEnd();
    void DebugLogState(const char* tag) const;

    int GetMode() const;
    void SetMode(int mode);
    int GetSelectionMode() const;
    void SetSelectionMode(int mode);
    bool GetAutoMirrorOnSideSwap() const;
    void SetAutoMirrorOnSideSwap(bool enabled);
    int GetLoopKeyCode() const;
    void SetLoopKeyCode(int keyCode);
    float GetLoopSetupSeconds() const;
    void SetLoopSetupSeconds(float seconds);
    float GetLoopEndingSeconds() const;
    void SetLoopEndingSeconds(float seconds);
    bool GetLoopRestartLabState() const;
    void SetLoopRestartLabState(bool enabled);
    int GetLoopRestartMode() const;
    void SetLoopRestartMode(int mode);
    bool IsLoopActive() const;
    bool GetLoopSetupCountdown(float* outRemainingSeconds, float* outTotalSeconds) const;
    bool HasLoopCustomSnapshot() const;
    bool CaptureLoopCustomSnapshot();
    bool LoadLoopCustomSnapshot();
    void ClearLoopCustomSnapshot();

    const std::vector<PlaybackEntry>& GetEntries() const;
    std::vector<PlaybackEntry>& GetEntriesMutable();

    TriggerConfig& GetTrigger(TriggerType type);
    const TriggerConfig& GetTrigger(TriggerType type) const;

    bool AddPlaybackFile(const std::string& sourcePath, const std::string& displayName, bool forceLoadIncompatible = false);
    CompatibilityManager::Result ProbePlaybackCompatibility(const std::string& playbackPath) const;
    bool CaptureSlotToLibrary(int slot, const std::string& displayName);
    bool StartReplayRecording(bool recordP1);
    bool StopReplayRecordingAndSave(const std::string& displayName);
    void CancelReplayRecording(const char* reason = nullptr);
    bool IsReplayRecording() const;
    bool IsReplayRecordingAsP1() const;
    int GetReplayRecordingStartFrame() const;
    bool RemoveEntryByIndex(size_t idx);
    bool MoveEntry(size_t fromIdx, size_t toIdx);
    void SetAllEntriesEnabled(bool enabled);
    bool RenameEntry(size_t idx, const std::string& newName);

    bool LoadEntryIntoSlot(size_t idx, int slot);
    bool SaveEntryFromSlot(size_t idx, int slot);
    bool ReadEntryPlayback(size_t idx, bool* outFacingLeft, std::vector<char>* outFrames);
    bool WriteEntryPlayback(size_t idx, bool facingLeft, const std::vector<char>& frames);
    bool SaveEntryToFile(size_t idx, const std::string& outputPath);
    bool PlayEntryNow(size_t idx);

    void ClearAll();

    bool SaveProfile(const std::string& profilePath);
    bool LoadProfile(const std::string& profilePath, bool forceLoadIncompatible = false);
    CompatibilityManager::Result ProbeProfileCompatibility(const std::string& profilePath) const;

    std::string GetActiveProfilePath() const;
    void SetActiveProfilePath(const std::string& path);

    std::string GetStatusText() const;
    const std::deque<ToastMessage>& GetToasts() const;
    void PruneExpiredToasts();
    void PushToast(const std::string& text, unsigned long long durationMs = 5000);
    void PushStickyToast(const std::string& key, const std::string& text);
    void RemoveStickyToast(const std::string& key);

    static const int kMaxFramesPerPlayback = 1200;

private:
    UnlimitedPlaybackManager();

    void EnsureFolders();
    std::string GetLibraryFolder() const;
    std::string GetProfileFolder() const;

    std::string MakeEntryId();
    std::string SanitizeFileName(const std::string& input) const;
    std::string BuildUniqueRelativePath(const std::string& preferredName) const;
    std::string EnsureEntryLibraryRelativePath(size_t idx);

    bool ReadPlaybackFile(const std::string& fullPath, CachedPlayback* out, bool forceLoadIncompatible = false);
    bool WritePlaybackFile(const std::string& fullPath, bool facingLeft, const std::vector<char>& frames);
    bool IsReplayMatchActive() const;
    bool BuildPlaybackFramesFromReplayRange(int round, int startFrame, int endFrameExclusive, int recordedPlayer, std::vector<char>* outFrames) const;

    bool PickEntryIndexForTrigger(TriggerType trigger, size_t* outIndex);
    bool TryFireTrigger(TriggerType trigger, int currentFrame);
    void ProcessLoopTick(int currentFrame);
    void StartLoop(int currentFrame);
    void StopLoop(const char* reason = nullptr);
    bool TryStartLoopPlayback();
    void ResetLoopPlaybackCompletionState();
    void ObserveLoopPlaybackActionState();
    bool IsLoopPlaybackAnimationComplete(int currentFrame);
    bool IsKeyPressedEdge(int virtualKey);
    void SyncKeyEdgeState();
    bool IsTriggerKeyDown(int virtualKey) const;
    void ResetTriggerRuntimeState(bool enableRuntime);
    void LogRuntimeGateState(const char* tag) const;
    void LogEntryCacheSummary(const char* tag) const;

    bool ShouldTriggerWakeup();
    bool ShouldTriggerGap();
    bool ShouldTriggerOnBlock();
    bool ShouldTriggerOnHit();
    bool ShouldTriggerThrowTech();
    std::vector<size_t> BuildCandidatesForTrigger(TriggerType trigger);
    bool TryGetCurrentFacingLeft(bool* outFacingLeft) const;
    unsigned char MirrorDirectionalNibble(unsigned char dir) const;
    void MirrorPlaybackInputsInPlace(std::vector<char>& frames) const;
    void BackupRuntimeSlotIfNeeded();
    void TryRestoreRuntimeSlotAfterPlayback();
    void ResetRuntimePlaybackState(bool discardBackupOnly);
    void StartRuntimePlayback(const std::vector<char>& frames, int facingToLoad);
    void ExecutePendingPlayNow();
    void LogSlot4PlaybackDiagnostics();
    bool EnsureLoopSnapshotApparatus(bool preserveCustomSnapshot = false);
    bool RestoreLoopCustomSnapshot(bool showToast);
    bool ApplyLoopRestart();
    void StartNativeTrainingResetCombo(LoopResetMode mode);
    void CaptureNativeTrainingResetInputSnapshot(LoopResetMode mode);
    void NeutralizeNativeTrainingResetDirections(LoopResetMode mode);
    void ReleaseNativeTrainingResetCombo();
    void SendNativeTrainingResetKey(WORD virtualKey, bool keyDown) const;
    int LoopSecondsToFrames(float seconds) const;

    int m_mode = Mode_Default;
    bool m_initialized = false;
    std::string m_activeProfilePath;
    std::vector<PlaybackEntry> m_entries;
    std::unordered_map<std::string, CachedPlayback> m_cache;
    std::array<TriggerConfig, Trigger_Count> m_triggers;
    int m_selectionMode = Selection_Random;
    bool m_autoMirrorOnSideSwap = true;
    std::array<size_t, Trigger_Count> m_sequentialIndex = {};
    std::array<std::vector<size_t>, Trigger_Count> m_nonRepeatPools = {};
    std::string m_statusText;
    std::deque<ToastMessage> m_toasts;
    unsigned long long m_entrySerial = 0;
    std::string m_lastLoadedProfileFolder;

    // Runtime edge tracking for game-state triggers
    bool m_prevWakeupCondition = false;
    bool m_prevGapCondition = false;
    bool m_prevOnBlockCondition = false;
    bool m_prevOnHitCondition = false;
    bool m_prevThrowTechCondition = false;
    std::array<bool, 256> m_prevKeyDown = {};
    std::array<bool, 16> m_prevControllerBindDown = {};
    bool m_keyPressTriggerArmed = false;
    bool m_triggerRuntimeEnabled = true;
    bool m_profileRuntimeSuppressedUntilReset = false;
    int m_lastObservedFrame = -1;
    bool m_runtimeSlotBackupValid = false;
    bool m_runtimeSlotRestorePending = false;
    bool m_pendingPlayNowRequested = false;
    size_t m_pendingPlayNowIndex = 0;
    bool m_diagSlot4WasActive = false;
    int m_diagSlot4LastLoggedPosition = -2;
    bool m_runtimeSlotBackupFacingLeft = false;
    std::vector<char> m_runtimeSlotBackupFrames;
    int m_runtimeSlotNumber = 1;
    bool m_runtimeActiveSlotBackupValid = false;
    int m_runtimeActiveSlotBackup = 0;
    bool m_runtimePlaybackTypeBackupValid = false;
    int m_runtimePlaybackTypeBackup = 0;
    bool m_replayRecordingActive = false;
    bool m_replayRecordingAsP1 = true;
    int m_replayRecordingRound = 0;
    int m_replayRecordingStartFrame = 0;
    int m_loopKeyCode = VK_F7;
    float m_loopSetupSeconds = 0.0f;
    float m_loopEndingSeconds = 0.0f;
    bool m_loopRestartLabState = false;
    int m_loopRestartMode = LoopReset_Middle;
    bool m_loopActive = false;
    enum LoopPhase {
        LoopPhase_Idle = 0,
        LoopPhase_Setup,
        LoopPhase_Playing,
        LoopPhase_Ending,
    };
    LoopPhase m_loopPhase = LoopPhase_Idle;
    int m_loopPhaseStartFrame = -1;
    bool m_loopRestartAppliedForCycle = false;
    bool m_loopPlaybackObservedNonIdle = false;
    int m_loopPlaybackInputEndedFrame = -1;
    int m_loopPlaybackIdleSinceFrame = -1;
    bool m_loopNativeResetPulseActive = false;
    unsigned long long m_loopNativeResetReleaseAtMs = 0;
    std::array<WORD, 3> m_loopNativeResetKeys = {};
    std::vector<WORD> m_loopNativeResetRestoreKeys;
    std::vector<char> m_loopCustomSnapshotBytes;
    int m_loopCustomSnapshotSize = 0;
    int m_loopCustomSnapshotSlotIndex = -1;
    SnapshotApparatus* m_loopSnapshotApparatus = nullptr;

    PlaybackManager m_runtimePlaybackManager;
};
