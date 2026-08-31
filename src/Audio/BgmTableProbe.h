#pragma once
#include <string>
#include <vector>

// TEMPORARY RESEARCH INSTRUMENTATION - not a shipping feature.
//
// Answers the two open questions in docs/Research/BgmReplacementFeasibility.md:
//   1. does the game's BGM reader accept a SUBDIRECTORY in the filename table string?
//   2. is a loaded bank cached across matches, or is the table re-read on every load?
//
// The table at VA 0x9DC650 (.rdata) holds 175 char* entries; indices 0..159 are BGM
// .pac filenames, appended verbatim onto "data/" + "sound/BGM/" at load time. Patching
// an entry redirects the load with no code hook - see the report for the call sites.
namespace BgmTableProbe
{
	static const int kEntryCount = 175;   // 0..159 BGM, 160..174 voice format strings
	static const int kBgmEntryCount = 160;

	// Snapshots all original pointers. Safe to call repeatedly.
	void Initialize();
	bool IsInitialized();

	uintptr_t GetTableAddress();

	// Current / original string for a table index (nullptr if out of range).
	const char* GetEntry(int index);
	const char* GetOriginalEntry(int index);

	// Table index whose original filename matches "<baseName>.pac", else -1.
	int FindIndexByBaseName(const std::string& baseName);

	// Diagnostic reads of every "what is playing" source we know of. None of these is
	// trustworthy on its own - audioMgr+0x1690 holds unrelated values during matches
	// (see MusicManager.cpp:971) - so the checks below target an explicit index instead.
	int GetLiveBgmId();          // audioMgr+0x1690
	int GetMusicSelectId();      // character-select music cursor (g_gameVals.musicSelect_X)
	int GetManagerTrackId();     // MusicManager's own tracked id (needs rotation enabled)

	// Points entry `index` at `relPath` (relative to data/sound/BGM/). The string is
	// owned by the probe and outlives the call. Reads the pointer back and logs it.
	bool PatchEntry(int index, const std::string& relPath);

	bool RestoreEntry(int index);
	void RestoreAll();

	// --- the two scripted checks, against an explicitly chosen table index -----------
	// Copies that entry's .pac into data/Sound/BGM/imtest/ and points the entry at
	// "imtest/<name>.pac". Music surviving a reload => subdirectories work; silence =>
	// the reader rejects them.
	bool RedirectIndexToSubdir(int index, std::string* statusOut);

	// Points the entry at a file that does not exist. Music continuing after a reload
	// => the bank is cached; silence => the table is re-read on every load.
	bool RedirectIndexToMissing(int index, std::string* statusOut);

	// Original filenames of the BGM entries, for the picker.
	const std::vector<std::string>& GetBgmEntryNames();

	// What the last check touched, for the UI to display.
	int GetLastTouchedIndex();
	const char* GetLastTouchedBaseName();
}
