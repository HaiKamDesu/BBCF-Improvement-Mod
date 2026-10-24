#pragma once
#include "NetworkManager.h"

#include "Game/Room/Room.h"
#include "Core/utils.h"
#include <isteamfriends.h>
#include <string>
#include <vector>

#define MAX_PLAYERS_IN_ROOM 8

struct IMPlayer
{
	int roomMemberIndex;
	CSteamID steamID;
	std::string steamName;
	int matchPlayerIndex;
	// From the ModVersionInfo in their acknowledge (8.9.2+). Empty/0 for an older build,
	// which sends an empty acknowledge.
	std::string modVersion;
	uint16_t modCaps = 0;
	bool versionKnown = false;

	IMPlayer() : roomMemberIndex(-1), matchPlayerIndex(-1)
	{
	}

	IMPlayer(int roomMemberIndex, uint64_t steamID, const char* steamName)
		: roomMemberIndex(roomMemberIndex), steamID(steamID),
		steamName(steamName), matchPlayerIndex(-1)
	{
	}

	IMPlayer(IMPlayer other, int matchPlayerIndex)
		: roomMemberIndex(other.roomMemberIndex), steamID(other.steamID),
		steamName(other.steamName), matchPlayerIndex(matchPlayerIndex),
		modVersion(other.modVersion), modCaps(other.modCaps), versionKnown(other.versionKnown)
	{
	}
};

class RoomManager
{
public:
	RoomManager(NetworkManager* pNetworkManager, ISteamFriends* pSteamFriends, CSteamID steamID);
	~RoomManager();

	void SendAcknowledge(Packet* packet);
	void AcceptAcknowledge(Packet* packet);
	void JoinRoom(Room* pRoom);
	bool IsRoomFunctional() const;
	void SendPacketToSameMatchIMPlayers(Packet* packet);
	void SendPacketToSameMatchIMPlayersNonSpectator(Packet* packet);
	bool IsPacketFromSameRoom(Packet* packet) const;
	bool IsPacketFromSameMatchNonSpectator(Packet* packet) const;
	bool IsThisPlayerSpectator() const;
	bool IsThisPlayerInMatch() const;
	void SetFFAThisPlayerIndex(int* pFFAThisPlayerIndex);
	uint16_t GetThisPlayerMatchPlayerIndex() const;
	uint16_t GetPlayerMatchPlayerIndexByRoomMemberIndex(uint16_t index) const;
	const std::string GetRoomTypeName() const;
	// Reads the current room's type. Returns false (leaving *outType untouched) when
	// there is no readable room struct, so callers can't mistake "no room" for a type.
	bool TryGetRoomType(RoomType* outType) const;
	std::vector<IMPlayer> GetIMPlayersInCurrentMatch() const;
	std::vector<IMPlayer> GetIMPlayersInCurrentRoom() const;
	// The mod-user record for this Steam ID in the current room, or false (no mod, or not
	// acknowledged yet). Copies into *out so callers never hold a pointer into m_imPlayers.
	bool FindIMPlayer(uint64_t steamId, IMPlayer* out) const;
	std::vector<const RoomMemberEntry*> GetOtherRoomMemberEntriesInCurrentMatch() const;
	// Public since 2026-09-26: the ranked stale-room check reads slots directly.
	const RoomMemberEntry* GetThisPlayerRoomMemberEntry() const;
	const RoomMemberEntry* GetRoomMemberEntryByIndex(uint16_t index) const;

	std::vector<IMPlayer> GetIMPlayersInCurrentMatchNonSpec() const;
	static RoomSettingsStatic* GetRoomSettingsStaticBaseAdress();
	bool ChangeRematchAmnt(signed int new_amnt);

private:
	void SendAnnounce();
	void SendAcknowledgeTo(const CSteamID& to, bool replyWanted);
	static void ApplyModVersionInfo(const Packet* packet, IMPlayer* player);
	void AddIMPlayerToRoom(const IMPlayer& player);
	void RemoveIMPlayerFromRoom(uint16_t index);
	bool IsPacketFromSameMatch(Packet* packet) const;
	bool IsPacketFromSpectator(Packet* packet) const;;
	uint16_t GetThisPlayerRoomMemberIndex() const;
	bool IsPlayerInRoom(const IMPlayer& player) const;
	bool IsThisPlayer(const uint64_t otherSteamID) const;
	
	const char* GetPlayerSteamName(uint64_t steamID) const;

	std::vector<IMPlayer> m_imPlayers;
	CSteamID m_thisPlayerSteamID;

	// Free-for-All fix
	int* m_pFFAThisPlayerIndex;

	// Interfaces
	NetworkManager* m_pNetworkManager;
	ISteamFriends* m_pSteamFriends;
	Room* m_pRoom;
	RoomSettingsStatic* m_pRoomSettings;

	// For debug purposes
	friend class DebugWindow;
};
