#pragma once
#include <string>

// Reads the loudness tag a file may already carry.
//
// ReplayGain (what MP3Gain, foobar2000, loudgain and most library managers write) records
// how much a track should be turned up or down so that it sits at the same perceived
// loudness as everything else. The number comes from a proper EBU R128 / ITU BS.1770
// measurement - perceptual weighting, not a naive average - which is exactly the thing
// this mod has no business trying to compute itself.
//
// So when a file carries one, it is used as the starting volume, and the user's own slider
// is an offset on top of it. A file with no tag simply starts at zero, as before.
namespace ReplayGain
{
	struct Tag
	{
		bool  found = false;
		float trackGainDb = 0.0f;   // how much to apply, relative to the -18 LUFS reference
		bool  hasPeak = false;
		float trackPeak = 0.0f;     // 1.0 = full scale, from the tag rather than measured
	};

	// Looks for a tag in `path` (UTF-8). Understands the places these actually get
	// written: ID3v2 TXXX frames, APEv2 items, FLAC metadata blocks, and the comment
	// header of Ogg Vorbis and Opus streams. Never throws; a file it cannot make sense of
	// simply comes back with found == false.
	Tag Read(const std::string& path);
}
