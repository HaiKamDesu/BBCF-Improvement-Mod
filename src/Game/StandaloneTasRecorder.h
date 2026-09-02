#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "Game/TasManager.h"

class StandaloneTasRecorder
{
public:
    static StandaloneTasRecorder& Instance();

    bool IsRecording() const { return m_recording; }
    bool IsPendingExport() const { return m_pendingExport; }
    std::size_t GetFrameCount() const { return m_movie.size(); }

    bool Start();
    void Stop();
    void Update();
    bool Export(const std::string& path);
    void FinalizeExport();
    void Abort();

private:
    StandaloneTasRecorder() = default;
    StandaloneTasRecorder(const StandaloneTasRecorder&) = delete;
    StandaloneTasRecorder& operator=(const StandaloneTasRecorder&) = delete;

    bool m_recording = false;
    bool m_pendingExport = false;
    bool m_hasGameFrame = false;
    unsigned int m_lastGameFrame = 0;
    std::vector<TasFrameInput> m_movie;
};