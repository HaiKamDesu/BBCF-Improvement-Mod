#include "CustomMusicConverter.h"
#include "AudioDecode.h"
#include "PacFile.h"
#include "ReplayGain.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Core/Settings.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <algorithm>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <vector>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

// ============================================================================
// Logging
// ============================================================================
static void LogCustom(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    LOG(1, "%s", buf);
}

// ============================================================================
// Constants — verified against all 186 native BBCF BGM .pac files
// ============================================================================

// Custom directory relative to the game's working directory
static const char* CUSTOM_DIR_REL = "data/Sound/BGM/custom";
// Output directory for converted .pac files
static const char* BGM_DIR_REL = "data/Sound/BGM";

// All custom tracks are transcoded to 44100 Hz stereo WMA Standard. This is the
// geometry used by 135 of the game's 186 native BGM tracks: WMA mini-format tag
// 3, blockAlign INDEX 6, which the XACT runtime maps to 4459-byte WMA packets.
// (The other native variant is 48 kHz / index 13 / 4096-byte packets; 44.1 kHz
// is the safe universal target, so sources at other rates get resampled.)
static const unsigned int TARGET_RATE = 44100;
static const unsigned int TARGET_CHANNELS = 2;
static const unsigned int WMA_PACKET_SIZE = 4459;      // native blkIdx=6 packet size
static const unsigned int WMA_BLOCK_ALIGN_INDEX = 6;   // WAVEBANKMINIWAVEFORMAT wBlockAlign
static const unsigned int WMA_AVG_BYTES_PER_SEC = 12003; // ~96 kbps CBR, per the encoder's own type list
static const unsigned int MAX_CUE_NAME_LEN = 14;       // cap for GENERATED custom-track names
// Jukebox custom tracks keep the complete, known-working native sound-bank identity.
// The generated c##### filename is only the outer .pac filename used for lookup.
static const char* CUSTOM_JUKEBOX_BANK_NAME = "000_btl_rg";
// The sound bank's two name fields are a fixed 64 bytes each, so a bank/cue name can be
// up to 63 chars + NUL. Native names go up to 23 ("088_btl_bangthem2_short"), which is
// what a replacement bank has to be able to carry.
static const unsigned int MAX_BANK_NAME_LEN = 63;
// Bumped whenever the generated .pac layout OR the audio written into it changes, so
// cached files from an older converter are rebuilt rather than reused. v4 added
// auto-normalized gain, so every v3 cache holds un-normalized audio.
// v5 added the PCM wave-bank format, so the stamp now records which format produced
// the cache; a track built as WMA is not reusable once the format switches, and vice
// versa. See ChooseBankFormat().
static const unsigned int CONVERTER_VERSION = 5;

// ============================================================================
// Wave-bank format selection
// ============================================================================
// The converter can write either format. WMA is ~10x smaller and is what every
// shipped BGM track uses, but producing it needs a PCM->WMAudioV8 encoder MFT,
// and Wine/Proton registers no audio encoder of any kind - so on Linux the WMA
// path cannot run at all.
//
// PCM is the fallback, and it is a real one rather than a guess: the game ships
// 1989 PCM wave-bank entries (every character voice line) and plays them every
// match, so xactengine2_10 handles wFormatTag 0 natively. Voice banks even mix
// PCM and WMA entries, so the runtime dispatches per entry.
// See docs/Research/LinuxWineCompatibility.md.
enum class BankFormat { Wma, Pcm };

static bool HasWmaEncoderMft() {
    // Function-local static, so the probe runs exactly once even though conversions
    // happen on a worker thread.
    static const bool available = []() -> bool {
        // Own MFStartup/MFShutdown pair: this can be asked before any conversion has
        // started. Both are refcounted, so nesting inside a caller's pair is fine.
        if (FAILED(MFStartup(MF_VERSION)))
            return false;

        MFT_REGISTER_TYPE_INFO inType  = { MFMediaType_Audio, MFAudioFormat_PCM };
        MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Audio, MFAudioFormat_WMAudioV8 };
        IMFActivate** activates = NULL;
        UINT32 count = 0;
        const HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_ALL,
                                     &inType, &outType, &activates, &count);
        if (SUCCEEDED(hr)) {
            for (UINT32 i = 0; i < count; ++i)
                if (activates[i]) activates[i]->Release();
            if (activates) CoTaskMemFree(activates);
        }
        MFShutdown();

        const bool found = SUCCEEDED(hr) && count > 0;
        LogCustom("WMA encoder MFT: %s (hr=0x%08lX, %u found)\n",
                  found ? "available" : "NOT available", (unsigned long)hr, count);
        return found;
    }();
    return available;
}


// enum: 0 = Auto, 1 = WMA, 2 = PCM - matches MusicWaveBankFormat in settings.def
// and kMusicWaveBankFormatOptions in SettingsIniWindow.cpp. Keep all three in step.
static BankFormat ChooseBankFormat() {
    switch (Settings::settingsIni.musicWaveBankFormat) {
    case 1:  return BankFormat::Wma;
    case 2:  return BankFormat::Pcm;
    default: return HasWmaEncoderMft() ? BankFormat::Wma : BankFormat::Pcm;
    }
}

static const char* BankFormatName(BankFormat f) {
    return f == BankFormat::Wma ? "WMA" : "PCM";
}

// Sanitize an MP3 basename to lowercase [a-z0-9_] for use inside a cue name.
static std::string SanitizeCueName(const std::string& base) {
    std::string result;
    result.reserve(base.size());
    for (char c : base) {
        if (c >= 'A' && c <= 'Z') {
            result += (char)(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            result += c;
        } else if (c == ' ' || c == '-' || c == '.' || c == '(' || c == ')' || c == '\'' || c == '!' || c == '&') {
            result += '_';
        }
        // else: skip the character
    }
    while (!result.empty() && result.back() == '_') result.pop_back();
    while (!result.empty() && result.front() == '_') result.erase(result.begin());
    if (result.empty()) result = "track";
    return result;
}

// Lowercased extension without the dot, or "" if the name has none.
static std::string GetExtensionLower(const std::string& filename) {
    const size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return std::string();
    std::string extLower;
    for (size_t i = dot + 1; i < filename.size(); ++i)
        extLower += (char)tolower((unsigned char)filename[i]);
    return extLower;
}

// Display name = filename without its extension, for any format we support.
static std::string GetDisplayName(const std::string& filename) {
    const std::string ext = GetExtensionLower(filename);
    if (!ext.empty() && AudioDecode::IsSupportedExtension(ext))
        return filename.substr(0, filename.size() - ext.size() - 1);
    return filename;
}

// ============================================================================
// MP3 -> WMA transcoding via Windows Media Foundation
// ============================================================================
// Decodes the MP3 to PCM (44100 Hz stereo 16-bit, resampling / upmixing as
// needed), then encodes to WMA Standard via the raw WMA encoder MFT. The MFT
// emits one IMFSample per WMA packet; at the selected CBR output type each
// packet is exactly WMA_PACKET_SIZE (4459) bytes — the same geometry as the
// native BBCF tracks. Per-packet decoded-sample counts (from the encoder's
// sample durations) are recorded for the wave bank's seek table.
static bool TranscodeToWma(const std::string& srcPath,
                           float gainDb,
                           float* headroomOut,
                           float* tagGainOut,
                           std::vector<unsigned char>& wmaData,
                           std::vector<unsigned int>& pktSamples,
                           unsigned long long* outTotalSamples) {
    HRESULT hr;
    wmaData.clear();
    pktSamples.clear();
    *outTotalSamples = 0;

    // --- Decode the source file to PCM, then apply the user's gain ---
    // Format support lives entirely in AudioDecode: this function neither knows
    // nor cares whether the source was an mp3, a flac or an ogg by the time it
    // gets here. Gain is baked into the PCM because the game plays the finished
    // .pac through its own XACT engine, where the mod has no volume control at
    // all, so it has to be part of the audio itself.
    std::vector<unsigned char> pcmData;
    std::string decodeError;
    if (!AudioDecode::DecodeFile(srcPath, TARGET_RATE, TARGET_CHANNELS, pcmData, &decodeError)) {
        LogCustom("Decode failed for '%s': %s\n", srcPath.c_str(), decodeError.c_str());
        return false;
    }
    if (headroomOut)
        *headroomOut = -AudioDecode::HeadroomDb(pcmData); // peak -6 dBFS -> +6 dB to spare

    // A file that has been through MP3Gain, foobar2000, loudgain or anything else that
    // writes ReplayGain already knows how far off a sensible level it is, and that number
    // comes from a real loudness measurement. Start from it and let the user's setting be
    // an adjustment on top, rather than ignoring work that has already been done.
    const ReplayGain::Tag tag = ReplayGain::Read(srcPath);
    if (tagGainOut)
        *tagGainOut = tag.found ? tag.trackGainDb : 0.0f;

    const float effectiveGain = (tag.found ? tag.trackGainDb : 0.0f) + gainDb;
    AudioDecode::ApplyGainDb(pcmData, effectiveGain, TARGET_CHANNELS, TARGET_RATE);

    double pcmSeconds = (double)pcmData.size() / (TARGET_RATE * TARGET_CHANNELS * 2);
    LogCustom("Transcode: %.1f s of PCM from '%s' at %+.1f dB (tag %+.1f, offset %+.1f)\n",
        pcmSeconds, srcPath.c_str(), effectiveGain, tag.found ? tag.trackGainDb : 0.0f, gainDb);

    // --- Locate the WMA Standard encoder MFT ---
    IMFTransform* pEncoder = NULL;
    {
        MFT_REGISTER_TYPE_INFO inputType = { MFMediaType_Audio, MFAudioFormat_PCM };
        MFT_REGISTER_TYPE_INFO outputType = { MFMediaType_Audio, MFAudioFormat_WMAudioV8 };
        IMFActivate** ppActivate = NULL;
        UINT32 count = 0;
        hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputType, &outputType, &ppActivate, &count);
        if (FAILED(hr) || count == 0) {
            LogCustom("MF: No WMA Standard encoder MFT found (0x%08X, count=%u)\n", hr, count);
            return false;
        }
        hr = ppActivate[0]->ActivateObject(IID_IMFTransform, (void**)&pEncoder);
        for (UINT32 i = 0; i < count; i++) ppActivate[i]->Release();
        CoTaskMemFree(ppActivate);
        if (FAILED(hr) || !pEncoder) {
            LogCustom("MF: Failed to activate WMA encoder (0x%08X)\n", hr);
            return false;
        }
    }

    // Input type: decoded PCM
    IMFMediaType* pInputType = NULL;
    MFCreateMediaType(&pInputType);
    pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pInputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pInputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, TARGET_RATE);
    pInputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, TARGET_CHANNELS);
    pInputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pInputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, TARGET_CHANNELS * 2);
    pInputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, TARGET_RATE * TARGET_CHANNELS * 2);
    hr = pEncoder->SetInputType(0, pInputType, 0);
    pInputType->Release();
    if (FAILED(hr)) {
        LogCustom("MF: WMA encoder rejected PCM input type (0x%08X)\n", hr);
        pEncoder->Release();
        return false;
    }

    // Output type: enumerate the encoder's offered types and pick the one that
    // matches the native BBCF geometry exactly: 44100 Hz, stereo, 4459-byte
    // packets (~96 kbps CBR). The probe of this machine's encoder shows this
    // type exists (bytesPerSec=12003, blockAlign=4459).
    IMFMediaType* pSelectedOutput = NULL;
    for (DWORD i = 0; i < 256; i++) {
        IMFMediaType* pAvail = NULL;
        hr = pEncoder->GetOutputAvailableType(0, i, &pAvail);
        if (FAILED(hr)) break;
        UINT32 r = 0, ch = 0, blk = 0;
        pAvail->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
        pAvail->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        pAvail->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &blk);
        if (r == TARGET_RATE && ch == TARGET_CHANNELS && blk == WMA_PACKET_SIZE) {
            pSelectedOutput = pAvail;
            break;
        }
        pAvail->Release();
    }
    if (!pSelectedOutput) {
        LogCustom("MF: WMA encoder has no %u Hz / %u-byte-packet output type\n",
            TARGET_RATE, WMA_PACKET_SIZE);
        pEncoder->Release();
        return false;
    }
    hr = pEncoder->SetOutputType(0, pSelectedOutput, 0);
    pSelectedOutput->Release();
    if (FAILED(hr)) {
        LogCustom("MF: Failed to set WMA encoder output type (0x%08X)\n", hr);
        pEncoder->Release();
        return false;
    }

    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    pEncoder->GetOutputStreamInfo(0, &streamInfo);

    hr = pEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        LogCustom("MF: Failed to begin streaming (0x%08X)\n", hr);
        pEncoder->Release();
        return false;
    }

    // --- Feed PCM, collect fixed-size WMA packets ---
    const DWORD chunkSize = TARGET_RATE * TARGET_CHANNELS * 2; // 1 second of PCM
    size_t pcmOffset = 0;
    bool sentEos = false;
    bool finished = false;
    unsigned int nonUniformPackets = 0;

    while (!finished) {
        if (pcmOffset < pcmData.size()) {
            DWORD thisChunk = (DWORD)((pcmData.size() - pcmOffset > chunkSize)
                ? chunkSize : (pcmData.size() - pcmOffset));
            IMFSample* pInSample = NULL;
            IMFMediaBuffer* pInBuf = NULL;
            MFCreateMemoryBuffer(thisChunk, &pInBuf);
            BYTE* pInData = NULL;
            pInBuf->Lock(&pInData, NULL, NULL);
            memcpy(pInData, pcmData.data() + pcmOffset, thisChunk);
            pInBuf->Unlock();
            pInBuf->SetCurrentLength(thisChunk);
            MFCreateSample(&pInSample);
            pInSample->AddBuffer(pInBuf);
            LONGLONG sampleTime = (LONGLONG)pcmOffset * 10000000LL / (TARGET_RATE * TARGET_CHANNELS * 2);
            pInSample->SetSampleTime(sampleTime);
            pInSample->SetSampleDuration((LONGLONG)thisChunk * 10000000LL / (TARGET_RATE * TARGET_CHANNELS * 2));
            hr = pEncoder->ProcessInput(0, pInSample, 0);
            pInSample->Release();
            pInBuf->Release();
            if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
                LogCustom("MF: WMA encoder ProcessInput failed (0x%08X)\n", hr);
                break;
            }
            pcmOffset += thisChunk;
        } else if (!sentEos) {
            pEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            pEncoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            sentEos = true;
        }

        // Drain all available output
        bool gotAny = false;
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER outputBuf = {};
            DWORD status = 0;
            IMFSample* pOutSample = NULL;
            IMFMediaBuffer* pOutBuf = NULL;
            if (!(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                MFCreateSample(&pOutSample);
                MFCreateMemoryBuffer(streamInfo.cbSize ? streamInfo.cbSize : WMA_PACKET_SIZE, &pOutBuf);
                pOutSample->AddBuffer(pOutBuf);
                outputBuf.pSample = pOutSample;
            }
            hr = pEncoder->ProcessOutput(0, 1, &outputBuf, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (pOutSample) pOutSample->Release();
                if (pOutBuf) pOutBuf->Release();
                break;
            }
            if (hr != S_OK) {
                if (pOutSample) pOutSample->Release();
                if (pOutBuf) pOutBuf->Release();
                if (sentEos) finished = true;
                break;
            }
            gotAny = true;

            IMFSample* pResultSample = outputBuf.pSample;
            if (pResultSample) {
                LONGLONG dur100ns = 0;
                pResultSample->GetSampleDuration(&dur100ns);
                unsigned int samples = (unsigned int)(dur100ns * TARGET_RATE / 10000000LL);
                if (samples == 0) samples = 16384; // typical WMA packet duration fallback

                IMFMediaBuffer* pResultBuf = NULL;
                pResultSample->ConvertToContiguousBuffer(&pResultBuf);
                if (pResultBuf) {
                    BYTE* pData = NULL;
                    DWORD cbData = 0;
                    pResultBuf->Lock(&pData, NULL, &cbData);
                    // Enforce the fixed packet geometry the XACT bank declares.
                    // (The CBR encoder produces exactly 4459-byte packets; this
                    // guard keeps a stray size from corrupting the play region.)
                    size_t take = (cbData <= WMA_PACKET_SIZE) ? cbData : WMA_PACKET_SIZE;
                    size_t base = wmaData.size();
                    wmaData.resize(base + WMA_PACKET_SIZE, 0);
                    memcpy(wmaData.data() + base, pData, take);
                    if (cbData != WMA_PACKET_SIZE) nonUniformPackets++;
                    pktSamples.push_back(samples);
                    pResultBuf->Unlock();
                    pResultBuf->Release();
                }
            }
            if (pOutSample) pOutSample->Release();
            if (pOutBuf) pOutBuf->Release();
            if (outputBuf.pSample && outputBuf.pSample != pOutSample) {
                outputBuf.pSample->Release();
            }
        }
        if (sentEos && !gotAny) finished = true;
    }

    pEncoder->Release();

    if (wmaData.empty()) {
        LogCustom("Transcode: no WMA data produced for '%s'\n", srcPath.c_str());
        return false;
    }
    unsigned long long totalSamples = 0;
    for (unsigned int s : pktSamples) totalSamples += s;
    *outTotalSamples = totalSamples;

    if (nonUniformPackets) {
        LogCustom("MF: WARNING — %u packet(s) were not %u bytes (padded/trimmed)\n",
            nonUniformPackets, WMA_PACKET_SIZE);
    }
    LogCustom("MF: Encoded %zu WMA bytes (%zu packets, %.1f s) from '%s'\n",
        wmaData.size(), pktSamples.size(), (double)totalSamples / TARGET_RATE, srcPath.c_str());
    return true;
}

// ============================================================================
// PCM path - no encoder, so nothing here can fail the way the WMA path does
// ============================================================================
// Same front half as TranscodeToWma (decode, then bake the gain into the samples,
// because the game plays the finished .pac through XACT where the mod has no volume
// control) and then simply stops, since the wave bank stores these samples directly.
static bool TranscodeToPcm(const std::string& srcPath,
                           float gainDb,
                           float* headroomOut,
                           float* tagGainOut,
                           std::vector<unsigned char>& pcmData,
                           unsigned long long* outTotalSamples) {
    pcmData.clear();
    *outTotalSamples = 0;

    std::string decodeError;
    if (!AudioDecode::DecodeFile(srcPath, TARGET_RATE, TARGET_CHANNELS, pcmData, &decodeError)) {
        LogCustom("Decode failed for '%s': %s\n", srcPath.c_str(), decodeError.c_str());
        return false;
    }

    // Identical to the WMA path, deliberately: same headroom sign convention, same
    // tag-plus-offset gain, so a track sounds the same whichever format it is built as.
    if (headroomOut)
        *headroomOut = -AudioDecode::HeadroomDb(pcmData); // peak -6 dBFS -> +6 dB to spare

    const ReplayGain::Tag tag = ReplayGain::Read(srcPath);
    if (tagGainOut)
        *tagGainOut = tag.found ? tag.trackGainDb : 0.0f;

    const float effectiveGain = (tag.found ? tag.trackGainDb : 0.0f) + gainDb;
    AudioDecode::ApplyGainDb(pcmData, effectiveGain, TARGET_CHANNELS, TARGET_RATE);

    const size_t frameBytes = TARGET_CHANNELS * 2; // 16-bit
    pcmData.resize((pcmData.size() / frameBytes) * frameBytes); // whole frames only
    *outTotalSamples = pcmData.size() / frameBytes;

    const double seconds = (double)pcmData.size() / (TARGET_RATE * TARGET_CHANNELS * 2);
    LogCustom("PCM: %.1f s from '%s' at %+.1f dB (tag %+.1f, offset %+.1f) - %zu bytes\n",
        seconds, srcPath.c_str(), effectiveGain,
        tag.found ? tag.trackGainDb : 0.0f, gainDb, pcmData.size());
    return *outTotalSamples > 0;
}

// ============================================================================
// PCM wave bank
// ============================================================================
// Same container as the WMA bank; the differences are all in one entry's format
// word plus the absence of a seek table. Verified against the game's own PCM banks
// (docs/Research/LinuxWineCompatibility.md):
//   - wFormatTag = 0, bit 31 = 1 for 16-bit, wBlockAlign = channels * 2
//   - duration * channels * bytesPerSample == PlayRegion.length, without exception
//   - no SeekTables segment: 1985 of the 1989 shipped PCM entries sit in banks that
//     have none, and the other four mark their entry 0xFFFFFFFF. dwFlags still
//     carries 0x00080000, which every shipped bank sets either way.
static std::vector<unsigned char> BuildWaveBankPcm(const std::string& bankName,
                                                   const std::vector<unsigned char>& pcmData,
                                                   unsigned long long durationSamples) {
    const unsigned int blockAlign = TARGET_CHANNELS * 2;
    const unsigned int miniFormat = (0u & 0x3)                                  // PCM
                                  | ((TARGET_CHANNELS & 0x7) << 2)
                                  | ((TARGET_RATE & 0x3FFFF) << 5)
                                  | ((blockAlign & 0xFF) << 23)
                                  | (1u << 31);                                 // 16-bit

    std::vector<unsigned char> bankData(96, 0);
    *(unsigned int*)&bankData[0] = 0x00080000;
    *(unsigned int*)&bankData[4] = 1;
    size_t nameLen = bankName.size();
    if (nameLen > MAX_BANK_NAME_LEN) nameLen = MAX_BANK_NAME_LEN;
    memcpy(&bankData[8], bankName.c_str(), nameLen);
    *(unsigned int*)&bankData[72] = 24; // md_elem_size
    *(unsigned int*)&bankData[76] = 64; // nm_elem_size
    *(unsigned int*)&bankData[80] = 4;  // alignment
    *(unsigned int*)&bankData[84] = 0;  // bank-level miniFormat

    std::vector<unsigned char> entryMeta(24, 0);
    *(unsigned int*)&entryMeta[0]  = (unsigned int)((durationSamples & 0x0FFFFFFFull) << 4);
    *(unsigned int*)&entryMeta[4]  = miniFormat;
    *(unsigned int*)&entryMeta[8]  = 0;                                // PlayRegion offset
    *(unsigned int*)&entryMeta[12] = (unsigned int)pcmData.size();     // PlayRegion length
    *(unsigned int*)&entryMeta[16] = 0;                                // LoopRegion start
    *(unsigned int*)&entryMeta[20] = 0;                                // LoopRegion total

    const unsigned int headerSize = 52;
    const unsigned int seg0_off = headerSize, seg0_len = (unsigned int)bankData.size();
    const unsigned int seg1_off = seg0_off + seg0_len, seg1_len = (unsigned int)entryMeta.size();
    const unsigned int seg2_off = 0, seg2_len = 0; // SeekTables: absent
    const unsigned int seg3_off = 0, seg3_len = 0; // EntryNames: absent
    const unsigned int seg4_off = (seg1_off + seg1_len + 3) & ~3u;
    const unsigned int seg4_len = ((unsigned int)pcmData.size() + 3) & ~3u;

    std::vector<unsigned char> xwb(seg4_off + seg4_len, 0);
    memcpy(&xwb[0], "WBND", 4);
    *(unsigned int*)&xwb[4] = 46;
    *(unsigned int*)&xwb[8] = 44;
    *(unsigned int*)&xwb[12] = seg0_off; *(unsigned int*)&xwb[16] = seg0_len;
    *(unsigned int*)&xwb[20] = seg1_off; *(unsigned int*)&xwb[24] = seg1_len;
    *(unsigned int*)&xwb[28] = seg2_off; *(unsigned int*)&xwb[32] = seg2_len;
    *(unsigned int*)&xwb[36] = seg3_off; *(unsigned int*)&xwb[40] = seg3_len;
    *(unsigned int*)&xwb[44] = seg4_off; *(unsigned int*)&xwb[48] = seg4_len;
    memcpy(&xwb[seg0_off], bankData.data(), bankData.size());
    memcpy(&xwb[seg1_off], entryMeta.data(), entryMeta.size());
    if (!pcmData.empty())
        memcpy(&xwb[seg4_off], pcmData.data(), pcmData.size());
    return xwb;
}

// ============================================================================
// XACT Wave Bank ("WBND") generation — byte-for-byte the native layout
// ============================================================================
// Verified layout (from the game's own files; see tools/analyze_pac_deep.py):
//   +0x00 "WBND", +0x04 tool version (46), +0x08 format version (44)
//   +0x0C..+0x33: five (offset,length) segment pairs:
//       seg0=BankData, seg1=EntryMetaData, seg2=SeekTables,
//       seg3=EntryNames (absent for BGM), seg4=EntryWaveData
//   BankData (96 bytes): dwFlags=0x00080000 (WAVEBANK_FLAGS_SEEKTABLES),
//       dwEntryCount=1, szBankName[64], mdElemSize=24, nmElemSize=64,
//       alignment=4, miniFormat=0, + 8 build-time bytes (left zero)
//   EntryMetaData (24 bytes per entry):
//       d0 = flags(low 4 bits, =0) | Duration_in_samples(high 28 bits)
//       d1 = WAVEBANKMINIWAVEFORMAT — for 44.1 kHz stereo WMA: 0x0315888B
//            (tag=3 WMA, ch=2, rate=44100, blockAlign index=6, bits=0)
//       PlayRegion {offset=0, length = packetCount * 4459}
//       LoopRegion {0,0} (BGM does not loop)
//   SeekTables: [0, packetCount, cumulative decoded PCM bytes after packet i...]
//       (each value = samples-so-far * 4 for 16-bit stereo; the last entry is
//       exactly 4 * Duration — the relation every native file satisfies)
//   EntryWaveData: the concatenated fixed-size WMA packets, padded to 4 bytes.
static std::vector<unsigned char> BuildWaveBank(const std::string& bankName,
                                                const std::vector<unsigned char>& wmaData,
                                                const std::vector<unsigned int>& pktSamples,
                                                unsigned long long durationSamples) {
    // Per-entry WAVEBANKMINIWAVEFORMAT for 44.1 kHz stereo WMA (blkIdx 6).
    unsigned int miniFormat = (3u & 0x3)
                            | ((TARGET_CHANNELS & 0x7) << 2)
                            | ((TARGET_RATE & 0x3FFFF) << 5)
                            | ((WMA_BLOCK_ALIGN_INDEX & 0x1FF) << 23)
                            | (0u << 31);

    unsigned int packetCount = (unsigned int)pktSamples.size();

    // --- SeekTables (seg2) ---
    std::vector<unsigned int> seekTable;
    seekTable.reserve(2 + packetCount);
    seekTable.push_back(0);
    seekTable.push_back(packetCount);
    unsigned long long cumBytes = 0;
    for (unsigned int i = 0; i < packetCount; i++) {
        cumBytes += (unsigned long long)pktSamples[i] * (TARGET_CHANNELS * 2);
        seekTable.push_back((unsigned int)cumBytes);
    }
    unsigned int seg2_len = (unsigned int)(seekTable.size() * sizeof(unsigned int));

    // --- BankData (seg0) — 96 bytes ---
    std::vector<unsigned char> bankData(96, 0);
    *(unsigned int*)&bankData[0] = 0x00080000; // dwFlags = WAVEBANK_FLAGS_SEEKTABLES
    *(unsigned int*)&bankData[4] = 1;          // dwEntryCount = 1
    size_t nameLen = bankName.size();
    if (nameLen > 63) nameLen = 63;
    memcpy(&bankData[8], bankName.c_str(), nameLen);
    *(unsigned int*)&bankData[72] = 24; // md_elem_size
    *(unsigned int*)&bankData[76] = 64; // nm_elem_size
    *(unsigned int*)&bankData[80] = 4;  // alignment
    *(unsigned int*)&bankData[84] = 0;  // bank-level miniFormat (0 in every native file)
    // bankData[88..95]: build-time CRC/timestamp bytes in native files; zero is fine

    // --- EntryMetaData (seg1) — 24 bytes ---
    std::vector<unsigned char> entryMeta(24, 0);
    unsigned int durationField = (unsigned int)(durationSamples & 0x0FFFFFFFull);
    unsigned int d0 = (durationField << 4); // flags in the low 4 bits = 0
    *(unsigned int*)&entryMeta[0] = d0;
    *(unsigned int*)&entryMeta[4] = miniFormat;
    *(unsigned int*)&entryMeta[8] = 0;                                // PlayRegion offset
    *(unsigned int*)&entryMeta[12] = (unsigned int)wmaData.size();    // PlayRegion length
    *(unsigned int*)&entryMeta[16] = 0;                               // LoopRegion start
    *(unsigned int*)&entryMeta[20] = 0;                               // LoopRegion total (no loop)

    // --- Segment layout ---
    unsigned int headerSize = 52; // 4 + 4 + 4 + 5 * 8
    unsigned int seg0_off = headerSize;
    unsigned int seg0_len = (unsigned int)bankData.size(); // 96

    unsigned int seg1_off = seg0_off + seg0_len;
    unsigned int seg1_len = (unsigned int)entryMeta.size(); // 24

    unsigned int seg2_off = seg1_off + seg1_len;

    unsigned int seg3_off = 0; // EntryNames absent (matches native BGM)
    unsigned int seg3_len = 0;

    // EntryWaveData (seg4): the segment length is the packet data padded to a
    // 4-byte boundary (matches native: e.g. PlayRegion 0x3750CF in a 0x3750D0
    // segment). PlayRegion.length above keeps the exact unpadded size.
    unsigned int seg4_off = (seg2_off + seg2_len + 3) & ~3u;
    unsigned int seg4_len = ((unsigned int)wmaData.size() + 3) & ~3u;

    unsigned int totalSize = seg4_off + seg4_len;
    std::vector<unsigned char> xwb(totalSize, 0);

    // Header
    memcpy(&xwb[0], "WBND", 4);
    *(unsigned int*)&xwb[4] = 46; // tool version (matches native)
    *(unsigned int*)&xwb[8] = 44; // format version (matches native)

    // Segment pairs
    *(unsigned int*)&xwb[12] = seg0_off; *(unsigned int*)&xwb[16] = seg0_len;
    *(unsigned int*)&xwb[20] = seg1_off; *(unsigned int*)&xwb[24] = seg1_len;
    *(unsigned int*)&xwb[28] = seg2_off; *(unsigned int*)&xwb[32] = seg2_len;
    *(unsigned int*)&xwb[36] = seg3_off; *(unsigned int*)&xwb[40] = seg3_len;
    *(unsigned int*)&xwb[44] = seg4_off; *(unsigned int*)&xwb[48] = seg4_len;

    // Segment data
    memcpy(&xwb[seg0_off], bankData.data(), bankData.size());
    memcpy(&xwb[seg1_off], entryMeta.data(), entryMeta.size());
    memcpy(&xwb[seg2_off], seekTable.data(), seg2_len);
    memcpy(&xwb[seg4_off], wmaData.data(), wmaData.size());

    return xwb;
}

// ============================================================================
// XACT Sound Bank ("SDBK") generation
// ============================================================================
// xactengine2_10.dll's sound bank validation checks ONLY: size >= 0x8A, magic
// "SDBK", and the format version u16 at +6 == 0x2B (disassembled from the
// DLL). The CRC/timestamp fields are build-time metadata, not verified at
// runtime.
//
// Native BGM .xsb files are a fixed 0x120-byte structure followed by the cue
// name + NUL (total = 0x120 + len + 1), with the name ALSO in two 64-byte
// zero-padded fields at +0x4A and +0x8A, and a u16 at +0x1E holding len+1.
//
// Confirmed against four native banks of differing name length, which are
// byte-identical apart from those fields and a build GUID at +0x08:
//     000_btl_rg (10) = 299    050_btl_rgvsjn (14) = 303
//     950_btl_rgvsjn_old (18) = 307    084_btl_bangthem_short (22) = 311
// i.e. size = 0x120 + len + 1 exactly, and [0x1E] = len + 1 in every one.
//
// The name matters: the game asks its sound bank for a cue BY NAME, and a
// .pac's cue name is always its own base filename. So a bank generated to
// stand in for `008_btl_bn.pac` must carry the cue name `008_btl_bn`.
static std::vector<unsigned char> BuildSoundBank(const std::string& cueName) {
    // Exact 299-byte native XSB extracted directly from working track 000_btl_rg.pac
    static const unsigned char NATIVE_000_XSB[299] = {
        0x53, 0x44, 0x42, 0x4b, 0x2e, 0x00, 0x2b, 0x00, 0x3e, 0x99, 0xa4, 0xd9, 0x20, 0xfa, 0xc5, 0x48,
        0xd7, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x01, 0x01, 0x00, 0x0b, 0x00,
        0x00, 0x00, 0xf5, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x20, 0x01, 0x00, 0x00, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x8a, 0x00, 0x00, 0x00, 0xfa, 0x00,
        0x00, 0x00, 0x1a, 0x01, 0x00, 0x00, 0xca, 0x00, 0x00, 0x00, 0x30, 0x30, 0x30, 0x5f, 0x62, 0x74,
        0x6c, 0x5f, 0x72, 0x67, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x30, 0x5f, 0x62, 0x74,
        0x6c, 0x5f, 0x72, 0x67, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x02, 0x00, 0xb4, 0x00, 0x00,
        0x00, 0x2b, 0x00, 0x01, 0x07, 0x00, 0x01, 0xfb, 0x00, 0x00, 0x00, 0xb4, 0xe4, 0x00, 0x00, 0x00,
        0xc0, 0x5d, 0xe8, 0x03, 0x01, 0x01, 0x00, 0x00, 0x20, 0x00, 0x00, 0xff, 0x0c, 0x00, 0x00, 0x00,
        0xff, 0x00, 0x00, 0x00, 0x00, 0x04, 0xca, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x20, 0x01, 0x00, 0x00, 0xff, 0xff,
        0x30, 0x30, 0x30, 0x5f, 0x62, 0x74, 0x6c, 0x5f, 0x72, 0x67, 0x00,
    };

    // Offsets of the three name slots and the size-driving length field.
    const size_t kFixedPrefix = 0x120;  // everything before the trailing name string
    const size_t kNameFieldA  = 0x4A;
    const size_t kNameFieldB  = 0x8A;
    const size_t kNameFieldSz = 64;
    const size_t kLenField    = 0x1E;   // u16 = name length + 1

    std::string name = cueName;
    if (name.size() > MAX_BANK_NAME_LEN)
        name.resize(MAX_BANK_NAME_LEN);

    // Take the native bank's fixed prefix and regrow the trailing name string.
    std::vector<unsigned char> xsb(NATIVE_000_XSB, NATIVE_000_XSB + kFixedPrefix);

    memset(&xsb[kNameFieldA], 0, kNameFieldSz);
    memcpy(&xsb[kNameFieldA], name.data(), name.size());
    memset(&xsb[kNameFieldB], 0, kNameFieldSz);
    memcpy(&xsb[kNameFieldB], name.data(), name.size());

    *(unsigned short*)&xsb[kLenField] = (unsigned short)(name.size() + 1);

    xsb.insert(xsb.end(), name.begin(), name.end());
    xsb.push_back(0);

    return xsb;
}

// ============================================================================
// FPAC container (.pac) assembly — matches the native file table layout
// ============================================================================
// Verified native layout (see tools/dump_fpac_crc.py):
//   +0x00 "FPAC"; +0x04 dataStart; +0x08 totalSize; +0x0C fileCount(=2);
//   +0x10 = 1; +0x14 nameField
//   File table at +0x20, stride = nameField + 16:
//     entry[i]: name[nameField], index u32, offset u32 (rel. dataStart),
//               size u32, 16 bytes padding
//   Sub-file data: .xsb first at dataStart, .xwb at dataStart + align16(xsbSize)
// Geometry is derived from the name length so it matches the native files for any
// name: nameField = align4(len + 2), stride = align16(nameField + 16). Verified to
// reproduce all 186 shipped .pac files byte-for-byte in the header and file table.
static std::vector<unsigned char> BuildFpacContainer(const std::string& cueName,
                                                     const std::vector<unsigned char>& xsb,
                                                     const std::vector<unsigned char>& xwb) {
    std::string xsbName = cueName + ".xsb";
    std::string xwbName = cueName + ".xwb";

    size_t maxNameLen = (xsbName.size() > xwbName.size()) ? xsbName.size() : xwbName.size();
    // name + NUL + at least one pad byte, 4-aligned. The extra byte matters: with plain
    // align4(len + 1) the 21 shipped files whose name length is already 4-aligned come
    // out 4 bytes short. This formula reproduces the file-table geometry of all 186
    // shipped .pac files exactly.
    unsigned int nameField = (unsigned int)((maxNameLen + 2 + 3) & ~3u);
    if (nameField < 0x10) nameField = 0x10;
    // Native stride is align16(nameField + 16), not nameField + 16 - verified against
    // five shipped .pac files (nameField 16 -> 32; 20/24/28 -> 48). Getting this wrong
    // produced a 4/8-byte-misaligned file table that no native file has, which matters
    // more now that replacement names push nameField past 16.
    unsigned int stride = ((nameField + 16) + 15) & ~15u;
    unsigned int fileCount = 2;
    unsigned int dataStart = 0x20 + stride * fileCount; // 0x60 or 0x80, as in native files

    // Sub-file offsets (relative to dataStart); .xwb starts 16-byte aligned
    unsigned int xsbOffset = 0;
    unsigned int xsbSize = (unsigned int)xsb.size();
    unsigned int xwbOffset = (xsbSize + 0xF) & ~0xFu;
    unsigned int xwbSize = (unsigned int)xwb.size();

    unsigned int totalSize = dataStart + xwbOffset + xwbSize;
    std::vector<unsigned char> pac(totalSize, 0);

    // FPAC header
    memcpy(&pac[0x00], "FPAC", 4);
    *(unsigned int*)&pac[0x04] = dataStart;
    *(unsigned int*)&pac[0x08] = totalSize;
    *(unsigned int*)&pac[0x0C] = fileCount;
    *(unsigned int*)&pac[0x10] = 1;          // constant 1 in every native file
    *(unsigned int*)&pac[0x14] = nameField;

    // File table entry 0: .xsb
    unsigned int e0 = 0x20;
    memcpy(&pac[e0], xsbName.c_str(), xsbName.size());
    *(unsigned int*)&pac[e0 + nameField + 0] = 0;          // index
    *(unsigned int*)&pac[e0 + nameField + 4] = xsbOffset;  // offset
    *(unsigned int*)&pac[e0 + nameField + 8] = xsbSize;    // size

    // File table entry 1: .xwb
    unsigned int e1 = 0x20 + stride;
    memcpy(&pac[e1], xwbName.c_str(), xwbName.size());
    *(unsigned int*)&pac[e1 + nameField + 0] = 1;          // index
    *(unsigned int*)&pac[e1 + nameField + 4] = xwbOffset;  // offset
    *(unsigned int*)&pac[e1 + nameField + 8] = xwbSize;    // size

    // Sub-file data
    memcpy(&pac[dataStart + xsbOffset], xsb.data(), xsb.size());
    memcpy(&pac[dataStart + xwbOffset], xwb.data(), xwb.size());

    return pac;
}

// ============================================================================
// Public API: ConvertCustomMusicOnStartup
// ============================================================================
static unsigned int StableTrackHash(const std::string& filename) {
    unsigned int hash = 2166136261u; // FNV-1a
    for (unsigned char c : filename) {
        hash ^= static_cast<unsigned char>(tolower(c));
        hash *= 16777619u;
    }
    return hash;
}

// Transcode one MP3 and write it out as a game-native .pac whose wave bank, sound bank,
// cue and FPAC sub-files all carry `cueName`. Assumes Media Foundation is already
// started (see ConvertMp3ToPac for the standalone entry point). Writes via a temp file
// so an interrupted run never leaves a torn .pac behind.
static bool ConvertOneTrack(const std::string& srcPath,
                            float gainDb,
                            float* headroomOut,
                            float* tagGainOut,
                            const std::string& outPacPath,
                            const std::string& cueName,
                            unsigned long long* durationSamplesOut,
                            std::string* errorOut) {
    auto fail = [&](const std::string& msg) {
        if (errorOut) *errorOut = msg;
        LogCustom("%s\n", msg.c_str());
        return false;
    };

    const BankFormat format = ChooseBankFormat();
    std::vector<unsigned char> audioData;
    std::vector<unsigned int> pktSamples;
    unsigned long long durationSamples = 0;
    if (format == BankFormat::Wma) {
        if (!TranscodeToWma(srcPath, gainDb, headroomOut, tagGainOut, audioData, pktSamples, &durationSamples))
            return fail("Could not decode '" + srcPath + "'");
    } else {
        if (!TranscodeToPcm(srcPath, gainDb, headroomOut, tagGainOut, audioData, &durationSamples))
            return fail("Could not decode '" + srcPath + "'");
    }
    if (durationSamples == 0 || durationSamples > 0x0FFFFFFFull)
        return fail("Implausible duration for '" + srcPath + "'");

    std::vector<unsigned char> xwb = (format == BankFormat::Wma)
        ? BuildWaveBank(cueName, audioData, pktSamples, durationSamples)
        : BuildWaveBankPcm(cueName, audioData, durationSamples);
    std::vector<unsigned char> xsb = BuildSoundBank(cueName);
    std::vector<unsigned char> pac = BuildFpacContainer(cueName, xsb, xwb);

    const std::string tmpPath = outPacPath + ".tmp";
    HANDLE hOut = CreateFileA(tmpPath.c_str(), GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE) {
        char buf[320];
        sprintf_s(buf, "Could not create '%s' (error %lu)", tmpPath.c_str(), GetLastError());
        return fail(buf);
    }
    DWORD written = 0;
    BOOL ok = WriteFile(hOut, pac.data(), (DWORD)pac.size(), &written, NULL);
    CloseHandle(hOut);
    if (!ok || written != (DWORD)pac.size()) {
        DeleteFileA(tmpPath.c_str());
        char buf[192];
        sprintf_s(buf, "Short write (%lu of %zu bytes)", written, pac.size());
        return fail(buf);
    }
    if (!MoveFileExA(tmpPath.c_str(), outPacPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        DeleteFileA(tmpPath.c_str());
        char buf[320];
        sprintf_s(buf, "Could not move '%s' into place (error %lu)", tmpPath.c_str(), GetLastError());
        return fail(buf);
    }

    if (durationSamplesOut) *durationSamplesOut = durationSamples;
    return true;
}

// Pull one sub-file out of an FPAC container by extension, returning its bytes and its
// base name (the sub-file name minus the extension). Mirrors MusicManager's parser: the
// table stride comes from dataStart, never from nameField.
static bool ExtractPacSubFile(const std::vector<unsigned char>& pac,
                              const char* wantExt,
                              std::vector<unsigned char>* dataOut,
                              std::string* baseNameOut) {
    const size_t size = pac.size();
    if (size < 0x20 || memcmp(pac.data(), "FPAC", 4) != 0) return false;

    auto rd = [&](size_t off) -> unsigned int {
        if (off + 4 > size) return 0;
        return *(const unsigned int*)(pac.data() + off);
    };

    const unsigned int dataStart = rd(0x04);
    const unsigned int fileCount = rd(0x0C);
    const unsigned int nameField = rd(0x14);
    if (dataStart < 0x20 || dataStart > size) return false;
    if (fileCount < 1 || fileCount > 8) return false;
    if (nameField < 4 || nameField > 256) return false;
    const unsigned int stride = (dataStart - 0x20) / fileCount;
    if (stride < nameField + 12) return false;

    const size_t extLen = strlen(wantExt);
    for (unsigned int i = 0; i < fileCount; ++i) {
        const size_t e = 0x20 + (size_t)i * stride;
        if (e + nameField + 12 > size) return false;

        const char* nm = (const char*)pac.data() + e;
        size_t nl = 0;
        while (nl < nameField && nm[nl] != '\0') ++nl;
        if (nl < extLen || _strnicmp(nm + nl - extLen, wantExt, extLen) != 0) continue;

        const unsigned int off = rd(e + nameField + 4);
        const unsigned int sz = rd(e + nameField + 8);
        if (sz == 0 || (size_t)dataStart + off + sz > size) return false;

        dataOut->assign(pac.begin() + dataStart + off, pac.begin() + dataStart + off + sz);
        baseNameOut->assign(nm, nl - extLen);
        return true;
    }
    return false;
}

bool ConvertAudioToReplacementPac(const std::string& srcPath,
                                  float gainDb,
                                  const std::string& originalPacPath,
                                  const std::string& outPacPath,
                                  std::string* errorOut,
                                  float* headroomOut,
                                  float* tagGainOut) {
    auto fail = [&](const std::string& msg) {
        if (errorOut) *errorOut = msg;
        LogCustom("%s\n", msg.c_str());
        return false;
    };

    // Read the shipped track we are standing in for. Its sound bank is reused BYTE FOR
    // BYTE rather than generated: besides the cue name it carries per-track authoring
    // values (a pair of 0/0xFFFF reference fields at +0xFA and near the tail differ
    // between shipped tracks), and copying them is the only way to be sure a replacement
    // behaves exactly like the track it replaces.
    //
    // PacFile::Read unwraps the DFASFPAC envelope when there is one. Anyone running a
    // music mod already has compressed BGM pacs in place of the shipped ones, and asking
    // them to go find pristine originals to build a replacement is not an answer.
    std::vector<unsigned char> originalPac;
    std::string readError;
    if (!PacFile::Read(originalPacPath, originalPac, &readError))
        return fail(readError);

    std::vector<unsigned char> originalXsb;
    std::string baseName;
    if (!ExtractPacSubFile(originalPac, ".xsb", &originalXsb, &baseName))
        return fail("No sound bank inside '" + originalPacPath + "' - not a BGM .pac? ("
            + PacFile::DescribeHeader(originalPac) + ")");

    LogCustom("Replacement for \"%s\": reusing its %zu-byte sound bank verbatim\n",
        baseName.c_str(), originalXsb.size());

    const BankFormat format = ChooseBankFormat();

    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool coInitialized = SUCCEEDED(hrCo);
    // Only the WMA path needs Media Foundation. Requiring it for PCM too would fail the
    // conversion on exactly the systems PCM exists to serve.
    bool mfStarted = false;
    if (format == BankFormat::Wma) {
        if (FAILED(MFStartup(MF_VERSION))) {
            if (coInitialized) CoUninitialize();
            return fail("Media Foundation is unavailable on this system");
        }
        mfStarted = true;
    }

    std::vector<unsigned char> audioData;
    std::vector<unsigned int> pktSamples;
    unsigned long long durationSamples = 0;
    bool ok = (format == BankFormat::Wma)
        ? TranscodeToWma(srcPath, gainDb, headroomOut, tagGainOut, audioData, pktSamples, &durationSamples)
        : TranscodeToPcm(srcPath, gainDb, headroomOut, tagGainOut, audioData, &durationSamples);
    if (ok && (durationSamples == 0 || durationSamples > 0x0FFFFFFFull)) ok = false;

    if (ok) {
        // The wave bank must carry the SAME name, because the reused sound bank refers
        // to it by that name.
        std::vector<unsigned char> xwb = (format == BankFormat::Wma)
            ? BuildWaveBank(baseName, audioData, pktSamples, durationSamples)
            : BuildWaveBankPcm(baseName, audioData, durationSamples);
        std::vector<unsigned char> pac = BuildFpacContainer(baseName, originalXsb, xwb);

        const std::string tmpPath = outPacPath + ".tmp";
        HANDLE hOut = CreateFileA(tmpPath.c_str(), GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hOut == INVALID_HANDLE_VALUE) {
            ok = false;
            if (errorOut) *errorOut = "Could not create the output file";
        } else {
            DWORD written = 0;
            const BOOL wrote = WriteFile(hOut, pac.data(), (DWORD)pac.size(), &written, NULL);
            CloseHandle(hOut);
            if (!wrote || written != (DWORD)pac.size()) {
                DeleteFileA(tmpPath.c_str());
                ok = false;
                if (errorOut) *errorOut = "Short write on the output file";
            } else if (!MoveFileExA(tmpPath.c_str(), outPacPath.c_str(),
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
                DeleteFileA(tmpPath.c_str());
                ok = false;
                if (errorOut) *errorOut = "Could not move the converted file into place";
            } else {
                const int durSec = (int)(durationSamples / TARGET_RATE);
                LogCustom("SUCCESS: '%s' replaces \"%s\" -> '%s' (~%02d:%02d)\n",
                    srcPath.c_str(), baseName.c_str(), outPacPath.c_str(),
                    durSec / 60, durSec % 60);
            }
        }
    } else if (errorOut && errorOut->empty()) {
        *errorOut = "Could not decode '" + srcPath + "'";
    }

    if (mfStarted) MFShutdown();
    if (coInitialized) CoUninitialize();
    return ok;
}

bool ConvertAudioToPac(const std::string& srcPath,
                       float gainDb,
                       const std::string& outPacPath,
                     const std::string& cueName,
                     std::string* errorOut) {
    if (cueName.empty()) {
        if (errorOut) *errorOut = "Empty cue name";
        return false;
    }

    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool coInitialized = SUCCEEDED(hrCo);
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        if (coInitialized) CoUninitialize();
        if (errorOut) *errorOut = "Media Foundation is unavailable on this system";
        return false;
    }

    LogCustom("Converting '%s' -> '%s' (cue \"%s\")\n",
        srcPath.c_str(), outPacPath.c_str(), cueName.c_str());
    const bool ok = ConvertOneTrack(srcPath, gainDb, nullptr, nullptr, outPacPath, cueName, NULL, errorOut);

    MFShutdown();
    if (coInitialized) CoUninitialize();
    return ok;
}

std::vector<CustomTrackInfo> ConvertCustomMusicOnStartup(const CustomMusicProgressCallback& progress,
                                                         const CustomMusicGainLookup& gainLookup) {
    std::vector<CustomTrackInfo> result;

    // Media Foundation's MFT activation needs COM on this thread. The game's
    // main thread usually has it already (XACT/COM), but don't assume: init it
    // defensively and balance it on every exit path. If the thread already has
    // an apartment, CoInitializeEx fails with RPC_E_CHANGED_MODE and we simply
    // use the existing one (and must NOT CoUninitialize it).
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool coInitialized = SUCCEEDED(hrCo);

    // Initialize Media Foundation (refcounted; safe to call repeatedly)
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LogCustom("Failed to initialize Media Foundation (0x%08X) — custom music disabled\n", hr);
        if (coInitialized) CoUninitialize();
        return result;
    }

    // Ensure the custom directory exists
    CreateDirectoryA(GamePath(CUSTOM_DIR_REL).c_str(), NULL);

    // Enumerate the folder once and keep whatever we can decode, rather than
    // globbing per extension - one pass, and the supported list lives in exactly
    // one place (AudioDecode) so the Jukebox and the file picker cannot drift.
    std::vector<std::string> mp3Files;
    {
        // Wide enumeration, names kept as UTF-8. The ANSI walk turned any filename outside
        // the system codepage into question marks - a song named in Japanese on an English
        // install then looked like a file that did not exist.
        std::wstring searchPattern = GamePathW(utf8_to_utf16(std::string(CUSTOM_DIR_REL) + "\\*.*"));
        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                const std::string name = utf16_to_utf8(findData.cFileName);
                if (AudioDecode::IsSupportedExtension(GetExtensionLower(name)))
                    mp3Files.push_back(name);
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }
    }
    if (mp3Files.empty()) {
        LogCustom("No supported audio files found in %s\n", GamePath(CUSTOM_DIR_REL).c_str());
        MFShutdown();
        if (coInitialized) CoUninitialize();
        return result;
    }

    // A cached .pac from an older converter carries a stale cue name, which the play
    // path would then fail to resolve. Rebuild everything when the stamp doesn't match.
    const std::string stampPath = GamePath(std::string(CUSTOM_DIR_REL) + "\\.converter_version");
    bool forceRebuild = true;
    {
        HANDLE hStamp = CreateFileA(stampPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hStamp != INVALID_HANDLE_VALUE) {
            // "<version> <format>". A cache built as WMA is unusable once the format
            // switches to PCM and vice versa, so the format is part of the stamp rather
            // than only the version. An older stamp has no format field and reads as
            // empty, which never matches and so rebuilds - which is what we want.
            char stampBuf[32] = {};
            DWORD got = 0;
            if (ReadFile(hStamp, stampBuf, sizeof(stampBuf) - 1, &got, NULL)) {
                unsigned int stampedVersion = 0;
                char stampedFormat[8] = {};
                sscanf_s(stampBuf, "%u %7s", &stampedVersion,
                         stampedFormat, (unsigned)_countof(stampedFormat));
                forceRebuild = (stampedVersion != CONVERTER_VERSION) ||
                               (strcmp(stampedFormat, BankFormatName(ChooseBankFormat())) != 0);
            }
            CloseHandle(hStamp);
        }
    }
    LogCustom("Converter v%u, wave-bank format %s%s\n", CONVERTER_VERSION,
              BankFormatName(ChooseBankFormat()),
              Settings::settingsIni.musicWaveBankFormat == 0 ? " (auto)" : " (forced by setting)");
    if (forceRebuild)
        LogCustom("Converter version %u - rebuilding every cached .pac\n", CONVERTER_VERSION);

    // Sort for deterministic ID/cue-name assignment across runs
    std::sort(mp3Files.begin(), mp3Files.end());
    LogCustom("Found %d custom audio file(s) in %s\n", (int)mp3Files.size(), GamePath(CUSTOM_DIR_REL).c_str());
    if (progress) progress(0, (int)mp3Files.size(), "Scanning custom music");

    std::vector<int> assignedIds;
    for (size_t fileIndex = 0; fileIndex < mp3Files.size(); ++fileIndex) {
        const std::string& mp3Filename = mp3Files[fileIndex];
        if (progress) progress((int)fileIndex, (int)mp3Files.size(), mp3Filename);
        int trackId = 10000 + (int)(StableTrackHash(mp3Filename) % 90000u);
        while (std::find(assignedIds.begin(), assignedIds.end(), trackId) != assignedIds.end()) {
            trackId = (trackId == 99999) ? 10000 : trackId + 1;
        }
        assignedIds.push_back(trackId);
        std::string displayName = GetDisplayName(mp3Filename);
        std::string sanitized = SanitizeCueName(displayName);

        // Cue name: "c<id>_<name>" capped at 14 chars. The id prefix alone
        // guarantees uniqueness (ids are assigned in sorted-file order).
        if (sanitized.size() > 16) sanitized.resize(16); // bound the sprintf below
        char cueBuf[32];
        sprintf_s(cueBuf, "c%05d_%s", trackId, sanitized.c_str());
        std::string cueName = cueBuf;
        if (cueName.size() > MAX_CUE_NAME_LEN) cueName.resize(MAX_CUE_NAME_LEN);

        std::string pacFilename = cueName; // without .pac extension
        std::string pacPath = GamePath(std::string(BGM_DIR_REL) + "/" + cueName + ".pac");
        std::string mp3Path = GamePath(std::string(CUSTOM_DIR_REL) + "/" + mp3Filename);

        const float gainDb = gainLookup ? gainLookup(mp3Filename) : 0.0f;

        // --- Cache check: reuse an up-to-date .pac ---
        // The volume is baked into the .pac, so a cached one is only good if it was built
        // at the volume the user is asking for now. The gain is recorded beside it; a
        // mismatch reconverts exactly like a newer source file does.
        // The stamp records the EFFECTIVE gain - the file's own ReplayGain plus the
        // user's offset - so retagging a song reconverts it just as changing the offset
        // does, even though neither the timestamp nor the offset moved.
        const ReplayGain::Tag sourceTag = ReplayGain::Read(mp3Path);
        const float effectiveGain = (sourceTag.found ? sourceTag.trackGainDb : 0.0f) + gainDb;

        const std::string gainStampPath = pacPath + ".gain";
        bool gainMatches = true;
        {
            float cachedGain = 0.0f;
            std::ifstream stamp(utf8_to_utf16(gainStampPath));
            if (stamp.is_open())
                stamp >> cachedGain;
            else
                cachedGain = 0.0f; // no stamp means it was built before volumes existed
            gainMatches = (cachedGain == effectiveGain);
        }
        WIN32_FILE_ATTRIBUTE_DATA pacInfo = {}, mp3Info = {};
        bool pacExists = GetFileAttributesExA(pacPath.c_str(), GetFileExInfoStandard, &pacInfo) != 0;
        bool mp3Stat = GetFileAttributesExW(utf8_to_utf16(mp3Path).c_str(),
            GetFileExInfoStandard, &mp3Info) != 0;
        if (!forceRebuild && pacExists && mp3Stat && gainMatches &&
            CompareFileTime(&mp3Info.ftLastWriteTime, &pacInfo.ftLastWriteTime) <= 0) {
            LogCustom("Using cached PAC for '%s' -> %s.pac\n", mp3Filename.c_str(), cueName.c_str());
            CustomTrackInfo info;
            info.id = trackId;
            info.displayName = displayName;
            info.pacFilename = pacFilename;
            info.pacPath = pacPath;
            info.sourceName = mp3Filename;
            info.tagGainDb = sourceTag.found ? sourceTag.trackGainDb : 0.0f;
            info.hasTagGain = sourceTag.found;
            result.push_back(info);
            continue;
        }
        if (pacExists) {
            LogCustom("MP3 '%s' is newer than cached PAC — reconverting\n", mp3Filename.c_str());
        }

        // --- Convert: MP3 -> PCM -> WMA -> XACT banks -> FPAC .pac ---
        // Keep Jukebox custom tracks on the byte-proven native 000_btl_rg bank/cue
        // identity. Rewriting the template bank to the generated c##### name makes
        // XACT reject it in AA_CSoundBank_XACT::AddSoundBank. The outer .pac keeps
        // its unique generated filename, while the internal XSB/XWB stays native.
        unsigned long long durationSamples = 0;
        std::string convertError;
        float tagGain = 0.0f;
        if (!ConvertOneTrack(mp3Path, gainDb, nullptr, &tagGain, pacPath, CUSTOM_JUKEBOX_BANK_NAME, &durationSamples, &convertError)) {
            LogCustom("SKIPPING '%s': %s\n", mp3Filename.c_str(), convertError.c_str());
            continue;
        }

        {
            std::ofstream stamp(utf8_to_utf16(gainStampPath));
            if (stamp.is_open())
                stamp << effectiveGain;
        }

        int durSec = (int)(durationSamples / TARGET_RATE);
        LogCustom("SUCCESS: '%s' -> '%s.pac' (~%02d:%02d)\n",
            mp3Filename.c_str(), cueName.c_str(), durSec / 60, durSec % 60);

        CustomTrackInfo info;
        info.id = trackId;
        info.displayName = displayName;
        info.pacFilename = pacFilename;
        info.pacPath = pacPath;
        info.sourceName = mp3Filename;
        info.tagGainDb = tagGain;
        info.hasTagGain = sourceTag.found;
        result.push_back(info);
    }

    {
        HANDLE hStamp = CreateFileA(stampPath.c_str(), GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hStamp != INVALID_HANDLE_VALUE) {
            char stampBuf[32];
            int n = sprintf_s(stampBuf, "%u %s", CONVERTER_VERSION,
                              BankFormatName(ChooseBankFormat()));
            DWORD written = 0;
            WriteFile(hStamp, stampBuf, (DWORD)n, &written, NULL);
            CloseHandle(hStamp);
        }
    }

    MFShutdown();
    if (coInitialized) CoUninitialize();

    LogCustom("Custom music processing complete: %d track(s) ready\n", (int)result.size());
    if (progress) progress((int)mp3Files.size(), (int)mp3Files.size(), "Custom music ready");
    return result;
}
