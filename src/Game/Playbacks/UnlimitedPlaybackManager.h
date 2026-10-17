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

class CharData;

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
        // The moment the dummy recovers from being hit - hitstun running out - which is
        // what "on hit" meant before the rework. Trigger_OnHit, the moment the dummy IS
        // hit, is shown as "On Hitstun". Appended rather than slotted in, because the
        // trigger number is what saved actions are keyed by.
        Trigger_OnHitRecovery,
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
        // Which of the trigger's actions went off last, and when (wall clock, like
        // lastFiredMs), so the panel can light that action's row too.
        int lastFiredActionIndex = -1;
        unsigned long long lastFiredActionMs = 0;
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
        // An action's own delay, which starts once the trigger has fired and picked it.
        // Held as the action's index and the dummy it was picked for, and resolved again
        // when the wait is over: a move list can be re-parsed, a burst has to look at
        // whether the dummy is airborne THEN, and a CF slot is read when it plays.
        int pendingActionFrame = -1;
        int pendingActionIndex = -1;
        const void* pendingActionOwner = nullptr;
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
        std::array<bool, Trigger_Count> triggerEnabled = { true, true, true, true, true, true, true, true };
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
        // The action's own delay, any source, and which action of the trigger it was - so
        // a delayed one can be resolved again once the wait is over.
        int delayFrames = 0;
        int actionIndex = -1;
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
    // The same for one action of the trigger: 0 unless it is the one that went off last.
    unsigned long long GetActionLastFiredMs(TriggerType trigger, size_t actionIndex) const {
        const TriggerConfig& config = m_triggers[trigger];
        return config.lastFiredActionIndex == static_cast<int>(actionIndex) ? config.lastFiredActionMs : 0;
    }
    // Plays one specific action of a trigger right away, ignoring the trigger's condition,
    // cooldown and delay. Deferred to the next pre-tick like PlayEntryNow, for the same
    // one-frame-alignment reason.
    bool PlayActionNow(TriggerType trigger, size_t actionIndex);
    // The raw two-bytes-per-frame buffer a recording slot holds and its facing, for copying
    // it into an action. Reads the backup while the runtime has the slot borrowed, so it is
    // the user's recording and never the runtime's temporary playback.
    bool ReadSlotRecording(int slot, std::vector<char>* outRawFrames, bool* outFacingLeft) const;
    // The same for a playback file on disk: its raw frames and facing, capped at what a slot
    // can play. What the training file export embeds, so the file does not have to travel.
    bool ReadPlaybackFileRecording(const std::string& path, std::vector<char>* outRawFrames, bool* outFacingLeft);

    // What each trigger's condition did on the most recent game frame, for the frame history
    // bar's markers. Computed every frame of a training match whether or not the trigger is
    // armed, so the bar can show where a wakeup begins and ends before anything is set up.
    //
    // The three moments, in frame order:
    //   windowStart  the state or countdown the trigger watches began this frame. This is the
    //                earliest frame any negative delay could ever fire on.
    //   actionable   the delay-0 frame: the LAST frame the dummy cannot act. Inputs delivered
    //                on it come out on the first free frame, which is the convention the block
    //                gap has always used (blockstun == 1).
    //   detected     the condition, with the configured delay applied, was met this frame.
    //   fired        the action actually started this frame (after any positive delay).
    //
    // framesToActionable is the prediction the negative delay is measured against: 0 on the
    // actionable frame, N when it is N frames away, -1 when the dummy is not in a window or
    // the length of the state is not known yet. Where it comes from per trigger is in
    // docs/Research/TriggerTimingInvestigation.md.
    struct TriggerFrameEvents {
        bool windowStart = false;
        bool actionable = false;
        bool detected = false;
        bool fired = false;
        int framesToActionable = -1;
        // The dummy's state that opened the window, when the window is a state.
        const char* stateName = nullptr;
    };
    const std::array<TriggerFrameEvents, Trigger_Count>& GetTriggerFrameEvents() const {
        return m_triggerEvents;
    }
    // The game frame (pFrameCount) the events describe; -1 before any frame was seen.
    int GetTriggerFrameEventsFrame() const { return m_triggerEventsFrame; }

    // Whether the timing model wants the dummy's script parsed. Wakeup and throw-tech lengths
    // are read from the dummy's own script states, so ScrWindow's parse gate asks this too.
    // True while either of those triggers is armed, or while a consumer of the events has
    // asked for them (the frame history bar) regardless of what is armed.
    bool NeedsDummyScriptForTiming() const;
    void SetTimingEventsWanted(bool wanted) { m_timingEventsWanted = wanted; }
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
    // The same for one of a trigger's actions (a playback source; animations have no frames).
    bool LoadActionIntoSlot(TriggerType trigger, size_t actionIndex, int slot);
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

private:
    PlaybackLibrary& EditTarget();
    const PlaybackLibrary& EditTarget() const;
public:
    CompatibilityManager::Result ProbeProfileCompatibility(const std::string& profilePath) const;

    std::string GetActiveProfilePath() const;
    void SetActiveProfilePath(const std::string& path);

    // Forces one of the dummy's own script states on it right now, with no trigger involved
    // and nothing stored. Same two writes the animation trigger performs; it exists so the
    // animation picker can preview a move without the user having to commit to it first.
    bool PlayAnimationNow(scrState* state);

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

    // Turns a trigger's action into something the runtime can play. With actionIndex < 0
    // the trigger picks per its selection mode; otherwise exactly that action is used,
    // which is what the row's "play now" button asks for.
    bool ResolveTriggerAction(TriggerType trigger, ResolvedAction* out, int actionIndex = -1);
    // Runs a resolved action: forces the script state, or borrows a CF slot and starts the
    // playback. Stamps the trigger's cooldown and fired time either way.
    bool ExecuteResolvedAction(TriggerType trigger, ResolvedAction& resolved, int currentFrame);
    bool TriggerHasSomethingToPlay(TriggerType trigger);
    // Starts anything whose delay has run out, and drops a pending fire whose trigger was
    // disarmed or reconfigured while it was counting down.
    void ProcessPendingTriggerDelays(int currentFrame);
    void ReassertPendingBurst(int currentFrame);
    void ProcessPendingActionDelays(int currentFrame);
    static void MarkActionFired(TriggerConfig& config, int actionIndex);
    // The script jump an animation action performs, on whoever the dummy is now.
    bool ForceDummyAnimation(const scrState* state);
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
    bool ShouldTriggerOnHitRecovery();
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
    std::array<TriggerConfig, Trigger_Count> m_triggers;
    bool m_autoMirrorOnSideSwap = true;
    std::array<size_t, Trigger_Count> m_sequentialIndex = {};
    std::array<std::vector<size_t>, Trigger_Count> m_nonRepeatPools = {};
    std::string m_statusText;
    std::deque<ToastMessage> m_toasts;
    unsigned long long m_entrySerial = 0;
    std::string m_lastLoadedProfileFolder;

    // Per-frame trigger timing. See GetTriggerFrameEvents.
    std::array<TriggerFrameEvents, Trigger_Count> m_triggerEvents = {};
    int m_triggerEventsFrame = -1;
    bool m_timingEventsWanted = false;
    // Which of the dummy's states is being timed, so a window opens on the frame the state
    // is entered and closes when it is left. Entry is a change of currentAction only: a
    // forced animation restarts actionTime without changing it, and re-opening on that is
    // how the old wakeup trigger fired forever after its own move came out.
    std::string m_timedPrevAction;
    int m_timedPrevActionTime = 0;
    int m_timedLastActionTime = 0;
    int m_timedPrevBlockstun = 0;
    int m_timedPrevHitstun = 0;
    // The open timed-state window: which trigger, which state, and the game frame of its
    // first actionable frame (the first frame a move can START). Kept by frame number, not
    // by the dummy's state, so the actionable marker still lands when the dummy leaves the
    // state before it - which it does whenever a reversal comes out early.
    int m_timedWindowTrigger = -1;
    const char* m_timedWindowState = nullptr;
    int m_timedWindowStartFrame = -1;
    int m_timedActionableFrame = -1;
    // Same idea for the countdown triggers: the frame blockstun / hitstun reaches 0.
    int m_gapActionableFrame = -1;
    int m_recActionableFrame = -1;
    // Frames of [UP][TRACE] left to write after something fired, so the frames the inputs
    // land on are in the log too.
    int m_traceFramesLeft = 0;
    // The earliest frame a move was seen to start out of a timed state, keyed
    // "<charIndex>:<state>", in that state's own actionTime. A cross-check only: every exit
    // into a move is compared with the engine rule and a MISMATCH is logged when it came out
    // earlier than the rule allows. It never overrides the rule.
    std::unordered_map<std::string, int> m_learnedActionableAt;
    // actionTime of the first frame a move can start in the named state, from the engine
    // rule (docs/Research/TriggerTimingInvestigation.md "The first-actionable rule"). With
    // normals=false that is the earliest move of any kind (a getup lets specials out on frame
    // 14); normals=true is the first frame a normal, dash or jump can start (a getup: 20).
    // outSource names the rule.
    int PredictActionableAt(const CharData* dummy, const char* stateName, const char** outSource,
        bool normals = false) const;
    // TriggerTimingProbe (settings.ini, Debug): on each timed-state or block-gap window the
    // dummy is fed A/B on alternate frames through the runtime playback, and the first frame
    // a move starts is logged against the rule's prediction as "[UP][PROBE]".
    void StartTimingProbe(const char* stateName, int charIndex, int currentFrame,
        int predictedFirst, int predictedSpecials, int playFrames);
    void UpdateTimingProbe(int currentFrame, const std::string& action, int actionTime);
    bool m_probeActive = false;
    std::string m_probeState;
    int m_probeCharIndex = -1;
    int m_probeWindowStart = -1;
    int m_probePredicted = -1;
    int m_probePredictedSpecials = -1;
    int m_probeDeadline = -1;
    // How long each of the dummy's timed states actually ran, keyed "<charIndex>:<state>".
    // Filled on every clean exit into a neutral state. Only the neutral tech (a physics hop)
    // uses it as its length; for every other state the script plus the engine rule is the
    // length, this is used only while the script is unread, and a disagreement is logged as
    // a MISMATCH.
    std::unordered_map<std::string, int> m_learnedStateFrames;
    void UpdateTriggerTiming(int currentFrame);
    // Frames the dummy's named state runs for: its script (a ground roll's with the engine's
    // cut of the held sprite applied), the measured hop for the neutral tech, what was
    // observed when the script is unread, else -1. outSource names the source.
    int PredictStateFrames(const CharData* dummy, const char* stateName, const char** outSource = nullptr) const;

    // Runtime edge tracking for game-state triggers
    bool m_prevWakeupCondition = false;
    bool m_prevGapCondition = false;
    bool m_prevOnBlockCondition = false;
    bool m_prevOnHitCondition = false;
    bool m_prevOnHitRecoveryCondition = false;
    bool m_prevThrowTechCondition = false;
    bool m_keyPressTriggerArmed = false;
    bool m_triggerRuntimeEnabled = true;
    bool m_profileRuntimeSuppressedUntilReset = false;
    int m_lastObservedFrame = -1;
    bool m_runtimeSlotBackupValid = false;
    bool m_runtimeSlotRestorePending = false;
    bool m_pendingPlayNowRequested = false;
    size_t m_pendingPlayNowIndex = 0;
    // "Play now" on a dummy-action row: which trigger's action, run on the next pre-tick
    // for the same reason as the library's play-now.
    bool m_pendingPlayActionRequested = false;
    int m_pendingPlayActionTrigger = 0;
    size_t m_pendingPlayActionIndex = 0;
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
    // A burst handed to the game through the action override, re-requested every frame until
    // it comes out (see ReassertPendingBurst). -1 = none pending.
    int m_burstOverrideUntilFrame = -1;
    int m_burstOverrideStartFrame = -1;
    int m_burstOverrideReasserts = 0;

    PlaybackManager m_runtimePlaybackManager;
};
