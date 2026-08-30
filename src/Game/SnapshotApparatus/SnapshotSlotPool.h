#pragma once

#include <string>

/*
	Who owns which of the game's ten savestate slots.

	The game keeps exactly ten rollback slots (SnapshotManager::_saved_states_related_struct[10],
	10.06 MiB each) and every SnapshotApparatus in the mod writes into that same array. Each
	apparatus counts its own saves from zero and picks its slot with `snapshot_count % 10`, so
	every feature independently starts at slot 0 and silently overwrites whatever was there:
	saving a training state while the TAS editor holds a base state put both in slot 0.

	This hands out disjoint ranges instead. A feature reserves the slots it needs, gets a base
	index, and addresses its own slots logically from 0 - so no caller has to know where its
	range physically sits.

	Unreserved apparatuses keep the historical behaviour (base 0, the whole ring), which is what
	the replay-mode consumers still want: ReplayRewind uses the full ring as a rolling checkpoint
	buffer, and it can never be live at the same time as the training-mode features that reserve.
*/
namespace SnapshotSlotPool
{
	// The game's array is a hard ten; see GhidraDefs.h SnapshotManager.
	constexpr int kSlotCount = 10;

	// Reserves `count` contiguous slots. Returns the base index, or -1 if the ring cannot
	// satisfy the request - callers must keep working in that case, just without isolation.
	int Acquire(const char* owner, int count);

	// Returns a previously acquired range. Safe to call with base < 0 (a failed Acquire).
	void Release(int base, int count);

	// "0:tas_base 1-4:tas_keyframes 5:training_states ..." for the log and the debug window.
	std::string DescribeUsage();
}
