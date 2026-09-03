#pragma once
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <utility>
#include <future>
#include <atomic>
#include <mutex>
#include "CustomMusicConverter.h"

struct MusicTrack {
    int id;
    std::string name;
    std::string category;
};

enum class MusicRotationMode {
    Random,       // legacy (removed from the UI; folded into Shuffle) — kept so old
                  // RotationMode=0 config values map cleanly
    Sequential,
    Shuffle
};

enum class RematchTrackMode {
    CharacterSelect,
    ResumeLast,
    PlayNext
};

class MusicManager {
public:
    static MusicManager& GetInstance();

    void Initialize();
    void Update();

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    const MusicTrack* GetCurrentTrack() const { return m_currentTrack; }
    int GetCurrentTrackId() const { return m_currentTrackId; }
    int GetGameMusicId() const { return m_gameMusicId; }

    const std::vector<MusicTrack>& GetAllTracks() const { return m_tracks; }
    std::vector<MusicTrack> GetEnabledTracks() const;

    void ToggleTrackEnabled(int trackId);
    bool IsTrackEnabled(int trackId) const;

    // Enable/disable every track in a category (e.g. "btl", "vs", "old") at once.
    void SetCategoryEnabled(const std::string& category, bool enabled);
    // 1 = all enabled, 0 = all disabled, -1 = mixed (or empty category).
    int GetCategoryEnabledState(const std::string& category) const;

    // `force` reloads a track that is already playing. Normally that is skipped, but
    // after the file has been rewritten - a volume change reconverts it - the game is
    // still holding the old one and has to be made to read it again.
    void PlayTrack(int trackId, bool force = false);

    // Play an arbitrary .pac for auditioning, given a path relative to data/Sound/BGM/
    // (without the .pac extension) and the cue name inside it. Used by the replacement
    // browser to let you hear a track before and after swapping it. Only works inside a
    // match, where the audio engine is live; returns false otherwise.
    bool PreviewPac(const std::string& relPathNoExt, const std::string& cueName);
    void PlayNextTrack();
    void StartCustomMusicDiscovery();
    // Runs discovery again after a file has been added to the custom folder. Discovery is
    // otherwise once per session, so without this an imported track only appears after a
    // restart. Ignored while a scan is already running.
    void RescanCustomMusic();

    // Per-song volume for custom Jukebox tracks, in dB. Keyed by the source file name
    // rather than the track id, because ids are derived from a hash and would move if the
    // naming ever changed, while the file name is what the user actually recognises.
    //
    // The volume is baked into the converted .pac, so setting it queues a reconversion.
    float GetCustomTrackVolumeDb(int trackId) const;
    void SetCustomTrackVolumeDb(int trackId, float volumeDb);
    // Source file for a custom track id, or empty for a native one.
    std::string GetCustomTrackSourceName(int trackId) const;
    // The song's own ReplayGain, already applied on top of the user's offset. `found`
    // is false when the file carried no tag.
    bool GetCustomTrackTagGainDb(int trackId, float& outGainDb) const;
    // Publish completed async conversions to the live track list. Called from
    // both the game tick and Jukebox draw path so the custom category appears
    // immediately even when game logic is not advancing.
    void PollCustomMusicDiscovery();
    bool IsCustomMusicLoading() const { return m_customMusicLoading.load(); }
    bool HasStartedCustomMusicDiscovery() const { return m_customMusicStarted.load(); }
    float GetCustomMusicProgress() const;
    std::string GetCustomMusicStatus() const;
    int GetCustomTrackCount() const { return m_customTrackCount.load(); }

    // True when actually in a match (Training / Challenge / local VS / online),
    // i.e. MatchState_Fight — the only time the jukebox drives music.
    bool IsInMatch() const;

    // True when the Jukebox should show playback info (current track / timer):
    // in the match scene and NOT in the middle of leaving it via the confirm
    // dialog. False at Character Select / menus and during the exit transition,
    // so the UI shows "None" / 00:00 instead of a track that stopped playing.
    bool ShouldShowPlayback() const;

    // True when the mod has taken over BGM (played a custom track via XACT) and
    // thus left non-native state in Bank[13] that scene transitions must clean up.
    bool IsControllingBgm() const { return m_modControllingBgm || m_customBgmLoaded; }

    void SetRotationMode(MusicRotationMode mode) { m_rotationMode = mode; }
    MusicRotationMode GetRotationMode() const { return m_rotationMode; }

    void SetRepeatSingle(bool val) { m_repeatSingle = val; }
    bool IsRepeatSingle() const { return m_repeatSingle; }

    void SetRematchTrackMode(RematchTrackMode mode) { m_rematchTrackMode = mode; }
    RematchTrackMode GetRematchTrackMode() const { return m_rematchTrackMode; }

    void SavePreferences();
    void LoadPreferences();
    void ResetPreferences();

    // Dynamic track id ranges. Custom Jukebox songs and BGM replacements both appear in
    // the track list, and both need ids that can never collide with a native one or be
    // written into the game's own BGM-id fields. They are separate ranges because the two
    // features own different per-track state (a custom song's volume lives here, a
    // replacement's gain lives in BgmReplacementManager) and each must be able to rebuild
    // its own rows without disturbing the other's.
    static const int kCustomTrackIdBase = 10000;
    static const int kReplacementTrackIdBase = 20000;

    static bool IsCustomTrackId(int trackId) {
        return trackId >= kCustomTrackIdBase && trackId < kReplacementTrackIdBase;
    }
    static bool IsReplacementTrackId(int trackId) { return trackId >= kReplacementTrackIdBase; }
    // The BgmReplacementManager table index a published replacement row came from, or -1 for
    // any other id. The mapping lives with the id scheme so no caller has to redo the
    // arithmetic and get it wrong.
    static int GetReplacementTableIndex(int trackId) {
        return IsReplacementTrackId(trackId) ? trackId - kReplacementTrackIdBase : -1;
    }

    static const char* GetBgmFilename(int trackId);
    // The XACT cue to play a track by, which is NOT always its filename:
    //   - a replacement .pac keeps the ORIGINAL track's base filename as its cue, which is
    //     the whole reason the pointer swap works, so its path and cue differ
    //   - a custom Jukebox .pac is built from the 000_btl_rg template and keeps that cue
    // nullptr for an id with no file.
    static const char* GetBgmCueName(int trackId);
    static int GetTrackDuration(int trackId);

    // Publishes the BGM replacements the user has assigned as tracks of their own, so they
    // can be played from the Jukebox directly instead of only being heard when the game
    // loads the track they stand in for. Cheap to call every frame: it only rebuilds when
    // the set of active replacements has actually changed. Called from the game tick and
    // from the Jukebox draw path, for the same reason PollCustomMusicDiscovery is.
    void SyncReplacementTracks();

    // Custom track filename lookup (dynamic, parallels the static UNKNOWN_TRACK_FILES).
    // Populated once at startup by DiscoverCustomTracks(). Each entry:
    // {trackId, bgmFilename} e.g. {10000, "c10000_my_song"} (no .pac extension).
    // Custom track IDs are >= 10000 so they can never collide with a native ID
    // and can never be written into the game's own BGM-id fields.
    static std::vector<std::pair<int, std::string>> s_customTrackFiles;

    // Replacement tracks need a cue that differs from their path, so they carry more than
    // the id->filename pair a custom track does.
    struct ReplacementTrackFile {
        int id = -1;
        std::string pacPath;   // "BBCFIM_Music/008_btl_bn", relative to data/Sound/BGM
        std::string cueName;   // "008_btl_bn" - the original base filename
    };
    static std::vector<ReplacementTrackFile> s_replacementTrackFiles;

    int GetSongPlaybackFrames() const { return m_songPlaybackFrames; }
    std::string GetSongTimeString() const;

    // Effective advance threshold: the current track's true duration (read from
    // its XACT wave bank) when known, otherwise the precomputed per-track table.
    int GetRotationThresholdFrames() const;
    int GetCurrentTrackDurationFrames() const { return m_currentTrackDurationFrames; }

    // Soft-reset the custom BGM (stop+clear the bank, null the slot, reset the
    // track-id / music-select cursors). Safe no-op if no custom BGM is loaded.
    // Called on scene exit (e.g. Training -> Character Select) so the game loads
    // its normal scene BGM instead of erroring on a non-selectable leftover track.
    void UnloadCustomBgm();

    // Force-clear the mod's BGM footprint (Bank[13] -> EMPTY, null scratch slot,
    // present the selectable anchor) so the game's own Character Select XACT-init
    // rebuilds Bank[13] natively. Unlike RestoreAnchorForSceneExit (which reloads
    // via our direct-COM pipeline and re-creates foreign bank state), this leaves
    // the bank empty. Unconditional (runs even after the dialog-open restore).
    void ClearBgmForSceneExit();

    // Restore the initially-selected "anchor" track (the one chosen at Character
    // Select) through the normal pipeline, so leaving Training for Character
    // Select presents a valid selectable track everywhere (BGM slot, Bank[13],
    // audioMgr, musicSelect) instead of the non-selectable track we were playing
    // (which otherwise errors Character Select -> red debug screen). This makes
    // Character Select show the original song as if the playlist never cycled.
    void RestoreAnchorForSceneExit();

    // Backup BGM cleanup for the match-end -> victory-screen transition, for
    // flows that don't hit the primary cleanup (UpdateMusicState clearing on
    // MatchState -> VictoryScreen). Same proven cleanup as the Character
    // Select exit (ClearBgmForSceneExit), with the scratch-slot buffer
    // orphaned first. No-op unless the mod took over BGM.
    void RestoreNativeBgmForMatchEnd();

    void OnMatchInit();
    void ResetRotationTimer() { m_framesSinceLastChange = 0; m_songPlaybackFrames = 0; }

    static int* s_musicSelectX;
    static int* s_musicSelectY;

    // Asynchronous cue playback fields
    bool m_pendingPlay = false;
    void* m_pendingSoundObj = nullptr;
    std::string m_pendingCueName;
    int m_pendingPlayRetries = 0;

private:
    MusicManager();
    ~MusicManager() = default;

    void BuildTrackList();
    void DiscoverCustomTracks();
    void RegisterCustomTracks(const std::vector<CustomTrackInfo>& customTracks);
    // Re-appends the whole dynamic region (custom songs, then replacements) from the cached
    // rows below. Both features live past kCustomTrackIdBase and the Jukebox groups the list
    // by walking it in order, so a category has to stay contiguous - which means whichever
    // of the two changed, the region is rebuilt as a whole rather than patched in place.
    void RebuildDynamicTracks();
    void ChangeMusicIfNeeded();
    void UpdateMusicState();
    void ShufflePlaylist();
    int SelectNextTrack();
    int SelectNextTrackAfter(int trackId);
    void ApplyPendingRematchTrack();
    bool IsVersusMode() const;
    void DetectSceneExitAndUnload();
    bool PlayTrackPhysically(uintptr_t modBase, int trackId, const char* bgmName, int* outDurationFrames, int presentedId, const char* cueOverride = nullptr);

    std::vector<MusicTrack> m_tracks;
    // The dynamic rows, kept so RebuildDynamicTracks can re-append them without asking
    // either feature to rediscover anything.
    std::vector<MusicTrack> m_customTrackRows;
    std::vector<MusicTrack> m_replacementTrackRows;
    unsigned int m_replacementSignature = 0;
    bool m_replacementsSynced = false;
    std::map<int, bool> m_trackEnabled;
    const MusicTrack* m_currentTrack = nullptr;
    int m_currentTrackId = -1;
    int m_gameMusicId = -1;
    int m_framesSinceLastChange = 0;
    int m_songPlaybackFrames = 0;
    int m_sequentialIndex = 0;

    // Off by default: rotation advancing at a song's end is a deliberate choice, and on by
    // default it silently replaced whatever the user had put in a track's place.
    bool m_enabled = false;
    bool m_initialized = false;
    bool m_customTracksDiscovered = false; // one-shot guard for DiscoverCustomTracks
    std::atomic<bool> m_customMusicStarted{ false };
    std::atomic<bool> m_customMusicLoading{ false };
    std::atomic<int> m_customMusicCurrent{ 0 };
    std::atomic<int> m_customMusicTotal{ 0 };
    std::atomic<int> m_customTrackCount{ 0 };
    std::map<std::string, float> m_customTrackVolume;   // source file name -> dB
    std::map<int, std::string> m_customTrackSource;     // track id -> source file name
    std::map<int, float> m_customTrackTagGain;          // track id -> its own ReplayGain
    std::mutex m_customMusicPollMutex;
    mutable std::mutex m_customMusicStatusMutex;
    std::string m_customMusicStatus = "Custom music loads when the Jukebox is opened";
    std::future<std::vector<CustomTrackInfo>> m_customMusicFuture;

    MusicRotationMode m_rotationMode = MusicRotationMode::Sequential;
    bool m_repeatSingle = false;
    RematchTrackMode m_rematchTrackMode = RematchTrackMode::CharacterSelect;

    std::vector<int> m_shuffledPlaylist;
    int m_shuffleIndex = 0;

    // Last-resort advance threshold (frames) used only if a track's length is
    // somehow unknown; every known track has a real duration (wave bank / table).
    static const int MIN_FRAMES_BETWEEN_CHANGES = 7200;
    // True length of the currently-playing track in frames (60fps), read from its
    // XACT wave bank. 0 = unknown -> use the precomputed per-track table.
    int m_currentTrackDurationFrames = 0;
    int m_lastGameState = -1;      // last GameState (scene); for scene-exit detection
    int m_lastMatchState = -1;     // last MatchState; for match-end (-> VictoryScreen) detection
    int m_lastPlaylistTrackId = -1; // last track successfully played by the Jukebox in the current VS/Online set
    int m_pendingRematchTrackId = -1;
    bool m_rematchPending = false;
    bool m_customBgmLoaded = false; // true once we've taken over BGM (needs soft-reset on exit)
    bool m_modControllingBgm = false; // true once the mod is the authority on the current track
    int m_anchorTrackId = 0;        // supported track id presented to the game (never a vs/old/sys id)

    // Native audioManager BGM slot 0 (+0x118) state captured before we first
    // deactivate it, so scene exit can restore it. The game's native Character
    // Select BGM init needs this slot active; leaving it deactivated is what
    // produces the red debug Character Select.
    int m_origSlot0Active = 1;
    int m_origSlot0State = 0;
    bool m_audioSlot0Captured = false;
    std::chrono::steady_clock::time_point m_songStartTime;

    // "Return to Character Select?" confirm-dialog handling: restore the anchor
    // track while the dialog is up (so the exit sees a selectable track), suspend
    // rotation, and re-play the interrupted track if the user cancels.
    bool m_confirmDialogActive = false;
    int m_preDialogTrackId = -1;
    int m_dialogClosedTimer = 0;
    bool m_dialogSeenInRender = false; // set each render frame if the confirm dialog is visible
    bool CheckConfirmDialogUp();

    // Hysteresis for the dialog signal (render phase): overlays such as the
    // versus/online round-countdown can flash the scanned UI slots for a frame
    // or two; only a signal that persists for a short run is exposed as "seen".
    bool m_dialogSeenRunActive = false;
    std::chrono::steady_clock::time_point m_dialogSeenRunStart;
public:
    // Called from the render path (the dialog's message id is only present in the
    // render-phase UI buffer). Updates m_dialogSeenInRender for Update() to act on.
    void PollDialogRenderPhase();
};

MusicManager& GetMusicManager();
