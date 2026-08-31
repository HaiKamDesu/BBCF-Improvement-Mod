#pragma once
#include <string>
#include <vector>
#include <functional>

// Information about a discovered custom track (after conversion).
struct CustomTrackInfo {
    int id;                   // Assigned track ID (10000+)
    std::string displayName;  // Human-readable name (from MP3 filename)
    std::string pacFilename;  // e.g. "c10000_my_song" (no .pac extension); also the XACT cue name
    std::string pacPath;      // Full relative path e.g. "data/Sound/BGM/c10000_my_song.pac"
};

// Scan data/Sound/BGM/custom/ for .mp3 files, convert any new ones to
// XACT 2.x-compatible .pac files, and return metadata for all discovered
// custom tracks. Safe to call multiple times (idempotent): an up-to-date
// cached .pac is reused; only new/modified MP3s are transcoded.
//
// The conversion pipeline (all geometry reverse-engineered from the game's own
// 186 BGM .pac files and the behavior of xactengine2_10.dll):
//   1. Enumerate *.mp3 in data/Sound/BGM/custom/
//   2. Decode MP3 -> PCM (44100 Hz, stereo, 16-bit) via Media Foundation
//   3. Encode PCM -> WMA Standard (wmav2) CBR ~96 kbps via the WMA encoder MFT,
//      which emits fixed 4459-byte packets (the exact packet geometry of the
//      native 44.1 kHz BBCF tracks, blockAlign index 6)
//   4. Build the XACT wave bank ("WBND"): BankData + EntryMetaData + SeekTables
//      + wave data, byte-for-byte the native layout
//   5. Build the XACT sound bank ("SDBK") from the native template (the engine
//      validates magic/version/size only), reusing the native "000_btl_rg" cue
//   6. Package both into an FPAC container (.pac), matching native FPAC layout
//
// Returns the list of custom tracks (both freshly converted and cached). On any
// failure the affected track is skipped (logged) and the rest still convert, so
// a bad MP3 can never block the mod from loading.
// Convert a single MP3 into a game-native .pac whose wave bank, sound bank, cue name and
// FPAC sub-file names all carry `cueName`.
//
// The cue name is what makes a .pac usable as a REPLACEMENT for a shipped track: the game
// asks its sound bank for a cue by name, and a native .pac's cue is always its own base
// filename. So standing in for `008_btl_bn.pac` means passing cueName "008_btl_bn".
// Names up to 63 chars are supported (the bank's name fields are 64 bytes); the longest
// native name is 23.
//
// Returns false and fills `errorOut` (if given) on failure, leaving any existing file at
// `outPacPath` untouched.
bool ConvertMp3ToPac(const std::string& mp3Path,
                     const std::string& outPacPath,
                     const std::string& cueName,
                     std::string* errorOut = nullptr);

// Convert an MP3 into a drop-in replacement for a shipped track.
//
// Unlike ConvertMp3ToPac this does not generate a sound bank - it reuses the original
// track's own, byte for byte, and swaps only the wave data. That preserves the cue name
// AND the per-track authoring values baked into it, so the replacement behaves exactly
// like the track it stands in for. Prefer this whenever an original exists.
//
// `originalPacPath` is the shipped .pac (e.g. "data/Sound/BGM/008_btl_bn.pac");
// `outPacPath` should keep that same base filename in a mod-owned folder, since the
// game resolves a cue by the file's base name.
bool ConvertMp3ToReplacementPac(const std::string& mp3Path,
                                const std::string& originalPacPath,
                                const std::string& outPacPath,
                                std::string* errorOut = nullptr);

using CustomMusicProgressCallback = std::function<void(int, int, const std::string&)>;
std::vector<CustomTrackInfo> ConvertCustomMusicOnStartup(
    const CustomMusicProgressCallback& progress = CustomMusicProgressCallback());
