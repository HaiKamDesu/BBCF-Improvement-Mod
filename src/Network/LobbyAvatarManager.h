#pragma once

#include "Network/LobbyAvatarSource.h"

// Remembers the lobby avatar (icon, colour and the two accessory slots) across launches
// and puts it back on when you next connect to network mode.
//
// Read src/Network/LobbyAvatarSource.h first, then the "avatar SOURCE structs" block in
// src/Game/GhidraDefs.h. They carry the reverse engineering this file depends on.
//
// THE TWO COPIES, AND WHY BOTH HAVE TO BE WRITTEN
//
// The four avatar values exist twice: as originals in the save data manager (the "source"),
// and as a derived copy inside the player's own network profile blob. Which one you write
// decides what happens, and neither one alone is enough.
//
// The BLOB is what the Online page's sliders show and what FUN_0073C550 packs into the
// lobby announce other players render from. Nothing else displays or transmits it. So the
// blob must hold the value or the feature is invisible.
//
// The SOURCE is what the game rebuilds the blob FROM, and what its own equip menu writes.
// An accessory that menu cannot offer is absent from the source, so any rebuild re-derives
// the old value over the top. So the source must hold the value or it does not stick.
//
// The rebuild (FUN_004924B0, via FUN_00494810 with a non-zero argument) is reachable from
// exactly two call sites, and RE'd 2026-09-08 both are state 0 -- the entry state -- of a
// customization screen's own state machine: SCENE_CDcc's per-frame update through
// FUN_00546DA0's 11-state table, and FUN_00708FB0's 9-state table. It therefore runs when
// the player opens the game's avatar screen and never during a network connect. Confirmed
// in-game: a session whose source held 202 with the blob left alone showed 50 on the Online
// page the whole time. What DOES write the blob on a connect is the game's profile load,
// which fills it from the stored profile in two steps, zeros first.
//
// WHY THE BLOB WRITE HAS TO BE RATIONED
//
// The blob is checksummed and uploaded to Steam, and a write landing between the upload
// path's seal and the upload strategy's re-verify fails the step with no Steam round-trip.
// Lobby entry is gated behind that step. In one reporter's captures the network state
// machine looped state1 16 -> 12 at 1 Hz for 120s, every 12 timestamp-identical to a mod
// blob write, while a session that never wrote went 16 -> 13 and entered immediately.
//
// The harm was in writing the blob repeatedly, while the profile load and the handshake
// were still in flight, with no bound but a 120s backstop. So this waits for both copies to
// stop moving before touching the blob at all, then writes it once, and if the game moves
// it again re-applies on a small fixed budget and gives up rather than looping. Worst case
// is a few seconds of retry instead of two minutes, and it cannot fight forever.
//
// Nothing is saved to settings.ini until the values have held still, because during a
// connect the blob passes through all-zeros and the game can copy a downloaded profile back
// over the source -- saving either would overwrite the set the user chose, which is how the
// first cut of this feature destroyed a saved avatar.
//
// Any edit on the Online page ends the apply phase immediately: the user's hand always wins.
class LobbyAvatarManager
{
public:
	static LobbyAvatarManager& GetInstance();

	// Driven once per rendered frame from WindowManager::HandleButtons. The avatar lives
	// on menu screens, where the battle frame counter hook is idle, so this cannot hang
	// off the frame counter the way match-time features do.
	void Tick();

	// Called by the Online page when one of the avatar sliders is dragged. Ends the apply
	// phase so the value under the user's mouse wins, and writes the edit through to the
	// source as well so the game's next rebuild does not undo it.
	void OnUserEdited();

private:
	LobbyAvatarManager() = default;

	using AvatarValues = LobbyAvatarSource::Values;

	static bool BlobReady();
	static AvatarValues ReadBlob();
	static bool WriteBlob(const AvatarValues& values);
	static bool IsComplete(const AvatarValues& values);

	AvatarValues LoadRemembered() const;
	void SaveRemembered(const AvatarValues& values);

	void BeginApplying(const char* reason);
	void TickApplying(unsigned long long now);
	void TickSettled(unsigned long long now);
	void ApplyBlob(unsigned long long now, const char* reason);
	void LogBoth(unsigned long long now, const char* what) const;

	bool m_addressesSeen = false;
	int m_lastGameState = -1;
	bool m_sourceUnavailableLogged = false;

	// Apply phase. m_hasTarget is false when there is nothing remembered yet: the phase
	// still runs, so the game's own load is not mistaken for a user choice, it just watches
	// instead of writing.
	bool m_applying = false;
	bool m_hasTarget = false;
	AvatarValues m_target;
	AvatarValues m_lastSource;
	AvatarValues m_lastBlob;
	bool m_haveLastSource = false;
	bool m_haveLastBlob = false;

	unsigned long long m_applyStartedTick = 0;
	unsigned long long m_applyEndedTick = 0;
	unsigned long long m_lastSourceChangeTick = 0;
	unsigned long long m_lastBlobChangeTick = 0;
	unsigned long long m_lastHeartbeatTick = 0;

	// Rations. Source writes are free, so their budget only exists to stop us fighting a
	// genuine equip-menu change; blob writes are the ones that can fail the handshake.
	int m_sourceReapplyBudget = 0;
	int m_blobWriteBudget = 0;

	// Diagnostic for the round of testing this design came out of: whether the game was ever
	// seen to rebuild the blob from the source on its own. The previous attempt assumed it
	// would; this one deliberately does not rely on it, and records whether it ever happens.
	bool m_sawGameRebuildBlob = false;

	// Debounce for writing settings.ini, so dragging a slider does not produce one file
	// write per frame.
	AvatarValues m_pendingSave;
	unsigned long long m_pendingSaveSinceTick = 0;
	bool m_hasPendingSave = false;
};
