#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Plays a decoded PCM buffer straight out of the default output device, with a
// gain that can be changed while it is playing.
//
// This is the mod's only audio playback path. Everything else about music goes
// through the game's own XACT engine, which means a track's volume can only be
// auditioned by re-encoding it and starting a match. That is not a usable way
// to tune a gain slider, which is why this exists: the user hears the effect of
// the slider immediately, anywhere in the game, and the chosen gain is only
// baked into the .pac when they apply it.
//
// The preview is entirely mod-side. It does not touch the game's audio engine,
// its BGM volume setting, or any game state.
class AudioPreviewPlayer
{
public:
    static AudioPreviewPlayer& Get();

    // Starts playing `pcm` (interleaved s16). Any previous preview is stopped
    // first. Returns false if an output device could not be opened.
    bool Play(std::vector<unsigned char>&& pcm, unsigned int rate, unsigned int channels);

    void Stop();
    bool IsPlaying() const { return m_playing.load(); }

    // Live. Safe to call while playing; takes effect on the next buffer.
    void SetGainDb(float gainDb);
    float GetGainDb() const { return m_gainDb.load(); }

    double GetPositionSeconds() const;
    double GetDurationSeconds() const;

    // Releases the output device. Called on mod shutdown; leaving a WASAPI
    // device open while the host process tears down can hang the exit.
    void Shutdown();

    const std::string& LastError() const { return m_lastError; }

private:
    AudioPreviewPlayer() = default;
    ~AudioPreviewPlayer();
    AudioPreviewPlayer(const AudioPreviewPlayer&) = delete;
    AudioPreviewPlayer& operator=(const AudioPreviewPlayer&) = delete;

    static void DataCallback(void* pDevice, void* pOutput, const void* pInput, unsigned int frameCount);
    void Mix(void* pOutput, unsigned int frameCount);

    void StopLocked();

    mutable std::mutex m_mutex;      // guards m_pcm and the device lifetime
    std::vector<unsigned char> m_pcm;
    unsigned int m_rate = 44100;
    unsigned int m_channels = 2;

    std::atomic<bool> m_playing{ false };
    std::atomic<float> m_gainDb{ 0.0f };
    std::atomic<unsigned long long> m_cursorFrames{ 0 };

    void* m_device = nullptr;        // ma_device*, heap-allocated to keep miniaudio out of this header
    std::string m_lastError;
};
