#pragma once

// Remembers the lobby avatar (icon, colour and the two accessory slots) across launches
// and puts it back on when you next connect to network mode.
//
// Read src/Network/ProfileBlobSeal.h before touching anything here. The short version:
// g_gameVals.playerAvatarBaseAddr is netUserData+0xD0, the player's own 0x6800 profile blob
// that the game uploads to Steam as bbdc.dat, and it is checksummed. Every write into it
// must be followed by ProfileBlobSeal::Reseal() on the same frame, or profile uploads start
// failing and the game reports that you have no network profile. WriteLive() does that.
//
// Confirmed in-game 2026-09-05: the re-apply held over both of the game's own writes, the
// blob was still valid at settle, and the TUS latch never tripped across a 12-minute
// session. Default-on since.
//
// The other difficulty is timing. The avatar fields become readable the moment the game
// builds its network object, but the game does not fill them in until several seconds later
// -- measured at ~3.6s on one connect and ~9s on another, in two steps: first the struct
// reads all zero, then the profile's real avatar lands. A re-apply that writes once when the
// addresses appear is therefore silently undone, and a "remember what is live now" pass that
// runs before the profile has landed will happily save those zeros over the set the user
// actually chose. That is exactly how the first cut of this destroyed a saved avatar (see
// the 11:50:25 -> 11:50:35 sequence in DebugHistory/DEBUG_20260905_115237.txt).
//
// So the tick does not use a fixed window. After the addresses appear it enters a settling
// phase: it keeps writing the remembered values back over anything that changes them, and
// only considers the game done once the values have sat still for a few seconds. Nothing
// is saved during settling. Once settled, later changes -- on the Online page or in the
// game's own equip menu -- become the new remembered set.
//
// Any edit on the Online page ends settling immediately: the user's hand always wins over
// a re-apply that is still in progress.
class LobbyAvatarManager
{
public:
	static LobbyAvatarManager& GetInstance();

	// Driven once per rendered frame from WindowManager::HandleButtons. The avatar lives
	// on menu screens, where the battle frame counter hook is idle, so this cannot hang
	// off the frame counter the way match-time features do.
	void Tick();

	// Called by the Online page when one of the avatar sliders is dragged. Ends settling
	// so the value under the user's mouse wins, and marks the new values to be remembered.
	void OnUserEdited();

private:
	LobbyAvatarManager() = default;

	struct AvatarValues
	{
		int icon = -1;
		int color = -1;
		int accessory1 = -1;
		int accessory2 = -1;

		bool operator==(const AvatarValues& other) const
		{
			return icon == other.icon && color == other.color
				&& accessory1 == other.accessory1 && accessory2 == other.accessory2;
		}
		bool operator!=(const AvatarValues& other) const { return !(*this == other); }
	};

	static bool AddressesReady();
	static AvatarValues ReadLive();
	static void WriteLive(const AvatarValues& values);
	static bool IsComplete(const AvatarValues& values);

	AvatarValues LoadRemembered() const;
	void SaveRemembered(const AvatarValues& values);

	void BeginSettling(const char* reason);
	void TickSettling(unsigned long long now);
	void TickRemember(unsigned long long now);

	bool m_addressesSeen = false;
	int m_lastGameState = -1;

	// Settling phase. m_hasTarget is false when there is nothing remembered yet: the phase
	// still runs, so the game's own load is not mistaken for a user choice, it just watches
	// instead of writing.
	bool m_settling = false;
	bool m_hasTarget = false;
	AvatarValues m_target;
	AvatarValues m_lastSeen;
	unsigned long long m_settlingStartedTick = 0;
	unsigned long long m_lastChangeTick = 0;

	// Debounce for writing settings.ini, so dragging a slider does not produce one file
	// write per frame.
	AvatarValues m_pendingSave;
	unsigned long long m_pendingSaveSinceTick = 0;
	bool m_hasPendingSave = false;
};
