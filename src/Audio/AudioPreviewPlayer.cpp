#include "AudioPreviewPlayer.h"
#include "Core/logger.h"

#include <cmath>
#include <cstring>

#include "miniaudio.h"

AudioPreviewPlayer& AudioPreviewPlayer::Get()
{
    static AudioPreviewPlayer instance;
    return instance;
}

AudioPreviewPlayer::~AudioPreviewPlayer()
{
    Shutdown();
}

void AudioPreviewPlayer::DataCallback(void* pDevice, void* pOutput, const void* pInput, unsigned int frameCount)
{
    (void)pInput;
    ma_device* device = static_cast<ma_device*>(pDevice);
    AudioPreviewPlayer* self = static_cast<AudioPreviewPlayer*>(device->pUserData);
    if (self)
        self->Mix(pOutput, frameCount);
}

// Runs on miniaudio's audio thread. It must not block, so it does not take
// m_mutex: m_pcm is only ever replaced while the device is stopped, and the
// cursor and gain are atomics.
void AudioPreviewPlayer::Mix(void* pOutput, unsigned int frameCount)
{
    short* out = static_cast<short*>(pOutput);
    const size_t totalFrames = m_pcm.size() / (m_channels * sizeof(short));
    const unsigned long long cursor = m_cursorFrames.load();

    if (!m_playing.load() || cursor >= totalFrames)
    {
        memset(out, 0, (size_t)frameCount * m_channels * sizeof(short));
        if (cursor >= totalFrames)
            m_playing.store(false);
        return;
    }

    const double scale = pow(10.0, m_gainDb.load() / 20.0);
    const short* src = reinterpret_cast<const short*>(m_pcm.data());

    unsigned int framesToWrite = frameCount;
    if (cursor + framesToWrite > totalFrames)
        framesToWrite = (unsigned int)(totalFrames - cursor);

    for (unsigned int f = 0; f < framesToWrite; ++f)
    {
        for (unsigned int c = 0; c < m_channels; ++c)
        {
            const size_t index = (size_t)(cursor + f) * m_channels + c;
            double scaled = (double)src[index] * scale;
            if (scaled > 32767.0) scaled = 32767.0;
            else if (scaled < -32768.0) scaled = -32768.0;
            out[(size_t)f * m_channels + c] = (short)scaled;
        }
    }

    // Pad the tail of the final buffer rather than leaving stale audio in it.
    if (framesToWrite < frameCount)
    {
        memset(out + (size_t)framesToWrite * m_channels, 0,
               (size_t)(frameCount - framesToWrite) * m_channels * sizeof(short));
    }

    m_cursorFrames.store(cursor + framesToWrite);
}

bool AudioPreviewPlayer::Play(std::vector<unsigned char>&& pcm, unsigned int rate, unsigned int channels)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    StopLocked();

    if (pcm.empty() || channels == 0)
    {
        m_lastError = "there is nothing decoded to play";
        return false;
    }

    m_pcm = std::move(pcm);
    m_rate = rate;
    m_channels = channels;
    m_cursorFrames.store(0);

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = channels;
    config.sampleRate = rate;
    config.dataCallback = (ma_device_data_proc)&AudioPreviewPlayer::DataCallback;
    config.pUserData = this;

    ma_device* device = new ma_device();
    if (ma_device_init(NULL, &config, device) != MA_SUCCESS)
    {
        delete device;
        m_pcm.clear();
        m_lastError = "no audio output device could be opened";
        LOG(1, "AudioPreviewPlayer: ma_device_init failed\n");
        return false;
    }

    m_device = device;
    m_playing.store(true);

    if (ma_device_start(device) != MA_SUCCESS)
    {
        m_playing.store(false);
        ma_device_uninit(device);
        delete device;
        m_device = nullptr;
        m_pcm.clear();
        m_lastError = "the audio output device refused to start";
        LOG(1, "AudioPreviewPlayer: ma_device_start failed\n");
        return false;
    }

    m_lastError.clear();
    return true;
}

void AudioPreviewPlayer::StopLocked()
{
    m_playing.store(false);
    if (m_device)
    {
        ma_device* device = static_cast<ma_device*>(m_device);
        ma_device_uninit(device); // stops the device and joins its thread
        delete device;
        m_device = nullptr;
    }
    m_pcm.clear();
    m_cursorFrames.store(0);
}

void AudioPreviewPlayer::Stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    StopLocked();
}

void AudioPreviewPlayer::Shutdown()
{
    Stop();
}

void AudioPreviewPlayer::SetGainDb(float gainDb)
{
    m_gainDb.store(gainDb);
}

double AudioPreviewPlayer::GetPositionSeconds() const
{
    if (m_rate == 0)
        return 0.0;
    return (double)m_cursorFrames.load() / (double)m_rate;
}

double AudioPreviewPlayer::GetDurationSeconds() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_rate == 0 || m_channels == 0)
        return 0.0;
    return (double)(m_pcm.size() / (m_channels * sizeof(short))) / (double)m_rate;
}
