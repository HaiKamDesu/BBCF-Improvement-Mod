#pragma once
#include "PaletteCache.h"

#include <atomic>

struct PaletteLoadOutcome
{
	PaletteSet set;

	int charsFromCache = 0;
	int charsFromDisk = 0;
	int filesRead = 0;      // palette files actually opened this run
	int totalPalettes = 0;  // custom palettes in the result, excluding the "Default" entries
	bool cacheWritten = false;
	bool cancelled = false;
};

// Builds the full palette set from BBCF_IM\Palettes, taking each character from
// PaletteCache.bin when that character's folder is unchanged and reading it off disk
// otherwise, then rewriting the cache if anything had to be read.
//
// Runs on a worker thread, so it deliberately touches no PaletteManager state, no ImGui
// state and no overlay logger - per-character log lines are accumulated into the set and
// replayed on the game thread. The file logger is mutex-protected and safe to use here.
//
// 'cancel' is polled between files; when it flips the run bails out early and sets
// outcome.cancelled, and the partial set must be discarded.
void PaletteFolderLoader_Run(PaletteLoadOutcome& outcome, const std::atomic<bool>* cancel);
