#pragma once
#include <string>
#include <vector>

// Decoding front-end for every user-supplied audio file the mod accepts.
//
// Everything downstream of this header speaks one format only: interleaved
// signed 16-bit PCM at the rate/channel count the caller asks for (in practice
// 44100 Hz stereo, the geometry the XACT converter needs). Two decoders sit
// behind it:
//
//   miniaudio  - wav, mp3, flac, ogg/vorbis. Handles resampling and channel
//                conversion itself, so it always returns the requested layout.
//   Media Foundation - the fallback, which covers the container formats
//                miniaudio does not know (m4a/aac, wma) using whatever codecs
//                the user's Windows install happens to have.
//
// miniaudio is tried first because its coverage is identical on every machine;
// MF's is not, and its failure modes are less legible.
namespace AudioDecode
{
    // Peak and RMS of a decoded buffer, in dBFS (0 dB = full scale, negative
    // below it). Silence reports -INFINITY_DB for both.
    struct Loudness
    {
        float peakDb;
        float rmsDb;
    };

    // Value both Loudness fields take for a fully silent buffer. Not -INFINITY:
    // it has to survive being written to and read back from an ini.
    extern const float kSilenceDb;

    // The RMS level custom tracks are normalized to by default. Provisional -
    // it has not been measured against the game's own BGM yet. See the comment
    // on the definition in AudioDecode.cpp before relying on it.
    extern const float kDefaultTargetRmsDb;

    // How loud a track should end up. `automatic` measures the decoded audio and
    // normalizes it toward kDefaultTargetRmsDb, which is what stops one custom
    // track being twice the volume of the next; `manualDb` is the user's own
    // offset when they have overridden it.
    struct GainSpec
    {
        bool automatic = true;
        float manualDb = 0.0f;
    };

    // The gain `spec` actually resolves to for a decoded buffer, in dB.
    float ResolveGainDb(const GainSpec& spec, const std::vector<unsigned char>& pcm);

    // Decodes `path` to interleaved s16 PCM. Returns false and fills `err` with
    // a user-presentable sentence on failure.
    bool DecodeFile(const std::string& path,
                    unsigned int targetRate,
                    unsigned int targetChannels,
                    std::vector<unsigned char>& outPcm,
                    std::string* err);

    // Scales `pcm` in place by `gainDb`, saturating rather than wrapping. A
    // gain of 0 dB returns immediately without touching the buffer.
    void ApplyGainDb(std::vector<unsigned char>& pcm, float gainDb);

    Loudness Analyze(const std::vector<unsigned char>& pcm);

    // Gain that would bring `loudness` to `targetRmsDb`, clamped so that a very
    // quiet track cannot be pushed into clipping. Returns 0 for silence.
    float SuggestGainDb(const Loudness& loudness, float targetRmsDb);

    // Lowercase, no leading dot, e.g. "mp3". Used by the file picker and the
    // Jukebox folder scan so both agree on what is importable.
    const std::vector<std::string>& SupportedExtensions();
    bool IsSupportedExtension(const std::string& extLower);

    // "Audio files (*.mp3;*.wav;...)" / "*.mp3;*.wav;..." for the file dialog.
    std::string FilterDescription();
    std::string FilterPattern();
}
