#pragma once
#include "impl_format.h"

#include <string>
#include <vector>

// One character's fully-resolved palette list, as it ends up in
// PaletteManager::m_customPalettes[charIndex] - element 0 is the synthetic "Default"
// placeholder, so indices here are the same indices FindCustomPalIndex returns.
struct PaletteCharSet
{
	// Fingerprint of the character's palette folder: file count plus every entry's
	// relative path, size and last-write time. Anything the user adds, removes, renames
	// or edits changes it, which is what invalidates this character's cached entry.
	unsigned long long keyHash = 0;

	std::vector<IMPL_data_t> palettes;

	// The overlay-log lines this character's load produced ('\n'-separated, already
	// formatted). Cached alongside the data so a cache hit reproduces the exact same
	// log the slow path would have written.
	std::string log;
};

// Indexed by CharIndex; always getCharactersCount() entries.
typedef std::vector<PaletteCharSet> PaletteSet;

// Below this many custom palettes the cache is not worth having: the slow path already
// finishes in well under a second, and a cache would cost more disk than the boot time it
// saves. Only collections big enough for the stall to be noticeable get one.
#define PALETTE_CACHE_MIN_PALETTES 200

// Reads BBCF_IM\PaletteCache.bin into 'out'. Returns false (leaving 'out' untouched) if
// the file is missing, truncated, or was written by a different build/struct layout.
// A false return is never an error the caller has to handle - it just means every
// character has to be read off disk.
bool PaletteCache_Read(PaletteSet& out);

// Writes 'set' to BBCF_IM\PaletteCache.bin via a temp file + atomic replace, so an
// interrupted write can never leave a half-file behind. Returns false on any I/O
// failure; the caller carries on regardless, it only costs the next boot's speedup.
bool PaletteCache_Write(const PaletteSet& set);

// Deletes the cache file. Used when the on-disk cache is known to be unusable.
void PaletteCache_Delete();

// FNV-1a over a byte range, seeded with 'seed' so callers can chain fields into one hash.
unsigned long long PaletteCache_HashBytes(const void* data, size_t len, unsigned long long seed);
