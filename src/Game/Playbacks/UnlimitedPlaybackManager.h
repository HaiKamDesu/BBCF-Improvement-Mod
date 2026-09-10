#pragma once

#include "Game/Playbacks/PlaybackManager.h"
#include "Game/Playbacks/CompatibilityManager.h"
#include "Game/SnapshotApparatus/SnapshotApparatus.h"
#include "Game/Scr/ScrStateEntry.h"

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
        // Armed. The gate TryFireTrigger checks first; kept in step with the dummy-action
        // table by DummyActionManager::SyncTriggerEnable.
        bool enabled = false;
        // Blocks the same trigger from firing again for this many frames.
        int cooldownFrames = 1;
        // Wall clock of the last time this trigger fired, so the UI can flash the row. Not
        // a frame number: the row is drawn whether or not the game is advancing.
        unsigned long long lastFiredMs = 0;
        // Frames between the condition and the action. Positive waits, so a reversal can be
        // made late on purpose. NEGATIVE fires early, which only means anything for a
        // condition the game counts down to - the block gap, via blockstun - and is how a
        // reversal's motion gets delivered before the gap so the attack lands on the first
        // actionable frame. One field rather than two, because "wait 3" and "fire 3 early"
        // are the same axis.
        int delayFrames = 0;
        int lastTriggeredFrame = -999999;
        // Set when a delayed trigger is counting down; -1 when nothing is pending.
        int pendingFireFrame = -1;
        // Flip the inputs when the dummy is on the other side from the one the action was
        // authored or recorded on. Per trigger, because one trigger may hold a
        // side-agnostic motion and another a corner-specific setup.
        bool autoMirror = true;
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

    // A named collection of recorded playbacks, self-contained on disk. This is what a
    // "profile" already was: SaveProfile embeds every entry's playback bytes in the file, so
    // one of these is portable on its own.
    //
    // Gathered into a struct so the manager can hold more than one at a time, which is what
    // assigning a different library to each trigger needs. Stage 1 of that work keeps exactly
    // one instance, so this is a pure regrouping with no behaviour change.
    struct PlaybackLibrary {
        // File this was loaded from or last saved to. Empty for the unsaved working set.
        std::string path;
        std::vector<PlaybackEntry> entries;
        // Playback bytes, keyed by entry id. Not every entry is necessarily loaded.
        std::unordered_map<std::string, CachedPlayback> cache;
        // How to choose between candidates. Kept here as the default applied when this
        // library is first assigned to a trigger; the live mode belongs to the assignment.
        int selectionMode = Selection_Random;
    };

    // Manager-level settings a library file may also carry, from back when one file was the
    // whole working set. Handed back by ParseLibraryFile so only LoadProfile applies them.
    // What a trigger's action resolved to: a frame buffer ready for the runtime, plus
    // enough to name it in a toast and a log line.
    struct ResolvedAction {
        // Playback sources land here: a frame buffer for the runtime.
        std::vector<char> frames;
        bool facingLeft = false;
        // An animation instead forces one of the dummy's own script states, so it carries a
        // state rather than inputs and skips the CF slot entirely.
        scrState* animation = nullptr;
        int animationDelayFrames = 0;
        // A burst is not a script jump: the game consumes an action-override name instead,
        // which is how the old burst-on-hit toggle did it.
        bool viaActionOverride = false;
        std::string name;
        const char* sourceLabel = "";
    };

    struct LibraryFileExtras {
        std::array<TriggerConfig, Trigger_Count> triggers;
        bool autoMirror = true;
        bool valid = false;
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

    // Work that Tick() is not allowed to do itself, run once a frame from the overlay after
    // its windows are drawn. Tick() comes from a naked asm hook in the middle of the game's
    // frame update; building a SnapshotApparatus there calls the game's own network init and
    // crashes it, so the loop's snapshot is prepared and the loop actually started here -
    // the same frame phase the training save states were moved to for the same reason.
    void RunDeferredSetup();

    // For the dummy-action rows: how long ago this trigger fired, and whether the loop is
    // currently running, so a row can show that it just went off.
    unsigned long long GetTriggerLastFiredMs(TriggerType trigger) const {
        return m_triggers[trigger].lastFiredMs;
    }
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
    bool IsLoopPositionSetupActive() const;
    bool HasLoopCustomSnapshot() const;
    // True when the stored snapshot was captured for this reset mode (auto-captured for
    // Left/Middle/Right, user-captured for Custom), so starting the loop can skip setup.
    bool IsLoopSnapshotReadyForMode(int mode) const;
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
    // Stops the recording and hands the captured playback back instead of filing it in a
    // library, so it can be written wherever the caller's file picker ends up pointing.
    //
    // Deliberately not "stop and export to this path": the recording has to end the moment
    // the user says so, because the replay keeps playing while a file dialog is up and the
    // end frame would drift by however long they spent choosing a name.
    bool StopReplayRecordingToBuffer(std::vector<char>* outTrimmed, char* outFacing);
    void CancelReplayRecording(const char* reason = nullptr);
    bool IsReplayRecording() const;
    bool IsReplayRecordingAsP1() const;
    int GetReplayRecordingStartFrame() const;
    bool RemoveEntryByIndex(size_t idx);
    int RemoveEntriesByIndices(const std::vector<size_t>& indices);
    bool MoveEntry(size_t fromIdx, size_t toIdx);
    bool MoveEntries(const std::vector<size_t>& fromIndicesSorted, size_t insertionIndex, size_t* outInsertedAt = nullptr);
    void SetAllEntriesEnabled(bool enabled);
    void SetEntriesEnabled(const std::vector<size_t>& indices, bool enabled);
    bool RenameEntry(size_t idx, const std::string& newName);

    bool LoadEntryIntoSlot(size_t idx, int slot);
    bool SaveEntryFromSlot(size_t idx, int slot);
    bool ReadEntryPlayback(size_t idx, bool* outFacingLeft, std::vector<char>* outFrames);
    bool WriteEntryPlayback(size_t idx, bool facingLeft, const std::vector<char>& frames);
    bool SaveEntryToFile(size_t idx, const std::string& outputPath);
    bool PlayEntryNow(size_t idx);

    // While an entry is played back, the runtime borrows one CF slot and owes the user a
    // restore of that slot's original contents. Anything else that writes to a CF slot has
    // to know about it, otherwise the pending restore silently reverts the write.
    int GetBorrowedCfSlot() const; // 0 when no slot is currently borrowed
    bool AbsorbExternalSlotWrite(int slot, const std::vector<char>& trimmedFrames, bool facingLeft);
    bool ReadBorrowedCfSlot(int slot, std::vector<char>* outTrimmedFrames, char* outFacing) const;

    void ClearAll();

    bool SaveProfile(const std::string& profilePath);
    bool LoadProfile(const std::string& profilePath, bool forceLoadIncompatible = false);

    // Reads a library file without touching manager state. Public so the trigger table can
    // load whichever library a trigger names.
    bool ParseLibraryFile(const std::string& resolvedPath, bool forceLoadIncompatible,
        PlaybackLibrary* out, LibraryFileExtras* extras = nullptr);

    // The library a trigger draws from, loaded on demand and kept for as long as some
    // trigger still names it. Null when the trigger is not library-backed, or the file has
    // gone away since it was assigned.
    const PlaybackLibrary* LibraryForTrigger(TriggerType trigger);

    // Drops loaded libraries nothing names any more.
    void PruneUnusedLibraries();

    // Where library files live, for the picker that assigns one to a trigger.
    std::string GetLibraryFolderPublic() const { return GetProfileFolder(); }

    // Which library every "working set" operation acts on - the entry list, add, remove,
    // reorder, rename, save and load.
    //
    // The library UI makes two dozen calls into this manager and every one of them used to
    // mean "the one library there is". Rather than threading a library through all of them,
    // the manager is pointed at a target and the UI is reused verbatim: the Playback Library
    // window edits the working set, and configuring a trigger points it at that trigger's
    // library for as long as its editor is on screen.
    //
    // Never null. Passing null restores the working set.
    void SetEditTarget(PlaybackLibrary* library);
    PlaybackLibrary* GetEditTarget();
    // The library a trigger names, loaded on demand, for pointing the editor at.
    PlaybackLibrary* MutableLibraryForTrigger(TriggerType trigger);
    // Whether this trigger's own library holds anything, so a library built in the modal
    // without being saved to a file still counts as configured.
    bool TriggerLibraryHasEntries(TriggerType trigger) const;
    // Throws away everything this trigger's library held. Called when the row is cleared, so
    // building the trigger again starts from an empty library rather than quietly inheriting
    // the entries the deleted one had loaded.
    void ResetTriggerLibrary(TriggerType trigger);

private:
    PlaybackLibrary& EditTarget();
    const PlaybackLibrary& EditTarget() const;
public:
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

    bool AbsorbExternalSlotWriteRaw(int slot, const std::vector<char>& rawFrames, bool facingLeft);
    bool TryReadBorrowedSlot(int slot, std::vector<char>* outRawFrames, bool* outFacingLeft) const;

    bool ReadPlaybackFile(const std::string& fullPath, CachedPlayback* out, bool forceLoadIncompatible = false);
    bool WritePlaybackFile(const std::string& fullPath, bool facingLeft, const std::vector<char>& frames);
    bool IsReplayMatchActive() const;
    bool BuildPlaybackFramesFromReplayRange(int round, int startFrame, int endFrameExclusive, int recordedPlayer, std::vector<char>* outFrames) const;

    bool PickEntryIndexForTrigger(TriggerType trigger, const PlaybackLibrary& library,
        int selectionMode, size_t* outIndex);
    bool ResolveTriggerAction(TriggerType trigger, ResolvedAction* out);
    bool TriggerHasSomethingToPlay(TriggerType trigger);
    // Starts anything whose delay has run out, and drops a pending fire whose trigger was
    // disarmed or reconfigured while it was counting down.
    void ProcessPendingTriggerDelays(int currentFrame);
    bool StartResolvedAction(TriggerType trigger, int currentFrame);
    bool TryFireTrigger(TriggerType trigger, int currentFrame);
    void ProcessLoopTick(int currentFrame);
    void StartLoop(int currentFrame);
    // The loop's start/stop key, polled whenever a loop is configured rather than only while
    // one is running.
    void ProcessLoopHotkey(int currentFrame);
    // Whether a SnapshotApparatus may be built at this point in the frame. False while
    // Tick() is on the stack: that runs from a naked asm hook in the middle of the game's
    // own frame update, and building one there calls the game's network init and crashes
    // it. Leaves a note for RunDeferredSetup to do it from the overlay instead.
    bool CanBuildSnapshotApparatusHere() const;
    void StopLoop(const char* reason = nullptr);
    bool TryStartLoopPlayback();
    void ResetLoopPlaybackCompletionState();
    void ObserveLoopPlaybackActionState();
    bool IsLoopPlaybackAnimationComplete(int currentFrame);
    void ResetTriggerRuntimeState(bool enableRuntime);
    void LogRuntimeGateState(const char* tag) const;
    void LogEntryCacheSummary(const char* tag) const;

    bool ShouldTriggerWakeup();
    bool ShouldTriggerGap();
    bool ShouldTriggerOnBlock();
    bool ShouldTriggerOnHit();
    bool ShouldTriggerThrowTech();
    std::vector<size_t> BuildCandidates(const PlaybackLibrary& library);
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
    bool CaptureLoopSnapshotInternal();
    bool RestoreLoopCustomSnapshot(bool showToast);
    void BeginLoopPositionSetup(int currentFrame);
    void ProcessLoopPositionSetup(int currentFrame);
    bool ApplyLoopRestart();
    void StartNativeTrainingResetCombo(LoopResetMode mode);
    void CaptureNativeTrainingResetInputSnapshot(LoopResetMode mode);
    void NeutralizeNativeTrainingResetDirections(LoopResetMode mode);
    void ReleaseNativeTrainingResetCombo();
    void SendNativeTrainingResetKey(WORD virtualKey, bool keyDown) const;
    int LoopSecondsToFrames(float seconds) const;

    int m_mode = Mode_Default;
    bool m_initialized = false;
    // The working set: what the library window edits by default.
    PlaybackLibrary m_library;
    // Where working-set operations are currently pointed. Never null.
    PlaybackLibrary* m_editTarget = nullptr;

    // Libraries loaded because a trigger names them, keyed by resolved path. Up to seven
    // triggers can each name a different one, so there is no single "current" library any
    // more as far as firing is concerned.
    // One library per trigger, NOT one per file path.
    //
    // Keyed by path, two triggers naming different files still shared the single working
    // set whenever neither had a path yet, so configuring On Wakeup edited On Block Gap's
    // list as well. Keyed by trigger, each row owns its entries, enable checkboxes,
    // ordering and picking order outright, and loading the same file into two triggers
    // gives each its own copy to enable and reorder independently.
    //
    // This is also the instance the row's modal edits, so what you see configured is the
    // exact object the trigger fires from - an edit-one-copy-fire-another split would be
    // invisible and maddening.
    std::array<PlaybackLibrary, Trigger_Count> m_triggerLibraries;
    std::array<TriggerConfig, Trigger_Count> m_triggers;
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
        // One-time takeover when reset mode is Left/Middle/Right and no matching snapshot
        // exists yet: force the native training reset to the target position, wait for it to
        // settle, auto-capture a snapshot, then the loop reuses that snapshot every cycle.
        LoopPhase_PositionSetup,
    };
    LoopPhase m_loopPhase = LoopPhase_Idle;
    int m_loopPhaseStartFrame = -1;
    bool m_loopRestartAppliedForCycle = false;
    bool m_loopPlaybackObservedNonIdle = false;
    int m_loopPlaybackInputEndedFrame = -1;
    int m_loopPlaybackIdleSinceFrame = -1;
    bool m_loopNativeResetPulseActive = false;
    // Logic ticks left before the forced reset combo is released. Kept as short as possible
    // (and cut to 0 as soon as the reset is observed) so the held direction cannot walk the
    // character away from the freshly reset position before the snapshot is taken.
    int m_loopNativeResetHoldTicksLeft = 0;
    std::array<WORD, 3> m_loopNativeResetKeys = {};
    std::vector<WORD> m_loopNativeResetRestoreKeys;
    std::vector<char> m_loopCustomSnapshotBytes;
    int m_loopCustomSnapshotSize = 0;
    int m_loopCustomSnapshotSlotIndex = -1;
    // Which LoopResetMode the stored snapshot was captured for (-1 = none).
    int m_loopSnapshotSourceMode = -1;
    // Countdown (in observed logic ticks) between releasing the forced reset combo and
    // auto-capturing the snapshot; frame-counter rollback safe. -1 = not waiting.
    int m_loopPositionSetupSettleTicksLeft = -1;
    SnapshotApparatus* m_loopSnapshotApparatus = nullptr;
    // Set while Tick() is on the stack, i.e. while we are inside the game's frame update.
    bool m_inHookTick = false;
    // Mutable because the "can I build here?" test is a const query that has to be able to
    // leave a note asking the safe phase to do it instead.
    mutable bool m_loopSnapshotPrepareRequested = false;
    bool m_loopStartRequested = false;
    // One burst per combo: set when a burst is armed, cleared when hitstun ends.
    bool m_onHitBurstLatched = false;

    PlaybackManager m_runtimePlaybackManager;
};
