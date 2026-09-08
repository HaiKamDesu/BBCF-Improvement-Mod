#pragma once
#include <Windows.h>

// Signature scanning, split out of HookManager so it can be measured and tested on its own.
//
// Why it matters: hook placement runs in the D3D device wrapper's constructor, on the game's
// main thread, serialized in front of the game's first frame - it cannot be moved off-thread
// (SteamDRM has to have unpacked the exe first, and the hooks have to exist before the game
// runs). One report measured 1.55s there: ~65 hooks, each brute-force scanning the whole
// 23MB BBCF.exe image with no early-out. On a cold boot that also demand-pages the entire
// image off disk. So the scan itself has to be cheap.

struct ScanRange
{
	const unsigned char* base;
	size_t size;
};

#define PATTERN_SCAN_MAX_RANGES 16

// Fills 'out' with the module's executable sections, lowest address first, and returns how
// many it wrote. Falls back to a single whole-image range if the PE headers do not parse.
int PatternScan_ExecutableRanges(HMODULE module, ScanRange* out, int maxRanges);

// The whole mapped image as one range - the pre-section-aware behaviour, kept as a fallback
// for a pattern that does not turn up in the executable sections.
ScanRange PatternScan_WholeImage(HMODULE module);

// True if 'pattern' (with 'mask', 'x' = must match, anything else = wildcard) matches the
// bytes at 'addr'. Cheap: the cost is the pattern length, not the image size.
bool PatternScan_MatchesAt(const unsigned char* addr, const char* pattern, const char* mask);

// First address in 'range' where the pattern matches, or nullptr. Unlike the original loop
// this breaks out of the byte compare on the first mismatch.
const unsigned char* PatternScan_Find(const ScanRange& range, const char* pattern, const char* mask);
