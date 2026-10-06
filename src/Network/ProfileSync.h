#pragma once

#include <string>

// The network-entry profile sync, rewritten.
//
// Entering network mode runs the game's D-Code sync task (FUN_004B0970, outer network
// task 3). It downloads the player's own profile from ArcSys's server and, in vanilla,
// treats EVERY way that download can fail - a dropped session, a slow server, a
// corrupt reply - as "the server has no profile for you". It then asks "Cannot find
// player information. Would you like to create a new profile?", and OK uploads a blank
// profile over the real one straight away. That is the "my profile got reset" bug.
//
// This module replaces that task outright with a C++ reimplementation that behaves the
// same in every state except the failure path:
//   - a failed download is retried (up to 3 times, signing in again first), behind the
//     same "connecting" screen;
//   - if it still fails, the game's own dormant "Failed to retrieve player information.
//     Try again?" dialog is shown instead. Creating a new profile is still possible, but
//     only through Create New and then the "the information on the server will be reset"
//     confirmation;
//   - when the server answers that it has no profile at all and a backup made by this
//     module exists for the same Steam account, the backup is restored and uploaded.
//
// Two guards sit outside the task: the profile upload (FUN_004A96D0) refuses to send a
// failed or unrequested default profile, and the TUS-latch auto-clear in
// NetworkStallDiagnostics holds while the profile did not come from the server.
//
// Why the backup cannot be used to undo a lost match: it is refreshed from every
// confirmed server load and upload, so it never lags behind the server, and it is only
// ever read when the server says it holds nothing for this account - which a player
// cannot make it say. Full RE: docs/Research/ProfileResetPromptInvestigation.md.
namespace ProfileSync
{
	// Patches the task and the upload guard. Called once from placeHooks_bbcf. Does
	// nothing (vanilla behaviour) when ProfileSyncRewrite is off or the bytes at either
	// site are not the ones this was written against.
	void InstallHooks();

	// False while the player's own profile buffer holds something that must never reach
	// the server: a download in progress or failed (the buffer is zeroed), or a default
	// profile the player did not ask for. Always true when the rewrite is not installed.
	bool OwnProfileSafeToUpload();

	// A profile upload just finished successfully. Commits the copy taken when that
	// upload started to the local backup. Any thread.
	void OnProfileUploadConfirmed();

	bool IsInstalled();

	// ---- Pre-update snapshot of the network profile (see Core/SaveDataSafety.h) ----

	// Whether this Steam account has a network profile snapshot from before `version`.
	bool HasSnapshot(const std::string& version);
	// Restore that snapshot the next time the profile loads from the server (the next entry
	// into Network mode), and upload it. The profile it replaces is kept in
	// BBCF_IM\ProfileBackup\replaced. Survives a restart until it has run.
	bool ScheduleSnapshotRestore(const std::string& version);
	void CancelSnapshotRestore();
	// The version a restore is scheduled from, or empty.
	std::string PendingSnapshotRestore();
}
