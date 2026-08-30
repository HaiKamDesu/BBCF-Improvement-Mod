#pragma once
#include "Game/GhidraDefs.h"
#include "Game/CharData.h"
#include "Game/SnapshotApparatus/SnapshotSlotPool.h"
#include <cstddef>
#include <map>
#define SNAPSHOT_PREALLOC_SIZE  1

class Snapshot {
public:
	char padding[0xa10000]; 
	//btw chardata resides on buf + 0x623E10 for P1 and for p2 buf + 0x623E10 + 0x24978 for training mode THIS IS NOT TRUE, SEEMS TO CHANGE
	
};

//!!!!!!!!!!!!Uncomment this later(and all the functions related to it) when I go back to working on rewind!!!! leaving out for possible crash reasons
//static Snapshot snapshot_replay_pre_allocated[SNAPSHOT_PREALLOC_SIZE]; //keeping this with only one element for now while its not used for any implementation to save space
//!!!!!!!!!!!!!!!!


class SnapshotApparatus {


public:
	unsigned int snapshot_count;
	CharData* p1_ptr;//p1 CharData*
	CharData* p2_ptr; //p2 CharData*
	int last_saved_snapshot_size;
	//p1 and p2 ptrs are used for now to determine when I need to remake the snapshot
	GGPOSessionCallbacks*  callbacks_ptr;
	//Snapshot* p_snapshot_reseve;
	//Snapshot** pp_snapshot_reseve;
	SnapshotApparatus();
	~SnapshotApparatus();

	// Claims `count` slots of the game's ring for this apparatus alone. Until this is called
	// the apparatus behaves exactly as it always has: base 0, the whole ring. Returns false
	// when the ring is full, and the apparatus stays on that shared fallback.
	bool ReserveSlots(const char* owner, int count);

	// Physical ring slot for a logical index inside this apparatus's range. Callers only ever
	// deal in logical indices, so nothing outside needs to know where the range sits.
	int slot_for(unsigned int logicalIndex) const;
	// Logical index this apparatus last saved into, or -1 if it has saved nothing. Pass it
	// straight back to load_snapshot_index to reload that exact state.
	int last_saved_slot() const;

	bool save_snapshot(Snapshot** pbuf);
	// save_snapshot, but into a chosen logical slot instead of the next one in sequence.
	bool save_snapshot_index(int logicalIndex);
	bool save_snapshot_prealloc();
	bool load_snapshot(Snapshot* buf);
	bool load_snapshot_sized(const void* buf, size_t buf_size);
	bool load_snapshot_prealloc(int index);
	bool load_snapshot_index(int index);
	bool check_if_valid(CharData* p1, CharData* p2);
	void clear_count();
	bool clear_framecounts();
	int get_nearest_prealloc_frame(int current_frame, std::map<int, Snapshot*> frame_snap_map);
	int get_last_saved_snapshot_size() const;

private:
	bool save_into_slot(int target_slot, Snapshot** pbuf_mine);

	int slot_base = 0;
	int slot_count = SnapshotSlotPool::kSlotCount;
	bool slots_reserved = false;
};
