#pragma once

#include <cstdint>
#include <string>

// "Clear cache" for the game folder: finds what the mod (and the game's own crash handler) leave
// lying around and deletes the categories the player picks.
//
// Only files that nothing needs to keep working are listed: logs, crash dumps, update and
// settings backups, caches that rebuild themselves, leftover .tmp files from interrupted writes.
// Never touched: settings.ini, palettes.ini, menus.ini, every other ini, palettes, music packs,
// recordings, training files, rank history, the current DEBUG.txt, the pending update in
// Updater\stage, the game's own data\ and Save\ folders.
//
// Scanning and deleting run on a worker thread; poll IsBusy() from the UI.
namespace ModCleanup
{
	enum Category
	{
		Cat_DebugHistory,
		Cat_DiagnosticLogs,
		Cat_CrashReports,
		Cat_ConfigBackups,
		Cat_OldVersions,
		Cat_OldSaveSnapshots,
		Cat_TempFiles,
		Cat_Caches,
		Cat_DeveloperData,
		Cat_Count
	};

	// English; the UI passes them through L().
	const char* CategoryLabel(Category category);
	const char* CategoryDescription(Category category);

	struct CategoryTotals
	{
		int files = 0;
		uint64_t bytes = 0;
	};

	struct ScanSummary
	{
		bool valid = false;
		CategoryTotals totals[Cat_Count];
	};

	struct ClearSummary
	{
		bool valid = false;
		int deletedFiles = 0;
		uint64_t freedBytes = 0;
		int skippedInUse = 0;   // open in this or another process; tried again next time
		int failed = 0;
	};

	void StartScan();
	// Deletes every file of the selected categories, then rescans.
	void StartClear(const bool selected[Cat_Count]);

	bool IsBusy();
	ScanSummary GetScan();
	ClearSummary GetLastClear();

	// "1.8 GB", "340 KB".
	std::string FormatBytes(uint64_t bytes);
}
