#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// A way back to before this mod version touched anything.
//
// The first time a mod version runs, it copies the game's local save data (Save\bbsave.dat,
// bbrd.dat, replay_list.dat) into BBCF_IM\ProfileBackup\preupdate\<version>\ before the game
// has even started. ProfileSync adds the network profile to the same folder the first time it
// downloads it under that version, before that version can upload anything.
//
// Either half can be put back from the Online page:
//   - local save data: scheduled, and copied over Save\ at the next launch from DllMain,
//     before the game reads it. The files it replaces are kept in BBCF_IM\ProfileBackup\replaced\.
//   - the network profile: see ProfileSync::ScheduleSnapshotRestore.
//
// This is a deliberate rollback, so it can undo progress; it exists for the case where an
// update breaks a profile, and it says so before it does anything.
namespace SaveDataSafety
{
	struct Snapshot
	{
		std::string version;       // mod version the snapshot was taken before, e.g. "v8.8"
		std::wstring folder;       // BBCF_IM\ProfileBackup\preupdate\<version>
		SYSTEMTIME takenLocal = {};
		bool hasLocalSave = false;
	};

	// DllMain, PROCESS_ATTACH. Applies a scheduled local restore, then snapshots Save\ for this
	// mod version if that has not happened yet. File operations only: loader lock is held.
	void OnProcessAttach();
	// Writes what OnProcessAttach did to the log, once the log file is open.
	void FlushAttachLog();

	// Newest first.
	std::vector<Snapshot> List();

	std::wstring SnapshotFolder(const std::string& version);

	// Copy that snapshot's local save over Save\ at the next launch.
	bool ScheduleLocalRestore(const std::string& version);
	void CancelLocalRestore();
	// The version a local restore is scheduled from, or empty.
	std::string PendingLocalRestore();
}
