#pragma once

// The avatar fields the game derives its network profile blob FROM, rather than the
// derived copy inside the blob itself.
//
// Read the "avatar SOURCE structs" block in src/Game/GhidraDefs.h before touching this.
// The short version, all RE'd 2026-09-08:
//
// The four avatar values exist twice. The copies inside the profile blob (what
// ProfileBlobSeal and the Online page's sliders edit) are a cache: the game rebuilds
// them from CSaveDataManager whenever it refreshes the profile, so a value poked only
// into the blob is put back the moment that happens. The originals live in the save
// data manager, a statically allocated singleton:
//
//     icon        savemgr + 0x6388     stored as icon + 1; the game clamps it itself
//     colour      savemgr + 0x638C
//     accessory1  savemgr + 0x63A0 + 0x9A4    slot A, feeds blob byte 0x61C4
//     accessory2  savemgr + 0x7304 + 0x9A4    slot B, feeds blob byte 0x61C5
//
// Writing here instead of the blob matters for two reasons.
//
// It is what stops the game undoing us. An accessory the game's own equip menu cannot
// offer is absent from both sources, so a rebuild re-derives the old value over the blob,
// and a re-apply that only pokes the blob is in an unbounded fight with the game. Note
// that these fields are not themselves persistent: measured 2026-09-08, they start every
// launch as zeros and are filled by the game copying the downloaded profile in, so writing
// them lasts for the session and no longer.
//
// And it cannot break anything. The blob is checksummed and uploaded to Steam; a write
// into it that lands between the upload path's seal and the upload strategy's re-verify
// fails the step without Steam ever being contacted, and lobby entry is gated behind
// that step. That is what made a reporter's connects take 19-120s each. The source
// structs are plain save state -- nothing checksums them, nothing uploads them, and the
// game itself writes them from its equip menu -- so a write here can never fail a
// handshake no matter when it lands. The game reads the source and rebuilds, seals and
// uploads the blob on its own schedule, with no window for us to land inside.
//
// Every write is range-checked first. The ids reach an unbounded [table + id*4] index in
// the accessory draw path, so an out-of-range value there would be an out-of-bounds read;
// see the MAX_* / COUNT_AvtAccPointEntries note in GhidraDefs.h for why the limits below
// are the right ones.
namespace LobbyAvatarSource
{
	struct Values
	{
		int icon = -1;
		int color = -1;
		int accessory1 = -1;
		int accessory2 = -1;

		bool operator==(const Values& other) const
		{
			return icon == other.icon && color == other.color
				&& accessory1 == other.accessory1 && accessory2 == other.accessory2;
		}
		bool operator!=(const Values& other) const { return !(*this == other); }
	};

	// True once the save data manager is constructed and its two accessory slots carry
	// the byte-target flags the game's init gives them. That flag pair is a sentinel:
	// slot A must read 0 and slot B must read 1, so a wrong base address or a manager
	// that has not been built yet fails this instead of being written to.
	bool Available();

	// Both return false rather than guessing when Available() is false. Values that are
	// out of range are refused by Write() as a whole -- it never writes a partial set.
	bool Read(Values* out);
	bool Write(const Values& values);

	// Whether every field is inside the ranges the game can actually handle.
	bool InRange(const Values& values);
}
