#include "CustomMusicConverter.h"
#include "Core/logger.h"

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
static const char* CUSTOM_DIR = "data/Sound/BGM/custom";
// Output directory for converted .pac files
static const char* BGM_DIR = "data/Sound/BGM";

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
// The sound bank's two name fields are a fixed 64 bytes each, so a bank/cue name can be
// up to 63 chars + NUL. Native names go up to 23 ("088_btl_bangthem2_short"), which is
// what a replacement bank has to be able to carry.
static const unsigned int MAX_BANK_NAME_LEN = 63;
// Bumped whenever the generated .pac layout changes, so cached files from an older
// converter are rebuilt instead of being reused with a stale cue name.
static const unsigned int CONVERTER_VERSION = 2;

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

// Display name = MP3 filename without its extension.
static std::string GetDisplayName(const std::string& mp3Filename) {
    std::string name = mp3Filename;
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        std::string ext = name.substr(dot + 1);
        std::string extLower;
        for (char c : ext) extLower += (char)tolower((unsigned char)c);
        if (extLower == "mp3") name = name.substr(0, dot);
    }
    return name;
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
static bool TranscodeMp3ToWma(const std::string& mp3Path,
                              std::vector<unsigned char>& wmaData,
                              std::vector<unsigned int>& pktSamples,
                              unsigned long long* outTotalSamples) {
    HRESULT hr;
    wmaData.clear();
    pktSamples.clear();
    *outTotalSamples = 0;

    // --- Path to wide string ---
    int wlen = MultiByteToWideChar(CP_UTF8, 0, mp3Path.c_str(), -1, NULL, 0);
    if (wlen <= 0) {
        LogCustom("MF: Bad path for '%s'\n", mp3Path.c_str());
        return false;
    }
    std::vector<wchar_t> wpath(wlen);
    MultiByteToWideChar(CP_UTF8, 0, mp3Path.c_str(), -1, wpath.data(), wlen);

    // --- Decode MP3 -> PCM via source reader ---
    IMFSourceReader* pReader = NULL;
    hr = MFCreateSourceReaderFromURL(wpath.data(), NULL, &pReader);
    if (FAILED(hr)) {
        LogCustom("MF: Failed to create source reader for '%s' (0x%08X) — not a decodable MP3?\n",
            mp3Path.c_str(), hr);
        return false;
    }

    IMFMediaType* pNativeType = NULL;
    hr = pReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &pNativeType);
    if (FAILED(hr)) {
        LogCustom("MF: No audio stream in '%s' (0x%08X)\n", mp3Path.c_str(), hr);
        pReader->Release();
        return false;
    }
    UINT32 nativeRate = 0, nativeChannels = 0;
    pNativeType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &nativeRate);
    pNativeType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &nativeChannels);
    pNativeType->Release();
    LogCustom("MF: '%s' native format: %u Hz, %u ch\n", mp3Path.c_str(), nativeRate, nativeChannels);

    // Request 44100 Hz stereo PCM. The source reader inserts the resampler /
    // channel converter automatically when the source differs.
    IMFMediaType* pPcmType = NULL;
    hr = MFCreateMediaType(&pPcmType);
    if (FAILED(hr)) { pReader->Release(); return false; }
    pPcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pPcmType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pPcmType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, TARGET_RATE);
    pPcmType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, TARGET_CHANNELS);
    pPcmType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pPcmType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, TARGET_CHANNELS * 2);
    pPcmType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, TARGET_RATE * TARGET_CHANNELS * 2);
    hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pPcmType);
    pPcmType->Release();
    if (FAILED(hr)) {
        LogCustom("MF: Failed to set PCM output type (0x%08X)\n", hr);
        pReader->Release();
        return false;
    }

    // Read all PCM samples. If the reader still delivers mono (some sources
    // refuse the channel conversion), upmix manually afterwards.
    std::vector<unsigned char> pcmData;
    UINT32 pcmChannels = TARGET_CHANNELS;
    {
        IMFMediaType* pCur = NULL;
        if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pCur))) {
            UINT32 ch = 0;
            pCur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            if (ch == 1 || ch == 2) pcmChannels = ch;
            pCur->Release();
        }
    }
    for (;;) {
        DWORD flags = 0;
        IMFSample* pSample = NULL;
        hr = pReader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &pSample);
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (pSample) pSample->Release();
            break;
        }
        if (flags & 0x00000008) { // MF_SOURCE_READERF_CURRENTMEDIACHANGED
            IMFMediaType* pCur = NULL;
            if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pCur))) {
                UINT32 ch = 0;
                pCur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                if (ch == 1 || ch == 2) pcmChannels = ch;
                pCur->Release();
            }
        }
        if (pSample) {
            IMFMediaBuffer* pBuffer = NULL;
            if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuffer))) {
                BYTE* pData = NULL;
                DWORD cbData = 0;
                if (SUCCEEDED(pBuffer->Lock(&pData, NULL, &cbData))) {
                    pcmData.insert(pcmData.end(), pData, pData + cbData);
                    pBuffer->Unlock();
                }
                pBuffer->Release();
            }
            pSample->Release();
        }
    }
    pReader->Release();

    // Upmix mono -> stereo (duplicate each sample)
    if (pcmChannels == 1) {
        std::vector<unsigned char> stereo;
        stereo.reserve(pcmData.size() * 2);
        for (size_t i = 0; i + 1 < pcmData.size(); i += 2) {
            stereo.push_back(pcmData[i]);
            stereo.push_back(pcmData[i + 1]);
            stereo.push_back(pcmData[i]);
            stereo.push_back(pcmData[i + 1]);
        }
        pcmData.swap(stereo);
    }

    if (pcmData.empty()) {
        LogCustom("MF: No PCM data decoded from '%s'\n", mp3Path.c_str());
        return false;
    }
    double pcmSeconds = (double)pcmData.size() / (TARGET_RATE * TARGET_CHANNELS * 2);
    LogCustom("MF: Decoded %.1f s of PCM from '%s'\n", pcmSeconds, mp3Path.c_str());

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
        LogCustom("MF: No WMA data produced for '%s'\n", mp3Path.c_str());
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
        wmaData.size(), pktSamples.size(), (double)totalSamples / TARGET_RATE, mp3Path.c_str());
    return true;
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
static bool ConvertOneMp3(const std::string& mp3Path,
                          const std::string& outPacPath,
                          const std::string& cueName,
                          unsigned long long* durationSamplesOut,
                          std::string* errorOut) {
    auto fail = [&](const std::string& msg) {
        if (errorOut) *errorOut = msg;
        LogCustom("%s\n", msg.c_str());
        return false;
    };

    std::vector<unsigned char> wmaData;
    std::vector<unsigned int> pktSamples;
    unsigned long long durationSamples = 0;
    if (!TranscodeMp3ToWma(mp3Path, wmaData, pktSamples, &durationSamples))
        return fail("Could not decode '" + mp3Path + "' - is it a valid MP3?");
    if (durationSamples == 0 || durationSamples > 0x0FFFFFFFull)
        return fail("Implausible duration for '" + mp3Path + "'");

    std::vector<unsigned char> xwb = BuildWaveBank(cueName, wmaData, pktSamples, durationSamples);
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

bool ConvertMp3ToReplacementPac(const std::string& mp3Path,
                                const std::string& originalPacPath,
                                const std::string& outPacPath,
                                std::string* errorOut) {
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
    HANDLE hIn = CreateFileA(originalPacPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hIn == INVALID_HANDLE_VALUE) {
        char buf[320];
        sprintf_s(buf, "Could not open original track '%s' (error %lu)",
            originalPacPath.c_str(), GetLastError());
        return fail(buf);
    }
    const DWORD inSize = GetFileSize(hIn, NULL);
    if (inSize == INVALID_FILE_SIZE || inSize == 0) {
        CloseHandle(hIn);
        return fail("Original track '" + originalPacPath + "' is empty or unreadable");
    }
    std::vector<unsigned char> originalPac(inSize);
    DWORD got = 0;
    const BOOL readOk = ReadFile(hIn, originalPac.data(), inSize, &got, NULL);
    CloseHandle(hIn);
    if (!readOk || got != inSize)
        return fail("Short read on original track '" + originalPacPath + "'");

    std::vector<unsigned char> originalXsb;
    std::string baseName;
    if (!ExtractPacSubFile(originalPac, ".xsb", &originalXsb, &baseName))
        return fail("No sound bank inside '" + originalPacPath + "' - not a BGM .pac?");

    LogCustom("Replacement for \"%s\": reusing its %zu-byte sound bank verbatim\n",
        baseName.c_str(), originalXsb.size());

    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool coInitialized = SUCCEEDED(hrCo);
    if (FAILED(MFStartup(MF_VERSION))) {
        if (coInitialized) CoUninitialize();
        return fail("Media Foundation is unavailable on this system");
    }

    std::vector<unsigned char> wmaData;
    std::vector<unsigned int> pktSamples;
    unsigned long long durationSamples = 0;
    bool ok = TranscodeMp3ToWma(mp3Path, wmaData, pktSamples, &durationSamples);
    if (ok && (durationSamples == 0 || durationSamples > 0x0FFFFFFFull)) ok = false;

    if (ok) {
        // The wave bank must carry the SAME name, because the reused sound bank refers
        // to it by that name.
        std::vector<unsigned char> xwb = BuildWaveBank(baseName, wmaData, pktSamples, durationSamples);
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
                    mp3Path.c_str(), baseName.c_str(), outPacPath.c_str(),
                    durSec / 60, durSec % 60);
            }
        }
    } else if (errorOut && errorOut->empty()) {
        *errorOut = "Could not decode '" + mp3Path + "' - is it a valid MP3?";
    }

    MFShutdown();
    if (coInitialized) CoUninitialize();
    return ok;
}

bool ConvertMp3ToPac(const std::string& mp3Path,
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
        mp3Path.c_str(), outPacPath.c_str(), cueName.c_str());
    const bool ok = ConvertOneMp3(mp3Path, outPacPath, cueName, NULL, errorOut);

    MFShutdown();
    if (coInitialized) CoUninitialize();
    return ok;
}

std::vector<CustomTrackInfo> ConvertCustomMusicOnStartup(const CustomMusicProgressCallback& progress) {
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
    CreateDirectoryA(CUSTOM_DIR, NULL);

    // Scan for .mp3 files (case-insensitive on Windows)
    std::string searchPattern = std::string(CUSTOM_DIR) + "\\*.mp3";
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        LogCustom("No custom MP3 files found in %s\n", CUSTOM_DIR);
        MFShutdown();
        if (coInitialized) CoUninitialize();
        return result;
    }

    std::vector<std::string> mp3Files;
    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            mp3Files.push_back(findData.cFileName);
        }
    } while (FindNextFileA(hFind, &findData));
    FindClose(hFind);

    // A cached .pac from an older converter carries a stale cue name, which the play
    // path would then fail to resolve. Rebuild everything when the stamp doesn't match.
    const std::string stampPath = std::string(CUSTOM_DIR) + "\\.converter_version";
    bool forceRebuild = true;
    {
        HANDLE hStamp = CreateFileA(stampPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hStamp != INVALID_HANDLE_VALUE) {
            char stampBuf[32] = {};
            DWORD got = 0;
            if (ReadFile(hStamp, stampBuf, sizeof(stampBuf) - 1, &got, NULL))
                forceRebuild = ((unsigned int)atoi(stampBuf) != CONVERTER_VERSION);
            CloseHandle(hStamp);
        }
    }
    if (forceRebuild)
        LogCustom("Converter version %u - rebuilding every cached .pac\n", CONVERTER_VERSION);

    // Sort for deterministic ID/cue-name assignment across runs
    std::sort(mp3Files.begin(), mp3Files.end());
    LogCustom("Found %d custom MP3 file(s) in %s\n", (int)mp3Files.size(), CUSTOM_DIR);
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
        std::string pacPath = std::string(BGM_DIR) + "/" + cueName + ".pac";
        std::string mp3Path = std::string(CUSTOM_DIR) + "/" + mp3Filename;

        // --- Cache check: reuse an up-to-date .pac ---
        WIN32_FILE_ATTRIBUTE_DATA pacInfo = {}, mp3Info = {};
        bool pacExists = GetFileAttributesExA(pacPath.c_str(), GetFileExInfoStandard, &pacInfo) != 0;
        bool mp3Stat = GetFileAttributesExA(mp3Path.c_str(), GetFileExInfoStandard, &mp3Info) != 0;
        if (!forceRebuild && pacExists && mp3Stat &&
            CompareFileTime(&mp3Info.ftLastWriteTime, &pacInfo.ftLastWriteTime) <= 0) {
            LogCustom("Using cached PAC for '%s' -> %s.pac\n", mp3Filename.c_str(), cueName.c_str());
            CustomTrackInfo info;
            info.id = trackId;
            info.displayName = displayName;
            info.pacFilename = pacFilename;
            info.pacPath = pacPath;
            result.push_back(info);
            continue;
        }
        if (pacExists) {
            LogCustom("MP3 '%s' is newer than cached PAC — reconverting\n", mp3Filename.c_str());
        }

        // --- Convert: MP3 -> PCM -> WMA -> XACT banks -> FPAC .pac ---
        unsigned long long durationSamples = 0;
        std::string convertError;
        if (!ConvertOneMp3(mp3Path, pacPath, cueName, &durationSamples, &convertError)) {
            LogCustom("SKIPPING '%s': %s\n", mp3Filename.c_str(), convertError.c_str());
            continue;
        }

        int durSec = (int)(durationSamples / TARGET_RATE);
        LogCustom("SUCCESS: '%s' -> '%s.pac' (~%02d:%02d)\n",
            mp3Filename.c_str(), cueName.c_str(), durSec / 60, durSec % 60);

        CustomTrackInfo info;
        info.id = trackId;
        info.displayName = displayName;
        info.pacFilename = pacFilename;
        info.pacPath = pacPath;
        result.push_back(info);
    }

    {
        HANDLE hStamp = CreateFileA(stampPath.c_str(), GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hStamp != INVALID_HANDLE_VALUE) {
            char stampBuf[32];
            int n = sprintf_s(stampBuf, "%u", CONVERTER_VERSION);
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
