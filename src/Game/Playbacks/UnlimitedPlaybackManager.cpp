#include "UnlimitedPlaybackManager.h"

#include "Game/Playbacks/DummyActionManager.h"

#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Core/HotkeyManager.h"
#include "Game/gamestates.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>

namespace {
const char* kDefaultProfileName = "default.upl";
const char* kProfileFormatVersionKey = "format_version";
const char* kProfileFormatKindKey = "format_kind";
const char* kProfileFormatKindValue = "unlimited_profile";
const char* kEmbeddedEntryDataKey = "entry_data";
const char kPlaybackHeaderMagic[4] = { 'U', 'P', 'B', '2' };
constexpr int kDedicatedRuntimePlaybackSlot = 4;

std::string FormatLocalized(const char* key, ...) {
    const char* format = L(key).c_str();
    va_list args;
    va_start(args, key);
    const int required = std::vsnprintf(nullptr, 0, format, args);
    va_end(args);

    if (required <= 0) {
        return std::string(format);
    }

    std::string out;
    out.resize(static_cast<size_t>(required) + 1);
    va_start(args, key);
    std::vsnprintf(&out[0], out.size(), format, args);
    va_end(args);
    out.resize(static_cast<size_t>(required));
    return out;
}

bool IsLoopCompletionIdleAction(const std::string& currentAction) {
    static const std::array<const char*, 17> idleActions = {
        "_NEUTRAL",
        "CmnActStand",
        "CmnActStandTurn",
        "CmnActStand2Crouch",
        "CmnActCrouch",
        "CmnActCrouchTurn",
        "CmnActCrouch2Stand",
        "CmnActFWalk",
        "CmnActBWalk",
        "CmnActFDashStop",
        "CmnActBDashStop",
        "CmnActJumpPre",
        "CmnActJumpUpper",
        "CmnActJumpDown",
        "CmnActJumpUpperEnd",
        "CmnActJumpLanding",
        "CmnActLandingStiffEnd",
    };

    return std::find(idleActions.begin(), idleActions.end(), currentAction) != idleActions.end();
}

constexpr int kLoopCompletionMinimumPostInputFrames = 45;
constexpr int kLoopCompletionIdleStableFrames = 12;
constexpr int kLoopCompletionNoActionFallbackFrames = 90;
constexpr int kLoopNativeResetSettleFrames = 45;
// How many observed logic ticks the forced direction+reset combo is held before release.
// Kept minimal (just enough for the game's input poll to register the press) so the held
// direction cannot walk the character off the reset position; the reset itself is also
// detected via the frame-counter rollback and releases the combo immediately.
constexpr int kLoopNativeResetHoldTicks = 2;
constexpr char kLoopPositionSetupToastKey[] = "loop_position_setup";

bool PathExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
}

bool IsAbsolutePath(const std::string& path) {
    if (path.size() >= 2 && path[1] == ':') {
        return true;
    }
    if (!path.empty() && (path[0] == '\\' || path[0] == '/')) {
        return true;
    }
    return false;
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char tail = a.back();
    if (tail == '/' || tail == '\\') {
        return a + b;
    }
    return a + "/" + b;
}

void EnsureDirectoryRecursive(const std::string& dir) {
    if (dir.empty() || PathExists(dir)) {
        return;
    }

    std::string current;
    current.reserve(dir.size());
    for (size_t i = 0; i < dir.size(); ++i) {
        const char ch = dir[i];
        current.push_back(ch);
        if (ch == '/' || ch == '\\') {
            if (!current.empty() && !PathExists(current)) {
                CreateDirectoryA(current.c_str(), nullptr);
            }
        }
    }
    if (!PathExists(current)) {
        CreateDirectoryA(current.c_str(), nullptr);
    }
}

std::string Trim(const std::string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        out.push_back(item);
    }
    return out;
}

char HexDigit(unsigned char value) {
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('A' + (value - 10));
}

bool TryParseHexNibble(char c, unsigned char* outValue) {
    if (!outValue) {
        return false;
    }
    if (c >= '0' && c <= '9') {
        *outValue = static_cast<unsigned char>(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *outValue = static_cast<unsigned char>(10 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *outValue = static_cast<unsigned char>(10 + (c - 'A'));
        return true;
    }
    return false;
}

std::string EncodeHex(const std::vector<char>& data) {
    std::string out;
    out.resize(data.size() * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        const unsigned char value = static_cast<unsigned char>(data[i]);
        out[(i * 2) + 0] = HexDigit(static_cast<unsigned char>((value >> 4) & 0xF));
        out[(i * 2) + 1] = HexDigit(static_cast<unsigned char>(value & 0xF));
    }
    return out;
}

uint32_t ComputePlaybackDigest(const std::vector<char>& data) {
    uint32_t hash = 2166136261u;
    for (unsigned char byte : data) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

std::string PreviewPlaybackBytes(const std::vector<char>& data, size_t maxBytes) {
    const size_t count = (std::min)(data.size(), maxBytes);
    std::vector<char> preview(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(count));
    return EncodeHex(preview);
}

std::vector<char> CompactPlaybackBytes(const std::vector<char>& rawBytes) {
    std::vector<char> compact;
    compact.reserve(rawBytes.size() / 2);
    for (size_t i = 0; (i + 1) < rawBytes.size(); i += 2) {
        compact.push_back(rawBytes[i]);
    }
    return compact;
}

std::vector<char> ExpandPlaybackBytes(const std::vector<char>& compactBytes) {
    std::vector<char> raw;
    raw.reserve(compactBytes.size() * 2);
    for (char input : compactBytes) {
        raw.push_back(input);
        raw.push_back(0);
    }
    return raw;
}

bool DecodeHex(const std::string& text, std::vector<char>* outData) {
    if (!outData || (text.size() % 2) != 0) {
        return false;
    }
    outData->clear();
    outData->reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        unsigned char hi = 0;
        unsigned char lo = 0;
        if (!TryParseHexNibble(text[i], &hi) || !TryParseHexNibble(text[i + 1], &lo)) {
            outData->clear();
            return false;
        }
        outData->push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

bool ParsePlaybackBytes(
    const std::vector<char>& data,
    UnlimitedPlaybackManager::CachedPlayback* out,
    bool forceLoadIncompatible,
    std::string* outFailureReason) {
    if (!out) {
        if (outFailureReason) {
            *outFailureReason = L("Invalid playback destination.");
        }
        return false;
    }
    if (data.size() <= 1) {
        if (outFailureReason) {
            *outFailureReason = L("Playback payload was too small.");
        }
        return false;
    }

    const bool hasHeader =
        data.size() >= 8 &&
        data[0] == kPlaybackHeaderMagic[0] &&
        data[1] == kPlaybackHeaderMagic[1] &&
        data[2] == kPlaybackHeaderMagic[2] &&
        data[3] == kPlaybackHeaderMagic[3];

    if (hasHeader) {
        CompatibilityManager::FileVersion detected = {
            static_cast<unsigned char>(data[4]),
            static_cast<unsigned char>(data[5]),
        };
        const auto compatibility = CompatibilityManager::EvaluatePlayback(detected, true);
        if (compatibility.action == CompatibilityManager::Action_Reject) {
            if (outFailureReason) {
                *outFailureReason = FormatLocalized(
                    "Playback rejected: file v%s, code v%s.",
                    CompatibilityManager::ToString(compatibility.detected).c_str(),
                    CompatibilityManager::ToString(compatibility.current).c_str());
            }
            return false;
        }
        if (compatibility.action == CompatibilityManager::Action_Confirm && !forceLoadIncompatible) {
            if (outFailureReason) {
                *outFailureReason = FormatLocalized(
                    "Playback rejected (newer format): file v%s, code v%s.",
                    CompatibilityManager::ToString(compatibility.detected).c_str(),
                    CompatibilityManager::ToString(compatibility.current).c_str());
            }
            return false;
        }
        const unsigned char flags = static_cast<unsigned char>(data[6]);
        out->facingLeft = (flags & 0x1) != 0;
        out->frames.assign(data.begin() + 8, data.end());
    } else {
        if (outFailureReason) {
            *outFailureReason = L("Playback file header is missing.");
        }
        return false;
    }

    if (out->frames.size() > static_cast<size_t>(UnlimitedPlaybackManager::kMaxFramesPerPlayback) * 2) {
        out->frames.resize(static_cast<size_t>(UnlimitedPlaybackManager::kMaxFramesPerPlayback) * 2);
    }
    out->loaded = true;
    return true;
}

std::vector<char> SerializePlaybackBytes(bool facingLeft, const std::vector<char>& frames) {
    std::vector<char> data;
    const size_t maxWrite = (std::min)(frames.size(), static_cast<size_t>(UnlimitedPlaybackManager::kMaxFramesPerPlayback) * 2);
    data.reserve(8 + maxWrite);
    data.push_back(kPlaybackHeaderMagic[0]);
    data.push_back(kPlaybackHeaderMagic[1]);
    data.push_back(kPlaybackHeaderMagic[2]);
    data.push_back(kPlaybackHeaderMagic[3]);
    const CompatibilityManager::FileVersion version = CompatibilityManager::CurrentPlaybackVersion();
    data.push_back(static_cast<char>(version.major));
    data.push_back(static_cast<char>(version.minor));
    data.push_back(static_cast<char>(facingLeft ? 0x1 : 0x0));
    data.push_back(static_cast<char>(0));
    data.insert(data.end(), frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(maxWrite));
    return data;
}

bool ReadProfileFormatVersion(
    const std::string& path,
    CompatibilityManager::FileVersion* outVersion,
    bool* outHasExplicitVersion) {
    if (!outVersion || !outHasExplicitVersion) {
        return false;
    }
    *outVersion = { 1, 0 };
    *outHasExplicitVersion = false;

    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        const std::string key = Trim(line.substr(0, pos));
        const std::string value = Trim(line.substr(pos + 1));
        if (key == kProfileFormatVersionKey) {
            CompatibilityManager::FileVersion parsed;
            if (CompatibilityManager::ParseVersion(value, &parsed)) {
                *outVersion = parsed;
                *outHasExplicitVersion = true;
                return true;
            }
        }
    }
    return false;
}

bool ReadPlaybackFormatVersion(
    const std::string& path,
    CompatibilityManager::FileVersion* outVersion,
    bool* outHasHeader) {
    if (!outVersion || !outHasHeader) {
        return false;
    }
    *outVersion = { 1, 0 };
    *outHasHeader = false;

    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        return false;
    }

    char header[8] = {};
    file.read(header, 8);
    const std::streamsize readSize = file.gcount();
    if (readSize < 1) {
        return false;
    }

    if (readSize >= 8 &&
        header[0] == kPlaybackHeaderMagic[0] &&
        header[1] == kPlaybackHeaderMagic[1] &&
        header[2] == kPlaybackHeaderMagic[2] &&
        header[3] == kPlaybackHeaderMagic[3]) {
        *outHasHeader = true;
        outVersion->major = static_cast<unsigned char>(header[4]);
        outVersion->minor = static_cast<unsigned char>(header[5]);
    }

    return true;
}

std::string TriggerKeyName(UnlimitedPlaybackManager::TriggerType t) {
    switch (t) {
    case UnlimitedPlaybackManager::Trigger_Wakeup: return "wakeup";
    case UnlimitedPlaybackManager::Trigger_Gap: return "gap";
    case UnlimitedPlaybackManager::Trigger_OnBlock: return "onblock";
    case UnlimitedPlaybackManager::Trigger_OnHit: return "onhit";
    case UnlimitedPlaybackManager::Trigger_ThrowTech: return "throwtech";
    case UnlimitedPlaybackManager::Trigger_KeyPress: return "keypress";
    case UnlimitedPlaybackManager::Trigger_OnLoop: return "onloop";
    default: return "unknown";
    }
}

const char* TriggerDisplayName(UnlimitedPlaybackManager::TriggerType t) {
    switch (t) {
    case UnlimitedPlaybackManager::Trigger_Wakeup: return L("Wakeup").c_str();
    case UnlimitedPlaybackManager::Trigger_Gap: return L("Gap").c_str();
    case UnlimitedPlaybackManager::Trigger_OnBlock: return L("On Block").c_str();
    case UnlimitedPlaybackManager::Trigger_OnHit: return L("On Hit").c_str();
    case UnlimitedPlaybackManager::Trigger_ThrowTech: return L("Throw Tech").c_str();
    case UnlimitedPlaybackManager::Trigger_KeyPress: return L("Key Press").c_str();
    case UnlimitedPlaybackManager::Trigger_OnLoop: return L("On loop").c_str();
    default: return L("Unknown").c_str();
    }
}

UnlimitedPlaybackManager::TriggerType ParseTriggerKey(const std::string& s, bool* ok) {
    *ok = true;
    if (s == "wakeup") return UnlimitedPlaybackManager::Trigger_Wakeup;
    if (s == "gap") return UnlimitedPlaybackManager::Trigger_Gap;
    if (s == "onblock") return UnlimitedPlaybackManager::Trigger_OnBlock;
    if (s == "onhit") return UnlimitedPlaybackManager::Trigger_OnHit;
    if (s == "throwtech") return UnlimitedPlaybackManager::Trigger_ThrowTech;
    if (s == "keypress") return UnlimitedPlaybackManager::Trigger_KeyPress;
    if (s == "onloop") return UnlimitedPlaybackManager::Trigger_OnLoop;
    *ok = false;
    return UnlimitedPlaybackManager::Trigger_Wakeup;
}
}

UnlimitedPlaybackManager::PlaybackLibrary& UnlimitedPlaybackManager::EditTarget() {
    return m_editTarget ? *m_editTarget : m_library;
}

const UnlimitedPlaybackManager::PlaybackLibrary& UnlimitedPlaybackManager::EditTarget() const {
    return m_editTarget ? *m_editTarget : m_library;
}

void UnlimitedPlaybackManager::SetEditTarget(PlaybackLibrary* library) {
    m_editTarget = library;
}

UnlimitedPlaybackManager::PlaybackLibrary* UnlimitedPlaybackManager::GetEditTarget() {
    return &EditTarget();
}

UnlimitedPlaybackManager::PlaybackLibrary* UnlimitedPlaybackManager::MutableLibraryForTrigger(
    TriggerType trigger) {
    // Loads the assignment if needed, then hands back this trigger's own slot so the editor
    // works on exactly the object the trigger fires from.
    LibraryForTrigger(trigger);
    return &m_triggerLibraries[trigger];
}

void UnlimitedPlaybackManager::ResetTriggerLibrary(TriggerType trigger) {
    m_triggerLibraries[trigger] = PlaybackLibrary{};
    // The picking bookkeeping is per trigger too, and a sequential index or a
    // no-repeat pool left pointing into the old entry list is meaningless now.
    m_sequentialIndex[trigger] = 0;
    m_nonRepeatPools[trigger].clear();
    // Nothing has been edited in this slot yet, so the editor must not still be aimed at it.
    if (GetEditTarget() == &m_triggerLibraries[trigger]) {
        SetEditTarget(nullptr);
    }
}

bool UnlimitedPlaybackManager::TriggerLibraryHasEntries(TriggerType trigger) const {
    return !m_triggerLibraries[trigger].entries.empty();
}

UnlimitedPlaybackManager& UnlimitedPlaybackManager::Instance() {
    static UnlimitedPlaybackManager instance;
    return instance;
}

UnlimitedPlaybackManager::UnlimitedPlaybackManager() {
    m_mode = Mode_Unlimited;
    for (int i = 0; i < Trigger_Count; ++i) {
        m_triggers[i].enabled = (i != Trigger_KeyPress && i != Trigger_OnLoop);
        m_triggers[i].cooldownFrames = 1;
    }
}

void UnlimitedPlaybackManager::InitializeIfNeeded() {
    if (m_initialized) {
        return;
    }

    EditTarget().path.clear();
    m_lastLoadedProfileFolder.clear();
    // The playback and loop keybinds live in HotkeyManager (settings.ini, rebindable from the
    // Settings window like every other hotkey), not in profiles and not here.
    m_loopSetupSeconds = (std::max)(0.0f, Settings::settingsIni.unlimitedPlaybackLoopSetupSeconds);
    m_loopEndingSeconds = (std::max)(0.0f, Settings::settingsIni.unlimitedPlaybackLoopEndingSeconds);
    m_loopRestartLabState = Settings::settingsIni.unlimitedPlaybackLoopRestartLabState;
    const int savedRestartMode = Settings::settingsIni.unlimitedPlaybackLoopRestartMode;
    m_loopRestartMode = (savedRestartMode >= LoopReset_Middle && savedRestartMode <= LoopReset_Custom)
        ? savedRestartMode
        : LoopReset_Middle;
    m_initialized = true;
}

// Runs from a hook on FUN_0056B1F0 (the game's per-logic-tick Update, confirmed via Ghidra
// decompile - see docs/Research/PlaybackTimingAddendumReport.txt), which calls the per-player
// input consumer (including the native playback-slot reader) before it ever reaches the
// GetFrameCounter hook that Tick() runs from. Firing StartRuntimePlayback() from Tick() means the
// consumer for the current tick has already read the pre-playback state, so the recording's first
// frame is silently skipped and everything starts one real tick late. Running the same write here
// instead lands before that tick's read, matching how the native training menu's own "start
// playback" (also just a write to the same fields, from the menu-input code path) behaves.
// Only "Play Now" is moved here: it's a simple, already-decided user-intent flag with no
// real-time game-state dependency, so checking it one tick earlier carries no risk. The
// trigger-based auto-play paths (wakeup/gap/on-hit/loop/etc.) are NOT moved here because their
// condition checks in Tick() do depend on real-time state that may not be updated yet at this
// earlier point in the frame; they keep their existing (correct, already-tested) detection timing
// and so still have the same one-tick-late start as before - a known follow-up, not fixed by this.
namespace {
    // Sets a flag for as long as it is in scope, so a function with many early returns can
    // mark the whole of itself as "running inside the game's frame update".
    struct HookPhaseGuard {
        bool& flag;
        explicit HookPhaseGuard(bool& f) : flag(f) { flag = true; }
        ~HookPhaseGuard() { flag = false; }
    };
}

void UnlimitedPlaybackManager::RunPreTick() {
    // The other hook entry point, and the same rule applies: this runs from inside the
    // game's own frame update, so nothing reached from here may build a SnapshotApparatus.
    HookPhaseGuard hookPhase(m_inHookTick);

    static bool loggedAlive = false;
    if (!loggedAlive) {
        LOG(7, "[UP][diag] RunPreTick hook is alive (first call).\n");
        loggedAlive = true;
    }
    ExecutePendingPlayNow();
}

void UnlimitedPlaybackManager::Tick() {
    // Marks everything below as running inside the game's frame update, so nothing reached
    // from here builds a SnapshotApparatus. RAII because Tick has a dozen early returns.
    // See CanBuildSnapshotApparatusHere.
    HookPhaseGuard hookPhase(m_inHookTick);

    InitializeIfNeeded();
    PruneExpiredToasts();

    const bool inTrainingMatch =
        g_gameVals.pGameMode &&
        g_gameVals.pGameState &&
        (*g_gameVals.pGameMode == GameMode_Training) &&
        (*g_gameVals.pGameState == GameState_InMatch) &&
        (GetGameSceneStatus() >= GameSceneStatus_Running) &&
        !g_interfaces.player2.IsCharDataNullPtr();

    if (!inTrainingMatch) {
        if (m_runtimeSlotRestorePending || m_runtimeSlotBackupValid) {
            LOG(1, "[UP] Leaving training match; stopping playback and restoring runtime slot.\n");
        }
        // Match-end cleanup now runs from MatchState::OnMatchEnd while the training block is still valid.
        // Once we're out of training, only clear our own bookkeeping and avoid touching native playback state.
        ResetRuntimePlaybackState(true);
        m_pendingPlayNowRequested = false;
        m_lastObservedFrame = -1;
        m_prevWakeupCondition = false;
        m_prevGapCondition = false;
        m_prevOnBlockCondition = false;
        m_prevOnHitCondition = false;
        m_prevThrowTechCondition = false;
        StopLoop(nullptr);
        ClearLoopCustomSnapshot();
    } else {
        TryRestoreRuntimeSlotAfterPlayback();
        // NOTE: the "Play Now" pending request is now executed from RunPreTick() (hooked earlier
        // in the frame, before the game's active-slot playback input read), not here - see
        // RunPreTick() for why.
        LogSlot4PlaybackDiagnostics();
    }

    if (!inTrainingMatch || !m_triggerRuntimeEnabled || m_profileRuntimeSuppressedUntilReset) {
        return;
    }

    if (!g_gameVals.pFrameCount) {
        return;
    }

    if (!m_keyPressTriggerArmed && !HotkeyManager::IsDown(HotkeyManager::Hotkey_UnlimitedPlaybackTrigger)) {
        LogRuntimeGateState("Tick arming key-press triggers");
        m_keyPressTriggerArmed = true;
        LOG(1, "[UP] Key-press triggers armed.\n");
    }

    const int frame = g_gameVals.pFrameCount ? *g_gameVals.pFrameCount : 0;
    if (m_lastObservedFrame >= 0 && frame < m_lastObservedFrame) {
        ForceResetTriggers(L("Trigger runtime resynced after training reset.").c_str());
        if (m_loopActive) {
            m_loopPhaseStartFrame = frame;
        }
        if (m_loopNativeResetPulseActive) {
            // The training reset we forced just happened; drop the held combo this very tick
            // so the direction key cannot walk the character off the reset position.
            m_loopNativeResetHoldTicksLeft = 0;
        }
    }
    m_lastObservedFrame = frame;

    // Every frame, not on edit. The dummy-action table is what says which triggers are
    // armed, and a modal finishing its configuration changes that answer - hanging the
    // update off the UI would mean actions only worked while their menu was open.
    DummyActionManager::Instance().SyncTriggerEnable();

    // What the two sides think is armed, logged only when it changes. The point is to make a
    // trigger that does nothing say which link is broken - the action, the enable flag, or
    // the firing condition - rather than being silent about all three.
    {
        DummyActionManager& actions = DummyActionManager::Instance();
        char state[Trigger_Count * 24 + 1] = {};
        int used = 0;
        for (int i = 0; i < Trigger_Count; ++i) {
            const DummyActionManager::Action& action =
                actions.Get(static_cast<TriggerType>(i));
            used += sprintf_s(state + used, sizeof(state) - used, "%s%d:src%d/run%d/en%d",
                i ? " " : "",
                i,
                static_cast<int>(action.source),
                actions.IsRunnable(static_cast<TriggerType>(i)) ? 1 : 0,
                m_triggers[i].enabled ? 1 : 0);
        }
        static std::string lastState;
        if (lastState != state) {
            lastState = state;
            LOG(1, "[UP][ACTIONS] %s\n", state);
        }
    }

    ProcessPendingTriggerDelays(frame);

    // The loop used to be gated on Trigger_OnLoop being *armed* and then returned, so
    // merely having a loop action configured silently disabled all six other triggers -
    // which is why a wakeup or on-hit action stopped working as soon as a loop was set up.
    // Triggers are meant to be independent, so the loop is now just another one of them.
    // Polled whenever a loop is configured, running or not.
    if (m_triggers[Trigger_OnLoop].enabled) {
        ProcessLoopHotkey(frame);
    }

    if (m_loopActive) {
        ProcessLoopTick(frame);

        // The one exception: while the loop is physically resetting the lab it overrides
        // inputs and moves the characters, so letting another trigger fire into that would
        // fight it for the CF slot and land the action somewhere meaningless.
        if (m_loopPhase == LoopPhase_PositionSetup || m_loopPhase == LoopPhase_Ending) {
            return;
        }
    }

    TryFireTrigger(Trigger_KeyPress, frame);
    TryFireTrigger(Trigger_Wakeup, frame);
    TryFireTrigger(Trigger_Gap, frame);
    TryFireTrigger(Trigger_OnBlock, frame);
    TryFireTrigger(Trigger_OnHit, frame);
    TryFireTrigger(Trigger_ThrowTech, frame);
}

void UnlimitedPlaybackManager::OnMatchInit() {
    InitializeIfNeeded();
    LogRuntimeGateState("OnMatchInit before reset");
    ResetTriggerRuntimeState(!m_profileRuntimeSuppressedUntilReset);
    LogRuntimeGateState("OnMatchInit after reset");
}

void UnlimitedPlaybackManager::ForceResetTriggers(const char* toastText) {
    LogRuntimeGateState("ForceResetTriggers before clear suppression");
    m_profileRuntimeSuppressedUntilReset = false;
    ResetTriggerRuntimeState(true);
    LogRuntimeGateState("ForceResetTriggers after reset");
    const char* text = toastText;
    if (!text) {
        text = L("Trigger runtime state reset.").c_str();
    }
    if (text[0] != '\0') {
        PushToast(text);
    }
}

void UnlimitedPlaybackManager::ResetTriggerRuntimeState(bool enableRuntime) {
    LOG(1,
        "[UP][STATE] ResetTriggerRuntimeState begin enableRuntime=%d oldEnabled=%d oldSuppressed=%d oldArmed=%d oldLastFrame=%d entries=%u cache=%u mode=%d\n",
        enableRuntime ? 1 : 0,
        m_triggerRuntimeEnabled ? 1 : 0,
        m_profileRuntimeSuppressedUntilReset ? 1 : 0,
        m_keyPressTriggerArmed ? 1 : 0,
        m_lastObservedFrame,
        static_cast<unsigned int>(EditTarget().entries.size()),
        static_cast<unsigned int>(EditTarget().cache.size()),
        m_mode);
    m_triggerRuntimeEnabled = enableRuntime;
    m_lastObservedFrame = -1;
    for (int i = 0; i < Trigger_Count; ++i) {
        m_triggers[i].lastTriggeredFrame = -999999;
        // A countdown left over from before the reset would fire into a match that has
        // moved on, so it goes with the rest of the runtime state.
        m_triggers[i].pendingFireFrame = -1;
    }

    m_prevWakeupCondition = false;
    m_prevGapCondition = false;
    m_prevOnBlockCondition = false;
    m_prevOnHitCondition = false;
    m_prevThrowTechCondition = false;
    m_keyPressTriggerArmed = false;
    LogRuntimeGateState("ResetTriggerRuntimeState end");
}

void UnlimitedPlaybackManager::LogRuntimeGateState(const char* tag) const {
    const int frame = g_gameVals.pFrameCount ? *g_gameVals.pFrameCount : -1;
    const int gameMode = g_gameVals.pGameMode ? *g_gameVals.pGameMode : -1;
    const int gameState = g_gameVals.pGameState ? *g_gameVals.pGameState : -1;
    LOG(1,
        "[UP][STATE] %s mode=%d enabled=%d suppressed=%d armed=%d frame=%d gameMode=%d gameState=%d entries=%u cache=%u activeProfile='%s'\n",
        tag ? tag : "(null)",
        m_mode,
        m_triggerRuntimeEnabled ? 1 : 0,
        m_profileRuntimeSuppressedUntilReset ? 1 : 0,
        m_keyPressTriggerArmed ? 1 : 0,
        frame,
        gameMode,
        gameState,
        static_cast<unsigned int>(EditTarget().entries.size()),
        static_cast<unsigned int>(EditTarget().cache.size()),
        EditTarget().path.c_str());
}

void UnlimitedPlaybackManager::LogEntryCacheSummary(const char* tag) const {
    LOG(1,
        "[UP][DATA] %s entries=%u cache=%u activeProfile='%s'\n",
        tag ? tag : "(null)",
        static_cast<unsigned int>(EditTarget().entries.size()),
        static_cast<unsigned int>(EditTarget().cache.size()),
        EditTarget().path.c_str());

    for (size_t i = 0; i < EditTarget().entries.size(); ++i) {
        const auto& entry = EditTarget().entries[i];
        const auto cacheIt = EditTarget().cache.find(entry.id);
        const bool cacheLoaded = cacheIt != EditTarget().cache.end() && cacheIt->second.loaded;
        const unsigned int frameBytes = cacheLoaded ? static_cast<unsigned int>(cacheIt->second.frames.size()) : 0U;
        const int facing = cacheLoaded ? (cacheIt->second.facingLeft ? 1 : 0) : -1;
        LOG(1,
            "[UP][DATA]   idx=%u id='%s' name='%s' enabled=%d weight=%.3f rel='%s' cacheLoaded=%d frameBytes=%u facing=%d digest=0x%08X preview='%s' trig=[%d,%d,%d,%d,%d,%d,%d]\n",
            static_cast<unsigned int>(i),
            entry.id.c_str(),
            entry.name.c_str(),
            entry.enabled ? 1 : 0,
            entry.weight,
            entry.relativePath.c_str(),
            cacheLoaded ? 1 : 0,
            frameBytes,
            facing,
            cacheLoaded ? ComputePlaybackDigest(cacheIt->second.frames) : 0U,
            cacheLoaded ? PreviewPlaybackBytes(cacheIt->second.frames, 16).c_str() : "",
            entry.triggerEnabled[0] ? 1 : 0,
            entry.triggerEnabled[1] ? 1 : 0,
            entry.triggerEnabled[2] ? 1 : 0,
            entry.triggerEnabled[3] ? 1 : 0,
            entry.triggerEnabled[4] ? 1 : 0,
            entry.triggerEnabled[5] ? 1 : 0,
            entry.triggerEnabled[6] ? 1 : 0);
    }
}

void UnlimitedPlaybackManager::DebugLogState(const char* tag) const {
    LogRuntimeGateState(tag);
    LogEntryCacheSummary(tag);
}

void UnlimitedPlaybackManager::OnMatchEnd() {
    InitializeIfNeeded();
    m_triggerRuntimeEnabled = false;
    const bool hasRuntimeOwnership =
        m_runtimeSlotRestorePending ||
        m_runtimeSlotBackupValid ||
        m_runtimeActiveSlotBackupValid ||
        m_runtimePlaybackTypeBackupValid;

    if (hasRuntimeOwnership) {
        LOG(1, "[UP] OnMatchEnd cleanup: stopping playback and discarding runtime slot restore.\n");
        if (m_runtimePlaybackManager.playback_control_p) {
            m_runtimePlaybackManager.set_playback_control(0);
        }
        m_runtimePlaybackManager.set_playback_position(0);
        if (m_runtimeActiveSlotBackupValid) {
            const int restoredSlot = m_runtimeActiveSlotBackup + 1;
            if (restoredSlot >= 1 && restoredSlot <= 4) {
                m_runtimePlaybackManager.set_active_slot(restoredSlot);
            }
        }
        if (m_runtimePlaybackTypeBackupValid) {
            m_runtimePlaybackManager.set_playback_type(m_runtimePlaybackTypeBackup);
        }
    }
    m_runtimeSlotBackupFrames.clear();
    m_runtimeSlotBackupValid = false;
    m_runtimeSlotRestorePending = false;
    m_runtimeSlotNumber = 1;
    m_runtimeActiveSlotBackupValid = false;
    m_runtimeActiveSlotBackup = 0;
    m_runtimePlaybackTypeBackupValid = false;
    m_runtimePlaybackTypeBackup = 0;
    m_lastObservedFrame = -1;
    m_prevWakeupCondition = false;
    m_prevGapCondition = false;
    m_prevOnBlockCondition = false;
    m_prevOnHitCondition = false;
    m_prevThrowTechCondition = false;
    m_keyPressTriggerArmed = false;
    StopLoop(nullptr);
    ClearLoopCustomSnapshot();
}

int UnlimitedPlaybackManager::GetMode() const {
    return m_mode;
}

void UnlimitedPlaybackManager::SetMode(int mode) {
    if (mode < Mode_Default || mode > Mode_Unlimited) {
        mode = Mode_Default;
    }
    m_mode = mode;
}

int UnlimitedPlaybackManager::GetSelectionMode() const {
    return EditTarget().selectionMode;
}

void UnlimitedPlaybackManager::SetSelectionMode(int mode) {
    if (mode < Selection_Random || mode > Selection_NonRepeatingRandom) {
        mode = Selection_Random;
    }
    EditTarget().selectionMode = mode;
    for (int i = 0; i < Trigger_Count; ++i) {
        m_sequentialIndex[i] = 0;
        m_nonRepeatPools[i].clear();
    }
}

bool UnlimitedPlaybackManager::GetAutoMirrorOnSideSwap() const {
    return m_autoMirrorOnSideSwap;
}

void UnlimitedPlaybackManager::SetAutoMirrorOnSideSwap(bool enabled) {
    m_autoMirrorOnSideSwap = enabled;
}

float UnlimitedPlaybackManager::GetLoopSetupSeconds() const {
    return m_loopSetupSeconds;
}

void UnlimitedPlaybackManager::SetLoopSetupSeconds(float seconds) {
    m_loopSetupSeconds = (std::max)(0.0f, seconds);
}

float UnlimitedPlaybackManager::GetLoopEndingSeconds() const {
    return m_loopEndingSeconds;
}

void UnlimitedPlaybackManager::SetLoopEndingSeconds(float seconds) {
    m_loopEndingSeconds = (std::max)(0.0f, seconds);
}

bool UnlimitedPlaybackManager::GetLoopRestartLabState() const {
    return m_loopRestartLabState;
}

void UnlimitedPlaybackManager::SetLoopRestartLabState(bool enabled) {
    m_loopRestartLabState = enabled;
}

int UnlimitedPlaybackManager::GetLoopRestartMode() const {
    return m_loopRestartMode;
}

void UnlimitedPlaybackManager::SetLoopRestartMode(int mode) {
    if (mode < LoopReset_Middle || mode > LoopReset_Custom) {
        mode = LoopReset_Middle;
    }
    m_loopRestartMode = mode;
}

bool UnlimitedPlaybackManager::IsLoopActive() const {
    return m_loopActive;
}

bool UnlimitedPlaybackManager::GetLoopSetupCountdown(float* outRemainingSeconds, float* outTotalSeconds) const {
    if (!m_loopActive || m_loopPhase != LoopPhase_Setup || !m_loopRestartAppliedForCycle || !g_gameVals.pFrameCount) {
        return false;
    }

    const int totalFrames = LoopSecondsToFrames(m_loopSetupSeconds);
    if (totalFrames <= 0) {
        return false;
    }

    const int endFrame = m_loopPhaseStartFrame + totalFrames;
    const int remainingFrames = (std::max)(0, endFrame - static_cast<int>(*g_gameVals.pFrameCount));
    if (remainingFrames <= 0) {
        return false;
    }

    if (outRemainingSeconds) {
        *outRemainingSeconds = static_cast<float>(remainingFrames) / 60.0f;
    }
    if (outTotalSeconds) {
        *outTotalSeconds = static_cast<float>(totalFrames) / 60.0f;
    }
    return true;
}

bool UnlimitedPlaybackManager::HasLoopCustomSnapshot() const {
    return m_loopCustomSnapshotSlotIndex >= 0 ||
        (!m_loopCustomSnapshotBytes.empty() && m_loopCustomSnapshotSize > 0);
}

bool UnlimitedPlaybackManager::IsLoopSnapshotReadyForMode(int mode) const {
    return HasLoopCustomSnapshot() && m_loopSnapshotSourceMode == mode;
}

bool UnlimitedPlaybackManager::IsLoopPositionSetupActive() const {
    return m_loopActive && m_loopPhase == LoopPhase_PositionSetup;
}

const std::vector<UnlimitedPlaybackManager::PlaybackEntry>& UnlimitedPlaybackManager::GetEntries() const {
    return EditTarget().entries;
}

std::vector<UnlimitedPlaybackManager::PlaybackEntry>& UnlimitedPlaybackManager::GetEntriesMutable() {
    return EditTarget().entries;
}

UnlimitedPlaybackManager::TriggerConfig& UnlimitedPlaybackManager::GetTrigger(TriggerType type) {
    return m_triggers[type];
}

const UnlimitedPlaybackManager::TriggerConfig& UnlimitedPlaybackManager::GetTrigger(TriggerType type) const {
    return m_triggers[type];
}

CompatibilityManager::Result UnlimitedPlaybackManager::ProbePlaybackCompatibility(const std::string& playbackPath) const {
    CompatibilityManager::FileVersion detected = { 1, 0 };
    bool hasHeader = false;
    if (!ReadPlaybackFormatVersion(playbackPath, &detected, &hasHeader)) {
        CompatibilityManager::Result r;
        r.action = CompatibilityManager::Action_Reject;
        r.detected = detected;
        r.current = CompatibilityManager::CurrentPlaybackVersion();
        r.reason = L("Could not read playback file.");
        r.canForce = false;
        return r;
    }
    return CompatibilityManager::EvaluatePlayback(detected, hasHeader);
}

bool UnlimitedPlaybackManager::AddPlaybackFile(const std::string& sourcePath, const std::string& displayName, bool forceLoadIncompatible) {
    InitializeIfNeeded();

    std::string src = sourcePath;
    if (!PathExists(src)) {
        PushToast(L("File not found."));
        return false;
    }

    CachedPlayback playback;
    if (!ReadPlaybackFile(src, &playback, forceLoadIncompatible)) {
        PushToast(L("Invalid playback file."));
        return false;
    }

    std::string fallbackName = sourcePath;
    const size_t slash = fallbackName.find_last_of("/\\");
    if (slash != std::string::npos) {
        fallbackName = fallbackName.substr(slash + 1);
    }
    const size_t dot = fallbackName.find_last_of('.');
    if (dot != std::string::npos) {
        fallbackName = fallbackName.substr(0, dot);
    }

    PlaybackEntry entry;
    entry.id = MakeEntryId();
    entry.name = displayName.empty() ? fallbackName : displayName;
    entry.relativePath = BuildUniqueRelativePath(entry.name);
    entry.enabled = true;
    entry.weight = 1.0f;
    EditTarget().entries.push_back(entry);
    EditTarget().cache[entry.id] = playback;

    PushToast(L("Playback imported."));
    return true;
}

bool UnlimitedPlaybackManager::CaptureSlotToLibrary(int slot, const std::string& displayName) {
    InitializeIfNeeded();

    if (slot < 1 || slot > 4) {
        PushToast(L("Slot must be between 1 and 4."));
        return false;
    }

    std::vector<char> frames;
    bool facingLeft = false;
    // While the runtime has this slot borrowed it holds our temporary playback data, not
    // the user's recording -- read what is going to be restored to it instead.
    if (!TryReadBorrowedSlot(slot, &frames, &facingLeft)) {
        PlaybackSlot pslot(slot);
        frames = pslot.get_slot_buffer_raw();
        facingLeft = pslot.get_facing_direction() != 0;
    }
    if (frames.size() > static_cast<size_t>(kMaxFramesPerPlayback) * 2) {
        frames.resize(static_cast<size_t>(kMaxFramesPerPlayback) * 2);
    }

    const std::string baseName = displayName.empty() ? FormatLocalized("Slot %d", slot) : displayName;

    PlaybackEntry entry;
    entry.id = MakeEntryId();
    entry.name = baseName;
    entry.relativePath = BuildUniqueRelativePath(baseName);
    entry.enabled = true;
    entry.weight = 1.0f;
    EditTarget().entries.push_back(entry);
    CachedPlayback playback;
    playback.loaded = true;
    playback.facingLeft = facingLeft;
    playback.frames = frames;
    EditTarget().cache[entry.id] = playback;

    PushToast(L("Captured slot to library."));
    return true;
}

bool UnlimitedPlaybackManager::StartReplayRecording(bool recordP1) {
    InitializeIfNeeded();

    if (!IsReplayMatchActive()) {
        PushToast(L("Start replay recording only while a replay match is active."));
        return false;
    }

    m_replayRecordingActive = true;
    m_replayRecordingAsP1 = recordP1;
    m_replayRecordingStartFrame = g_gameVals.pFrameCount ? *g_gameVals.pFrameCount : 0;
    m_replayRecordingRound = static_cast<int>(*(GetBbcfBaseAdress() + 0x11C034C));
    PushToast(FormatLocalized("Replay recording started: %s.", recordP1 ? "P1" : "P2"));
    return true;
}

bool UnlimitedPlaybackManager::StopReplayRecordingToBuffer(
    std::vector<char>* outTrimmed, char* outFacing) {
    InitializeIfNeeded();

    if (!outTrimmed || !outFacing) {
        return false;
    }
    if (!m_replayRecordingActive) {
        PushToast(L("No replay recording in progress."));
        return false;
    }
    if (!IsReplayMatchActive()) {
        CancelReplayRecording(L("Replay recording cancelled (left replay match).").c_str());
        return false;
    }

    const int endFrame = g_gameVals.pFrameCount ? *g_gameVals.pFrameCount : 0;
    if (endFrame <= m_replayRecordingStartFrame) {
        CancelReplayRecording(L("Replay recording cancelled (too short).").c_str());
        return false;
    }

    const int recordedPlayer = m_replayRecordingAsP1 ? 0 : 1;
    std::vector<char> frames;
    if (!BuildPlaybackFramesFromReplayRange(m_replayRecordingRound,
            m_replayRecordingStartFrame, endFrame, recordedPlayer, &frames)) {
        CancelReplayRecording(L("Replay recording cancelled (failed reading replay frames).").c_str());
        return false;
    }

    const bool facingLeft = m_replayRecordingAsP1
        ? (g_interfaces.player1.GetData() && g_interfaces.player1.GetData()->facingLeft2 != 0)
        : (g_interfaces.player2.GetData() && g_interfaces.player2.GetData()->facingLeft2 != 0);

    // Frames come back in the slot's raw two-bytes-per-frame layout; a playback file holds
    // one byte per frame after its facing byte.
    *outTrimmed = PlaybackManager::raw_to_trimmed(frames);
    *outFacing = facingLeft ? 1 : 0;

    m_replayRecordingActive = false;
    m_replayRecordingAsP1 = true;
    m_replayRecordingRound = 0;
    m_replayRecordingStartFrame = 0;

    LOG(1, "[UP] Replay capture stopped: %u frames, facing %d\n",
        static_cast<unsigned int>(outTrimmed->size()), static_cast<int>(*outFacing));
    return !outTrimmed->empty();
}

bool UnlimitedPlaybackManager::StopReplayRecordingAndSave(const std::string& displayName) {
    InitializeIfNeeded();

    if (!m_replayRecordingActive) {
        PushToast(L("No replay recording in progress."));
        return false;
    }

    if (!IsReplayMatchActive()) {
        CancelReplayRecording(L("Replay recording cancelled (left replay match).").c_str());
        return false;
    }

    const int endFrame = g_gameVals.pFrameCount ? *g_gameVals.pFrameCount : 0;
    if (endFrame <= m_replayRecordingStartFrame) {
        CancelReplayRecording(L("Replay recording cancelled (too short).").c_str());
        return false;
    }

    const int recordedPlayer = m_replayRecordingAsP1 ? 0 : 1;
    std::vector<char> frames;
    if (!BuildPlaybackFramesFromReplayRange(m_replayRecordingRound, m_replayRecordingStartFrame, endFrame, recordedPlayer, &frames)) {
        CancelReplayRecording(L("Replay recording cancelled (failed reading replay frames).").c_str());
        return false;
    }

    bool facingLeft = false;
    if (m_replayRecordingAsP1) {
        facingLeft = g_interfaces.player1.GetData() && g_interfaces.player1.GetData()->facingLeft2 != 0;
    } else {
        facingLeft = g_interfaces.player2.GetData() && g_interfaces.player2.GetData()->facingLeft2 != 0;
    }

    const std::string baseName = displayName.empty()
        ? FormatLocalized("Replay %s", m_replayRecordingAsP1 ? "P1" : "P2")
        : displayName;

    PlaybackEntry entry;
    entry.id = MakeEntryId();
    entry.name = baseName;
    entry.relativePath = BuildUniqueRelativePath(baseName);
    entry.enabled = true;
    entry.weight = 1.0f;
    EditTarget().entries.push_back(entry);
    CachedPlayback playback;
    playback.loaded = true;
    playback.facingLeft = facingLeft;
    playback.frames = frames;
    EditTarget().cache[entry.id] = playback;

    m_replayRecordingActive = false;
    m_replayRecordingAsP1 = true;
    m_replayRecordingRound = 0;
    m_replayRecordingStartFrame = 0;

    PushToast(L("Replay recording saved to library."));
    return true;
}

void UnlimitedPlaybackManager::CancelReplayRecording(const char* reason) {
    m_replayRecordingActive = false;
    m_replayRecordingAsP1 = true;
    m_replayRecordingRound = 0;
    m_replayRecordingStartFrame = 0;
    if (reason && reason[0] != '\0') {
        PushToast(reason);
    }
}

bool UnlimitedPlaybackManager::IsReplayRecording() const {
    return m_replayRecordingActive;
}

bool UnlimitedPlaybackManager::IsReplayRecordingAsP1() const {
    return m_replayRecordingAsP1;
}

int UnlimitedPlaybackManager::GetReplayRecordingStartFrame() const {
    return m_replayRecordingStartFrame;
}

bool UnlimitedPlaybackManager::RemoveEntryByIndex(size_t idx) {
    InitializeIfNeeded();

    if (idx >= EditTarget().entries.size()) {
        return false;
    }

    EditTarget().cache.erase(EditTarget().entries[idx].id);
    EditTarget().entries.erase(EditTarget().entries.begin() + idx);
    for (int i = 0; i < Trigger_Count; ++i) {
        m_sequentialIndex[i] = 0;
        m_nonRepeatPools[i].clear();
    }
    PushToast(L("Entry removed."));
    return true;
}

int UnlimitedPlaybackManager::RemoveEntriesByIndices(const std::vector<size_t>& indices) {
    InitializeIfNeeded();

    std::vector<size_t> sortedIndices(indices.begin(), indices.end());
    std::sort(sortedIndices.begin(), sortedIndices.end());

    int removedCount = 0;
    for (auto it = sortedIndices.rbegin(); it != sortedIndices.rend(); ++it) {
        const size_t idx = *it;
        if (idx >= EditTarget().entries.size()) {
            continue;
        }
        EditTarget().cache.erase(EditTarget().entries[idx].id);
        EditTarget().entries.erase(EditTarget().entries.begin() + static_cast<std::ptrdiff_t>(idx));
        ++removedCount;
    }
    if (removedCount > 0) {
        for (int i = 0; i < Trigger_Count; ++i) {
            m_sequentialIndex[i] = 0;
            m_nonRepeatPools[i].clear();
        }
        PushToast(removedCount == 1 ? L("Entry removed.") : FormatText(L("%d entries removed.").c_str(), removedCount));
    }
    return removedCount;
}

bool UnlimitedPlaybackManager::MoveEntry(size_t fromIdx, size_t toIdx) {
    InitializeIfNeeded();

    if (fromIdx >= EditTarget().entries.size() || toIdx >= EditTarget().entries.size() || fromIdx == toIdx) {
        return false;
    }

    PlaybackEntry entry = std::move(EditTarget().entries[fromIdx]);
    EditTarget().entries.erase(EditTarget().entries.begin() + static_cast<std::ptrdiff_t>(fromIdx));
    EditTarget().entries.insert(EditTarget().entries.begin() + static_cast<std::ptrdiff_t>(toIdx), std::move(entry));
    for (int i = 0; i < Trigger_Count; ++i) {
        m_sequentialIndex[i] = 0;
        m_nonRepeatPools[i].clear();
    }
    PushToast(L("Slot moved."));
    return true;
}

bool UnlimitedPlaybackManager::MoveEntries(const std::vector<size_t>& fromIndicesSorted, size_t insertionIndex, size_t* outInsertedAt) {
    InitializeIfNeeded();

    if (fromIndicesSorted.empty() || insertionIndex > EditTarget().entries.size()) {
        return false;
    }
    for (size_t idx : fromIndicesSorted) {
        if (idx >= EditTarget().entries.size()) {
            return false;
        }
    }

    std::vector<bool> isMoved(EditTarget().entries.size(), false);
    for (size_t idx : fromIndicesSorted) {
        isMoved[idx] = true;
    }

    size_t removedBeforeInsertion = 0;
    for (size_t idx : fromIndicesSorted) {
        if (idx < insertionIndex) {
            ++removedBeforeInsertion;
        }
    }

    std::vector<PlaybackEntry> moved;
    moved.reserve(fromIndicesSorted.size());
    std::vector<PlaybackEntry> remaining;
    remaining.reserve(EditTarget().entries.size() - fromIndicesSorted.size());
    for (size_t i = 0; i < EditTarget().entries.size(); ++i) {
        if (isMoved[i]) {
            moved.push_back(std::move(EditTarget().entries[i]));
        } else {
            remaining.push_back(std::move(EditTarget().entries[i]));
        }
    }

    size_t insertAt = insertionIndex - removedBeforeInsertion;
    if (insertAt > remaining.size()) {
        insertAt = remaining.size();
    }

    remaining.insert(
        remaining.begin() + static_cast<std::ptrdiff_t>(insertAt),
        std::make_move_iterator(moved.begin()),
        std::make_move_iterator(moved.end()));
    EditTarget().entries = std::move(remaining);

    for (int i = 0; i < Trigger_Count; ++i) {
        m_sequentialIndex[i] = 0;
        m_nonRepeatPools[i].clear();
    }
    if (outInsertedAt) {
        *outInsertedAt = insertAt;
    }
    PushToast(L("Slots moved."));
    return true;
}

void UnlimitedPlaybackManager::SetEntriesEnabled(const std::vector<size_t>& indices, bool enabled) {
    InitializeIfNeeded();

    for (size_t idx : indices) {
        if (idx < EditTarget().entries.size()) {
            EditTarget().entries[idx].enabled = enabled;
        }
    }
    for (int i = 0; i < Trigger_Count; ++i) {
        m_sequentialIndex[i] = 0;
        m_nonRepeatPools[i].clear();
    }
}

void UnlimitedPlaybackManager::SetAllEntriesEnabled(bool enabled) {
    InitializeIfNeeded();

    for (auto& entry : EditTarget().entries) {
        entry.enabled = enabled;
    }
    for (int i = 0; i < Trigger_Count; ++i) {
        m_sequentialIndex[i] = 0;
        m_nonRepeatPools[i].clear();
    }
    PushToast(enabled ? L("All slots enabled.") : L("All slots disabled."));
}

bool UnlimitedPlaybackManager::RenameEntry(size_t idx, const std::string& newName) {
    if (idx >= EditTarget().entries.size() || newName.empty()) {
        return false;
    }

    EditTarget().entries[idx].name = newName;
    PushToast(L("Entry renamed."));
    return true;
}

bool UnlimitedPlaybackManager::LoadEntryIntoSlot(size_t idx, int slot) {
    InitializeIfNeeded();

    if (idx >= EditTarget().entries.size() || slot < 1 || slot > 4) {
        return false;
    }

    const bool inTrainingMatch =
        g_gameVals.pGameMode &&
        g_gameVals.pGameState &&
        (*g_gameVals.pGameMode == GameMode_Training) &&
        (*g_gameVals.pGameState == GameState_InMatch) &&
        (GetGameSceneStatus() >= GameSceneStatus_Running) &&
        !g_interfaces.player2.IsCharDataNullPtr();
    if (!inTrainingMatch) {
        PushToast(L("Sending to a CF slot works only during a training match."));
        return false;
    }

    const auto& entry = EditTarget().entries[idx];
    auto it = EditTarget().cache.find(entry.id);
    if (it == EditTarget().cache.end() || !it->second.loaded) {
        PushToast(L("Failed loading entry."));
        return false;
    }

    std::vector<char> frames = it->second.frames;
    int facingToLoad = it->second.facingLeft ? 1 : 0;
    bool currentFacingLeft = false;
    if (TryGetCurrentFacingLeft(&currentFacingLeft) && currentFacingLeft != it->second.facingLeft) {
        if (!m_autoMirrorOnSideSwap) {
            facingToLoad = currentFacingLeft ? 1 : 0;
        }
    }

    m_runtimePlaybackManager.load_raw_into_slot(frames, facingToLoad, slot);
    // Keep the write if the runtime happens to have this very slot borrowed, instead of
    // letting the pending restore quietly undo it.
    AbsorbExternalSlotWriteRaw(slot, frames, facingToLoad != 0);
    PushToast(L("Entry loaded into slot."));
    return true;
}

bool UnlimitedPlaybackManager::SaveEntryFromSlot(size_t idx, int slot) {
    InitializeIfNeeded();

    if (idx >= EditTarget().entries.size() || slot < 1 || slot > 4) {
        return false;
    }

    std::vector<char> frames;
    bool facingLeft = false;
    // While the runtime has this slot borrowed it holds our temporary playback data, not
    // the user's recording -- read what is going to be restored to it instead.
    if (!TryReadBorrowedSlot(slot, &frames, &facingLeft)) {
        PlaybackSlot pslot(slot);
        frames = pslot.get_slot_buffer_raw();
        facingLeft = pslot.get_facing_direction() != 0;
    }
    if (frames.size() > static_cast<size_t>(kMaxFramesPerPlayback) * 2) {
        frames.resize(static_cast<size_t>(kMaxFramesPerPlayback) * 2);
    }

    const std::string relPath = EnsureEntryLibraryRelativePath(idx);
    (void)relPath;
    CachedPlayback playback;
    playback.loaded = true;
    playback.facingLeft = facingLeft;
    playback.frames = frames;
    EditTarget().cache[EditTarget().entries[idx].id] = playback;
    PushToast(L("Entry overwritten from slot."));
    return true;
}

bool UnlimitedPlaybackManager::ReadEntryPlayback(size_t idx, bool* outFacingLeft, std::vector<char>* outFrames) {
    InitializeIfNeeded();

    if (idx >= EditTarget().entries.size() || !outFacingLeft || !outFrames) {
        return false;
    }

    const auto& entry = EditTarget().entries[idx];
    auto it = EditTarget().cache.find(entry.id);
    if (it == EditTarget().cache.end() || !it->second.loaded) {
        return false;
    }

    *outFacingLeft = it->second.facingLeft;
    *outFrames = CompactPlaybackBytes(it->second.frames);
    return true;
}

bool UnlimitedPlaybackManager::WriteEntryPlayback(size_t idx, bool facingLeft, const std::vector<char>& frames) {
    InitializeIfNeeded();

    if (idx >= EditTarget().entries.size()) {
        return false;
    }

    std::vector<char> clampedFrames = frames;
    if (clampedFrames.size() > static_cast<size_t>(kMaxFramesPerPlayback)) {
        clampedFrames.resize(kMaxFramesPerPlayback);
    }

    const std::string relPath = EnsureEntryLibraryRelativePath(idx);
    (void)relPath;
    CachedPlayback playback;
    playback.loaded = true;
    playback.facingLeft = facingLeft;
    playback.frames = ExpandPlaybackBytes(clampedFrames);
    EditTarget().cache[EditTarget().entries[idx].id] = playback;
    PushToast(L("Entry saved."));
    return true;
}

bool UnlimitedPlaybackManager::SaveEntryToFile(size_t idx, const std::string& outputPath) {
    InitializeIfNeeded();

    if (idx >= EditTarget().entries.size() || outputPath.empty()) {
        return false;
    }

    const auto& entry = EditTarget().entries[idx];
    const auto it = EditTarget().cache.find(entry.id);
    if (it == EditTarget().cache.end() || !it->second.loaded) {
        PushToast(L("Failed saving entry to file."));
        return false;
    }

    if (!WritePlaybackFile(outputPath, it->second.facingLeft, it->second.frames)) {
        PushToast(L("Failed saving entry to file."));
        return false;
    }

    PushToast(L("Entry saved to file."));
    return true;
}

bool UnlimitedPlaybackManager::IsReplayMatchActive() const {
    return g_gameVals.pGameMode && g_gameVals.pGameState &&
        (*g_gameVals.pGameMode == GameMode_ReplayTheater) &&
        (*g_gameVals.pGameState == GameState_InMatch) &&
        !g_interfaces.player1.IsCharDataNullPtr() &&
        !g_interfaces.player2.IsCharDataNullPtr();
}

bool UnlimitedPlaybackManager::BuildPlaybackFramesFromReplayRange(
    int round,
    int startFrame,
    int endFrameExclusive,
    int recordedPlayer,
    std::vector<char>* outFrames) const {
    if (!outFrames || endFrameExclusive <= startFrame) {
        return false;
    }
    if (recordedPlayer < 0 || recordedPlayer > 1) {
        return false;
    }
    if (round < 0 || round > 2) {
        return false;
    }

    const int frameCount = (std::min)(endFrameExclusive - startFrame, kMaxFramesPerPlayback);
    if (frameCount <= 0) {
        return false;
    }

    char* base = GetBbcfBaseAdress();
    char* replayBase = base + 0x115B470 + 0x8d4;
    char* playerBase = replayBase + (0x7080 * recordedPlayer) + (0xE100 * round);
    outFrames->clear();
    outFrames->reserve(static_cast<size_t>(frameCount) * 2);
    for (int i = 0; i < frameCount; ++i) {
        const int frame = startFrame + i;
        char* recordedInput = playerBase + frame * 2;
        outFrames->push_back(recordedInput[0]);
        outFrames->push_back(recordedInput[1]);
    }
    return true;
}

bool UnlimitedPlaybackManager::PlayEntryNow(size_t idx) {
    InitializeIfNeeded();

    if (idx >= EditTarget().entries.size()) {
        return false;
    }

    const bool inTrainingMatch =
        g_gameVals.pGameMode &&
        g_gameVals.pGameState &&
        (*g_gameVals.pGameMode == GameMode_Training) &&
        (*g_gameVals.pGameState == GameState_InMatch) &&
        (GetGameSceneStatus() >= GameSceneStatus_Running) &&
        !g_interfaces.player2.IsCharDataNullPtr();
    if (!inTrainingMatch) {
        PushToast(L("Play now works only during a training match."));
        return false;
    }

    const auto& entry = EditTarget().entries[idx];
    auto it = EditTarget().cache.find(entry.id);
    if (it == EditTarget().cache.end() || !it->second.loaded) {
        PushToast(L("Failed loading entry."));
        return false;
    }

    // Defer the actual playback start to Tick() (the game's own logic-tick hook) instead of
    // firing it here, which runs from the render/Present hook (UI button click). Starting
    // playback off the logic tick can misalign the first sampled frame by one tick relative to
    // the native playback engine, which is enough to break tightly-timed inputs like microdashes.
    m_pendingPlayNowIndex = idx;
    m_pendingPlayNowRequested = true;
    LOG(7, "[UP][diag] PlayEntryNow queued idx=%u\n", static_cast<unsigned int>(idx));
    return true;
}

void UnlimitedPlaybackManager::ExecutePendingPlayNow() {
    if (!m_pendingPlayNowRequested) {
        return;
    }
    LOG(7, "[UP][diag] ExecutePendingPlayNow: consuming pending request, idx=%u\n",
        static_cast<unsigned int>(m_pendingPlayNowIndex));
    m_pendingPlayNowRequested = false;

    const size_t idx = m_pendingPlayNowIndex;
    if (idx >= EditTarget().entries.size()) {
        LOG(7, "[UP][diag] ExecutePendingPlayNow: idx out of range (entries=%u)\n",
            static_cast<unsigned int>(EditTarget().entries.size()));
        return;
    }

    const auto& entry = EditTarget().entries[idx];
    auto it = EditTarget().cache.find(entry.id);
    if (it == EditTarget().cache.end() || !it->second.loaded) {
        LOG(7, "[UP][diag] ExecutePendingPlayNow: cache miss/not loaded for entry '%s'\n", entry.name.c_str());
        PushToast(L("Failed loading entry."));
        return;
    }

    std::vector<char> frames = it->second.frames;
    int facingToLoad = it->second.facingLeft ? 1 : 0;
    bool mirrored = false;
    bool currentFacingLeft = false;
    if (TryGetCurrentFacingLeft(&currentFacingLeft) && currentFacingLeft != it->second.facingLeft) {
        mirrored = m_autoMirrorOnSideSwap;
        if (!m_autoMirrorOnSideSwap) {
            facingToLoad = currentFacingLeft ? 1 : 0;
        }
    }

    BackupRuntimeSlotIfNeeded();
    StartRuntimePlayback(frames, facingToLoad);
    m_runtimeSlotRestorePending = true;
    LOG(1, "[UP] PlayEntryNow started entry='%s' frames=%u facing=%d mirrored=%d\n",
        entry.name.c_str(),
        static_cast<unsigned int>(frames.size()),
        facingToLoad,
        mirrored ? 1 : 0);
    PushToast(FormatLocalized("Played: %s%s", entry.name.c_str(), mirrored ? L(" (mirrored)").c_str() : ""));
}

// DEV-ONLY DIAGNOSTIC (compiled out entirely unless DEBUG_LOG_LEVEL is bumped to 7 in logger.h):
// traces raw slot-4 playback stepping (native playback_position + the two bytes it's currently
// reading, plus both players' action/blockstun/hitstun) regardless of whether playback was
// started via "Play Now" or via native menu playback into CF slot 4 - lets us diff both against
// each other frame-by-frame. Useful again any time Unlimited Playback timing is suspected to have
// drifted from native training-menu playback; kept rather than deleted for that reason.
void UnlimitedPlaybackManager::LogSlot4PlaybackDiagnostics() {
#if DEBUG_LOG_LEVEL >= 7
    if (!m_runtimePlaybackManager.playback_control_p || !m_runtimePlaybackManager.active_slot_p ||
        !m_runtimePlaybackManager.bbcf_base_adress) {
        return;
    }

    short control = 0;
    std::memcpy(&control, m_runtimePlaybackManager.playback_control_p, sizeof(short));
    int activeSlot = -1;
    std::memcpy(&activeSlot, m_runtimePlaybackManager.active_slot_p, sizeof(int));

    const bool slot4Active = (control == 3) && (activeSlot == 3); // active_slot is 0-indexed
    if (!slot4Active) {
        if (m_diagSlot4WasActive) {
            LOG(7, "[UP][diag] slot4 playback ended. lastPos=%d\n", m_diagSlot4LastLoggedPosition);
        }
        m_diagSlot4WasActive = false;
        m_diagSlot4LastLoggedPosition = -2;
        return;
    }

    const int position = *reinterpret_cast<int*>(m_runtimePlaybackManager.bbcf_base_adress + 0x13AD940);

    if (!m_diagSlot4WasActive) {
        LOG(7, "[UP][diag] slot4 playback started. viaRuntimeTrigger=%d\n",
            m_runtimeSlotRestorePending ? 1 : 0);
        m_diagSlot4WasActive = true;
    }

    {
        // Log every tick unconditionally (no dedup on position change) - a dedup gate here
        // previously hid whether a position was ever actually observed vs. skipped when it
        // advanced more than once between two samples, which matters for pinning down exactly
        // when each tick's real observation happens relative to position advancement.
        char inputByte = 0;
        char auxByte = 0;
        if (m_runtimePlaybackManager.slots.size() >= 4 &&
            m_runtimePlaybackManager.slots[3].start_of_slot_inputs_p && position >= 0) {
            inputByte = *(m_runtimePlaybackManager.slots[3].start_of_slot_inputs_p + position * 2);
            auxByte = *(m_runtimePlaybackManager.slots[3].start_of_slot_inputs_p + position * 2 + 1);
        }
        const int frame = g_gameVals.pFrameCount ? *g_gameVals.pFrameCount : -1;

        const char* p1Action = "";
        int p1ActionTime = -1, p1Blockstun = -1, p1Hitstun = -1;
        const char* p2Action = "";
        int p2ActionTime = -1, p2Blockstun = -1, p2Hitstun = -1;
        if (!g_interfaces.player1.IsCharDataNullPtr()) {
            const auto* p1 = g_interfaces.player1.GetData();
            p1Action = p1->currentAction;
            p1ActionTime = p1->actionTime;
            p1Blockstun = p1->blockstun;
            p1Hitstun = p1->hitstun;
        }
        if (!g_interfaces.player2.IsCharDataNullPtr()) {
            const auto* p2 = g_interfaces.player2.GetData();
            p2Action = p2->currentAction;
            p2ActionTime = p2->actionTime;
            p2Blockstun = p2->blockstun;
            p2Hitstun = p2->hitstun;
        }

        LOG(7, "[UP][diag] frame=%d pbPos=%d input=0x%02X aux=0x%02X | p1=%s t=%d bs=%d hs=%d | p2=%s t=%d bs=%d hs=%d\n",
            frame, position, static_cast<unsigned char>(inputByte), static_cast<unsigned char>(auxByte),
            p1Action, p1ActionTime, p1Blockstun, p1Hitstun,
            p2Action, p2ActionTime, p2Blockstun, p2Hitstun);
        m_diagSlot4LastLoggedPosition = position;
    }
#endif
}

void UnlimitedPlaybackManager::ClearAll() {
    CancelReplayRecording(nullptr);
    m_triggerRuntimeEnabled = false;
    m_mode = Mode_Unlimited;
    EditTarget().entries.clear();
    EditTarget().cache.clear();
    for (int i = 0; i < Trigger_Count; ++i) {
        m_triggers[i].enabled = (i != Trigger_KeyPress && i != Trigger_OnLoop);
        m_triggers[i].cooldownFrames = 1;
        m_triggers[i].lastTriggeredFrame = -999999;
    }
    StopLoop(nullptr);
    ClearLoopCustomSnapshot();
    m_autoMirrorOnSideSwap = true;
    PushToast(L("Unlimited playback config cleared."));
}

bool UnlimitedPlaybackManager::SaveProfile(const std::string& profilePath) {
    std::string p = profilePath;
    if (!IsAbsolutePath(p)) {
        p = JoinPath(GetProfileFolder(), profilePath);
    }
    const size_t slash = p.find_last_of("/\\");
    if (slash != std::string::npos) {
        EnsureDirectoryRecursive(p.substr(0, slash));
    }

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        PushToast(L("Failed to save profile."));
        return false;
    }

    out << kProfileFormatKindKey << "=" << kProfileFormatKindValue << "\n";
    out << kProfileFormatVersionKey << "=" << CompatibilityManager::ToString(CompatibilityManager::CurrentProfileVersion()) << "\n";
    out << "version=1\n";
    out << "mode=" << m_mode << "\n";
    out << "selection_mode=" << EditTarget().selectionMode << "\n";
    out << "auto_mirror_side_swap=" << (m_autoMirrorOnSideSwap ? 1 : 0) << "\n";
    for (int i = 0; i < Trigger_Count; ++i) {
        out << "trigger." << TriggerKeyName(static_cast<TriggerType>(i)) << ".enabled=" << (m_triggers[i].enabled ? 1 : 0) << "\n";
        out << "trigger." << TriggerKeyName(static_cast<TriggerType>(i)) << ".cooldown=" << m_triggers[i].cooldownFrames << "\n";
        // keyCode is stored in settings.ini, not in profile files.
    }

    for (const auto& e : EditTarget().entries) {
        out << "entry="
            << e.id << "|"
            << e.name << "|"
            << e.relativePath << "|"
            << (e.enabled ? 1 : 0) << "|"
            << e.weight;
        for (int i = 0; i < Trigger_Count; ++i) {
            out << "|" << (e.triggerEnabled[i] ? 1 : 0);
        }
        out << "\n";

        CachedPlayback playback;
        bool havePlayback = false;
        const auto cacheIt = EditTarget().cache.find(e.id);
        if (cacheIt != EditTarget().cache.end() && cacheIt->second.loaded) {
            playback = cacheIt->second;
            havePlayback = true;
        }

        if (!havePlayback || !playback.loaded) {
            PushToast(FormatLocalized("Profile save failed: entry data missing for '%s'.", e.name.c_str()));
            return false;
        }

        const std::vector<char> serialized = SerializePlaybackBytes(playback.facingLeft, playback.frames);
        out << kEmbeddedEntryDataKey << "="
            << e.id << "|"
            << EncodeHex(serialized) << "\n";
    }

    out.close();
    EditTarget().path = p;
    PushToast(L("Profile saved."));
    return true;
}

CompatibilityManager::Result UnlimitedPlaybackManager::ProbeProfileCompatibility(const std::string& profilePath) const {
    std::string p = profilePath;
    if (!IsAbsolutePath(p)) {
        p = JoinPath(GetProfileFolder(), profilePath);
    }

    CompatibilityManager::FileVersion detected = { 0, 0 };
    bool hasExplicitVersion = false;
    if (!ReadProfileFormatVersion(p, &detected, &hasExplicitVersion)) {
        CompatibilityManager::Result r;
        r.action = CompatibilityManager::Action_Reject;
        r.detected = detected;
        r.current = CompatibilityManager::CurrentProfileVersion();
        r.reason = L("Could not read profile file.");
        r.canForce = false;
        return r;
    }

    return CompatibilityManager::EvaluateProfile(detected);
}

// Reads a library file into `out`, touching no manager state, so a library can be loaded
// for one trigger without disturbing the one the window is editing.
//
// Per-trigger flags a file may carry are handed back through `extras` rather than applied:
// which entries a trigger may use is decided by which library is assigned to it, not by
// flags stored inside the file. Only LoadProfile, which still restores a whole working set,
// has any use for them.
bool UnlimitedPlaybackManager::ParseLibraryFile(const std::string& resolvedPath,
    bool forceLoadIncompatible, PlaybackLibrary* out, LibraryFileExtras* extras) {
    if (!out || !PathExists(resolvedPath)) {
        return false;
    }
    const std::string& p = resolvedPath;

    std::ifstream in(p, std::ios::binary);
    if (!in.good()) {
        PushToast(L("Failed to open profile."));
        return false;
    }

    const CompatibilityManager::Result compatibility = ProbeProfileCompatibility(p);
    if (compatibility.action == CompatibilityManager::Action_Reject) {
        PushToast(FormatLocalized(
            "Profile compatibility rejected (file v%s, code v%s).",
            CompatibilityManager::ToString(compatibility.detected).c_str(),
            CompatibilityManager::ToString(compatibility.current).c_str()));
        return false;
    }
    if (compatibility.action == CompatibilityManager::Action_Confirm && !forceLoadIncompatible) {
        PushToast(FormatLocalized(
            "Profile version mismatch requires confirmation (file v%s, code v%s).",
            CompatibilityManager::ToString(compatibility.detected).c_str(),
            CompatibilityManager::ToString(compatibility.current).c_str()));
        return false;
    }
    std::vector<PlaybackEntry> parsedEntries;
    std::unordered_map<std::string, std::string> embeddedEntryDataHex;
    std::array<TriggerConfig, Trigger_Count> parsedTriggers = m_triggers;
    int parsedSelectionMode = EditTarget().selectionMode;
    bool parsedAutoMirror = m_autoMirrorOnSideSwap;

    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        const std::string key = Trim(line.substr(0, pos));
        const std::string value = Trim(line.substr(pos + 1));

        if (key == "mode") {
            continue;
        }
        if (key == kProfileFormatVersionKey || key == kProfileFormatKindKey || key == "version") {
            continue;
        }
        if (key == "selection_mode") {
            parsedSelectionMode = std::atoi(value.c_str());
            continue;
        }
        if (key == "auto_mirror_side_swap") {
            parsedAutoMirror = std::atoi(value.c_str()) != 0;
            continue;
        }

        if (StartsWith(key, "trigger.")) {
            auto parts = Split(key, '.');
            if (parts.size() != 3) {
                continue;
            }
            bool ok = false;
            const TriggerType t = ParseTriggerKey(parts[1], &ok);
            if (!ok) {
                continue;
            }
            if (parts[2] == "enabled") {
                parsedTriggers[t].enabled = std::atoi(value.c_str()) != 0;
            } else if (parts[2] == "cooldown") {
                parsedTriggers[t].cooldownFrames = (std::max)(1, std::atoi(value.c_str()));
            } else if (parts[2] == "key") {
                // keyCode lives in settings.ini; silently ignore legacy .key fields in profiles.
            }
            continue;
        }

        if (key == "entry") {
            auto parts = Split(value, '|');
            if (parts.size() < 5) {
                continue;
            }

            PlaybackEntry e;
            e.id = parts[0];
            e.name = parts[1];
            e.relativePath = parts[2];
            e.enabled = std::atoi(parts[3].c_str()) != 0;
            e.weight = static_cast<float>(std::atof(parts[4].c_str()));
            const size_t triggerStartIndex = 5;

            if (e.id.empty()) {
                e.id = MakeEntryId();
            }

            for (int i = 0; i < Trigger_Count; ++i) {
                const size_t idx = triggerStartIndex + static_cast<size_t>(i);
                if (idx < parts.size()) {
                    e.triggerEnabled[i] = std::atoi(parts[idx].c_str()) != 0;
                }
            }
            if (e.weight <= 0.0f) {
                e.weight = 0.01f;
            }
            parsedEntries.push_back(e);
            continue;
        }

        if (key == kEmbeddedEntryDataKey) {
            const size_t split = value.find('|');
            if (split == std::string::npos) {
                continue;
            }
            const std::string entryId = value.substr(0, split);
            const std::string encodedData = value.substr(split + 1);
            if (!entryId.empty() && !encodedData.empty()) {
                embeddedEntryDataHex[entryId] = encodedData;
            }
        }
    }

    std::unordered_map<std::string, CachedPlayback> parsedCache;
    for (const auto& e : parsedEntries) {
        const auto embeddedIt = embeddedEntryDataHex.find(e.id);
        if (embeddedIt == embeddedEntryDataHex.end()) {
            PushToast(FormatLocalized("Profile load failed: embedded playback missing for '%s'.", e.name.c_str()));
            return false;
        }

        std::vector<char> embeddedBytes;
        CachedPlayback playback;
        std::string failureReason;
        if (DecodeHex(embeddedIt->second, &embeddedBytes) &&
            ParsePlaybackBytes(embeddedBytes, &playback, forceLoadIncompatible, &failureReason)) {
            parsedCache[e.id] = playback;
            LOG(1,
                "[UP][DIAG] Parsed profile entry id='%s' name='%s' frameBytes=%u facing=%d digest=0x%08X preview='%s'\n",
                e.id.c_str(),
                e.name.c_str(),
                static_cast<unsigned int>(playback.frames.size()),
                playback.facingLeft ? 1 : 0,
                ComputePlaybackDigest(playback.frames),
                PreviewPlaybackBytes(playback.frames, 16).c_str());
        } else if (!failureReason.empty()) {
            PushToast(FormatLocalized("Profile load failed for '%s': %s", e.name.c_str(), failureReason.c_str()));
            return false;
        } else {
            PushToast(FormatLocalized("Profile load failed for '%s'.", e.name.c_str()));
            return false;
        }
    }

    out->entries = std::move(parsedEntries);
    out->cache = std::move(parsedCache);
    out->selectionMode = parsedSelectionMode;
    out->path = p;
    if (extras) {
        extras->triggers = parsedTriggers;
        extras->autoMirror = parsedAutoMirror;
        extras->valid = true;
    }
    return true;
}

bool UnlimitedPlaybackManager::LoadProfile(const std::string& profilePath, bool forceLoadIncompatible) {
    std::string p = profilePath;
    if (!IsAbsolutePath(p)) {
        p = JoinPath(GetProfileFolder(), profilePath);
    }

    if (!PathExists(p)) {
        return false;
    }

    LOG(1, "[UP][STATE] LoadProfile begin path='%s' force=%d\n", p.c_str(), forceLoadIncompatible ? 1 : 0);
    DebugLogState("LoadProfile begin");

    PlaybackLibrary parsed;
    LibraryFileExtras extras;
    if (!ParseLibraryFile(p, forceLoadIncompatible, &parsed, &extras)) {
        return false;
    }

    EditTarget() = std::move(parsed);
    if (extras.valid) {
        m_triggers = extras.triggers;
    }
    SetMode(Mode_Unlimited);
    SetSelectionMode(EditTarget().selectionMode);
    SetAutoMirrorOnSideSwap(extras.autoMirror);
    const size_t slash = p.find_last_of("/\\");
    if (slash != std::string::npos) {
        m_lastLoadedProfileFolder = p.substr(0, slash);
    } else {
        m_lastLoadedProfileFolder.clear();
    }


    ResetRuntimePlaybackState(true);
    ForceResetTriggers(L("Profile loaded. Trigger runtime synced.").c_str());
    DebugLogState("LoadProfile end");
    return true;
}

const UnlimitedPlaybackManager::PlaybackLibrary* UnlimitedPlaybackManager::LibraryForTrigger(
    TriggerType trigger) {
    const DummyActionManager::Action& action = DummyActionManager::Instance().Get(trigger);
    if (action.source != DummyActionManager::Source_Library || action.libraryPath.empty()) {
        return nullptr;
    }

    PlaybackLibrary& slot = m_triggerLibraries[trigger];

    // Load only when the assignment names a file this slot is not already holding. Editing
    // the slot without saving leaves the path alone, so an unchecked entry or a reorder is
    // not undone by a reload on the next shot.
    if (slot.path != action.libraryPath) {
        PlaybackLibrary loaded;
        if (!ParseLibraryFile(action.libraryPath, true, &loaded, nullptr)) {
            LOG(1, "[UP] Library for trigger '%s' failed to load: '%s'\n",
                TriggerDisplayName(trigger), action.libraryPath.c_str());
            return nullptr;
        }
        LOG(1, "[UP] Loaded library '%s' for trigger '%s' (%u entries)\n",
            loaded.path.c_str(), TriggerDisplayName(trigger),
            static_cast<unsigned int>(loaded.entries.size()));
        slot = std::move(loaded);
        // The action's picking order wins over whatever the file was saved with: it is what
        // the user last chose for THIS trigger, and it is the value the firing code reads.
        // Without this the modal would show the file's mode while the trigger used another.
        slot.selectionMode = action.selectionMode;
    }
    return &slot;
}

void UnlimitedPlaybackManager::PruneUnusedLibraries() {
    // A trigger that no longer draws from a library has no use for the entries it was
    // holding, and they would otherwise come back if it were pointed at a library again.
    const DummyActionManager& actions = DummyActionManager::Instance();
    for (int i = 0; i < Trigger_Count; ++i) {
        const DummyActionManager::Action& action = actions.Get(static_cast<TriggerType>(i));
        if (action.source != DummyActionManager::Source_Library
            && !m_triggerLibraries[i].entries.empty()) {
            m_triggerLibraries[i] = PlaybackLibrary{};
        }
    }
}

std::string UnlimitedPlaybackManager::GetActiveProfilePath() const {
    return EditTarget().path;
}

void UnlimitedPlaybackManager::SetActiveProfilePath(const std::string& path) {
    EditTarget().path = path;
}

std::string UnlimitedPlaybackManager::GetStatusText() const {
    return m_statusText;
}

const std::deque<UnlimitedPlaybackManager::ToastMessage>& UnlimitedPlaybackManager::GetToasts() const {
    return m_toasts;
}

void UnlimitedPlaybackManager::PruneExpiredToasts() {
    const unsigned long long now = GetTickCount64();
    for (auto it = m_toasts.begin(); it != m_toasts.end();) {
        if (!it->sticky && (now - it->createdAtMs) > it->durationMs) {
            it = m_toasts.erase(it);
        } else {
            ++it;
        }
    }
}

void UnlimitedPlaybackManager::PushToast(const std::string& text, unsigned long long durationMs) {
    ToastMessage t;
    t.text = text;
    t.durationMs = durationMs;
    t.createdAtMs = GetTickCount64();
    m_toasts.push_back(t);
    if (m_toasts.size() > 8) {
        m_toasts.pop_front();
    }
    m_statusText = text;
}

void UnlimitedPlaybackManager::PushStickyToast(const std::string& key, const std::string& text) {
    if (key.empty()) {
        return;
    }

    for (auto& toast : m_toasts) {
        if (toast.sticky && toast.key == key) {
            toast.text = text;
            toast.createdAtMs = GetTickCount64();
            toast.durationMs = 0;
            m_statusText = text;
            return;
        }
    }

    ToastMessage t;
    t.key = key;
    t.text = text;
    t.createdAtMs = GetTickCount64();
    t.durationMs = 0;
    t.sticky = true;
    m_toasts.push_back(t);
    if (m_toasts.size() > 8) {
        m_toasts.pop_front();
    }
    m_statusText = text;
}

void UnlimitedPlaybackManager::RemoveStickyToast(const std::string& key) {
    if (key.empty()) {
        return;
    }

    for (auto it = m_toasts.begin(); it != m_toasts.end(); ++it) {
        if (it->sticky && it->key == key) {
            m_toasts.erase(it);
            break;
        }
    }
}

void UnlimitedPlaybackManager::EnsureFolders() {
    EnsureDirectoryRecursive(GetProfileFolder());
}

std::string UnlimitedPlaybackManager::GetLibraryFolder() const {
    return GamePath("BBCF_IM/unlimited_playbacks/library");
}

std::string UnlimitedPlaybackManager::GetProfileFolder() const {
    return GamePath("BBCF_IM/unlimited_playbacks/profiles");
}

std::string UnlimitedPlaybackManager::MakeEntryId() {
    ++m_entrySerial;
    return "entry_" + std::to_string(static_cast<unsigned long long>(::GetTickCount64())) + "_" + std::to_string(m_entrySerial);
}

std::string UnlimitedPlaybackManager::SanitizeFileName(const std::string& input) const {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.') {
            out.push_back(c);
        } else if (c == ' ') {
            out.push_back('_');
        }
    }
    if (out.empty()) {
        out = "playback";
    }
    return out;
}

std::string UnlimitedPlaybackManager::BuildUniqueRelativePath(const std::string& preferredName) const {
    std::string base = SanitizeFileName(preferredName);
    if (base.size() > 48) {
        base.resize(48);
    }

    int suffix = 0;
    while (true) {
        std::string candidate = base;
        if (suffix > 0) {
            candidate += "_" + std::to_string(suffix);
        }
        candidate += ".playback";
        bool exists = false;
        for (size_t i = 0; i < EditTarget().entries.size(); ++i) {
            if (EditTarget().entries[i].relativePath == candidate) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            return candidate;
        }
        ++suffix;
    }
}

std::string UnlimitedPlaybackManager::EnsureEntryLibraryRelativePath(size_t idx) {
    if (idx >= EditTarget().entries.size()) {
        return "";
    }
    if (EditTarget().entries[idx].relativePath.empty() || IsAbsolutePath(EditTarget().entries[idx].relativePath)) {
        EditTarget().entries[idx].relativePath = BuildUniqueRelativePath(EditTarget().entries[idx].name.empty() ? "playback" : EditTarget().entries[idx].name);
    }
    return EditTarget().entries[idx].relativePath;
}

bool UnlimitedPlaybackManager::ReadPlaybackFile(const std::string& fullPath, CachedPlayback* out, bool forceLoadIncompatible) {
    std::ifstream file(fullPath, std::ios::binary);
    if (!file.good()) {
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size <= 1) {
        return false;
    }

    std::vector<char> data(static_cast<size_t>(size));
    file.read(&data[0], size);
    if (!file.good() && !file.eof()) {
        return false;
    }

    std::string failureReason;
    const bool ok = ParsePlaybackBytes(data, out, forceLoadIncompatible, &failureReason);
    if (!ok && !failureReason.empty()) {
        PushToast(failureReason);
    }
    return ok;
}

bool UnlimitedPlaybackManager::WritePlaybackFile(const std::string& fullPath, bool facingLeft, const std::vector<char>& frames) {
    const size_t slash = fullPath.find_last_of("/\\");
    if (slash != std::string::npos) {
        EnsureDirectoryRecursive(fullPath.substr(0, slash));
    }

    std::ofstream out(fullPath, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        return false;
    }

    const std::vector<char> serialized = SerializePlaybackBytes(facingLeft, frames);
    if (!serialized.empty()) {
        out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    }

    return out.good();
}

bool UnlimitedPlaybackManager::PickEntryIndexForTrigger(TriggerType trigger,
    const PlaybackLibrary& library, int selectionMode, size_t* outIndex) {
    std::vector<size_t> candidates = BuildCandidates(library);
    if (candidates.empty()) {
        return false;
    }
    static std::mt19937 rng(static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    if (selectionMode == Selection_Sequential) {
        const size_t pos = m_sequentialIndex[trigger] % candidates.size();
        *outIndex = candidates[pos];
        m_sequentialIndex[trigger] = (m_sequentialIndex[trigger] + 1) % candidates.size();
        return true;
    }

    auto weightedPick = [&](const std::vector<size_t>& source, size_t* picked) -> bool {
        double totalWeight = 0.0;
        for (size_t idx : source) {
            totalWeight += (double)library.entries[idx].weight;
        }
        if (totalWeight <= 0.0) {
            return false;
        }
        std::uniform_real_distribution<double> dist(0.0, totalWeight);
        const double roll = dist(rng);
        double acc = 0.0;
        for (size_t idx : source) {
            acc += (double)library.entries[idx].weight;
            if (roll <= acc) {
                *picked = idx;
                return true;
            }
        }
        *picked = source.back();
        return true;
    };

    if (selectionMode == Selection_NonRepeatingRandom) {
        auto& pool = m_nonRepeatPools[trigger];
        if (pool.empty()) {
            pool = candidates;
        } else {
            std::vector<size_t> filtered;
            for (size_t idx : pool) {
                if (std::find(candidates.begin(), candidates.end(), idx) != candidates.end()) {
                    filtered.push_back(idx);
                }
            }
            pool.swap(filtered);
            if (pool.empty()) {
                pool = candidates;
            }
        }

        size_t picked = 0;
        if (!weightedPick(pool, &picked)) {
            return false;
        }
        *outIndex = picked;
        pool.erase(std::remove(pool.begin(), pool.end(), picked), pool.end());
        return true;
    }

    return weightedPick(candidates, outIndex);
}

bool UnlimitedPlaybackManager::TryFireTrigger(TriggerType trigger, int currentFrame) {
    auto& config = m_triggers[trigger];

    // Why a trigger did not fire, logged only when the reason changes. Every one of these
    // was previously a silent return, which is why a dead trigger gave nothing to go on.
    const char* reject = nullptr;
    if (!config.enabled) {
        reject = "not armed";
    } else if ((currentFrame - config.lastTriggeredFrame) < (std::max)(1, config.cooldownFrames)) {
        reject = "cooling down";
    } else if (m_runtimeSlotBackupValid || m_runtimeSlotRestorePending) {
        reject = "a CF slot is still borrowed";
    }
    static const char* lastReject[Trigger_Count] = {};
    if (lastReject[trigger] != reject) {
        lastReject[trigger] = reject;
        LOG(1, "[UP][GATE] %s: %s\n", TriggerDisplayName(trigger),
            reject ? reject : "clear, waiting on its condition");
    }
    if (reject) {
        return false;
    }

    bool shouldFire = false;
    switch (trigger) {
    // ConsumePress, not WasPressed: this runs from a game-frame hook, which can tick more
    // than once per rendered frame, and one press must fire the trigger once.
    case Trigger_KeyPress: shouldFire = m_keyPressTriggerArmed &&
        HotkeyManager::ConsumePress(HotkeyManager::Hotkey_UnlimitedPlaybackTrigger); break;
    case Trigger_Wakeup: shouldFire = ShouldTriggerWakeup(); break;
    case Trigger_Gap: shouldFire = ShouldTriggerGap(); break;
    case Trigger_OnBlock: shouldFire = ShouldTriggerOnBlock(); break;
    case Trigger_OnHit: shouldFire = ShouldTriggerOnHit(); break;
    case Trigger_ThrowTech: shouldFire = ShouldTriggerThrowTech(); break;
    default: break;
    }

    if (!shouldFire) {
        return false;
    }

    LOG(1, "[UP] Trigger condition met: %s\n", TriggerDisplayName(trigger));
    PushToast(FormatLocalized("Trigger detected: %s", TriggerDisplayName(trigger)));

    // A delay means the condition has been met but the action is deliberately late, so the
    // cooldown starts now: the trigger must not re-detect while it is counting down.
    if (config.delayFrames > 0) {
        config.pendingFireFrame = currentFrame + config.delayFrames;
        config.lastTriggeredFrame = currentFrame;
        // Stamped at detection, so the row lights up when the condition happened rather
        // than after the delay, which is what tells you the trigger saw it at all.
        config.lastFiredMs = GetTickCount64();
        return true;
    }

    return StartResolvedAction(trigger, currentFrame);
}

void UnlimitedPlaybackManager::ProcessPendingTriggerDelays(int currentFrame) {
    for (int i = 0; i < Trigger_Count; ++i) {
        TriggerConfig& config = m_triggers[i];
        if (config.pendingFireFrame < 0) {
            continue;
        }
        // Disarmed or reconfigured mid-countdown: drop it rather than fire something the
        // trigger is no longer set to.
        if (!config.enabled) {
            config.pendingFireFrame = -1;
            continue;
        }
        if (currentFrame < config.pendingFireFrame) {
            continue;
        }
        config.pendingFireFrame = -1;
        StartResolvedAction(static_cast<TriggerType>(i), currentFrame);
    }
}

bool UnlimitedPlaybackManager::StartResolvedAction(TriggerType trigger, int currentFrame) {
    TriggerConfig& config = m_triggers[trigger];

    ResolvedAction resolved;
    if (!ResolveTriggerAction(trigger, &resolved)) {
        LOG(1, "[UP] Trigger '%s' fired but resolved to nothing: source=%d\n",
            TriggerDisplayName(trigger),
            static_cast<int>(DummyActionManager::Instance().Get(trigger).source));
        PushToast(L("Nothing to play for this trigger."));
        return false;
    }
    // Only the playback sources have frames. An animation or a burst carries a script state
    // instead, so checking for frames before the animation branch below rejected every one
    // of them - which is what made animations and burst look completely broken.
    if (!resolved.animation && resolved.frames.empty()) {
        LOG(1, "[UP] Trigger '%s' resolved to an empty frame buffer (source=%d)\n",
            TriggerDisplayName(trigger),
            static_cast<int>(DummyActionManager::Instance().Get(trigger).source));
        return false;
    }

    // An animation is not inputs: it forces the dummy's script straight to a state, so it
    // borrows no CF slot and needs no mirroring - a script state has no handedness.
    if (resolved.animation) {
        if (g_interfaces.player2.IsCharDataNullPtr()) {
            return false;
        }
        auto* p2 = g_interfaces.player2.GetData();
        if (resolved.viaActionOverride) {
            // The game reads this and performs the action itself, which is what makes a
            // burst come out of hitstun rather than being cut short by it.
            memcpy(&(p2->set_action_override), &(resolved.animation->name[0]), 20);
        }
        else {
            memcpy(&(p2->nextScriptLineLocationInMemory), &(resolved.animation->addr), 4);
            p2->frameCounterCurrentSprite = p2->frameLengthCurrentSprite2 - 1;
        }
        config.lastTriggeredFrame = currentFrame;
        config.lastFiredMs = GetTickCount64();
        LOG(1, "[UP] Trigger forced animation '%s' trigger='%s'\n",
            resolved.name.c_str(), TriggerDisplayName(trigger));
        PushToast(FormatLocalized("Triggered [%s]: %s",
            TriggerDisplayName(trigger), resolved.name.c_str()));
        return true;
    }

    // Only the playback sources borrow a slot, so the check belongs here rather than
    // blocking an animation that needs no slot at all.
    if (m_runtimeSlotBackupValid || m_runtimeSlotRestorePending) {
        return false;
    }

    // Mirroring is the game's own: it flips a buffer whose stored facing differs from the
    // side being played on. So "mirror" means leave the stored facing alone, and "do not
    // mirror" means overwrite it with the current side so it plays literally.
    int facingToLoad = resolved.facingLeft ? 1 : 0;
    bool mirrored = false;
    bool currentFacingLeft = false;
    if (TryGetCurrentFacingLeft(&currentFacingLeft) && currentFacingLeft != resolved.facingLeft) {
        mirrored = config.autoMirror;
        if (!config.autoMirror) {
            facingToLoad = currentFacingLeft ? 1 : 0;
        }
    }

    BackupRuntimeSlotIfNeeded();
    StartRuntimePlayback(resolved.frames, facingToLoad);
    m_runtimeSlotRestorePending = true;
    config.lastFiredMs = GetTickCount64();
    LOG(1, "[UP] Trigger started source='%s' name='%s' trigger='%s' frames=%u facing=%d mirrored=%d\n",
        resolved.sourceLabel,
        resolved.name.c_str(),
        TriggerDisplayName(trigger),
        static_cast<unsigned int>(resolved.frames.size()),
        facingToLoad,
        mirrored ? 1 : 0);

    config.lastTriggeredFrame = currentFrame;
    PushToast(FormatLocalized("Triggered [%s]: %s%s",
        TriggerDisplayName(trigger),
        resolved.name.c_str(),
        mirrored ? L(" (mirrored)").c_str() : ""));
    return true;
}

// Turns whatever a trigger is set to into frames the runtime can play.
//
// This is the point of folding the three old systems together: a recorded playback, a typed
// notation, a file and a CF slot all end up as the same thing - a frame buffer handed to
// StartRuntimePlayback - so only one of them needs to know anything about triggers. Animation
// is the exception and does not come through here at all: it forces a script state rather
// than feeding inputs, so it stays with the dummy-action tick that already knows how.
bool UnlimitedPlaybackManager::ResolveTriggerAction(TriggerType trigger, ResolvedAction* out) {
    if (!out) {
        return false;
    }
    const DummyActionManager::Action& action = DummyActionManager::Instance().Get(trigger);

    switch (action.source) {
    case DummyActionManager::Source_Library:
    {
        const PlaybackLibrary* library = LibraryForTrigger(trigger);
        if (!library) {
            return false;
        }
        size_t chosen = 0;
        if (!PickEntryIndexForTrigger(trigger, *library, action.selectionMode, &chosen)) {
            return false;
        }
        const PlaybackEntry& entry = library->entries[chosen];
        const auto cacheIt = library->cache.find(entry.id);
        if (cacheIt == library->cache.end() || !cacheIt->second.loaded) {
            return false;
        }
        out->frames = cacheIt->second.frames;
        out->facingLeft = cacheIt->second.facingLeft;
        out->name = entry.name;
        out->sourceLabel = "library";
        return true;
    }

    case DummyActionManager::Source_Notation:
    {
        if (action.notationFrames.empty()) {
            return false;
        }
        out->frames = action.notationFrames;
        // Notation is authored as if facing RIGHT, so "6" is toward the opponent. Reporting
        // it as facing-right is what lets the existing mirror path do its job: the game
        // flips a buffer whose recorded facing differs from the current one, which is how
        // recorded playbacks work on either side.
        //
        // Reporting the dummy's own facing here instead - which this did at first - meant
        // the facings never differed, so nothing was ever mirrored and the motion came out
        // reversed for a left-facing dummy.
        out->facingLeft = false;
        out->name = action.notation;
        out->sourceLabel = "notation";
        return true;
    }

    case DummyActionManager::Source_File:
    {
        if (action.filePath.empty()) {
            return false;
        }
        CachedPlayback playback;
        // Forced: a trigger firing mid-lab is the wrong moment to refuse over a version
        // mismatch the user already accepted when they picked the file.
        if (!ReadPlaybackFile(action.filePath, &playback, true) || !playback.loaded) {
            return false;
        }
        out->frames = playback.frames;
        out->facingLeft = playback.facingLeft;
        out->name = action.fileName;
        out->sourceLabel = "file";
        return true;
    }

    case DummyActionManager::Source_Burst:
    {
        // Replaces the old "Burst on hit" toggle. Ground and air bursts are separate states,
        // picked the same way that code picked them.
        if (g_interfaces.player2.IsCharDataNullPtr()) {
            return false;
        }
        if (g_interfaces.player2.states.empty()) {
            // Nothing has parsed the dummy's script yet. ScrWindow::DummyFeaturesInUse
            // reports burst as needing it, so this is a frame or two at match start rather
            // than a permanent state - say so once instead of silently doing nothing.
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOG(1, "[UP] Burst is armed but the dummy's script is not parsed yet.\n");
            }
            return false;
        }
        const bool airborne = g_interfaces.player2.GetData()->position_y > 0;
        const char* wanted = airborne ? "CmnActAirBurstBegin" : "CmnActBurstBegin";
        for (scrState* state : g_interfaces.player2.states) {
            if (state && state->name == wanted) {
                out->animation = state;
                out->viaActionOverride = true;
                out->name = state->name;
                out->sourceLabel = "burst";
                return true;
            }
        }
        return false;
    }

    case DummyActionManager::Source_Animation:
    {
        if (action.animations.empty()) {
            return false;
        }
        // More than one and the dummy picks at random, which is what the old panel did for
        // the four triggers it supported.
        const size_t pick = action.animations.size() == 1
            ? 0 : (static_cast<size_t>(std::rand()) % action.animations.size());
        scrState* state = action.animations[pick];
        if (!state) {
            return false;
        }
        out->animation = state;
        out->animationDelayFrames = pick < action.animationDelays.size()
            ? action.animationDelays[pick] : 0;
        out->name = state->name;
        out->sourceLabel = "animation";
        return true;
    }

    case DummyActionManager::Source_CfSlot:
    {
        if (action.cfSlot < 1 || action.cfSlot > 4) {
            return false;
        }
        std::vector<char> frames;
        bool facingLeft = false;
        // While the runtime has a slot borrowed it holds our own temporary playback, not
        // whatever the user recorded, so ask for the backup first - same order as
        // CaptureSlotToLibrary.
        if (!TryReadBorrowedSlot(action.cfSlot, &frames, &facingLeft)) {
            PlaybackSlot pslot(action.cfSlot);
            frames = pslot.get_slot_buffer_raw();
            facingLeft = pslot.get_facing_direction() != 0;
        }
        if (frames.empty()) {
            return false;
        }
        out->frames = std::move(frames);
        out->facingLeft = facingLeft;
        out->name = FormatLocalized("CF slot %d", action.cfSlot);
        out->sourceLabel = "cf slot";
        return true;
    }

    default:
        break;
    }
    return false;
}

// The loop's start/stop key. Deliberately separate from ProcessLoopTick: this has to be
// polled while the loop is NOT running, which is the whole point of a start key. It used to
// sit at the top of ProcessLoopTick, and once that was gated on the loop actually running -
// so a configured loop would stop hijacking every other trigger - the loop could no longer
// be started at all.
void UnlimitedPlaybackManager::ProcessLoopHotkey(int currentFrame) {
    if (!HotkeyManager::ConsumePress(HotkeyManager::Hotkey_UnlimitedPlaybackLoop)) {
        return;
    }
    if (m_loopActive) {
        StopLoop(L("Playback loop stopped.").c_str());
    } else {
        StartLoop(currentFrame);
    }
}

void UnlimitedPlaybackManager::ProcessLoopTick(int currentFrame) {
    if (m_loopNativeResetPulseActive) {
        NeutralizeNativeTrainingResetDirections(static_cast<LoopResetMode>(m_loopRestartMode));
        if (m_loopNativeResetHoldTicksLeft > 0) {
            --m_loopNativeResetHoldTicksLeft;
            return;
        }
        ReleaseNativeTrainingResetCombo();
        // Reset combo released; give the game time to finish the training reset (which can
        // also roll the frame counter back) before auto-capturing the position snapshot.
        m_loopPositionSetupSettleTicksLeft = kLoopNativeResetSettleFrames;
    }

    if (!m_loopActive) {
        return;
    }

    if (m_loopPhase == LoopPhase_PositionSetup) {
        ProcessLoopPositionSetup(currentFrame);
        return;
    }

    if (m_loopPhase == LoopPhase_Setup) {
        if (!m_loopRestartAppliedForCycle) {
            ApplyLoopRestart();
            if (m_loopNativeResetPulseActive) {
                return;
            }
            m_loopRestartAppliedForCycle = true;
            m_loopPhaseStartFrame = currentFrame;
        }
        if ((currentFrame - m_loopPhaseStartFrame) >= LoopSecondsToFrames(m_loopSetupSeconds)) {
            if (TryStartLoopPlayback()) {
                m_loopPhase = LoopPhase_Playing;
                m_loopPhaseStartFrame = currentFrame;
            } else {
                StopLoop(L("Playback loop stopped: no eligible playback.").c_str());
            }
        }
        return;
    }

    if (m_loopPhase == LoopPhase_Playing) {
        ObserveLoopPlaybackActionState();
        if (!m_runtimeSlotRestorePending && !m_runtimeSlotBackupValid) {
            if (m_loopPlaybackInputEndedFrame < 0) {
                m_loopPlaybackInputEndedFrame = currentFrame;
            }
            if (IsLoopPlaybackAnimationComplete(currentFrame)) {
                m_loopPhase = LoopPhase_Ending;
                m_loopPhaseStartFrame = currentFrame;
            }
        }
        return;
    }

    if (m_loopPhase == LoopPhase_Ending) {
        if ((currentFrame - m_loopPhaseStartFrame) >= LoopSecondsToFrames(m_loopEndingSeconds)) {
            m_loopPhase = LoopPhase_Setup;
            m_loopPhaseStartFrame = currentFrame;
            m_loopRestartAppliedForCycle = false;
        }
    }
}

bool UnlimitedPlaybackManager::CanBuildSnapshotApparatusHere() const {
    // See RunDeferredSetup. Building one while we are inside the game's frame update is
    // what crashed the loop; every other caller (the overlay's own draw pass, and
    // RunDeferredSetup) is fine.
    if (m_inHookTick) {
        m_loopSnapshotPrepareRequested = true;
        return false;
    }
    return true;
}

void UnlimitedPlaybackManager::RunDeferredSetup() {
    // Read from disk here rather than at construction: this is the overlay's phase, so file
    // IO is fine, and it happens before anything can fire.
    DummyActionManager::Instance().EnsureLoaded();

    const bool inTrainingMatch =
        g_gameVals.pGameMode && g_gameVals.pGameState &&
        (*g_gameVals.pGameMode == GameMode_Training) &&
        (*g_gameVals.pGameState == GameState_InMatch) &&
        !g_interfaces.player1.IsCharDataNullPtr() &&
        !g_interfaces.player2.IsCharDataNullPtr();

    if (!inTrainingMatch) {
        // Nothing to prepare, and a request must not survive to fire a loop at some
        // unrelated later moment.
        m_loopSnapshotPrepareRequested = false;
        m_loopStartRequested = false;
        return;
    }

    if (m_loopSnapshotPrepareRequested) {
        m_loopSnapshotPrepareRequested = false;
        EnsureLoopSnapshotApparatus(true);
    }

    if (m_loopStartRequested && m_loopSnapshotApparatus) {
        m_loopStartRequested = false;
        StartLoop(g_gameVals.pFrameCount ? *g_gameVals.pFrameCount : 0);
    }
}

void UnlimitedPlaybackManager::StartLoop(int currentFrame) {
    if (!TriggerHasSomethingToPlay(Trigger_OnLoop)) {
        PushToast(L("Playback loop not started: no eligible playback."));
        return;
    }
    if (m_loopRestartLabState &&
        m_loopRestartMode == LoopReset_Custom &&
        !IsLoopSnapshotReadyForMode(LoopReset_Custom)) {
        PushToast(L("Playback loop not started: custom snapshot missing."));
        return;
    }

    // The snapshot apparatus cannot be built from here. Its constructor NOPs three sites in
    // the game and calls the game's own network init, and Tick() - which is where a loop
    // hotkey arrives - runs from inside the naked asm hook GetFrameCounter(), between its
    // pushad and popad, mid-frame. Calling that init from there killed the game twice.
    //
    // So ask for it and start once it exists: RunDeferredSetup builds it from the overlay,
    // after the windows are drawn, which is the phase the training save states use and the
    // only one this has ever been safely entered from.
    if (m_loopRestartLabState && !EnsureLoopSnapshotApparatus(true)) {
        m_loopStartRequested = true;
        PushToast(L("Preparing the lab snapshot for the loop..."));
        return;
    }

    ResetRuntimePlaybackState(false);
    m_loopActive = true;
    m_loopRestartAppliedForCycle = false;
    ResetLoopPlaybackCompletionState();
    if (m_loopRestartLabState &&
        m_loopRestartMode != LoopReset_Custom &&
        !IsLoopSnapshotReadyForMode(m_loopRestartMode)) {
        BeginLoopPositionSetup(currentFrame);
    } else {
        m_loopPhase = LoopPhase_Setup;
        m_loopPhaseStartFrame = currentFrame;
    }
    PushToast(L("Playback loop started."));
}

void UnlimitedPlaybackManager::BeginLoopPositionSetup(int currentFrame) {
    ClearLoopCustomSnapshot();
    m_loopPhase = LoopPhase_PositionSetup;
    m_loopPhaseStartFrame = currentFrame;
    m_loopPositionSetupSettleTicksLeft = -1;
    PushStickyToast(kLoopPositionSetupToastKey, L("Setting up loop reset position - inputs are overridden for a moment..."));
    LOG(1, "[UP] Loop position setup started (mode=%d).\n", m_loopRestartMode);
    StartNativeTrainingResetCombo(static_cast<LoopResetMode>(m_loopRestartMode));
}

void UnlimitedPlaybackManager::ProcessLoopPositionSetup(int currentFrame) {
    if (m_loopPositionSetupSettleTicksLeft < 0) {
        // Still holding the forced reset combo; the pulse block at the top of
        // ProcessLoopTick releases it and starts the settle countdown.
        return;
    }
    if (m_loopPositionSetupSettleTicksLeft > 0) {
        --m_loopPositionSetupSettleTicksLeft;
        return;
    }

    m_loopPositionSetupSettleTicksLeft = -1;
    RemoveStickyToast(kLoopPositionSetupToastKey);
    if (!CaptureLoopSnapshotInternal()) {
        StopLoop(L("Playback loop stopped: reset position snapshot failed.").c_str());
        return;
    }
    m_loopSnapshotSourceMode = m_loopRestartMode;
    LOG(1, "[UP] Loop position setup complete (mode=%d); snapshot captured.\n", m_loopRestartMode);
    PushToast(L("Loop reset position saved."));
    // We are already sitting at the reset position, so skip this first cycle's restore.
    m_loopPhase = LoopPhase_Setup;
    m_loopPhaseStartFrame = currentFrame;
    m_loopRestartAppliedForCycle = true;
}

void UnlimitedPlaybackManager::StopLoop(const char* reason) {
    // A start still waiting for its snapshot must not fire after the user has stopped.
    m_loopStartRequested = false;
    const bool wasActive = m_loopActive;
    const bool inTrainingMatch =
        g_gameVals.pGameMode &&
        g_gameVals.pGameState &&
        (*g_gameVals.pGameMode == GameMode_Training) &&
        (*g_gameVals.pGameState == GameState_InMatch) &&
        (GetGameSceneStatus() >= GameSceneStatus_Running) &&
        !g_interfaces.player2.IsCharDataNullPtr();
    if (inTrainingMatch) {
        ResetRuntimePlaybackState(false);
    }
    m_loopActive = false;
    m_loopPhase = LoopPhase_Idle;
    m_loopPhaseStartFrame = -1;
    m_loopRestartAppliedForCycle = false;
    m_loopPositionSetupSettleTicksLeft = -1;
    RemoveStickyToast(kLoopPositionSetupToastKey);
    ResetLoopPlaybackCompletionState();
    ReleaseNativeTrainingResetCombo();
    if (wasActive && reason && reason[0] != '\0') {
        PushToast(reason);
    }
}

bool UnlimitedPlaybackManager::TryStartLoopPlayback() {
    // On Loop is a trigger like any other, so it resolves through the same path: whatever
    // the loop is set to - a library, a notation, a file, a slot - arrives here as frames.
    ResolvedAction resolved;
    if (!ResolveTriggerAction(Trigger_OnLoop, &resolved)) {
        return false;
    }

    int facingToLoad = resolved.facingLeft ? 1 : 0;
    bool mirrored = false;
    bool currentFacingLeft = false;
    if (TryGetCurrentFacingLeft(&currentFacingLeft) && currentFacingLeft != resolved.facingLeft) {
        mirrored = m_autoMirrorOnSideSwap;
        if (!m_autoMirrorOnSideSwap) {
            facingToLoad = currentFacingLeft ? 1 : 0;
        }
    }

    BackupRuntimeSlotIfNeeded();
    StartRuntimePlayback(resolved.frames, facingToLoad);
    m_runtimeSlotRestorePending = true;
    ResetLoopPlaybackCompletionState();
    PushToast(FormatLocalized("Loop played: %s%s",
        resolved.name.c_str(),
        mirrored ? L(" (mirrored)").c_str() : ""));
    return true;
}

// Cheap pre-check for "is this trigger going to be able to play anything", without doing the
// work of actually resolving it. Used before starting a loop, which is a lot of setup to go
// through only to find there was nothing to play.
bool UnlimitedPlaybackManager::TriggerHasSomethingToPlay(TriggerType trigger) {
    const DummyActionManager::Action& action = DummyActionManager::Instance().Get(trigger);
    if (action.source != DummyActionManager::Source_Library) {
        return DummyActionManager::Instance().IsRunnable(trigger);
    }
    const PlaybackLibrary* library = LibraryForTrigger(trigger);
    return library && !BuildCandidates(*library).empty();
}

void UnlimitedPlaybackManager::ResetLoopPlaybackCompletionState() {
    m_loopPlaybackObservedNonIdle = false;
    m_loopPlaybackInputEndedFrame = -1;
    m_loopPlaybackIdleSinceFrame = -1;
}

void UnlimitedPlaybackManager::ObserveLoopPlaybackActionState() {
    if (!g_interfaces.player1.IsCharDataNullPtr()) {
        const auto* p1 = g_interfaces.player1.GetData();
        if (p1 && !IsLoopCompletionIdleAction(std::string(p1->currentAction))) {
            m_loopPlaybackObservedNonIdle = true;
        }
    }

    if (!g_interfaces.player2.IsCharDataNullPtr()) {
        const auto* p2 = g_interfaces.player2.GetData();
        if (p2 && !IsLoopCompletionIdleAction(std::string(p2->currentAction))) {
            m_loopPlaybackObservedNonIdle = true;
        }
    }
}

bool UnlimitedPlaybackManager::IsLoopPlaybackAnimationComplete(int currentFrame) {
    if (m_loopPlaybackInputEndedFrame < 0) {
        return false;
    }

    bool anyPlayerAvailable = false;
    bool allAvailablePlayersIdle = true;

    if (!g_interfaces.player1.IsCharDataNullPtr()) {
        const auto* p1 = g_interfaces.player1.GetData();
        if (p1) {
            anyPlayerAvailable = true;
            const bool p1Idle = IsLoopCompletionIdleAction(std::string(p1->currentAction));
            if (!p1Idle) {
                m_loopPlaybackObservedNonIdle = true;
                allAvailablePlayersIdle = false;
            }
        }
    }

    if (!g_interfaces.player2.IsCharDataNullPtr()) {
        const auto* p2 = g_interfaces.player2.GetData();
        if (p2) {
            anyPlayerAvailable = true;
            const bool p2Idle = IsLoopCompletionIdleAction(std::string(p2->currentAction));
            if (!p2Idle) {
                m_loopPlaybackObservedNonIdle = true;
                allAvailablePlayersIdle = false;
            }
        }
    }

    if (!anyPlayerAvailable) {
        return true;
    }

    if (!allAvailablePlayersIdle) {
        m_loopPlaybackIdleSinceFrame = -1;
        return false;
    }

    const int framesSinceInputEnd = currentFrame - m_loopPlaybackInputEndedFrame;
    if (framesSinceInputEnd < kLoopCompletionMinimumPostInputFrames) {
        return false;
    }

    if (m_loopPlaybackIdleSinceFrame < 0) {
        m_loopPlaybackIdleSinceFrame = currentFrame;
    }
    const int stableIdleFrames = currentFrame - m_loopPlaybackIdleSinceFrame;
    if (m_loopPlaybackObservedNonIdle) {
        return stableIdleFrames >= kLoopCompletionIdleStableFrames;
    }

    // If the slot never drove either player into a non-idle action, do not hang the loop forever.
    return framesSinceInputEnd >= kLoopCompletionNoActionFallbackFrames;
}

bool UnlimitedPlaybackManager::EnsureLoopSnapshotApparatus(bool preserveCustomSnapshot) {
    if (g_interfaces.player1.IsCharDataNullPtr() || g_interfaces.player2.IsCharDataNullPtr()) {
        if (!preserveCustomSnapshot) {
            ClearLoopCustomSnapshot();
        }
        return false;
    }

    if (!m_loopSnapshotApparatus) {
        if (!CanBuildSnapshotApparatusHere()) {
            return false;
        }
        m_loopSnapshotApparatus = new SnapshotApparatus();
        m_loopSnapshotApparatus->ReserveSlots("playback_loop", 1);
        return m_loopSnapshotApparatus != nullptr;
    }

    if (!m_loopSnapshotApparatus->check_if_valid(g_interfaces.player1.GetData(), g_interfaces.player2.GetData())) {
        if (!CanBuildSnapshotApparatusHere()) {
            return false;
        }
        delete m_loopSnapshotApparatus;
        m_loopSnapshotApparatus = new SnapshotApparatus();
        m_loopSnapshotApparatus->ReserveSlots("playback_loop", 1);
        if (!preserveCustomSnapshot) {
            ClearLoopCustomSnapshot();
        }
    }
    return m_loopSnapshotApparatus != nullptr;
}

bool UnlimitedPlaybackManager::CaptureLoopSnapshotInternal() {
    if (!EnsureLoopSnapshotApparatus()) {
        return false;
    }

    const bool nativeOk = m_loopSnapshotApparatus->save_snapshot(nullptr);
    if (!nativeOk) {
        ClearLoopCustomSnapshot();
        return false;
    }
    m_loopCustomSnapshotSlotIndex = m_loopSnapshotApparatus->last_saved_slot();
    m_loopCustomSnapshotSize = m_loopSnapshotApparatus->get_last_saved_snapshot_size();

    m_loopCustomSnapshotBytes.clear();
    return true;
}

bool UnlimitedPlaybackManager::CaptureLoopCustomSnapshot() {
    InitializeIfNeeded();
    if (g_interfaces.player1.IsCharDataNullPtr() || g_interfaces.player2.IsCharDataNullPtr()) {
        PushToast(L("Custom loop snapshot failed: lab state unavailable."));
        return false;
    }
    if (!CaptureLoopSnapshotInternal()) {
        PushToast(L("Custom loop snapshot failed."));
        return false;
    }
    m_loopSnapshotSourceMode = LoopReset_Custom;

    PushToast(L("Custom loop snapshot captured for this lab session."));
    return true;
}

bool UnlimitedPlaybackManager::LoadLoopCustomSnapshot() {
    InitializeIfNeeded();
    return RestoreLoopCustomSnapshot(true);
}

void UnlimitedPlaybackManager::ClearLoopCustomSnapshot() {
    m_loopCustomSnapshotBytes.clear();
    m_loopCustomSnapshotSize = 0;
    m_loopCustomSnapshotSlotIndex = -1;
    m_loopSnapshotSourceMode = -1;
}

bool UnlimitedPlaybackManager::RestoreLoopCustomSnapshot(bool showToast) {
    if (!HasLoopCustomSnapshot() || !EnsureLoopSnapshotApparatus(true)) {
        if (showToast) {
            PushToast(L("Custom loop snapshot missing."));
        }
        return false;
    }

    if (m_loopCustomSnapshotSlotIndex >= 0) {
        const bool ok = m_loopSnapshotApparatus->load_snapshot_index(m_loopCustomSnapshotSlotIndex);
        if (showToast) {
            PushToast(ok ? L("Custom loop snapshot loaded.") : L("Custom loop snapshot load failed."));
        }
        if (ok) {
            return true;
        }
    }

    if (!m_loopCustomSnapshotBytes.empty() && m_loopCustomSnapshotSize > 0) {
        const bool ok = m_loopSnapshotApparatus->load_snapshot_sized(
            m_loopCustomSnapshotBytes.data(),
            static_cast<size_t>(m_loopCustomSnapshotSize));
        if (showToast) {
            PushToast(ok ? L("Custom loop snapshot loaded.") : L("Custom loop snapshot load failed."));
        }
        return ok;
    }

    if (showToast) {
        PushToast(L("Custom loop snapshot load failed."));
    }
    return false;
}

bool UnlimitedPlaybackManager::ApplyLoopRestart() {
    if (!m_loopRestartLabState) {
        return true;
    }

    const bool ok = RestoreLoopCustomSnapshot(false);
    if (!ok && HasLoopCustomSnapshot()) {
        PushToast(L("Custom loop snapshot load failed."));
    }
    return ok;
}

void UnlimitedPlaybackManager::StartNativeTrainingResetCombo(LoopResetMode mode) {
    ReleaseNativeTrainingResetCombo();
    CaptureNativeTrainingResetInputSnapshot(mode);
    NeutralizeNativeTrainingResetDirections(mode);

    WORD directionVk = 0;
    WORD alternateDirectionVk = 0;
    if (mode == LoopReset_Left) {
        directionVk = VK_LEFT;
        alternateDirectionVk = 'A';
    } else if (mode == LoopReset_Right) {
        directionVk = VK_RIGHT;
        alternateDirectionVk = 'D';
    } else {
        directionVk = VK_DOWN;
        alternateDirectionVk = 'S';
    }

    m_loopNativeResetKeys = { directionVk, alternateDirectionVk, VK_BACK };
    SendNativeTrainingResetKey(directionVk, true);
    SendNativeTrainingResetKey(alternateDirectionVk, true);
    SendNativeTrainingResetKey(VK_BACK, true);
    m_loopNativeResetPulseActive = true;
    m_loopNativeResetHoldTicksLeft = kLoopNativeResetHoldTicks;
}

void UnlimitedPlaybackManager::CaptureNativeTrainingResetInputSnapshot(LoopResetMode mode) {
    static const std::array<WORD, 8> directionKeys = {
        VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, 'A', 'D', 'W', 'S',
    };

    WORD keepPrimary = VK_DOWN;
    WORD keepAlternate = 'S';
    if (mode == LoopReset_Left) {
        keepPrimary = VK_LEFT;
        keepAlternate = 'A';
    } else if (mode == LoopReset_Right) {
        keepPrimary = VK_RIGHT;
        keepAlternate = 'D';
    }

    m_loopNativeResetRestoreKeys.clear();
    for (WORD key : directionKeys) {
        if (key == keepPrimary || key == keepAlternate) {
            continue;
        }
        if ((GetAsyncKeyState(key) & 0x8000) != 0) {
            m_loopNativeResetRestoreKeys.push_back(key);
        }
    }
}

void UnlimitedPlaybackManager::NeutralizeNativeTrainingResetDirections(LoopResetMode mode) {
    static const std::array<WORD, 8> directionKeys = {
        VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, 'A', 'D', 'W', 'S',
    };

    WORD keepPrimary = VK_DOWN;
    WORD keepAlternate = 'S';
    if (mode == LoopReset_Left) {
        keepPrimary = VK_LEFT;
        keepAlternate = 'A';
    } else if (mode == LoopReset_Right) {
        keepPrimary = VK_RIGHT;
        keepAlternate = 'D';
    }

    for (WORD key : directionKeys) {
        if (key != keepPrimary && key != keepAlternate) {
            SendNativeTrainingResetKey(key, false);
        }
    }
}

void UnlimitedPlaybackManager::ReleaseNativeTrainingResetCombo() {
    if (!m_loopNativeResetPulseActive) {
        return;
    }

    SendNativeTrainingResetKey(m_loopNativeResetKeys[2], false);
    SendNativeTrainingResetKey(m_loopNativeResetKeys[1], false);
    SendNativeTrainingResetKey(m_loopNativeResetKeys[0], false);
    for (WORD key : m_loopNativeResetRestoreKeys) {
        SendNativeTrainingResetKey(key, true);
    }
    m_loopNativeResetKeys = {};
    m_loopNativeResetRestoreKeys.clear();
    m_loopNativeResetPulseActive = false;
    m_loopNativeResetHoldTicksLeft = 0;
}

void UnlimitedPlaybackManager::SendNativeTrainingResetKey(WORD virtualKey, bool keyDown) const {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyA(virtualKey, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (keyDown ? 0 : KEYEVENTF_KEYUP);
    if (virtualKey == VK_LEFT || virtualKey == VK_RIGHT || virtualKey == VK_UP || virtualKey == VK_DOWN) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    SendInput(1, &input, sizeof(INPUT));
}

int UnlimitedPlaybackManager::LoopSecondsToFrames(float seconds) const {
    if (seconds <= 0.0f) {
        return 0;
    }
    return static_cast<int>(std::ceil(seconds * 60.0f));
}

void UnlimitedPlaybackManager::BackupRuntimeSlotIfNeeded() {
    if (m_runtimeSlotRestorePending || m_runtimeSlotBackupValid) {
        return;
    }

    m_runtimeActiveSlotBackupValid = false;
    m_runtimeActiveSlotBackup = 0;
    if (m_runtimePlaybackManager.active_slot_p) {
        m_runtimeActiveSlotBackup = *reinterpret_cast<int*>(m_runtimePlaybackManager.active_slot_p);
        m_runtimeActiveSlotBackupValid = true;
    }
    m_runtimePlaybackTypeBackupValid = false;
    m_runtimePlaybackTypeBackup = 0;
    if (m_runtimePlaybackManager.bbcf_base_adress) {
        m_runtimePlaybackTypeBackup =
            static_cast<int>(*(m_runtimePlaybackManager.bbcf_base_adress + 0x902BDC + 0x54 + 0xc + 0x4));
        m_runtimePlaybackTypeBackupValid = true;
    }
    m_runtimeSlotNumber = kDedicatedRuntimePlaybackSlot;
    {
        PlaybackSlot pslot(kDedicatedRuntimePlaybackSlot);
        m_runtimeSlotBackupFrames = pslot.get_slot_buffer_raw();
        m_runtimeSlotBackupFacingLeft = pslot.get_facing_direction() != 0;
    }
    m_runtimeSlotBackupValid = true;
}

int UnlimitedPlaybackManager::GetBorrowedCfSlot() const {
    if (!m_runtimeSlotBackupValid && !m_runtimeSlotRestorePending) {
        return 0;
    }
    return m_runtimeSlotNumber;
}

bool UnlimitedPlaybackManager::AbsorbExternalSlotWrite(int slot, const std::vector<char>& trimmedFrames, bool facingLeft) {
    std::vector<char> clampedFrames = trimmedFrames;
    if (clampedFrames.size() > static_cast<size_t>(kMaxFramesPerPlayback)) {
        clampedFrames.resize(kMaxFramesPerPlayback);
    }
    return AbsorbExternalSlotWriteRaw(slot, ExpandPlaybackBytes(clampedFrames), facingLeft);
}

bool UnlimitedPlaybackManager::AbsorbExternalSlotWriteRaw(int slot, const std::vector<char>& rawFrames, bool facingLeft) {
    if (!m_runtimeSlotBackupValid || slot != m_runtimeSlotNumber) {
        return false;
    }

    std::vector<char> clampedFrames = rawFrames;
    if (clampedFrames.size() > static_cast<size_t>(kMaxFramesPerPlayback) * 2) {
        clampedFrames.resize(static_cast<size_t>(kMaxFramesPerPlayback) * 2);
    }

    // The pending restore is what the slot will end up holding, so the write has to land
    // there too -- otherwise the restore overwrites it and the save looks like a no-op.
    m_runtimeSlotBackupFrames = clampedFrames;
    m_runtimeSlotBackupFacingLeft = facingLeft;
    LOG(1, "[UP] Absorbed external write to borrowed slot %d into pending restore. rawBytes=%u facing=%d\n",
        slot,
        static_cast<unsigned int>(clampedFrames.size()),
        facingLeft ? 1 : 0);
    return true;
}

bool UnlimitedPlaybackManager::ReadBorrowedCfSlot(int slot, std::vector<char>* outTrimmedFrames, char* outFacing) const {
    std::vector<char> rawFrames;
    bool facingLeft = false;
    if (!outTrimmedFrames || !outFacing || !TryReadBorrowedSlot(slot, &rawFrames, &facingLeft)) {
        return false;
    }
    *outTrimmedFrames = CompactPlaybackBytes(rawFrames);
    *outFacing = facingLeft ? 1 : 0;
    return true;
}

bool UnlimitedPlaybackManager::TryReadBorrowedSlot(int slot, std::vector<char>* outRawFrames, bool* outFacingLeft) const {
    if (!m_runtimeSlotBackupValid || slot != m_runtimeSlotNumber || !outRawFrames || !outFacingLeft) {
        return false;
    }
    *outRawFrames = m_runtimeSlotBackupFrames;
    *outFacingLeft = m_runtimeSlotBackupFacingLeft;
    return true;
}

void UnlimitedPlaybackManager::TryRestoreRuntimeSlotAfterPlayback() {
    if (!m_runtimeSlotRestorePending || !m_runtimeSlotBackupValid) {
        return;
    }

    int playbackControl = 0;
    if (!m_runtimePlaybackManager.playback_control_p) {
        return;
    }
    std::memcpy(&playbackControl, m_runtimePlaybackManager.playback_control_p, sizeof(short));
    if (playbackControl == 3) {
        return;
    }

    LOG(1, "[UP] Cleared runtime playback borrow after playback. backupSlot=%d backupFrames=%u backupFacing=%d\n",
        m_runtimeActiveSlotBackupValid ? (m_runtimeActiveSlotBackup + 1) : -1,
        static_cast<unsigned int>(m_runtimeSlotBackupFrames.size()),
        m_runtimeSlotBackupFacingLeft ? 1 : 0);
    if (m_runtimePlaybackManager.playback_control_p) {
        m_runtimePlaybackManager.set_playback_control(0);
    }
    m_runtimePlaybackManager.set_playback_position(0);
    m_runtimePlaybackManager.load_raw_into_slot(
        m_runtimeSlotBackupFrames,
        m_runtimeSlotBackupFacingLeft ? 1 : 0,
        m_runtimeSlotNumber);
    if (m_runtimeActiveSlotBackupValid) {
        const int restoredSlot = m_runtimeActiveSlotBackup + 1;
        if (restoredSlot >= 1 && restoredSlot <= 4) {
            m_runtimePlaybackManager.set_active_slot(restoredSlot);
        }
    }
    if (m_runtimePlaybackTypeBackupValid) {
        m_runtimePlaybackManager.set_playback_type(m_runtimePlaybackTypeBackup);
    }
    m_runtimeSlotBackupFrames.clear();
    m_runtimeSlotBackupValid = false;
    m_runtimeSlotRestorePending = false;
    m_runtimeSlotNumber = 1;
    m_runtimeActiveSlotBackupValid = false;
    m_runtimeActiveSlotBackup = 0;
    m_runtimePlaybackTypeBackupValid = false;
    m_runtimePlaybackTypeBackup = 0;
}

void UnlimitedPlaybackManager::ResetRuntimePlaybackState(bool discardBackupOnly) {
    if (!discardBackupOnly && m_runtimePlaybackManager.playback_control_p) {
        m_runtimePlaybackManager.set_playback_control(0);
    }
    if (!discardBackupOnly) {
        m_runtimePlaybackManager.set_playback_position(0);
    }
    if (!discardBackupOnly && m_runtimeSlotBackupValid) {
        m_runtimePlaybackManager.load_raw_into_slot(
            m_runtimeSlotBackupFrames,
            m_runtimeSlotBackupFacingLeft ? 1 : 0,
            m_runtimeSlotNumber);
        if (m_runtimeActiveSlotBackupValid) {
            const int restoredSlot = m_runtimeActiveSlotBackup + 1;
            if (restoredSlot >= 1 && restoredSlot <= 4) {
                m_runtimePlaybackManager.set_active_slot(restoredSlot);
            }
        }
        if (m_runtimePlaybackTypeBackupValid) {
            m_runtimePlaybackManager.set_playback_type(m_runtimePlaybackTypeBackup);
        }
        LOG(1, "[UP] Restored runtime slot during cleanup. frames=%u facing=%d\n",
            static_cast<unsigned int>(m_runtimeSlotBackupFrames.size()),
            m_runtimeSlotBackupFacingLeft ? 1 : 0);
    }
    m_runtimeSlotBackupFrames.clear();
    m_runtimeSlotBackupValid = false;
    m_runtimeSlotRestorePending = false;
    m_runtimeSlotNumber = 1;
    m_runtimeActiveSlotBackupValid = false;
    m_runtimeActiveSlotBackup = 0;
    m_runtimePlaybackTypeBackupValid = false;
    m_runtimePlaybackTypeBackup = 0;
}

void UnlimitedPlaybackManager::StartRuntimePlayback(const std::vector<char>& frames, int facingToLoad) {
    int beforeControl = -1;
    int beforeActiveSlot = -1;
    int beforePosition = -1;
    if (m_runtimePlaybackManager.playback_control_p) {
        beforeControl = static_cast<int>(*reinterpret_cast<short*>(m_runtimePlaybackManager.playback_control_p));
    }
    if (m_runtimePlaybackManager.active_slot_p) {
        beforeActiveSlot = *reinterpret_cast<int*>(m_runtimePlaybackManager.active_slot_p);
    }
    if (m_runtimePlaybackManager.bbcf_base_adress) {
        beforePosition = *reinterpret_cast<int*>(m_runtimePlaybackManager.bbcf_base_adress + 0x13AD940);
    }
    // Bytes and frames both, because they are not the same number and confusing the two is
    // exactly what made a notation action play for a single frame: the slot layout is two
    // bytes per frame, so load_raw_into_slot takes the count as size() / 2.
    LOG(1, "[UP] StartRuntimePlayback before: pbCtrl=%d activeSlot=%d pbPos=%d facing=%d bytes=%u frames=%u\n",
        beforeControl,
        beforeActiveSlot,
        beforePosition,
        facingToLoad,
        static_cast<unsigned int>(frames.size()),
        static_cast<unsigned int>(frames.size() / 2));

    // The buffer itself, so a playback that comes out wrong can be checked against what the
    // notation or recording was supposed to be, rather than inferred from the result.
    {
        std::string preview;
        const size_t shown = frames.size() < 40 ? frames.size() : 40;
        for (size_t i = 0; (i + 1) < shown; i += 2) {
            char one[16];
            sprintf_s(one, "%s%u", preview.empty() ? "" : " ",
                static_cast<unsigned int>(static_cast<unsigned char>(frames[i])));
            preview += one;
        }
        LOG(1, "[UP] StartRuntimePlayback inputs: %s%s\n", preview.c_str(),
            frames.size() > shown ? " ..." : "");
    }

    m_runtimePlaybackManager.set_playback_control(0);
    m_runtimePlaybackManager.load_raw_into_slot(frames, facingToLoad, m_runtimeSlotNumber);
    m_runtimePlaybackManager.set_active_slot(m_runtimeSlotNumber);
    m_runtimePlaybackManager.set_playback_type(0);
    // Position must be written AFTER the control transition to 3 (set_playback_control() calls
    // the native training-state setter, which appears to touch the playback cursor as part of
    // entering state 3).
    //
    // Writing -1, not 0: measured via live logging (see docs/Research - UnlimitedPlaybackManager
    // diagnostics) that starting at position 0 makes the native per-tick consumer read frame 1 as
    // its first applied input, not frame 0 - a consistent, constant one-index offset for the
    // entire playback (not a one-time startup delay; a real recorded frame is silently never
    // applied). This is consistent with the consumer using pre-increment semantics (increment the
    // cursor, THEN read at the new value) - so it needs to start one below the first real index.
    // Native training-menu-triggered playback does not exhibit this, implying its own reset state
    // already sits at -1 before the consumer's first increment.
    m_runtimePlaybackManager.set_playback_control(3);
    m_runtimePlaybackManager.set_playback_position(-1);

    int afterControl = -1;
    int afterActiveSlot = -1;
    int afterPosition = -1;
    if (m_runtimePlaybackManager.playback_control_p) {
        afterControl = static_cast<int>(*reinterpret_cast<short*>(m_runtimePlaybackManager.playback_control_p));
    }
    if (m_runtimePlaybackManager.active_slot_p) {
        afterActiveSlot = *reinterpret_cast<int*>(m_runtimePlaybackManager.active_slot_p);
    }
    if (m_runtimePlaybackManager.bbcf_base_adress) {
        afterPosition = *reinterpret_cast<int*>(m_runtimePlaybackManager.bbcf_base_adress + 0x13AD940);
    }
    LOG(1, "[UP] StartRuntimePlayback after: pbCtrl=%d activeSlot=%d pbPos=%d\n",
        afterControl,
        afterActiveSlot,
        afterPosition);
}

bool UnlimitedPlaybackManager::TryGetCurrentFacingLeft(bool* outFacingLeft) const {
    if (!outFacingLeft || g_interfaces.player2.IsCharDataNullPtr()) {
        return false;
    }

    const auto* p2 = g_interfaces.player2.GetData();
    if (!p2) {
        return false;
    }

    int facing = p2->facingLeft2;
    if (facing != 0 && facing != 1) {
        facing = p2->facingLeft;
    }

    *outFacingLeft = facing != 0;
    return true;
}

unsigned char UnlimitedPlaybackManager::MirrorDirectionalNibble(unsigned char dir) const {
    switch (dir) {
    case 1: return 3;
    case 3: return 1;
    case 4: return 6;
    case 6: return 4;
    case 7: return 9;
    case 9: return 7;
    default: return dir;
    }
}

void UnlimitedPlaybackManager::MirrorPlaybackInputsInPlace(std::vector<char>& frames) const {
    for (size_t i = 0; (i + 1) < frames.size(); i += 2) {
        unsigned char input = static_cast<unsigned char>(frames[i]);
        const unsigned char dir = input & 0x0F;
        const unsigned char mirroredDir = MirrorDirectionalNibble(dir);
        input = static_cast<unsigned char>((input & 0xF0) | mirroredDir);
        frames[i] = static_cast<char>(input);
    }
}

std::vector<size_t> UnlimitedPlaybackManager::BuildCandidates(const PlaybackLibrary& library) {
    // PlaybackEntry::triggerEnabled is deliberately not consulted. Which entries a trigger
    // may use is now decided by which library it names, so filtering again by a flag inside
    // the entry would silently hide entries from a library that was picked on purpose.
    std::vector<size_t> candidates;
    for (size_t i = 0; i < library.entries.size(); ++i) {
        const auto& e = library.entries[i];
        if (!e.enabled || e.weight <= 0.0f) {
            continue;
        }
        auto cacheIt = library.cache.find(e.id);
        if (cacheIt == library.cache.end() || !cacheIt->second.loaded) {
            continue;
        }
        candidates.push_back(i);
    }
    return candidates;
}

bool UnlimitedPlaybackManager::ShouldTriggerWakeup() {
    if (g_interfaces.player2.IsCharDataNullPtr()) {
        return false;
    }

    const auto* p2 = g_interfaces.player2.GetData();
    const std::string currentAction = p2->currentAction;
    const std::string lastAction = p2->lastAction;
    const int actionTime = p2->actionTime;

    // Taken verbatim from the old dummy-actions panel, including its notes: these frame
    // numbers were arrived at by testing, and CmnActUkemiLandN is deliberately absent in
    // favour of its Landing state.
    static const std::array<std::pair<const char*, int>, 6> wakeupStates = {
        std::make_pair("CmnActUkemiLandNLanding", 1),
        std::make_pair("CmnActUkemiLandF", 30),
        std::make_pair("CmnActUkemiLandB", 30),
        std::make_pair("CmnActFDown2Stand", 14),
        std::make_pair("CmnActBDown2Stand", 14),
        // Really an on-hit case, but it behaves like a wakeup, so it lived here.
        std::make_pair("CmnActUkemiStagger", 7),
    };

    // actionTime counts UP to the frame the state becomes actionable, so firing early means
    // a SMALLER value - the same trick the block gap and throw tech use. A 623C needs its
    // motion delivered before the dummy can act or the reversal comes out a few frames late.
    // Clamped at 1 because some of these states become actionable on frame 1 already, and
    // there is nothing earlier than that to ask for.
    const int lead = (std::max)(0, -m_triggers[Trigger_Wakeup].delayFrames);

    bool cond = false;
    for (const auto& ws : wakeupStates) {
        // Exact equality, and lastAction must differ from the state we matched.
        //
        // That last test is not decoration: forcing an animation moves the script pointer
        // without changing currentAction, so actionTime restarts and walks past the same
        // number again - and the trigger fired over and over, "permanently triggering on
        // loop" after the move came out. Substring matching made it worse by matching
        // several states at once. Both were mine; the panel this replaced had it right.
        const int wantTime = (std::max)(1, ws.second - lead);
        if (currentAction == ws.first && actionTime == wantTime && lastAction != ws.first) {
            cond = true;
            break;
        }
    }

    const bool edge = (cond && !m_prevWakeupCondition);
    m_prevWakeupCondition = cond;
    return edge;
}

bool UnlimitedPlaybackManager::ShouldTriggerGap() {
    if (g_interfaces.player2.IsCharDataNullPtr()) {
        return false;
    }

    // The moment blocking ENDS. blockstun counts down, so 1 is its last frame - acting on
    // that frame is what comes out on the first actionable one. Waiting for it to reach 0
    // costs a frame, because the guard state can skip its own end state on a frame-1 mash.
    //
    // A negative delay pulls that earlier still: a reversal needs its motion delivered
    // BEFORE the gap so the button lands on the first actionable frame rather than several
    // after it. Only possible because blockstun is a countdown, i.e. the gap is knowable in
    // advance - which is why a negative delay means nothing on the other triggers.
    const auto* p2 = g_interfaces.player2.GetData();
    const std::string currentAction = p2->currentAction;
    const int lead = (std::max)(0, -m_triggers[Trigger_Gap].delayFrames);
    const bool cond = (p2->blockstun == 1 + lead && currentAction.find("Guard") != std::string::npos);

    const bool edge = (cond && !m_prevGapCondition);
    m_prevGapCondition = cond;
    return edge;
}

bool UnlimitedPlaybackManager::ShouldTriggerOnBlock() {
    if (g_interfaces.player2.IsCharDataNullPtr()) {
        return false;
    }

    // The moment the dummy STARTS blocking - the rising edge of blockstun.
    //
    // This used to fire on the falling edge, i.e. as blockstun ended, which is the gap
    // trigger's job: the two were a frame apart and did the same thing, and the comment
    // here described gap behaviour. "On Block" now means what it says.
    const auto* p2 = g_interfaces.player2.GetData();
    const bool cond = p2->blockstun > 0;

    const bool edge = (cond && !m_prevOnBlockCondition);
    m_prevOnBlockCondition = cond;
    return edge;
}

bool UnlimitedPlaybackManager::ShouldTriggerOnHit() {
    if (g_interfaces.player2.IsCharDataNullPtr()) {
        return false;
    }

    const auto* p2 = g_interfaces.player2.GetData();
    const bool cond = p2->hitstun > 0;

    // Which edge depends on how the action is delivered, and getting this wrong is what made
    // "burst on hit" fire at the end of a combo instead of straight away.
    //
    // A burst is an action override: the game is handed the state name and comes out with it
    // as soon as bursting is legal, so it wants the moment the dummy is FIRST hit - the
    // rising edge - exactly as the old burst-on-hit toggle did. That is what makes it break
    // the combo rather than wait politely for the end of it.
    //
    // Input playback is the opposite: inputs fed during hitstun are simply eaten, so it has
    // to wait for hitstun to end (the falling edge) to come out at all.
    // "On Hit" means the moment the dummy IS hit, for every source - not once the combo is
    // over. Typing an overdrive into it is asking for the same thing a burst is, and the
    // reversal it lets you lab is the one that comes out of hitstun, which needs the inputs
    // delivered while the dummy is still in it so the game buffers them.
    //
    // This is the shape the old burst-on-hit toggle used, and it is a LEVEL test with a
    // latch rather than an edge: a hit landing on a dummy that is already in hitstun still
    // arms the action, which an edge test would miss entirely.
    const std::string currentAction = p2->currentAction;
    const bool alreadyBursting = currentAction.find("CmnActBurst") != std::string::npos;
    const bool teching = currentAction.find("CmnActUkemi") != std::string::npos;

    if (!cond) {
        // Out of hitstun: re-arm for the next combo. The original used a 700-frame cooldown,
        // which both allowed a second go inside one long combo and blocked the next combo.
        m_onHitBurstLatched = false;
    }
    m_prevOnHitCondition = cond;

    if (!cond || alreadyBursting || teching || m_onHitBurstLatched) {
        return false;
    }
    m_onHitBurstLatched = true;
    return true;
}

bool UnlimitedPlaybackManager::ShouldTriggerThrowTech() {
    if (g_interfaces.player2.IsCharDataNullPtr()) {
        return false;
    }

    // timeAfterTechIsPerformed counts up from the tech, and 29 is the frame acting on it
    // comes out. A negative delay therefore means a SMALLER value: same idea as the block
    // gap, so a reversal's motion can be delivered before the dummy is actionable instead
    // of a couple of frames into it.
    const auto* p2 = g_interfaces.player2.GetData();
    const std::string currentAction = p2->currentAction;
    const int lead = (std::max)(0, -m_triggers[Trigger_ThrowTech].delayFrames);
    const int wantTime = (std::max)(1, 29 - lead);
    const bool cond = (p2->timeAfterTechIsPerformed == wantTime
        && currentAction.find("LockReject") != std::string::npos);

    const bool edge = (cond && !m_prevThrowTechCondition);
    m_prevThrowTechCondition = cond;
    return edge;
}
