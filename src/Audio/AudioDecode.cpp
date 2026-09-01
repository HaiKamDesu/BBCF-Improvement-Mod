#include "AudioDecode.h"
#include "Core/logger.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
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


static void LogAudio(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    LOG(1, "%s", buf);
}

// Paths reaching this file are UTF-8 - that is what the file dialog and the folder scan
// both produce - so widening is a UTF-8 conversion, not an ANSI one.
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
// dr_flac and stb_vorbis). ".opus" is demuxed here and decoded by Windows'
// own Opus MFT (see the Opus section below). The rest go through Media
// Foundation, so they depend on the codecs the user's Windows install ships —
// which is why they are listed last and why a failure there reports "your
// Windows install cannot decode".
static const std::vector<std::string>& ExtensionList()
{
    static const std::vector<std::string> exts = {
        "mp3", "wav", "flac", "ogg",    // miniaudio
        "opus",                         // our Ogg demuxer + Windows' Opus MFT
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

// ============================================================================
// Opus path — .opus, and Opus inside a .ogg
// ============================================================================
// Opus needs a special case because neither of the other two decoders can take
// it: miniaudio has no Opus decoder at all, and Media Foundation — despite
// shipping a perfectly good "Microsoft Opus Audio Decoder MFT"
// (C:\Windows\SysWOW64\MSOpusDecoder.dll) — has no Ogg demuxer, so
// MFCreateSourceReaderFromURL on a .opus fails with
// MF_E_UNSUPPORTED_BYTESTREAM_TYPE (0xC00D36C4). Verified on Win10 22H2 from a
// 32-bit process: the registered media sources cover .webm/.mka but nothing
// Ogg-based (which is also why our .ogg support comes from stb_vorbis and not
// from MF).
//
// So the Ogg container is unwrapped here — it is a simple page/segment format,
// a couple of hundred lines — and the raw Opus packets are handed straight to
// the decoder MFT. That is the whole reason libopus/opusfile/libogg are not
// vendored: those are ~250 C source files and ~26 MB for one format, versus
// this file and no new build inputs.
//
// The cost is that the MFT is not present on every Windows install (it arrives
// with the Web Media Extensions component, which consumer Win10 1809+ and Win11
// have but N/LTSC/Server SKUs may not). When it is missing the failure is
// reported with the same sentence as the MF path, which is accurate: the user's
// Windows install genuinely cannot decode it.

// MFAudioFormat_Opus only exists in the Windows 10 SDK; spell it out so this
// file does not acquire an SDK floor. It is the standard media-type GUID for
// WAVE_FORMAT_OPUS (0x704F).
static const GUID kMFAudioFormat_Opus =
    { 0x0000704F, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
// CLSID_MSOpusDecoder — "Microsoft Opus Audio Decoder MFT".
static const CLSID kCLSID_MSOpusDecoder =
    { 0x63E17C10, 0x2D43, 0x4C42, { 0x8F, 0xE3, 0x8D, 0x8B, 0x63, 0xE4, 0x6A, 0x6A } };

static bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out)
{
    std::wstring wpath = Widen(path);
    if (wpath.empty())
        return false;

    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (LONGLONG)0x20000000)
    {
        // Half a gigabyte is far past any plausible BGM track; refusing here
        // keeps a stray huge file from being slurped into a 32-bit address space.
        CloseHandle(h);
        return false;
    }

    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    const BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &read, NULL);
    CloseHandle(h);
    if (!ok || read != out.size())
    {
        out.clear();
        return false;
    }
    return true;
}

// Splits an Ogg bitstream into the packets of its FIRST logical stream. Later
// chained/multiplexed streams are ignored — music files have exactly one.
// `outLastGranule` receives the granule position of the last page seen, which
// for Opus is the total decoded sample count at 48 kHz including the pre-skip.
static bool OggDemux(const std::vector<unsigned char>& file,
                     std::vector<std::vector<unsigned char> >& outPackets,
                     unsigned long long& outLastGranule)
{
    outPackets.clear();
    outLastGranule = 0;

    const size_t n = file.size();
    size_t i = 0;
    bool haveSerial = false;
    unsigned int serial = 0;
    std::vector<unsigned char> partial;

    while (i + 27 <= n)
    {
        if (memcmp(&file[i], "OggS", 4) != 0)
            return false;                       // not an Ogg stream (or corrupt)

        const unsigned char headerType = file[i + 5];
        const unsigned char segCount = file[i + 26];
        if (i + 27 + segCount > n)
            break;

        const unsigned char* segTable = &file[i + 27];
        size_t bodySize = 0;
        for (unsigned int s = 0; s < segCount; ++s)
            bodySize += segTable[s];

        const size_t bodyStart = i + 27 + segCount;
        if (bodyStart + bodySize > n)
            break;                              // truncated final page

        unsigned int pageSerial;
        memcpy(&pageSerial, &file[i + 14], 4);
        if (!haveSerial)
        {
            serial = pageSerial;
            haveSerial = true;
        }

        if (pageSerial == serial)
        {
            unsigned long long granule;
            memcpy(&granule, &file[i + 6], 8);
            if (granule != 0xFFFFFFFFFFFFFFFFull)   // -1 == "no packet finishes here"
                outLastGranule = granule;

            // Bit 0 of the header type says this page continues the packet the
            // previous page left open. If it does not, anything still buffered
            // was a torn packet and is dropped.
            if ((headerType & 0x01) == 0)
                partial.clear();

            size_t off = bodyStart;
            for (unsigned int s = 0; s < segCount; ++s)
            {
                partial.insert(partial.end(), file.begin() + off, file.begin() + off + segTable[s]);
                off += segTable[s];
                if (segTable[s] < 255)          // a segment < 255 bytes ends the packet
                {
                    outPackets.push_back(std::vector<unsigned char>());
                    outPackets.back().swap(partial);
                    partial.clear();
                }
            }
        }

        i = bodyStart + bodySize;
    }

    return !outPackets.empty();
}

static bool DecodeWithOpus(const std::string& path,
                           unsigned int targetRate,
                           unsigned int targetChannels,
                           std::vector<unsigned char>& outPcm,
                           std::string* err)
{
    // --- Is this actually an Opus stream? -----------------------------------
    // Everything up to here fails silently (returning false without touching
    // `err`), exactly like the miniaudio probe, so a non-Opus file falls
    // through to Media Foundation with its own error intact.
    std::vector<unsigned char> file;
    if (!ReadWholeFile(path, file))
        return false;

    std::vector<std::vector<unsigned char> > packets;
    unsigned long long lastGranule = 0;
    if (!OggDemux(file, packets, lastGranule))
        return false;
    if (packets.size() < 2 || packets[0].size() < 19 ||
        memcmp(packets[0].data(), "OpusHead", 8) != 0)
        return false;

    const unsigned int srcChannels = packets[0][9];
    unsigned short preSkip = 0;
    memcpy(&preSkip, &packets[0][10], 2);
    if (srcChannels == 0 || srcChannels > 8)
        return false;

    file.clear();       // the packets own their bytes now
    std::vector<unsigned char>().swap(file);

    // From here on it IS an Opus file, so every failure is reported.
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(hrCo);
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        if (coInitialized) CoUninitialize();
        if (err) *err = "your Windows install cannot decode that format";
        return false;
    }

    bool ok = false;
    IMFTransform* pMft = NULL;
    ma_data_converter converter;
    bool converterReady = false;

    do
    {
        hr = CoCreateInstance(kCLSID_MSOpusDecoder, NULL, CLSCTX_INPROC_SERVER,
                              IID_IMFTransform, (void**)&pMft);
        if (FAILED(hr) || !pMft)
        {
            LogAudio("AudioDecode: no Opus decoder MFT on this system (0x%08X)\n", hr);
            if (err) *err = "your Windows install cannot decode that format";
            break;
        }

        // The MFT wants the raw OpusHead (magic included) as its codec-private
        // blob — the same bytes a Matroska CodecPrivate would carry.
        IMFMediaType* pIn = NULL;
        if (FAILED(MFCreateMediaType(&pIn)) || !pIn)
        {
            if (err) *err = "that file could not be decoded";
            break;
        }
        pIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pIn->SetGUID(MF_MT_SUBTYPE, kMFAudioFormat_Opus);
        pIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, srcChannels);
        pIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);  // Opus always decodes at 48 kHz
        pIn->SetBlob(MF_MT_USER_DATA, packets[0].data(), (UINT32)packets[0].size());
        hr = pMft->SetInputType(0, pIn, 0);
        pIn->Release();
        if (FAILED(hr))
        {
            LogAudio("AudioDecode: Opus MFT rejected the input type (0x%08X, %u ch)\n", hr, srcChannels);
            if (err) *err = "your Windows install cannot decode that format";
            break;
        }

        // Whatever the MFT offers first: in practice 48 kHz float, source
        // channel count. Both float and s16 are handled below so a different
        // Windows build offering PCM instead does not break this.
        IMFMediaType* pOut = NULL;
        hr = pMft->GetOutputAvailableType(0, 0, &pOut);
        if (FAILED(hr) || !pOut)
        {
            if (err) *err = "your Windows install cannot decode that format";
            break;
        }
        GUID outSubtype = GUID_NULL;
        UINT32 outRate = 48000, outChannels = srcChannels, outBits = 32;
        pOut->GetGUID(MF_MT_SUBTYPE, &outSubtype);
        pOut->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &outRate);
        pOut->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &outChannels);
        pOut->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &outBits);
        hr = pMft->SetOutputType(0, pOut, 0);
        pOut->Release();
        if (FAILED(hr))
        {
            LogAudio("AudioDecode: Opus MFT rejected its own output type (0x%08X)\n", hr);
            if (err) *err = "your Windows install cannot decode that format";
            break;
        }

        const bool outIsFloat = (outSubtype == MFAudioFormat_Float);
        const bool outIsPcm16 = (outSubtype == MFAudioFormat_PCM && outBits == 16);
        if ((!outIsFloat && !outIsPcm16) || outChannels == 0 || outRate == 0)
        {
            LogAudio("AudioDecode: Opus MFT offered an unusable output format (%u bits, %u ch)\n",
                     outBits, outChannels);
            if (err) *err = "your Windows install cannot decode that format";
            break;
        }
        const size_t srcFrameSize = outChannels * (outIsFloat ? 4 : 2);

        // One converter does the rate change (48000 -> targetRate), the channel
        // conversion and the float -> s16 narrowing, so nothing has to hold the
        // whole float track in memory at once.
        ma_data_converter_config cc = ma_data_converter_config_init(
            outIsFloat ? ma_format_f32 : ma_format_s16, ma_format_s16,
            outChannels, targetChannels, outRate, targetRate);
        if (ma_data_converter_init(&cc, NULL, &converter) != MA_SUCCESS)
        {
            if (err) *err = "that file could not be decoded";
            break;
        }
        converterReady = true;

        MFT_OUTPUT_STREAM_INFO osi;
        memset(&osi, 0, sizeof(osi));
        pMft->GetOutputStreamInfo(0, &osi);
        const bool mftAllocates = (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        const DWORD outBufBytes = osi.cbSize ? osi.cbSize : (DWORD)(64 * 1024);
        const size_t dstFrameSize = targetChannels * sizeof(short);

        pMft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);

        outPcm.clear();
        unsigned long long framesToSkip = preSkip;      // gapless: encoder priming
        std::vector<unsigned char> convBuf(16384 * targetChannels * sizeof(short));

        // Drains everything the MFT is currently holding, converting as it goes.
        struct Drain
        {
            static void Run(IMFTransform* mft, bool mftAllocates, DWORD bufBytes,
                            size_t srcFrameSize, size_t dstFrameSize,
                            ma_data_converter* conv,
                            unsigned long long* framesToSkip,
                            std::vector<unsigned char>& convBuf,
                            std::vector<unsigned char>& out)
            {
                for (;;)
                {
                    IMFSample* pOutSample = NULL;
                    IMFMediaBuffer* pOutBuf = NULL;
                    if (!mftAllocates)
                    {
                        if (FAILED(MFCreateMemoryBuffer(bufBytes, &pOutBuf))) return;
                        if (FAILED(MFCreateSample(&pOutSample))) { pOutBuf->Release(); return; }
                        pOutSample->AddBuffer(pOutBuf);
                    }

                    MFT_OUTPUT_DATA_BUFFER odb;
                    memset(&odb, 0, sizeof(odb));
                    odb.pSample = pOutSample;
                    DWORD status = 0;
                    HRESULT h = mft->ProcessOutput(0, 1, &odb, &status);
                    if (odb.pEvents) { odb.pEvents->Release(); odb.pEvents = NULL; }
                    if (FAILED(h))
                    {
                        // MF_E_TRANSFORM_NEED_MORE_INPUT is the normal exit.
                        if (odb.pSample) odb.pSample->Release();
                        if (pOutBuf) pOutBuf->Release();
                        return;
                    }

                    IMFSample* pGot = odb.pSample;
                    IMFMediaBuffer* pGotBuf = NULL;
                    if (pGot && SUCCEEDED(pGot->ConvertToContiguousBuffer(&pGotBuf)))
                    {
                        BYTE* pData = NULL;
                        DWORD cb = 0;
                        if (SUCCEEDED(pGotBuf->Lock(&pData, NULL, &cb)))
                        {
                            unsigned long long frames = cb / srcFrameSize;
                            const unsigned char* src = pData;
                            if (*framesToSkip > 0)
                            {
                                const unsigned long long skip =
                                    (*framesToSkip < frames) ? *framesToSkip : frames;
                                src += (size_t)(skip * srcFrameSize);
                                frames -= skip;
                                *framesToSkip -= skip;
                            }
                            while (frames > 0)
                            {
                                ma_uint64 inFrames = frames;
                                ma_uint64 outFrames = convBuf.size() / dstFrameSize;
                                if (ma_data_converter_process_pcm_frames(
                                        conv, src, &inFrames, convBuf.data(), &outFrames) != MA_SUCCESS)
                                    break;
                                if (outFrames > 0)
                                    out.insert(out.end(), convBuf.begin(),
                                               convBuf.begin() + (size_t)(outFrames * dstFrameSize));
                                if (inFrames == 0 && outFrames == 0)
                                    break;                      // no progress; bail out
                                src += (size_t)(inFrames * srcFrameSize);
                                frames -= inFrames;
                            }
                            pGotBuf->Unlock();
                        }
                        pGotBuf->Release();
                    }
                    if (pGot) pGot->Release();
                    if (pOutBuf) pOutBuf->Release();
                }
            }
        };

        // The first two packets are the OpusHead and OpusTags headers.
        for (size_t p = 2; p < packets.size(); ++p)
        {
            if (packets[p].empty())
                continue;

            IMFMediaBuffer* pInBuf = NULL;
            if (FAILED(MFCreateMemoryBuffer((DWORD)packets[p].size(), &pInBuf)))
                break;
            BYTE* pDst = NULL;
            if (SUCCEEDED(pInBuf->Lock(&pDst, NULL, NULL)))
            {
                memcpy(pDst, packets[p].data(), packets[p].size());
                pInBuf->Unlock();
            }
            pInBuf->SetCurrentLength((DWORD)packets[p].size());

            IMFSample* pInSample = NULL;
            if (FAILED(MFCreateSample(&pInSample))) { pInBuf->Release(); break; }
            pInSample->AddBuffer(pInBuf);

            hr = pMft->ProcessInput(0, pInSample, 0);
            if (hr == MF_E_NOTACCEPTING)
            {
                Drain::Run(pMft, mftAllocates, outBufBytes, srcFrameSize, dstFrameSize,
                           &converter, &framesToSkip, convBuf, outPcm);
                hr = pMft->ProcessInput(0, pInSample, 0);
            }
            pInSample->Release();
            pInBuf->Release();
            if (FAILED(hr))
            {
                LogAudio("AudioDecode: Opus MFT rejected packet %zu (0x%08X)\n", p, hr);
                break;
            }

            Drain::Run(pMft, mftAllocates, outBufBytes, srcFrameSize, dstFrameSize,
                       &converter, &framesToSkip, convBuf, outPcm);
        }

        pMft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        pMft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        Drain::Run(pMft, mftAllocates, outBufBytes, srcFrameSize, dstFrameSize,
                   &converter, &framesToSkip, convBuf, outPcm);
        pMft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);

        // Gapless tail: the final granule position is the total 48 kHz sample
        // count including the pre-skip, so anything past it is encoder padding.
        if (lastGranule > preSkip)
        {
            const unsigned long long srcFrames = lastGranule - preSkip;
            const unsigned long long wantFrames =
                (srcFrames * targetRate + 24000) / 48000;
            const size_t wantBytes = (size_t)(wantFrames * dstFrameSize);
            if (wantBytes > 0 && wantBytes < outPcm.size())
                outPcm.resize(wantBytes);
        }

        if (outPcm.empty())
        {
            if (err) *err = "that file decoded to no audio at all";
            break;
        }
        ok = true;
    } while (false);

    if (converterReady)
        ma_data_converter_uninit(&converter, NULL);
    if (pMft)
        pMft->Release();
    MFShutdown();
    if (coInitialized)
        CoUninitialize();

    if (!ok)
        outPcm.clear();
    return ok;
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

    // Opus goes before the MF fallback because MF has no Ogg demuxer and would
    // only report a useless "unsupported bytestream". Sniffs for OpusHead and
    // returns false without touching `err` if the file is not Opus, so the MF
    // error below still wins for everything else. This also picks up an Opus
    // stream that was named .ogg, which miniaudio's stb_vorbis cannot read.
    std::string opusErr;
    if (DecodeWithOpus(path, targetRate, targetChannels, outPcm, &opusErr))
    {
        const double seconds = (double)outPcm.size() / (targetRate * targetChannels * 2);
        LogAudio("AudioDecode: decoded %.1f s from '%s' (Opus MFT)\n", seconds, path.c_str());
        return true;
    }
    if (!opusErr.empty())
    {
        // It *was* an Opus file and decoding it genuinely failed; MF cannot do
        // any better, so report that rather than its bytestream complaint.
        if (err) *err = opusErr;
        outPcm.clear();
        return false;
    }

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

// Limiter shape. These follow the usual look-ahead limiter design: analyse ahead of the
// signal, fade the gain reduction IN before the peak arrives rather than dropping onto it,
// and let it back out slowly enough not to pump. Dropping gain instantly - which is all a
// per-sample clamp can do - is what turns a loud track into a distorted one.
static const double kCeiling = 0.977;         // -0.2 dBFS, a little room for the encoder
static const size_t kBlockFrames = 16;        // envelope resolution, ~0.36 ms at 44.1 kHz
static const double kLookaheadMs = 5.0;       // how far ahead peaks are seen
static const double kReleaseMs = 200.0;       // slow enough not to pump on sustained loud parts

float HeadroomDb(const std::vector<unsigned char>& pcm)
{
    return Analyze(pcm).peakDb;
}

void ApplyGainDb(std::vector<unsigned char>& pcm, float gainDb,
                 unsigned int channels, unsigned int rate)
{
    if (gainDb == 0.0f || pcm.empty() || channels == 0 || rate == 0)
        return;

    const double scale = pow(10.0, gainDb / 20.0);
    short* samples = reinterpret_cast<short*>(pcm.data());
    const size_t frameCount = (pcm.size() / sizeof(short)) / channels;
    if (frameCount == 0)
        return;

    // The envelope is per block, not per sample: a four minute track is over ten million
    // frames and a float each would be tens of megabytes in a 32-bit process. At 0.36 ms a
    // block the ramps below still span dozens of blocks, so the curve is smooth.
    const size_t blockCount = (frameCount + kBlockFrames - 1) / kBlockFrames;
    std::vector<float> gain;
    try
    {
        gain.resize(blockCount);
    }
    catch (const std::bad_alloc&)
    {
        return; // leave the track alone rather than half-processing it
    }

    // What each block needs on its own, before any shaping.
    for (size_t b = 0; b < blockCount; b++)
    {
        const size_t first = b * kBlockFrames;
        const size_t last = (std::min)(first + kBlockFrames, frameCount);

        int peak = 0;
        for (size_t f = first; f < last; f++)
        {
            for (unsigned int c = 0; c < channels; c++)
            {
                const int value = samples[f * channels + c];
                const int magnitude = value < 0 ? -value : value;
                if (magnitude > peak)
                    peak = magnitude;
            }
        }

        const double scaled = ((double)peak / 32768.0) * scale;
        gain[b] = (scaled > kCeiling) ? (float)(kCeiling / scaled) : 1.0f;
    }

    // Fade in, backwards. Each block may be no louder than the one after it plus one step,
    // so the gain slides down over the whole look-ahead window and is already where it
    // needs to be when the peak lands. This is the part that keeps transients clean: the
    // reduction is a ramp, not a step.
    size_t lookaheadBlocks = (size_t)((kLookaheadMs * 0.001 * rate) / kBlockFrames);
    if (lookaheadBlocks < 1) lookaheadBlocks = 1;
    const float attackStep = 1.0f / (float)lookaheadBlocks;
    for (size_t b = blockCount - 1; b > 0; b--)
    {
        const float ceiling = gain[b] + attackStep;
        if (gain[b - 1] > ceiling)
            gain[b - 1] = ceiling;
    }

    // Release, forwards, and exponential rather than linear - a straight line back up is
    // audible as the level climbing, where an exponential curve moves quickly at first and
    // then settles, which is what the ear expects a room to do.
    size_t releaseBlocks = (size_t)((kReleaseMs * 0.001 * rate) / kBlockFrames);
    if (releaseBlocks < 1) releaseBlocks = 1;
    const float releaseCoef = (float)exp(-1.0 / (double)releaseBlocks);
    float running = gain[0];
    for (size_t b = 0; b < blockCount; b++)
    {
        if (gain[b] < running)
            running = gain[b];                                   // follow it down at once
        else
            running = gain[b] + (running - gain[b]) * releaseCoef; // ease back up
        gain[b] = running;
    }

    // Apply, interpolating across each block so the gain never steps.
    for (size_t f = 0; f < frameCount; f++)
    {
        const size_t b = f / kBlockFrames;
        const size_t next = (b + 1 < blockCount) ? b + 1 : b;
        const float mix = (float)(f % kBlockFrames) / (float)kBlockFrames;
        const double envelope = gain[b] + (gain[next] - gain[b]) * mix;
        const double total = scale * envelope;

        for (unsigned int c = 0; c < channels; c++)
        {
            // Round rather than truncate; truncation on every sample is a DC-ish bias and
            // its own small distortion.
            double value = (double)samples[f * channels + c] * total;
            value = value < 0.0 ? value - 0.5 : value + 0.5;
            if (value > 32767.0) value = 32767.0;
            else if (value < -32768.0) value = -32768.0;
            samples[f * channels + c] = (short)value;
        }
    }
}

} // namespace AudioDecode
