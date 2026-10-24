#include "NetworkManager.h"

#include "RoomManager.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Game/PerformanceMonitor.h"
#include "Overlay/Logger/ImGuiLogger.h"

NetworkManager::NetworkManager(SteamNetworkingWrapper* SteamNetworking, CSteamID steamID)
{
	m_pSteamNetworking = SteamNetworking;
	m_steamID = steamID;
}

NetworkManager::~NetworkManager()
{
}

bool NetworkManager::SendPacket(CSteamID* steamID, Packet* packet)
{
	LOG(2, "NetworkManager::SendPacket\n");

	packet->steamID = m_steamID.ConvertToUint64();

	LOG(2, "\tSending packet:\n");
	LOG(2, "\tversion: %s\n", RawMemoryArrayToString((unsigned char*)&packet->version, sizeof(packet->version)));
	LOG(2, "\tpacketType: %s\n", RawMemoryArrayToString((unsigned char*)&packet->packetType, sizeof(packet->packetType)));
	LOG(2, "\tpart: %s\n", RawMemoryArrayToString((unsigned char*)&packet->part, sizeof(packet->part)));
	LOG(2, "\tpacketSize: %s\n", RawMemoryArrayToString((unsigned char*)&packet->packetSize, sizeof(packet->packetSize)));
	LOG(2, "\troomPlayerIndex: %s\n", RawMemoryArrayToString((unsigned char*)&packet->roomMemberIndex, sizeof(packet->roomMemberIndex)));
	LOG(2, "\tsteamID: %s\n", RawMemoryArrayToString((unsigned char*)&packet->steamID, sizeof(packet->steamID)));
	LOG(2, "\tdataSize: %s\n", RawMemoryArrayToString((unsigned char*)&packet->dataSize, sizeof(packet->dataSize)));
	//LOG(2, "\tdata: %s\n", RawMemoryArrayToString((unsigned char*)&packet->data, sizeof(packet->data)));

	// Reliable, not unreliable.
	//
	// Every packet the mod sends is a one-shot piece of state - your palette, whether you
	// allow palette downloads, the Platinum voice pick, the agreed game mode, the replay
	// upload veto. None of it is re-sent on a timer and none of it is re-requested, so a
	// single dropped datagram means the other side simply never learns it, for the whole
	// match. k_EP2PSendUnreliable also DISCARDS the message outright when the P2P session
	// to that player is not open yet, and the palette burst goes out at match init, which
	// is exactly when it may not be. That is the "sometimes the opponent's custom palette
	// just doesn't show up" report: 11 packets fired into a session that may not exist,
	// with no retry behind them.
	//
	// These are a handful of sub-1200-byte control packets per match, not a per-frame
	// stream, so the cost of reliability here is nothing.
	EP2PSend sendType = k_EP2PSendReliable;

	return m_pSteamNetworking->SendP2PPacket(*steamID, packet, packet->packetSize, sendType, 0);
}

bool NetworkManager::SendPacketQuiet(CSteamID* steamID, Packet* packet, bool reliable)
{
	packet->steamID = m_steamID.ConvertToUint64();
	return m_pSteamNetworking->SendP2PPacket(*steamID, packet, packet->packetSize,
		reliable ? k_EP2PSendReliable : k_EP2PSendUnreliable, 0);
}

void NetworkManager::RecvPacket(Packet* packet)
{
	LOG(7, "NetworkManager::RecvPacket\n");

	if (!g_interfaces.pRoomManager->IsPacketFromSameRoom(packet))
	{
		LOG(2, "[error] Packet received from not a room member. RoomPlayerIndex: %d, SteamID: %llu\n",
			packet->roomMemberIndex, packet->steamID);

		return;
	}

	switch (packet->packetType)
	{
	case PacketType_IMID_Announce:
		g_interfaces.pRoomManager->SendAcknowledge(packet);
		break;

	case PacketType_IMID_Acknowledge:
		g_interfaces.pRoomManager->AcceptAcknowledge(packet);
		break;

	case PacketType_PaletteInfo:
		if (g_interfaces.pRoomManager->IsPacketFromSameMatchNonSpectator(packet))
		{
			g_interfaces.pOnlinePaletteManager->RecvPaletteInfoPacket(packet);
		}
		break;

	case PacketType_PaletteData:
		if (g_interfaces.pRoomManager->IsPacketFromSameMatchNonSpectator(packet))
		{
			g_interfaces.pOnlinePaletteManager->RecvPaletteDataPacket(packet);
		}
		break;

	case PacketType_PaletteDownloadPermission:
		if (g_interfaces.pRoomManager->IsPacketFromSameMatchNonSpectator(packet))
		{
			g_interfaces.pOnlinePaletteManager->RecvPaletteDownloadPermissionPacket(packet);
		}
		break;

	case PacketType_PlatinumVoiceChoice:
		if (g_interfaces.pRoomManager->IsPacketFromSameMatchNonSpectator(packet))
		{
			g_interfaces.pOnlinePaletteManager->RecvPlatinumVoiceChoicePacket(packet);
		}
		break;

	case PacketType_GameMode:
		if (g_interfaces.pRoomManager->IsPacketFromSameMatchNonSpectator(packet) &&
			*g_gameVals.pGameState == GameState_CharacterSelectionScreen)
		{
			g_interfaces.pOnlineGameModeManager->RecvGameModePacket(packet);
		}
		break;

	case PacketType_UploadReplayEnabled_Broadcast:
		//this packet will signal if either p1 or p2 in the match does not want to have the replay uploaded. Spectators won't send these broadcasts.
		LOG(2, "RECEIVED PACKET PacketType_UploadReplayEnabled_Broadcast\n");
		int allowUpload;
		memcpy(&allowUpload, packet->data, packet->dataSize);
		g_imGuiLogger->Log("Received PacketType_UploadReplayEnabled_Broadcast. \n\tdata: '%d'\n\t steamid: '%d'\n",
			allowUpload,
			packet->steamID
			);
		if (g_interfaces.pRoomManager->IsPacketFromSameMatchNonSpectator(packet)) 
		{
			g_interfaces.pReplayUploadManager->RecvReplayUploadEnabledBroadcastPacket(packet);
		}
		break;

	case PacketType_PerformanceReport:
		// Checks the sender is a player of this match itself, and needs a room entry for
		// this client before it asks (IsPacketFromSameMatch dereferences it).
		PerformanceMonitor::OnReportPacket(packet);
		break;

	case PacketType_PingLocation:
		// Accepted only from the opponent player of this match; parsed and dropped, never
		// logged or stored as text.
		PerformanceMonitor::OnPingLocationPacket(packet);
		break;

	default:
		LOG(2, "Unknown packet type received: %d\n", packet->packetType);
		g_imGuiLogger->Log("[error] Unknown packet type received (%d)\n", packet->packetType);
	}
}

bool NetworkManager::IsIMPacket(Packet* packet)
{
	return packet->version == IM_PACKET_VERSION;
}
