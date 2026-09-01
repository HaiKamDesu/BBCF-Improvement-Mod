#pragma once
#include <cmath>
#include <string>
#include <vector>

// Decoding front-end for every user-supplied audio file the mod accepts.
//
// Everything downstream of this header speaks one format only: interleaved
// signed 16-bit PCM at the rate/channel count the caller asks for (in practice
// 44100 Hz stereo, the geometry the XACT converter needs). Three decoders sit
// behind it:
//
//   miniaudio  - wav, mp3, flac, ogg/vorbis. Handles resampling and channel
//                conversion itself, so it always returns the requested layout.
//   Opus       - opus (and Opus carried in a .ogg). The Ogg container is
//                unwrapped by hand in AudioDecode.cpp and the packets go to
//                Windows' Opus decoder MFT, because Media Foundation ships the
//                codec but no Ogg demuxer to feed it.
//   Media Foundation - the fallback, which covers the container formats
//                miniaudio does not know (m4a/aac, wma) using whatever codecs
//                the user's Windows install happens to have.
//
// miniaudio is tried first because its coverage is identical on every machine;
// MF's is not, and its failure modes are less legible. Opus sits between them:
// it only claims files that really start with an OpusHead, so everything else
// still ends up at MF with MF's own error.
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


    // Decodes `path` to interleaved s16 PCM. Returns false and fills `err` with
    // a user-presentable sentence on failure.
    bool DecodeFile(const std::string& path,
                    unsigned int targetRate,
                    unsigned int targetChannels,
                    std::vector<unsigned char>& outPcm,
                    std::string* err);

    // Scales `pcm` in place by `gainDb`, holding peaks under full scale with a
    // look-ahead limiter.
    //
    // The limiter matters more than it sounds like it should. Clamping each sample on its
    // own - which is all you can do without looking ahead - reshapes the waveform and is
    // heard as harsh distortion across the whole track. This instead works out how much
    // gain reduction each short block needs, ramps into it BEFORE the loud part arrives
    // and eases back out afterwards, so what changes is the volume envelope rather than
    // the shape of the wave. That is the difference between a track that is louder and a
    // track that is louder and crunchy.
    //
    // `channels` and `rate` describe the buffer, since the ramp times are in milliseconds.
    void ApplyGainDb(std::vector<unsigned char>& pcm, float gainDb,
                     unsigned int channels, unsigned int rate);

    // Peak level of `pcm` in dBFS, i.e. how much gain it can take before anything has to
    // be limited at all. Below this the limiter never engages and the track is untouched.
    float HeadroomDb(const std::vector<unsigned char>& pcm);

    Loudness Analyze(const std::vector<unsigned char>& pcm);


    // Lowercase, no leading dot, e.g. "mp3". Used by the file picker and the
    // Jukebox folder scan so both agree on what is importable.
    const std::vector<std::string>& SupportedExtensions();
    bool IsSupportedExtension(const std::string& extLower);

    // "Audio files (*.mp3;*.wav;...)" / "*.mp3;*.wav;..." for the file dialog.
    std::string FilterDescription();
    std::string FilterPattern();
}
