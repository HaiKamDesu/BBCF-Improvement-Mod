#pragma once
#include "PaletteCache.h"

#include <atomic>
#include <string>
#include <vector>

// In-game colour slots per character, the range palettes.ini uses as its keys.
#define PALETTE_SLOT_COUNT 24

// palettes.ini's per-character slot assignments, read off disk with the palettes because it
// costs 134ms on a large install (1080 GetPrivateProfileString calls) and that is too much to
// spend on the render thread when someone presses Reload mid-match.
struct PaletteSlotsResult
{
	std::vector<std::vector<std::string> > slots;
	bool loadOnlinePalettes = true;
	bool read = false;
};

// Reads palettes.ini. Pure and thread-safe; PaletteManager::LoadPaletteSettingsFile is a
// synchronous wrapper over this so the match-start path is unchanged.
void PaletteFolderLoader_ReadSlots(PaletteSlotsResult& out);

struct PaletteLoadOutcome
{
	PaletteSet set;
	PaletteSlotsResult slots;

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
// 'forceFullReread' ignores the cache's contents and reads every palette off disk, then
// rewrites the cache. The cache keys each character on its files' path, size and last-write
// time, which cannot notice a rewrite that preserved all three; the Reload button is a user
// explicitly saying "go look at the disk again", so it does not get to make that assumption.
void PaletteFolderLoader_Run(PaletteLoadOutcome& outcome, const std::atomic<bool>* cancel,
	bool forceFullReread = false);
