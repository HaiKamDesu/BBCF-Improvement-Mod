#include "Game/StandaloneTasRecorder.h"

#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/logger.h"
#include "Game/gamestates.h"
#include "Hooks/hooks_battle_input.h"

#include <fstream>

StandaloneTasRecorder& StandaloneTasRecorder::Instance()
{
    static StandaloneTasRecorder recorder;
    return recorder;
}

bool StandaloneTasRecorder::Start()
{
    if (m_recording || m_pendingExport)
        return false;

    // Each refusal explains itself: without this the button is a no-op with no clue why.
    if (TasManager::Instance().IsActive() || TasManager::Instance().IsPlaybackRunning())
    {
        m_error = L("Leave the TAS combo editor before recording.");
        return false;
    }
    if (!g_gameVals.pGameMode || !g_gameVals.pGameState ||
        *g_gameVals.pGameMode != GameMode_Training || *g_gameVals.pGameState != GameState_InMatch)
    {
        m_error = L("TAS mode is available only during a training match.");
        return false;
    }
    if (!IsBattleInputHookInstalled())
    {
        m_error = L("TAS mode needs the controller hooks. Enable EnableControllerHooks in settings.ini and restart the game.");
        return false;
    }

    m_error.clear();
    m_movie.clear();
    m_recording = true;
    m_pendingExport = false;
    m_hasGameFrame = false;
    m_lastGameFrame = 0;
    LOG(1, "[TAS] standalone recorder started frame=%u\n",
        g_gameVals.pFrameCount ? *g_gameVals.pFrameCount : 0);
    return true;
}

void StandaloneTasRecorder::Stop()
{
    if (!m_recording)
        return;
    m_recording = false;
    m_pendingExport = !m_movie.empty();
    m_hasGameFrame = false;
    LOG(1, "[TAS] standalone recorder stopped frames=%u\n",
        static_cast<unsigned int>(m_movie.size()));
}

void StandaloneTasRecorder::Update()
{
    if (m_recording && (!g_gameVals.pGameMode || !g_gameVals.pGameState ||
        *g_gameVals.pGameMode != GameMode_Training || *g_gameVals.pGameState != GameState_InMatch))
    {
        Abort();
        return;
    }
    if (!m_recording || !g_gameVals.pFrameCount)
        return;
    const unsigned int frame = *g_gameVals.pFrameCount;
    if (m_hasGameFrame && m_lastGameFrame == frame)
        return;

    m_movie.push_back(TasFrameInput{ GetLastObservedBattleInputPacked(0), 5 });
    m_lastGameFrame = frame;
    m_hasGameFrame = true;
}

bool StandaloneTasRecorder::Export(const std::string& path)
{
    if (!m_pendingExport || m_movie.empty() || path.empty())
        return false;

    // V1 is the original numeric, input-only format understood by TasManager::ImportMovie.
    std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc);
    if (!output)
        return false;
    output << "BBCF_TAS_MOVIE_V1\n";
    output << "frames " << m_movie.size() << "\n";
    for (std::size_t i = 0; i < m_movie.size(); ++i)
        output << i << ' ' << m_movie[i].p1 << ' ' << m_movie[i].p2 << '\n';
    if (!output)
        return false;

    LOG(1, "[TAS] standalone recorder exported path=%s frames=%u\n",
        path.c_str(), static_cast<unsigned int>(m_movie.size()));
    return true;
}

void StandaloneTasRecorder::FinalizeExport()
{
    m_pendingExport = false;
    m_movie.clear();
    m_hasGameFrame = false;
    m_lastGameFrame = 0;
}

void StandaloneTasRecorder::Abort()
{
    if (m_recording)
        m_error = L("The recording was discarded because the training match ended.");
    m_recording = false;
    m_pendingExport = false;
    m_movie.clear();
    m_hasGameFrame = false;
    m_lastGameFrame = 0;
}
