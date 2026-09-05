#pragma once

// Checksum repair for the player's own network profile blob.
//
// g_gameVals.playerAvatarBaseAddr is not an avatar struct -- it is netUserData+0xD0, the
// player's own 0x6800-byte profile blob, the same buffer the game FileShare()s to Steam
// Cloud as bbdc.dat. Anything in the mod that pokes a value in there (the Online page's
// avatar sliders, LobbyAvatarManager) is editing a checksummed payload.
//
// The blob's layout, from FUN_0040DEC0 (the game's own sealer, called via FUN_004A1C10
// with size 0x6800 just before an upload):
//
//     +0x00  uint16  checksum          16-bit ones'-complement of the whole blob
//     +0x02  uint16  always zero       the sealer clears +0x00 as a dword, writes a word
//     +0x04  uint32  stamp             FUN_0040BF90(0), refreshed on every reseal
//
// FUN_0040DEC0 zeroes the dword at +0x00, sums the blob as 0x3400 little-endian words with
// end-around carry, and stores ~sum as a word at +0x00. FUN_0040DF10 is the matching
// verifier: it re-sums and passes only when the result is 0xFFFF.
//
// Why this has to exist: the upload does NOT just seal and send. FUN_004A96D0 seals the
// blob and queues the transfer, but uei::ThinkLogicStrategyUploadTUS::vftable+0x1C
// (FUN_0042EDD0) re-verifies the very same buffer at item-state 0, before any Steam call.
// A write that lands between those two points leaves the blob checksum-invalid, the upload
// fails without Steam ever being contacted, and the DAT_00CF77A8 TUS latch trips -- which
// is what "you don't have a profile" looks like in the menu. Nothing rewrites that region
// on its own, so once it goes invalid it stays invalid for the rest of the process.
// Measured 2026-09-05: a single avatar write tripped the latch 2.8 seconds later.
//
// So: after writing anything into the blob, reseal it. Then whichever of the two paths
// looks at it next finds a valid payload.
namespace ProfileBlobSeal
{
	// Recomputes and stores the blob's checksum. Call immediately after any write into the
	// blob, on the same frame. False means the blob is not mapped yet (before the first
	// network connection) and nothing was written.
	//
	// Deliberately does not touch the +0x04 stamp that the game's sealer refreshes: we are
	// repairing someone else's payload, not authoring a new revision of it, and the
	// verifier only looks at the checksum.
	bool Reseal();

	// True when the blob currently passes the game's own check. Only meaningful once the
	// blob exists; returns false when it does not.
	bool IsValid();
}
