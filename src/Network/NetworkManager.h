#pragma once
#include "Packet.h"

#include "SteamApiWrapper/SteamNetworkingWrapper.h"

#include <steam_api.h>

class NetworkManager
{
public:
	NetworkManager(SteamNetworkingWrapper* SteamNetworking, CSteamID steamID);
	~NetworkManager();
	bool SendPacket(CSteamID* steamID, Packet* packet);
	// Same as SendPacket without its per-packet debug log lines, for the one periodic packet
	// (the 1 Hz performance report) - SendPacket writes nine lines per call. Unreliable when
	// asked: a status that is superseded a second later should not be retransmitted.
	bool SendPacketQuiet(CSteamID* steamID, Packet* packet, bool reliable);
	void RecvPacket(Packet* packet);
	bool IsIMPacket(Packet* packet);

private:

	SteamNetworkingWrapper* m_pSteamNetworking;
	CSteamID m_steamID;
};