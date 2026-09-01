#include "AudioDecode.h"
#include "Core/logger.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "miniaudio.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

namespace AudioDecode
{

const float kSilenceDb = -120.0f;

// PROVISIONAL. This has NOT been measured against the game's own BGM — doing so
// means pulling WMA payloads back out of the shipped XACT wave banks, which
// nothing here does yet. -18 dBFS RMS is the usual resting level for mastered
// game music and is a sane starting point, but if custom tracks come out
// consistently louder or quieter than the native ones, this constant is the
// thing to correct. It is deliberately a single named value so that is a
// one-line change.
const float kDefaultTargetRmsDb = -18.0f;

static void LogAudio(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    LOG(1, "%s", buf);
}

static std::wstring Widen(const std::string& utf8)
{
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    if (wlen <= 0)
        return std::wstring();
    std::wstring w(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], wlen);
    return w;
}

// ============================================================================
// Supported formats
// ============================================================================
// The first four come from miniaudio's built-in decoders (dr_wav, dr_mp3,
// dr_flac and stb_vorbis). The rest go through Media Foundation, so they depend
// on the codecs the user's Windows install ships — which is why they are listed
// last and why a failure there reports "your Windows install cannot decode".
static const std::vector<std::string>& ExtensionList()
{
    static const std::vector<std::string> exts = {
        "mp3", "wav", "flac", "ogg",    // miniaudio
        "m4a", "aac", "wma"             // Media Foundation
    };
    return exts;
}

const std::vector<std::string>& SupportedExtensions()
{
    return ExtensionList();
}

bool IsSupportedExtension(const std::string& extLower)
{
    const std::vector<std::string>& exts = ExtensionList();
    return std::find(exts.begin(), exts.end(), extLower) != exts.end();
}

std::string FilterPattern()
{
    std::string pattern;
    for (const std::string& e : ExtensionList())
    {
        if (!pattern.empty())
            pattern += ";";
        pattern += "*." + e;
    }
    return pattern;
}

std::string FilterDescription()
{
    return "Audio files (" + FilterPattern() + ")";
}

// ============================================================================
// miniaudio path — wav / mp3 / flac / ogg
// ============================================================================
static bool DecodeWithMiniaudio(const std::string& path,
                                unsigned int targetRate,
                                unsigned int targetChannels,
                                std::vector<unsigned char>& outPcm,
                                std::string* err)
{
    std::wstring wpath = Widen(path);
    if (wpath.empty())
    {
        if (err) *err = "that path could not be read";
        return false;
    }

    ma_decoder_config config = ma_decoder_config_init(ma_format_s16, targetChannels, targetRate);

    ma_decoder decoder;
    if (ma_decoder_init_file_w(wpath.c_str(), &config, &decoder) != MA_SUCCESS)
        return false; // not a format miniaudio knows; caller falls through to MF

    const ma_uint32 frameSize = targetChannels * sizeof(ma_int16);
    const ma_uint64 chunkFrames = 4096;
    std::vector<unsigned char> chunk(static_cast<size_t>(chunkFrames) * frameSize);

    outPcm.clear();
    for (;;)
    {
        ma_uint64 framesRead = 0;
        ma_result result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), chunkFrames, &framesRead);
        if (framesRead > 0)
        {
            const size_t bytes = static_cast<size_t>(framesRead) * frameSize;
            outPcm.insert(outPcm.end(), chunk.begin(), chunk.begin() + bytes);
        }
        if (result != MA_SUCCESS || framesRead == 0)
            break;
    }

    ma_decoder_uninit(&decoder);

    if (outPcm.empty())
    {
        if (err) *err = "that file decoded to no audio at all";
        return false;
    }
    return true;
}

// ============================================================================
// Media Foundation path — m4a / aac / wma, and anything else MF happens to know
// ============================================================================
// Moved here verbatim in behaviour from CustomMusicConverter's old inline MP3
// decode. The source reader inserts the resampler and channel converter itself,
// but some sources refuse the channel conversion, so mono output is upmixed by
// hand afterwards.
static bool DecodeWithMediaFoundation(const std::string& path,
                                      unsigned int targetRate,
                                      unsigned int targetChannels,
                                      std::vector<unsigned char>& outPcm,
                                      std::string* err)
{
    std::wstring wpath = Widen(path);
    if (wpath.empty())
    {
        if (err) *err = "that path could not be read";
        return false;
    }

    IMFSourceReader* pReader = NULL;
    HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), NULL, &pReader);
    if (FAILED(hr))
    {
        LogAudio("AudioDecode: MF could not open '%s' (0x%08X)\n", path.c_str(), hr);
        if (err) *err = "your Windows install cannot decode that format";
        return false;
    }

    IMFMediaType* pNativeType = NULL;
    hr = pReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &pNativeType);
    if (FAILED(hr))
    {
        LogAudio("AudioDecode: no audio stream in '%s' (0x%08X)\n", path.c_str(), hr);
        pReader->Release();
        if (err) *err = "that file has no audio stream";
        return false;
    }
    UINT32 nativeRate = 0, nativeChannels = 0;
    pNativeType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &nativeRate);
    pNativeType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &nativeChannels);
    pNativeType->Release();
    LogAudio("AudioDecode: MF '%s' native format: %u Hz, %u ch\n", path.c_str(), nativeRate, nativeChannels);

    IMFMediaType* pPcmType = NULL;
    hr = MFCreateMediaType(&pPcmType);
    if (FAILED(hr)) { pReader->Release(); return false; }
    pPcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pPcmType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pPcmType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, targetRate);
    pPcmType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, targetChannels);
    pPcmType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pPcmType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, targetChannels * 2);
    pPcmType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, targetRate * targetChannels * 2);
    hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pPcmType);
    pPcmType->Release();
    if (FAILED(hr))
    {
        LogAudio("AudioDecode: MF refused the PCM output type (0x%08X)\n", hr);
        pReader->Release();
        if (err) *err = "your Windows install cannot decode that format";
        return false;
    }

    UINT32 pcmChannels = targetChannels;
    {
        IMFMediaType* pCur = NULL;
        if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pCur)))
        {
            UINT32 ch = 0;
            pCur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            if (ch == 1 || ch == 2) pcmChannels = ch;
            pCur->Release();
        }
    }

    outPcm.clear();
    for (;;)
    {
        DWORD flags = 0;
        IMFSample* pSample = NULL;
        hr = pReader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &pSample);
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            if (pSample) pSample->Release();
            break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
        {
            IMFMediaType* pCur = NULL;
            if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pCur)))
            {
                UINT32 ch = 0;
                pCur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                if (ch == 1 || ch == 2) pcmChannels = ch;
                pCur->Release();
            }
        }
        if (pSample)
        {
            IMFMediaBuffer* pBuffer = NULL;
            if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuffer)))
            {
                BYTE* pData = NULL;
                DWORD cbData = 0;
                if (SUCCEEDED(pBuffer->Lock(&pData, NULL, &cbData)))
                {
                    outPcm.insert(outPcm.end(), pData, pData + cbData);
                    pBuffer->Unlock();
                }
                pBuffer->Release();
            }
            pSample->Release();
        }
    }
    pReader->Release();

    // Upmix mono -> stereo (duplicate each sample)
    if (pcmChannels == 1 && targetChannels == 2)
    {
        std::vector<unsigned char> stereo;
        stereo.reserve(outPcm.size() * 2);
        for (size_t i = 0; i + 1 < outPcm.size(); i += 2)
        {
            stereo.push_back(outPcm[i]);
            stereo.push_back(outPcm[i + 1]);
            stereo.push_back(outPcm[i]);
            stereo.push_back(outPcm[i + 1]);
        }
        outPcm.swap(stereo);
    }

    if (outPcm.empty())
    {
        LogAudio("AudioDecode: MF decoded no PCM from '%s'\n", path.c_str());
        if (err) *err = "that file decoded to no audio at all";
        return false;
    }
    return true;
}

bool DecodeFile(const std::string& path,
                unsigned int targetRate,
                unsigned int targetChannels,
                std::vector<unsigned char>& outPcm,
                std::string* err)
{
    if (err) err->clear();

    std::string miniErr;
    if (DecodeWithMiniaudio(path, targetRate, targetChannels, outPcm, &miniErr))
    {
        const double seconds = (double)outPcm.size() / (targetRate * targetChannels * 2);
        LogAudio("AudioDecode: decoded %.1f s from '%s' (miniaudio)\n", seconds, path.c_str());
        return true;
    }
    // A miniaudio failure is not fatal on its own: it also fires for the
    // container formats only MF knows, which is the normal path for .m4a/.wma.
    if (DecodeWithMediaFoundation(path, targetRate, targetChannels, outPcm, err))
    {
        const double seconds = (double)outPcm.size() / (targetRate * targetChannels * 2);
        LogAudio("AudioDecode: decoded %.1f s from '%s' (Media Foundation)\n", seconds, path.c_str());
        return true;
    }

    if (err && err->empty())
        *err = miniErr.empty() ? "that file could not be decoded" : miniErr;
    outPcm.clear();
    return false;
}

// ============================================================================
// Gain and loudness
// ============================================================================
static float AmplitudeToDb(double amplitude)
{
    if (amplitude <= 0.0)
        return kSilenceDb;
    const float db = (float)(20.0 * log10(amplitude));
    return db < kSilenceDb ? kSilenceDb : db;
}

Loudness Analyze(const std::vector<unsigned char>& pcm)
{
    Loudness out;
    out.peakDb = kSilenceDb;
    out.rmsDb = kSilenceDb;

    const size_t count = pcm.size() / sizeof(short);
    if (count == 0)
        return out;

    const short* samples = reinterpret_cast<const short*>(pcm.data());
    int peak = 0;
    double sumSquares = 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        const int s = samples[i];
        const int magnitude = s < 0 ? -s : s;
        if (magnitude > peak)
            peak = magnitude;
        const double norm = (double)s / 32768.0;
        sumSquares += norm * norm;
    }

    out.peakDb = AmplitudeToDb((double)peak / 32768.0);
    out.rmsDb = AmplitudeToDb(sqrt(sumSquares / (double)count));
    return out;
}

void ApplyGainDb(std::vector<unsigned char>& pcm, float gainDb)
{
    if (gainDb == 0.0f || pcm.empty())
        return;

    const double scale = pow(10.0, gainDb / 20.0);
    short* samples = reinterpret_cast<short*>(pcm.data());
    const size_t count = pcm.size() / sizeof(short);
    for (size_t i = 0; i < count; ++i)
    {
        // Saturate. Wrapping here is the difference between a loud track and a
        // track full of clicks.
        double scaled = (double)samples[i] * scale;
        if (scaled > 32767.0) scaled = 32767.0;
        else if (scaled < -32768.0) scaled = -32768.0;
        samples[i] = (short)scaled;
    }
}

float ResolveGainDb(const GainSpec& spec, const std::vector<unsigned char>& pcm)
{
    if (!spec.automatic)
        return spec.manualDb;
    return SuggestGainDb(Analyze(pcm), kDefaultTargetRmsDb);
}

float SuggestGainDb(const Loudness& loudness, float targetRmsDb)
{
    if (loudness.rmsDb <= kSilenceDb)
        return 0.0f;

    float gain = targetRmsDb - loudness.rmsDb;

    // Never normalize a track into clipping. Leave a small amount of headroom
    // so that the WMA encode, which is lossy and can overshoot slightly, has
    // somewhere to go.
    const float kHeadroomDb = 1.0f;
    const float maxGain = -kHeadroomDb - loudness.peakDb;
    if (gain > maxGain)
        gain = maxGain;

    return gain;
}

} // namespace AudioDecode
