#include "TasManager.h"

#include "Core/ControllerOverrideManager.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Game/gamestates.h"
#include "Game/CharData.h"
#include "Game/characters.h"
#include "Hooks/hooks_battle_input.h"
#include "Overlay/Window/FrameHistory/FrameHistoryWindow.h"
#include "Overlay/WindowManager.h"

#include <Windows.h>

#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
namespace {
// TAS Movie length is not artificially capped.
constexpr unsigned int kPresentationLeadInFrames = 60;
constexpr unsigned int kPresentationLeadOutFrames = 240;
constexpr const char* kTasMovieHeaderV1 = "BBCF_TAS_MOVIE_V1";
constexpr const char* kTasMovieHeaderV2 = "BBCF_TAS_MOVIE_V2";

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
    std::string result(1, static_cast<char>('0' + (packed & 0x0F)));
    if (packed & 16) result += 'A';
    if (packed & 32) result += 'B';
    if (packed & 64) result += 'C';
    if (packed & 128) result += 'D';
    return result;
}

bool ParseHumanInput(const std::string& text, uint16_t* result) {
    if (!result) return false;
    uint16_t value = 0;
    bool sawDirection = false;
    for (char raw : text) {
        const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        if (ch >= '1' && ch <= '9') {
            if (sawDirection) return false;
            value = static_cast<uint16_t>(ch - '0');
            sawDirection = true;
        } else if (ch == 'A' || ch == 'B' || ch == 'C' || ch == 'D') {
            if (!sawDirection || (value & ButtonValue(ch))) return false;
            value = static_cast<uint16_t>(value + ButtonValue(ch));
        } else if (ch != ' ' && ch != '\t') {
            return false;
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

void TasManager::ClearSnapshot() {
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
        SetError("TAS mode is available only during a training match.");
        return;
    }
    // Every TAS input goes through OverrideBattleInputPacked, which only reaches the game
    // via the BattleInputWrite hook. That hook is skipped when controller hooks are
    // disabled, so without it playback would run and silently do nothing.
    if (!IsBattleInputHookInstalled()) {
        SetError("TAS mode needs the controller hooks. Enable EnableControllerHooks in settings.ini and restart the game.");
        return;
    }

    m_active = true;
    m_runState = TasRunState::Idle;
    m_playhead = 0;
    m_runTarget = 0;
    m_presentationFramesRemaining = 0;
    m_presentationMode = false;
    m_truncateOnReplayFinish = false;
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
    m_truncateOnReplayFinish = false;
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
    if (m_truncateOnReplayFinish) {
        m_movie.resize(m_playhead);
        m_truncateOnReplayFinish = false;
    }
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
    if (m_playhead >= m_runTarget || m_playhead >= m_movie.size()) {
        const bool completed = m_runState == TasRunState::PlayingMovie && m_playhead >= m_movie.size();
        if (completed) {
            if (m_presentationMode) {
                StartPresentationLeadOut();
            } else {
                // Preview is an editing pass. Freeze on the resulting state so
                // the next frame can be authored without losing the combo.
                StopPlayback(false);
                m_status = "Preview finished and paused at the movie end.";
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
        SetError("Edit movie input first.");
        return;
    }

    if (!LoadBaseSnapshot()) {
        return;
    }
    m_playhead = 0;
    m_presentationMode = presentationMode;
    m_truncateOnReplayFinish = false;

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
        SetError("Cannot advance frames during playback.");
        return;
    }
    if (!m_active || !HasBaseSnapshot() || count <= 0) {
        SetError("Save a base state before editing movie input.");
        return;
    }
    if (!m_inputsParsed && !ParseInputs()) {
        return;
    }

    const size_t frameCount = static_cast<size_t>(count);
    if (frameCount > (std::numeric_limits<size_t>::max)() - m_playhead) {
        SetError("Movie frame count exceeds the addressable size limit.");
        return;
    }
    const size_t end = m_playhead + frameCount;
    if (m_playhead < m_movie.size()) {
        m_movie.resize(m_playhead);
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
    m_status = "Parsed inputs reset to neutral.";
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
    m_playhead = 0;
    m_runTarget = 0;
    m_commandCursor = 0;
    m_truncateOnReplayFinish = false;
    m_runState = TasRunState::Idle;
    m_error.clear();
    m_status = "Movie reset.";
}

bool TasManager::AdvanceOneFrame() {
    return AdvanceFrames(1);
}

bool TasManager::AdvanceFrames(int count) {
    if (count <= 0) {
        return true;
    }
    if (IsPlaybackRunning()) {
        SetError("Cannot advance frames during playback.");
        return false;
    }
    EditAndAdvanceFrames(count);
    return m_error.empty();
}

bool TasManager::RewindFrames(int count) {
    if (count <= 0) {
        return true;
    }
    if (IsPlaybackRunning()) {
        SetError("Cannot rewind frames during playback.");
        return false;
    }
    if (!m_active || !HasBaseSnapshot()) {
        SetError("Save a base state before rewinding.");
        return false;
    }

    const size_t amount = static_cast<size_t>(count);
    const size_t target = amount >= m_playhead ? 0 : m_playhead - amount;
    if (!LoadBaseSnapshot()) {
        return false;
    }
    m_playhead = 0;
    if (target == 0) {
        m_movie.clear();
        m_runTarget = 0;
        m_runState = TasRunState::Idle;
        m_status = "Rewound to base and truncated the movie.";
        return true;
    }
    m_truncateOnReplayFinish = true;
    m_status = "Rewinding and truncating the movie.";
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
        SetError("Enter a training match and enable TAS mode first.");
        return false;
    }
    if (!m_snapshotOwner || !m_snapshotOwner->check_if_valid(
        g_interfaces.player1.GetData(), g_interfaces.player2.GetData())) {
        delete m_snapshotOwner;
        m_snapshotOwner = new SnapshotApparatus();
    }
    if (!m_snapshotOwner->save_snapshot(nullptr)) {
        SetError("Base-state save failed.");
        return false;
    }

    m_snapshotSize = m_snapshotOwner->get_last_saved_snapshot_size();
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
        SetError("No native base state is available.");
        return false;
    }
    ClearInputOverride();
    m_presentationFramesRemaining = 0;
    if (!m_snapshotOwner->load_snapshot(0)) {
        SetError("Native base-state load failed.");
        return false;
    }

    ++m_rerecordCount;
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
        SetError("Could not open the TAS file for writing.");
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
    output << "# Inputs use numpad notation: 7 8 9 / 4 5 6 / 1 2 3; suffixes A B C D are buttons.\n";
    for (size_t i = 0; i < m_movie.size(); ++i) {
        output << i << " | P1=" << HumanInput(m_movie[i].p1)
               << " | P2=" << HumanInput(m_movie[i].p2) << '\n';
    }
    if (!output) {
        SetError("Failed while writing the TAS file.");
        return false;
    }
    m_error.clear();
    m_status = "Exported " + path + ".";
    LOG(1, "[TAS] exported movie path=%s frames=%u\n", path.c_str(), static_cast<unsigned int>(m_movie.size()));
    return true;
}

bool TasManager::ImportMovie(const std::string& path) {
    std::ifstream input(path.c_str());
    if (!input) {
        SetError("Could not open the selected TAS file.");
        return false;
    }
    std::string header;
    if (!std::getline(input, header)) {
        SetError("The TAS file is empty.");
        return false;
    }
    if (!header.empty() && header.back() == '\r') header.pop_back();
    std::string framesLabel;
    size_t declaredCount = 0;
    if (header == kTasMovieHeaderV1) {
        if (!(input >> framesLabel >> declaredCount) || framesLabel != "frames" || declaredCount == 0) {
            SetError("The V1 TAS file has an invalid frame count.");
            return false;
        }
    } else if (header == kTasMovieHeaderV2) {
        if (!(input >> framesLabel >> declaredCount) || framesLabel != "frames" || declaredCount == 0) {
            SetError("The V2 TAS file has an invalid frame count.");
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
                    SetError("The V2 TAS file is missing frame data.");
                    return false;
                }
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t firstBar = line.find(" | P1=");
                const size_t secondBar = line.find(" | P2=", firstBar == std::string::npos ? 0 : firstBar + 6);
                if (firstBar == std::string::npos || secondBar == std::string::npos) {
                    SetError("The V2 TAS file contains an invalid frame.");
                    return false;
                }
                std::istringstream indexStream(line.substr(0, firstBar));
                size_t index = 0;
                if (!(indexStream >> index) || index != expected) {
                    SetError("The V2 TAS file contains an invalid frame index.");
                    return false;
                }
                const std::string p1Text = line.substr(firstBar + 6, secondBar - (firstBar + 6));
                const std::string p2Text = line.substr(secondBar + 6);
                uint16_t p1 = 0, p2 = 0;
                if (!ParseHumanInput(p1Text, &p1) || !ParseHumanInput(p2Text, &p2)) {
                    SetError("The V2 TAS file contains an invalid input.");
                    return false;
                }
                imported.push_back(TasFrameInput{p1, p2});
            }
            if (imported.size() != declaredCount) {
                SetError("The V2 TAS file is missing frame data.");
                return false;
            }
            ClearInputOverride(); ClearSnapshot(); m_movie.swap(imported);
            m_playhead = 0; m_runTarget = 0; m_presentationFramesRemaining = 0;
            m_presentationMode = false; m_truncateOnReplayFinish = false; m_runState = TasRunState::Idle;
            m_error.clear(); m_status = "Imported " + path + ". Save a matching base state before playback.";
            return true;
        }
        SetError("The V2 TAS file contains no frame data.");
        return false;
    } else {
        SetError("The TAS file has an unknown format.");
        return false;
    }
    std::vector<TasFrameInput> imported;
    imported.reserve(declaredCount);
    for (size_t expectedIndex = 0; expectedIndex < declaredCount; ++expectedIndex) {
        size_t index = 0; unsigned int p1 = 0, p2 = 0;
        if (!(input >> index >> p1 >> p2) || index != expectedIndex || p1 > UINT16_MAX || p2 > UINT16_MAX ||
            (p1 & 0xF) < 1 || (p1 & 0xF) > 9 || (p2 & 0xF) < 1 || (p2 & 0xF) > 9) {
            SetError("The V1 TAS file contains an invalid frame."); return false;
        }
        imported.push_back(TasFrameInput{static_cast<uint16_t>(p1), static_cast<uint16_t>(p2)});
    }
    ClearInputOverride(); ClearSnapshot(); m_movie.swap(imported);
    m_playhead = 0; m_runTarget = 0; m_presentationFramesRemaining = 0;
    m_presentationMode = false; m_truncateOnReplayFinish = false; m_runState = TasRunState::Idle;
    m_error.clear(); m_status = "Imported " + path + ". Save a matching base state before playback.";
    LOG(1, "[TAS] imported movie path=%s frames=%u\n", path.c_str(), static_cast<unsigned int>(m_movie.size()));
    return true;
}

bool TasManager::ParseOne(const std::string& text, std::vector<uint16_t>* out) const {
    if (!out) {
        return false;
    }
    out->clear();
    uint16_t pendingButtons = 0;
    bool sawDirection = false;
    for (char raw : text) {
        const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        if (ch >= '1' && ch <= '9') {
            if (pendingButtons && !out->empty()) {
                out->back() = static_cast<uint16_t>(out->back() + pendingButtons);
                pendingButtons = 0;
            }
            out->push_back(static_cast<uint16_t>(ch - '0'));
            sawDirection = true;
        } else if (ch == 'A' || ch == 'B' || ch == 'C' || ch == 'D') {
            if (!sawDirection || out->empty()) {
                return false;
            }
            pendingButtons = static_cast<uint16_t>(pendingButtons + ButtonValue(ch));
        } else if (ch != ' ' && ch != ',' && ch != '-') {
            return false;
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
    if (!ParseOne(m_p1Text, &p1)) {
        SetError("Invalid P1 input. Use examples such as 5C, 28D, 623C, or 656.");
        return false;
    }
    if (!ParseOne(m_p2Text, &p2)) {
        SetError("Invalid P2 input. Use examples such as 5C, 28D, 623C, or 656.");
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
