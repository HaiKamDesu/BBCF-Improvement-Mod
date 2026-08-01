#include "UnlimitedPlaybackManager.h"

#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Core/XInputRuntime.h"
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
constexpr int kControllerBindBase = 0x1000;

struct ControllerBindingDef {
    int code;
    WORD mask;         // 0 for analog triggers (L2/R2)
    bool isLeftTrigger;
    bool isRightTrigger;
};

const ControllerBindingDef kControllerBindings[] = {
    { kControllerBindBase + 0,  XINPUT_GAMEPAD_A,              false, false },
    { kControllerBindBase + 1,  XINPUT_GAMEPAD_B,              false, false },
    { kControllerBindBase + 2,  XINPUT_GAMEPAD_X,              false, false },
    { kControllerBindBase + 3,  XINPUT_GAMEPAD_Y,              false, false },
    { kControllerBindBase + 4,  XINPUT_GAMEPAD_LEFT_SHOULDER,  false, false },
    { kControllerBindBase + 5,  XINPUT_GAMEPAD_RIGHT_SHOULDER, false, false },
    { kControllerBindBase + 6,  XINPUT_GAMEPAD_BACK,           false, false },
    { kControllerBindBase + 7,  XINPUT_GAMEPAD_START,          false, false },
    { kControllerBindBase + 8,  XINPUT_GAMEPAD_LEFT_THUMB,     false, false },
    { kControllerBindBase + 9,  XINPUT_GAMEPAD_RIGHT_THUMB,    false, false },
    { kControllerBindBase + 10, XINPUT_GAMEPAD_DPAD_UP,        false, false },
    { kControllerBindBase + 11, XINPUT_GAMEPAD_DPAD_DOWN,      false, false },
    { kControllerBindBase + 12, XINPUT_GAMEPAD_DPAD_LEFT,      false, false },
    { kControllerBindBase + 13, XINPUT_GAMEPAD_DPAD_RIGHT,     false, false },
    { kControllerBindBase + 14, 0,                             true,  false }, // L2 / LT
    { kControllerBindBase + 15, 0,                             false, true  }, // R2 / RT
};

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

bool IsControllerBindCode(int code) {
    return code >= kControllerBindBase &&
        code < (kControllerBindBase + static_cast<int>(sizeof(kControllerBindings) / sizeof(kControllerBindings[0])));
}

bool IsGameWindowFocused() {
    return g_gameProc.hWndGameWindow && GetForegroundWindow() == g_gameProc.hWndGameWindow;
}

bool IsControllerBindingDown(const ControllerBindingDef& binding) {
    for (DWORD userIndex = 0; userIndex < XUSER_MAX_COUNT; ++userIndex) {
        XINPUT_STATE state = {};
        if (XInputRuntime::GetState(userIndex, &state) != ERROR_SUCCESS) {
            continue;
        }
        if (binding.isLeftTrigger) {
            if (state.Gamepad.bLeftTrigger > 128) return true;
        } else if (binding.isRightTrigger) {
            if (state.Gamepad.bRightTrigger > 128) return true;
        } else if ((state.Gamepad.wButtons & binding.mask) != 0) {
            return true;
        }
    }
    return false;
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

UnlimitedPlaybackManager& UnlimitedPlaybackManager::Instance() {
    static UnlimitedPlaybackManager instance;
    return instance;
}

UnlimitedPlaybackManager::UnlimitedPlaybackManager() {
    m_mode = Mode_Unlimited;
    for (int i = 0; i < Trigger_Count; ++i) {
        m_triggers[i].enabled = (i != Trigger_KeyPress && i != Trigger_OnLoop);
        m_triggers[i].cooldownFrames = 1;
        m_triggers[i].keyCode = VK_F6;
    }
}

void UnlimitedPlaybackManager::InitializeIfNeeded() {
    if (m_initialized) {
        return;
    }

    m_activeProfilePath.clear();
    m_lastLoadedProfileFolder.clear();
    // Trigger keybind lives in user settings, not in profiles.
    const int savedKeyCode = Settings::settingsIni.unlimitedPlaybackTriggerKeyCode;
    if (savedKeyCode != 0) {
        m_triggers[Trigger_KeyPress].keyCode = savedKeyCode;
    }
    const int savedLoopKeyCode = Settings::settingsIni.unlimitedPlaybackLoopKeyCode;
    m_loopKeyCode = savedLoopKeyCode != 0 ? savedLoopKeyCode : VK_F7;
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
void UnlimitedPlaybackManager::RunPreTick() {
    static bool loggedAlive = false;
    if (!loggedAlive) {
        LOG(7, "[UP][diag] RunPreTick hook is alive (first call).\n");
        loggedAlive = true;
    }
    ExecutePendingPlayNow();
}

void UnlimitedPlaybackManager::Tick() {
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

    if (!m_keyPressTriggerArmed && !IsTriggerKeyDown(m_triggers[Trigger_KeyPress].keyCode)) {
        LogRuntimeGateState("Tick arming key-press triggers");
        SyncKeyEdgeState();
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

    if (m_triggers[Trigger_OnLoop].enabled) {
        ProcessLoopTick(frame);
        return;
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
        static_cast<unsigned int>(m_entries.size()),
        static_cast<unsigned int>(m_cache.size()),
        m_mode);
    m_triggerRuntimeEnabled = enableRuntime;
    m_lastObservedFrame = -1;
    for (int i = 0; i < Trigger_Count; ++i) {
        m_triggers[i].lastTriggeredFrame = -999999;
    }

    m_prevWakeupCondition = false;
    m_prevGapCondition = false;
    m_prevOnBlockCondition = false;
    m_prevOnHitCondition = false;
    m_prevThrowTechCondition = false;
    SyncKeyEdgeState();
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
        static_cast<unsigned int>(m_entries.size()),
        static_cast<unsigned int>(m_cache.size()),
        m_activeProfilePath.c_str());
}

void UnlimitedPlaybackManager::LogEntryCacheSummary(const char* tag) const {
    LOG(1,
        "[UP][DATA] %s entries=%u cache=%u activeProfile='%s'\n",
        tag ? tag : "(null)",
        static_cast<unsigned int>(m_entries.size()),
        static_cast<unsigned int>(m_cache.size()),
        m_activeProfilePath.c_str());

    for (size_t i = 0; i < m_entries.size(); ++i) {
        const auto& entry = m_entries[i];
        const auto cacheIt = m_cache.find(entry.id);
        const bool cacheLoaded = cacheIt != m_cache.end() && cacheIt->second.loaded;
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
    SyncKeyEdgeState();
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
    return m_selectionMode;
}

void UnlimitedPlaybackManager::SetSelectionMode(int mode) {
    if (mode < Selection_Random || mode > Selection_NonRepeatingRandom) {
        mode = Selection_Random;
    }
    m_selectionMode = mode;
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

int UnlimitedPlaybackManager::GetLoopKeyCode() const {
    return m_loopKeyCode;
}

void UnlimitedPlaybackManager::SetLoopKeyCode(int keyCode) {
    m_loopKeyCode = keyCode != 0 ? keyCode : VK_F7;
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
    return m_entries;
}

std::vector<UnlimitedPlaybackManager::PlaybackEntry>& UnlimitedPlaybackManager::GetEntriesMutable() {
    return m_entries;
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
    m_entries.push_back(entry);
    m_cache[entry.id] = playback;

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
    m_entries.push_back(entry);
    CachedPlayback playback;
    playback.loaded = true;
    playback.facingLeft = facingLeft;
    playback.frames = frames;
    m_cache[entry.id] = playback;

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
    m_entries.push_back(entry);
    CachedPlayback playback;
    playback.loaded = true;
    playback.facingLeft = facingLeft;
    playback.frames = frames;
    m_cache[entry.id] = playback;

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

    if (idx >= m_entries.size()) {
        return false;
    }

    m_cache.erase(m_entries[idx].id);
    m_entries.erase(m_entries.begin() + idx);
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
        if (idx >= m_entries.size()) {
            continue;
        }
        m_cache.erase(m_entries[idx].id);
        m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(idx));
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

    if (fromIdx >= m_entries.size() || toIdx >= m_entries.size() || fromIdx == toIdx) {
        return false;
    }

    PlaybackEntry entry = std::move(m_entries[fromIdx]);
    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(fromIdx));
    m_entries.insert(m_entries.begin() + static_cast<std::ptrdiff_t>(toIdx), std::move(entry));
    for (int i = 0; i < Trigger_Count; ++i) {
        m_sequentialIndex[i] = 0;
        m_nonRepeatPools[i].clear();
    }
    PushToast(L("Slot moved."));
    return true;
}

bool UnlimitedPlaybackManager::MoveEntries(const std::vector<size_t>& fromIndicesSorted, size_t insertionIndex, size_t* outInsertedAt) {
    InitializeIfNeeded();

    if (fromIndicesSorted.empty() || insertionIndex > m_entries.size()) {
        return false;
    }
    for (size_t idx : fromIndicesSorted) {
        if (idx >= m_entries.size()) {
            return false;
        }
    }

    std::vector<bool> isMoved(m_entries.size(), false);
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
    remaining.reserve(m_entries.size() - fromIndicesSorted.size());
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (isMoved[i]) {
            moved.push_back(std::move(m_entries[i]));
        } else {
            remaining.push_back(std::move(m_entries[i]));
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
    m_entries = std::move(remaining);

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
        if (idx < m_entries.size()) {
            m_entries[idx].enabled = enabled;
        }
    }
    for (int i = 0; i < Trigger_Count; ++i) {
        m_sequentialIndex[i] = 0;
        m_nonRepeatPools[i].clear();
    }
}

void UnlimitedPlaybackManager::SetAllEntriesEnabled(bool enabled) {
    InitializeIfNeeded();

    for (auto& entry : m_entries) {
        entry.enabled = enabled;
    }
    for (int i = 0; i < Trigger_Count; ++i) {
        m_sequentialIndex[i] = 0;
        m_nonRepeatPools[i].clear();
    }
    PushToast(enabled ? L("All slots enabled.") : L("All slots disabled."));
}

bool UnlimitedPlaybackManager::RenameEntry(size_t idx, const std::string& newName) {
    if (idx >= m_entries.size() || newName.empty()) {
        return false;
    }

    m_entries[idx].name = newName;
    PushToast(L("Entry renamed."));
    return true;
}

bool UnlimitedPlaybackManager::LoadEntryIntoSlot(size_t idx, int slot) {
    InitializeIfNeeded();

    if (idx >= m_entries.size() || slot < 1 || slot > 4) {
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

    const auto& entry = m_entries[idx];
    auto it = m_cache.find(entry.id);
    if (it == m_cache.end() || !it->second.loaded) {
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

    if (idx >= m_entries.size() || slot < 1 || slot > 4) {
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
    m_cache[m_entries[idx].id] = playback;
    PushToast(L("Entry overwritten from slot."));
    return true;
}

bool UnlimitedPlaybackManager::ReadEntryPlayback(size_t idx, bool* outFacingLeft, std::vector<char>* outFrames) {
    InitializeIfNeeded();

    if (idx >= m_entries.size() || !outFacingLeft || !outFrames) {
        return false;
    }

    const auto& entry = m_entries[idx];
    auto it = m_cache.find(entry.id);
    if (it == m_cache.end() || !it->second.loaded) {
        return false;
    }

    *outFacingLeft = it->second.facingLeft;
    *outFrames = CompactPlaybackBytes(it->second.frames);
    return true;
}

bool UnlimitedPlaybackManager::WriteEntryPlayback(size_t idx, bool facingLeft, const std::vector<char>& frames) {
    InitializeIfNeeded();

    if (idx >= m_entries.size()) {
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
    m_cache[m_entries[idx].id] = playback;
    PushToast(L("Entry saved."));
    return true;
}

bool UnlimitedPlaybackManager::SaveEntryToFile(size_t idx, const std::string& outputPath) {
    InitializeIfNeeded();

    if (idx >= m_entries.size() || outputPath.empty()) {
        return false;
    }

    const auto& entry = m_entries[idx];
    const auto it = m_cache.find(entry.id);
    if (it == m_cache.end() || !it->second.loaded) {
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

    if (idx >= m_entries.size()) {
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

    const auto& entry = m_entries[idx];
    auto it = m_cache.find(entry.id);
    if (it == m_cache.end() || !it->second.loaded) {
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
    if (idx >= m_entries.size()) {
        LOG(7, "[UP][diag] ExecutePendingPlayNow: idx out of range (entries=%u)\n",
            static_cast<unsigned int>(m_entries.size()));
        return;
    }

    const auto& entry = m_entries[idx];
    auto it = m_cache.find(entry.id);
    if (it == m_cache.end() || !it->second.loaded) {
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
    m_entries.clear();
    m_cache.clear();
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
    out << "selection_mode=" << m_selectionMode << "\n";
    out << "auto_mirror_side_swap=" << (m_autoMirrorOnSideSwap ? 1 : 0) << "\n";
    for (int i = 0; i < Trigger_Count; ++i) {
        out << "trigger." << TriggerKeyName(static_cast<TriggerType>(i)) << ".enabled=" << (m_triggers[i].enabled ? 1 : 0) << "\n";
        out << "trigger." << TriggerKeyName(static_cast<TriggerType>(i)) << ".cooldown=" << m_triggers[i].cooldownFrames << "\n";
        // keyCode is stored in settings.ini, not in profile files.
    }

    for (const auto& e : m_entries) {
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
        const auto cacheIt = m_cache.find(e.id);
        if (cacheIt != m_cache.end() && cacheIt->second.loaded) {
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
    m_activeProfilePath = p;
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
    int parsedSelectionMode = m_selectionMode;
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

    m_entries = parsedEntries;
    m_triggers = parsedTriggers;
    SetMode(Mode_Unlimited);
    SetSelectionMode(parsedSelectionMode);
    SetAutoMirrorOnSideSwap(parsedAutoMirror);
    const size_t slash = p.find_last_of("/\\");
    if (slash != std::string::npos) {
        m_lastLoadedProfileFolder = p.substr(0, slash);
    } else {
        m_lastLoadedProfileFolder.clear();
    }

    m_cache.clear();
    m_cache = std::move(parsedCache);

    ResetRuntimePlaybackState(true);
    ForceResetTriggers(L("Profile loaded. Trigger runtime synced.").c_str());
    m_activeProfilePath = p;
    DebugLogState("LoadProfile end");
    return true;
}

std::string UnlimitedPlaybackManager::GetActiveProfilePath() const {
    return m_activeProfilePath;
}

void UnlimitedPlaybackManager::SetActiveProfilePath(const std::string& path) {
    m_activeProfilePath = path;
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
    return "BBCF_IM/unlimited_playbacks/library";
}

std::string UnlimitedPlaybackManager::GetProfileFolder() const {
    return "BBCF_IM/unlimited_playbacks/profiles";
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
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].relativePath == candidate) {
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
    if (idx >= m_entries.size()) {
        return "";
    }
    if (m_entries[idx].relativePath.empty() || IsAbsolutePath(m_entries[idx].relativePath)) {
        m_entries[idx].relativePath = BuildUniqueRelativePath(m_entries[idx].name.empty() ? "playback" : m_entries[idx].name);
    }
    return m_entries[idx].relativePath;
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

bool UnlimitedPlaybackManager::PickEntryIndexForTrigger(TriggerType trigger, size_t* outIndex) {
    std::vector<size_t> candidates = BuildCandidatesForTrigger(trigger);
    if (candidates.empty()) {
        return false;
    }
    static std::mt19937 rng(static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    if (m_selectionMode == Selection_Sequential) {
        const size_t pos = m_sequentialIndex[trigger] % candidates.size();
        *outIndex = candidates[pos];
        m_sequentialIndex[trigger] = (m_sequentialIndex[trigger] + 1) % candidates.size();
        return true;
    }

    auto weightedPick = [&](const std::vector<size_t>& source, size_t* picked) -> bool {
        double totalWeight = 0.0;
        for (size_t idx : source) {
            totalWeight += (double)m_entries[idx].weight;
        }
        if (totalWeight <= 0.0) {
            return false;
        }
        std::uniform_real_distribution<double> dist(0.0, totalWeight);
        const double roll = dist(rng);
        double acc = 0.0;
        for (size_t idx : source) {
            acc += (double)m_entries[idx].weight;
            if (roll <= acc) {
                *picked = idx;
                return true;
            }
        }
        *picked = source.back();
        return true;
    };

    if (m_selectionMode == Selection_NonRepeatingRandom) {
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
    if (!config.enabled) {
        return false;
    }

    if ((currentFrame - config.lastTriggeredFrame) < (std::max)(1, config.cooldownFrames)) {
        return false;
    }

    if (m_runtimeSlotBackupValid || m_runtimeSlotRestorePending) {
        return false;
    }

    bool shouldFire = false;
    switch (trigger) {
    case Trigger_KeyPress: shouldFire = IsKeyPressedEdge(config.keyCode); break;
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

    PushToast(FormatLocalized("Trigger detected: %s", TriggerDisplayName(trigger)));

    size_t chosen = 0;
    if (!PickEntryIndexForTrigger(trigger, &chosen)) {
        PushToast(L("No eligible playback in current trigger selection."));
        return false;
    }

    const auto& entry = m_entries[chosen];
    const auto cacheIt = m_cache.find(entry.id);
    if (cacheIt == m_cache.end() || !cacheIt->second.loaded) {
        PushToast(L("Selected playback cache missing."));
        return false;
    }

    std::vector<char> frames = cacheIt->second.frames;
    int facingToLoad = cacheIt->second.facingLeft ? 1 : 0;
    bool mirrored = false;
    bool currentFacingLeft = false;
    if (TryGetCurrentFacingLeft(&currentFacingLeft) && currentFacingLeft != cacheIt->second.facingLeft) {
        mirrored = m_autoMirrorOnSideSwap;
        if (!m_autoMirrorOnSideSwap) {
            facingToLoad = currentFacingLeft ? 1 : 0;
        }
    }

    BackupRuntimeSlotIfNeeded();
    StartRuntimePlayback(frames, facingToLoad);
    m_runtimeSlotRestorePending = true;
    LOG(1, "[UP] Trigger started entry='%s' trigger='%s' frames=%u facing=%d mirrored=%d\n",
        entry.name.c_str(),
        TriggerDisplayName(trigger),
        static_cast<unsigned int>(frames.size()),
        facingToLoad,
        mirrored ? 1 : 0);

    config.lastTriggeredFrame = currentFrame;
    PushToast(FormatLocalized("Triggered [%s]: %s%s",
        TriggerDisplayName(trigger),
        entry.name.c_str(),
        mirrored ? L(" (mirrored)").c_str() : ""));
    return true;
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

    if (IsKeyPressedEdge(m_loopKeyCode)) {
        if (m_loopActive) {
            StopLoop(L("Playback loop stopped.").c_str());
        } else {
            StartLoop(currentFrame);
        }
        return;
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

void UnlimitedPlaybackManager::StartLoop(int currentFrame) {
    if (BuildCandidatesForTrigger(Trigger_OnLoop).empty()) {
        PushToast(L("Playback loop not started: no eligible playback."));
        return;
    }
    if (m_loopRestartLabState &&
        m_loopRestartMode == LoopReset_Custom &&
        !IsLoopSnapshotReadyForMode(LoopReset_Custom)) {
        PushToast(L("Playback loop not started: custom snapshot missing."));
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
    size_t chosen = 0;
    if (!PickEntryIndexForTrigger(Trigger_OnLoop, &chosen)) {
        return false;
    }

    const auto& entry = m_entries[chosen];
    const auto cacheIt = m_cache.find(entry.id);
    if (cacheIt == m_cache.end() || !cacheIt->second.loaded) {
        PushToast(L("Selected playback cache missing."));
        return false;
    }

    std::vector<char> frames = cacheIt->second.frames;
    int facingToLoad = cacheIt->second.facingLeft ? 1 : 0;
    bool mirrored = false;
    bool currentFacingLeft = false;
    if (TryGetCurrentFacingLeft(&currentFacingLeft) && currentFacingLeft != cacheIt->second.facingLeft) {
        mirrored = m_autoMirrorOnSideSwap;
        if (!m_autoMirrorOnSideSwap) {
            facingToLoad = currentFacingLeft ? 1 : 0;
        }
    }

    BackupRuntimeSlotIfNeeded();
    StartRuntimePlayback(frames, facingToLoad);
    m_runtimeSlotRestorePending = true;
    ResetLoopPlaybackCompletionState();
    PushToast(FormatLocalized("Loop played: %s%s",
        entry.name.c_str(),
        mirrored ? L(" (mirrored)").c_str() : ""));
    return true;
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
        m_loopSnapshotApparatus = new SnapshotApparatus();
        return m_loopSnapshotApparatus != nullptr;
    }

    if (!m_loopSnapshotApparatus->check_if_valid(g_interfaces.player1.GetData(), g_interfaces.player2.GetData())) {
        delete m_loopSnapshotApparatus;
        m_loopSnapshotApparatus = new SnapshotApparatus();
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

    const int savedSlot = static_cast<int>(m_loopSnapshotApparatus->snapshot_count % 10);
    const bool nativeOk = m_loopSnapshotApparatus->save_snapshot(nullptr);
    if (!nativeOk) {
        ClearLoopCustomSnapshot();
        return false;
    }
    m_loopCustomSnapshotSlotIndex = savedSlot;
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
        const unsigned int oldCount = m_loopSnapshotApparatus->snapshot_count;
        m_loopSnapshotApparatus->snapshot_count = static_cast<unsigned int>(m_loopCustomSnapshotSlotIndex + 1);
        const bool ok = m_loopSnapshotApparatus->load_snapshot(nullptr);
        m_loopSnapshotApparatus->snapshot_count = oldCount;
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
    LOG(1, "[UP] StartRuntimePlayback before: pbCtrl=%d activeSlot=%d pbPos=%d facing=%d frames=%u\n",
        beforeControl,
        beforeActiveSlot,
        beforePosition,
        facingToLoad,
        static_cast<unsigned int>(frames.size()));

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

std::vector<size_t> UnlimitedPlaybackManager::BuildCandidatesForTrigger(TriggerType trigger) {
    std::vector<size_t> candidates;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const auto& e = m_entries[i];
        if (!e.enabled || e.weight <= 0.0f) {
            continue;
        }
        if (!e.triggerEnabled[static_cast<size_t>(trigger)]) {
            continue;
        }
        auto cacheIt = m_cache.find(e.id);
        if (cacheIt == m_cache.end() || !cacheIt->second.loaded) {
            continue;
        }
        candidates.push_back(i);
    }
    return candidates;
}

void UnlimitedPlaybackManager::SyncKeyEdgeState() {
    for (int vk = 0; vk < 256; ++vk) {
        m_prevKeyDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    for (int i = 0; i < static_cast<int>(sizeof(kControllerBindings) / sizeof(kControllerBindings[0])); ++i) {
        m_prevControllerBindDown[static_cast<size_t>(i)] = IsControllerBindingDown(kControllerBindings[i]);
    }
}

bool UnlimitedPlaybackManager::IsTriggerKeyDown(int virtualKey) const {
    if (!IsGameWindowFocused()) {
        return false;
    }
    if (IsControllerBindCode(virtualKey)) {
        return IsControllerBindingDown(kControllerBindings[virtualKey - kControllerBindBase]);
    }
    if (virtualKey <= 0 || virtualKey >= 256) {
        return false;
    }
    if (IsTypingInImGuiTextField()) {
        return false;
    }
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

bool UnlimitedPlaybackManager::IsKeyPressedEdge(int virtualKey) {
    // Typing into an overlay text field must not fire keyboard triggers. Controller
    // bindings are unaffected -- a gamepad button is never "typing".
    if (!IsGameWindowFocused() || (!IsControllerBindCode(virtualKey) && IsTypingInImGuiTextField())) {
        if (IsControllerBindCode(virtualKey)) {
            const int index = virtualKey - kControllerBindBase;
            m_prevControllerBindDown[static_cast<size_t>(index)] = false;
        } else if (virtualKey > 0 && virtualKey < 256) {
            m_prevKeyDown[virtualKey] = false;
        }
        return false;
    }
    if (IsControllerBindCode(virtualKey)) {
        const int index = virtualKey - kControllerBindBase;
        const bool isDown = IsControllerBindingDown(kControllerBindings[index]);
        const bool wasDown = m_prevControllerBindDown[static_cast<size_t>(index)];
        m_prevControllerBindDown[static_cast<size_t>(index)] = isDown;
        return m_keyPressTriggerArmed && isDown && !wasDown;
    }
    if (virtualKey <= 0 || virtualKey >= 256) {
        return false;
    }
    if (!m_keyPressTriggerArmed) {
        return false;
    }
    const bool isDown = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    const bool wasDown = m_prevKeyDown[virtualKey];
    m_prevKeyDown[virtualKey] = isDown;
    return isDown && !wasDown;
}

bool UnlimitedPlaybackManager::ShouldTriggerWakeup() {
    if (g_interfaces.player2.IsCharDataNullPtr()) {
        return false;
    }

    const std::string currentAction = g_interfaces.player2.GetData()->currentAction;
    const int actionTime = g_interfaces.player2.GetData()->actionTime;

    static const std::array<std::pair<const char*, int>, 5> wakeupStates = {
        std::make_pair("ActUkemiLandN", 30),
        std::make_pair("ActUkemiLandF", 30),
        std::make_pair("ActUkemiLandB", 30),
        std::make_pair("ActFDown2Stand", 14),
        std::make_pair("ActBDown2Stand", 14),
    };

    bool cond = false;
    for (const auto& ws : wakeupStates) {
        if (currentAction.find(ws.first) != std::string::npos && actionTime == ws.second) {
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

    const auto* p2 = g_interfaces.player2.GetData();
    const std::string currentAction = p2->currentAction;
    const bool cond = (p2->blockstun == 1 && currentAction.find("Guard") != std::string::npos);

    const bool edge = (cond && !m_prevGapCondition);
    m_prevGapCondition = cond;
    return edge;
}

bool UnlimitedPlaybackManager::ShouldTriggerOnBlock() {
    if (g_interfaces.player2.IsCharDataNullPtr()) {
        return false;
    }

    // Fire on the first frame after blockstun ends so the dummy can act immediately.
    const auto* p2 = g_interfaces.player2.GetData();
    const bool cond = p2->blockstun > 0;

    const bool edge = (!cond && m_prevOnBlockCondition);
    m_prevOnBlockCondition = cond;
    return edge;
}

bool UnlimitedPlaybackManager::ShouldTriggerOnHit() {
    if (g_interfaces.player2.IsCharDataNullPtr()) {
        return false;
    }

    // Fire on the first frame after hitstun ends so the dummy can act immediately.
    const auto* p2 = g_interfaces.player2.GetData();
    const bool cond = p2->hitstun > 0;

    const bool edge = (!cond && m_prevOnHitCondition);
    m_prevOnHitCondition = cond;
    return edge;
}

bool UnlimitedPlaybackManager::ShouldTriggerThrowTech() {
    if (g_interfaces.player2.IsCharDataNullPtr()) {
        return false;
    }

    const auto* p2 = g_interfaces.player2.GetData();
    const std::string currentAction = p2->currentAction;
    const bool cond = (p2->timeAfterTechIsPerformed == 29 && currentAction.find("LockReject") != std::string::npos);

    const bool edge = (cond && !m_prevThrowTechCondition);
    m_prevThrowTechCondition = cond;
    return edge;
}
