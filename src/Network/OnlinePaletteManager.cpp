#include "OnlinePaletteManager.h"

#include "Palette/impl_format.h"
#include "Palette/PaletteBlockList.h"

#include "Core/logger.h"
#include "Core/interfaces.h"
#include "Core/Settings.h"
#include "Game/gamestates.h"

OnlinePaletteManager::OnlinePaletteManager(PaletteManager* pPaletteManager, CharPaletteHandle* pP1CharPalHandle,
	CharPaletteHandle* pP2CharPalHandle, RoomManager* pRoomManager)
	: m_pPaletteManager(pPaletteManager), m_pP1CharPalHandle(pP1CharPalHandle), 
	m_pP2CharPalHandle(pP2CharPalHandle), m_pRoomManager(pRoomManager)
{
}

void OnlinePaletteManager::SendPalettePackets()
{
	LOG(2, "OnlinePaletteManager::SendPalettePackets\n");

	if (m_pRoomManager->IsThisPlayerSpectator())
		return;

	uint16_t thisPlayerMatchPlayerIndex = m_pRoomManager->GetThisPlayerMatchPlayerIndex();
	CharPaletteHandle& charPalHandle = GetPlayerCharPaletteHandle(thisPlayerMatchPlayerIndex);

	if (!IsPaletteHandleReady(charPalHandle))
	{
		LOG(1, "[OnlinePalette] Deferring local palette send until handle is ready (matchPlayerIndex=%u)\n",
			thisPlayerMatchPlayerIndex);
		return;
	}

	SendPaletteDownloadPermissionPacket(thisPlayerMatchPlayerIndex);
	SendPlatinumVoiceChoicePacket(thisPlayerMatchPlayerIndex);
	SendPaletteInfoPacket(charPalHandle, thisPlayerMatchPlayerIndex);
	SendPaletteDataPackets(charPalHandle, thisPlayerMatchPlayerIndex);
}

void OnlinePaletteManager::RecvPaletteDataPacket(Packet* packet)
{
	LOG(2, "OnlinePaletteManager::RecvPaletteDataPacket\n");

	if (!g_modVals.enableForeignPalettes)
		return;

	const uint16_t matchPlayerIndex = m_pRoomManager->GetPlayerMatchPlayerIndexByRoomMemberIndex(packet->roomMemberIndex);
	if (matchPlayerIndex > 1 || packet->part >= IMPL_PALETTE_FILES_COUNT || packet->dataSize < IMPL_PALETTE_DATALEN)
		return;

	// Held until all eight files are in: a palette can only be checked against the block
	// list whole, and showing it file by file until then would flash a blocked one.
	ReceivedPalette& received = m_received[matchPlayerIndex];
	if (received.decided && received.files[packet->part])
		received = ReceivedPalette(); // a file already used: this is the next palette
	memcpy_s((char*)received.data.file0 + packet->part * IMPL_PALETTE_DATALEN, IMPL_PALETTE_DATALEN,
		packet->data, IMPL_PALETTE_DATALEN);
	received.files[packet->part] = true;
	IdentifySender(received, packet->roomMemberIndex);
	TryApplyReceived(matchPlayerIndex);
}


void OnlinePaletteManager::RecvPaletteInfoPacket(Packet* packet)
{
	LOG(2, "OnlinePaletteManager::RecvPaletteInfoPacket\n");

	if (!g_modVals.enableForeignPalettes)
		return;

	const uint16_t matchPlayerIndex = m_pRoomManager->GetPlayerMatchPlayerIndexByRoomMemberIndex(packet->roomMemberIndex);
	if (matchPlayerIndex > 1 || packet->dataSize < sizeof(IMPL_info_t))
		return;

	ReceivedPalette& received = m_received[matchPlayerIndex];
	if (received.decided && received.haveInfo)
		received = ReceivedPalette(); // the info of the next palette
	memcpy_s(&received.data.palInfo, sizeof(IMPL_info_t), packet->data, sizeof(IMPL_info_t));
	received.haveInfo = true;
	IdentifySender(received, packet->roomMemberIndex);

	// The files usually come after the info; if they won the race, the palette is on screen
	// already and only its name is missing.
	if (received.decided && !received.withheld)
		m_pPaletteManager->SetCurrentPalInfo(GetPlayerCharPaletteHandle(matchPlayerIndex), received.data.palInfo);
	else
		TryApplyReceived(matchPlayerIndex);
}


void OnlinePaletteManager::RecvPaletteDownloadPermissionPacket(Packet* packet)
{
	LOG(2, "OnlinePaletteManager::RecvPaletteDownloadPermissionPacket\n");

	uint16_t matchPlayerIndex = m_pRoomManager->GetPlayerMatchPlayerIndexByRoomMemberIndex(packet->roomMemberIndex);
	if (matchPlayerIndex > 1 || packet->dataSize < sizeof(bool))
		return;

	bool allowDownload = false;
	memcpy_s(&allowDownload, sizeof(allowDownload), packet->data, sizeof(allowDownload));
	m_playerPaletteDownloadPermissions[matchPlayerIndex] =
		allowDownload ? PaletteDownloadPermission::Granted : PaletteDownloadPermission::Denied;
}

void OnlinePaletteManager::RecvPlatinumVoiceChoicePacket(Packet* packet)
{
	LOG(2, "OnlinePaletteManager::RecvPlatinumVoiceChoicePacket\n");

	uint16_t matchPlayerIndex = m_pRoomManager->GetPlayerMatchPlayerIndexByRoomMemberIndex(packet->roomMemberIndex);
	if (matchPlayerIndex > 1 || packet->dataSize < sizeof(uint8_t))
		return;

	uint8_t choice = 0;
	memcpy_s(&choice, sizeof(choice), packet->data, sizeof(choice));
	if (choice > 2)
		choice = 0;

	m_playerVoiceChoices[matchPlayerIndex] = (int8_t)choice;
	LOG(2, "[PlatVoice] recv opponent voice choice=%u for matchPlayerIndex=%u\n", choice, matchPlayerIndex);
}

int OnlinePaletteManager::GetPlayerVoiceChoice(uint16_t matchPlayerIndex) const
{
	if (matchPlayerIndex > 1)
		return 0;

	const int8_t choice = m_playerVoiceChoices[matchPlayerIndex];
	return choice > 0 ? choice : 0; // -1 (none) / 0 (Default) -> leave vanilla RNG
}

void OnlinePaletteManager::ProcessSavedPalettePackets()
{
	LOG(2, "OnlinePaletteManager::ProcessSavedPalettePackets\n");

	if (!m_pRoomManager->IsRoomFunctional())
		return;

	TryApplyReceived(0);
	TryApplyReceived(1);
}


void OnlinePaletteManager::ClearSavedPalettePacketQueues()
{
	LOG(2, "OnlinePaletteManager::ClearSavedPalettePacketQueues\n");

	m_received[0] = ReceivedPalette();
	m_received[1] = ReceivedPalette();
	m_matchInitPending = false;
	m_loggedMatchInitWait = false;
	m_playerPaletteDownloadPermissions[0] = PaletteDownloadPermission::Unknown;
	m_playerPaletteDownloadPermissions[1] = PaletteDownloadPermission::Unknown;
	m_playerVoiceChoices[0] = -1;
	m_playerVoiceChoices[1] = -1;
}


void OnlinePaletteManager::OnMatchInit()
{
	LOG(2, "OnlinePaletteManager::OnMatchInit\n");

	m_playerPaletteDownloadPermissions[0] = PaletteDownloadPermission::Unknown;
	m_playerPaletteDownloadPermissions[1] = PaletteDownloadPermission::Unknown;
	m_playerVoiceChoices[0] = -1;
	m_playerVoiceChoices[1] = -1;
	m_matchInitPending = true;
	m_loggedMatchInitWait = false;
	OnUpdate();
}

void OnlinePaletteManager::OnUpdate()
{
	if (!m_pRoomManager->IsRoomFunctional())
		return;

	// Received palettes waiting on their handle, and a block or unblock since last frame -
	// for spectators as much as for players.
	TryApplyReceived(0);
	TryApplyReceived(1);
	const int blockRevision = PaletteBlockList::Revision();
	if (blockRevision != m_blockRevision)
	{
		m_blockRevision = blockRevision;
		ReapplyBlocks();
	}

	if (m_pRoomManager->IsThisPlayerSpectator())
	{
		m_matchInitPending = false;
		m_loggedMatchInitWait = false;
		return;
	}

	if (!m_matchInitPending)
		return;

	CharPaletteHandle& localHandle = GetPlayerCharPaletteHandle(m_pRoomManager->GetThisPlayerMatchPlayerIndex());
	if (!IsPaletteHandleReady(localHandle))
	{
		if (!m_loggedMatchInitWait)
		{
			LOG(1, "[OnlinePalette] Waiting for local palette handle before match-init flush\n");
			m_loggedMatchInitWait = true;
		}
		return;
	}

	if (m_matchInitPending)
	{
		LOG(1, "[OnlinePalette] Local palette handle became ready; sending match-init palette packets\n");
		SendPalettePackets();
		m_matchInitPending = false;
		m_loggedMatchInitWait = false;
	}

	ProcessSavedPalettePackets();
}

bool OnlinePaletteManager::CanDownloadPalette(uint16_t matchPlayerIndex) const
{
	return GetDownloadPermission(matchPlayerIndex) == PaletteDownloadPermission::Granted;
}

OnlinePaletteManager::PaletteDownloadPermission OnlinePaletteManager::GetDownloadPermission(uint16_t matchPlayerIndex) const
{
	if (matchPlayerIndex > 1)
		return PaletteDownloadPermission::Unknown;

	return m_playerPaletteDownloadPermissions[matchPlayerIndex];
}

void OnlinePaletteManager::SendPaletteDownloadPermissionPacket(uint16_t roomMemberIndex)
{
	LOG(2, "OnlinePaletteManager::SendPaletteDownloadPermissionPacket\n");

	// Unset (-1) counts as not having given permission.
	bool allowDownload = Settings::settingsIni.allowPaletteDownloads == 1;
	Packet packet = Packet(
		(char*)&allowDownload,
		(uint16_t)sizeof(allowDownload),
		PacketType_PaletteDownloadPermission,
		roomMemberIndex
	);

	m_pRoomManager->SendPacketToSameMatchIMPlayers(&packet);
}

void OnlinePaletteManager::SendPlatinumVoiceChoicePacket(uint16_t roomMemberIndex)
{
	LOG(2, "OnlinePaletteManager::SendPlatinumVoiceChoicePacket\n");

	uint8_t choice = (uint8_t)Settings::settingsIni.platinumVoiceChoice;
	Packet packet(
		(char*)&choice,
		(uint16_t)sizeof(choice),
		PacketType_PlatinumVoiceChoice,
		roomMemberIndex
	);

	m_pRoomManager->SendPacketToSameMatchIMPlayers(&packet);
}

void OnlinePaletteManager::SendPaletteInfoPacket(CharPaletteHandle& charPalHandle, uint16_t roomMemberIndex)
{
	LOG(2, "OnlinePaletteManager::SendPaletteInfoPacket\n");

	Packet packet = Packet(
		(char*)&m_pPaletteManager->GetCurrentPalInfo(charPalHandle),
		(uint16_t)sizeof(IMPL_info_t),
		PacketType_PaletteInfo,
		roomMemberIndex
	);

	m_pRoomManager->SendPacketToSameMatchIMPlayers(&packet);
}

void OnlinePaletteManager::SendPaletteDataPackets(CharPaletteHandle& charPalHandle, uint16_t roomMemberIndex)
{
	LOG(2, "OnlinePaletteManager::SendPaletteDataPackets\n");

	for (int palFileIndex = 0; palFileIndex < IMPL_PALETTE_FILES_COUNT; palFileIndex++)
	{
		const char* palAddr = m_pPaletteManager->GetCurPalFileAddr((PaletteFile)palFileIndex, charPalHandle);
		if (palAddr == nullptr)
		{
			LOG(1, "[OnlinePalette] Aborting palette data send because file %d is no longer readable\n", palFileIndex);
			return;
		}

		Packet packet = Packet(
			(char*)palAddr,
			(uint16_t)IMPL_PALETTE_DATALEN,
			PacketType_PaletteData,
			roomMemberIndex,
			palFileIndex
		);

		m_pRoomManager->SendPacketToSameMatchIMPlayers(&packet);
	}
}



CharPaletteHandle& OnlinePaletteManager::GetPlayerCharPaletteHandle(uint16_t matchPlayerIndex)
{
	return matchPlayerIndex == 0 ? *m_pP1CharPalHandle : *m_pP2CharPalHandle;
}

bool OnlinePaletteManager::IsPaletteHandleReady(const CharPaletteHandle& charPalHandle) const
{
	return charPalHandle.IsPaletteDataReady();
}

void OnlinePaletteManager::IdentifySender(ReceivedPalette& received, uint16_t roomMemberIndex)
{
	if (received.senderSteamId)
		return;
	for (const IMPlayer& player : m_pRoomManager->GetIMPlayersInCurrentMatch())
	{
		if (player.roomMemberIndex == (int)roomMemberIndex)
		{
			received.senderSteamId = player.steamID.ConvertToUint64();
			received.senderName = player.steamName;
			if (PaletteBlockList::IsUserBlocked(received.senderSteamId))
				PaletteBlockList::UpdateUserName(received.senderSteamId, received.senderName);
			return;
		}
	}
}

bool OnlinePaletteManager::IsBlocked(const ReceivedPalette& received) const
{
	return PaletteBlockList::IsUserBlocked(received.senderSteamId) || PaletteBlockList::IsPaletteBlocked(received.hash);
}

void OnlinePaletteManager::ShowReceived(uint16_t matchPlayerIndex)
{
	ReceivedPalette& received = m_received[matchPlayerIndex];
	CharPaletteHandle& charPalHandle = GetPlayerCharPaletteHandle(matchPlayerIndex);
	for (int file = 0; file < IMPL_PALETTE_FILES_COUNT; file++)
		m_pPaletteManager->ReplacePaletteFile(received.data.file0 + file * IMPL_PALETTE_DATALEN, (PaletteFile)file, charPalHandle);
	if (received.haveInfo)
		m_pPaletteManager->SetCurrentPalInfo(charPalHandle, received.data.palInfo);
}

void OnlinePaletteManager::TryApplyReceived(uint16_t matchPlayerIndex)
{
	ReceivedPalette& received = m_received[matchPlayerIndex];
	if (received.decided || !g_modVals.enableForeignPalettes)
		return;
	for (int file = 0; file < IMPL_PALETTE_FILES_COUNT; file++)
		if (!received.files[file])
			return;
	if (!IsPaletteHandleReady(GetPlayerCharPaletteHandle(matchPlayerIndex)))
		return;

	received.hash = PaletteBlockList::HashPalette(received.data);
	received.decided = true;
	received.withheld = IsBlocked(received);
	if (received.withheld)
	{
		// Nothing to undo: the opponent was put back on their own colours at match init.
		LOG(1, "[OnlinePalette] Not showing palette from %s (matchPlayerIndex=%u): %s is blocked\n",
			received.senderName.c_str(), matchPlayerIndex,
			PaletteBlockList::IsUserBlocked(received.senderSteamId) ? "the player" : "the palette");
		return;
	}
	// Only reached once this battle's match init has run for that side (IsPaletteDataReady), so a
	// palette that arrived while we were still loading waits for it instead of being written with
	// the previous battle's colour.
	LOG(1, "[OnlinePalette] Showing palette '%s' from %s on matchPlayerIndex=%u (native colour %d)\n",
		received.haveInfo ? received.data.palInfo.palName : "?", received.senderName.c_str(), matchPlayerIndex,
		GetPlayerCharPaletteHandle(matchPlayerIndex).GetOrigPalIndex());
	ShowReceived(matchPlayerIndex);
}

void OnlinePaletteManager::ReapplyBlocks()
{
	for (uint16_t i = 0; i < 2; i++)
	{
		ReceivedPalette& received = m_received[i];
		if (!received.decided)
			continue;
		const bool blocked = IsBlocked(received);
		if (blocked && !received.withheld)
		{
			m_pPaletteManager->RestoreOrigPal(GetPlayerCharPaletteHandle(i));
			received.withheld = true;
		}
		else if (!blocked && received.withheld && g_modVals.enableForeignPalettes)
		{
			ShowReceived(i);
			received.withheld = false;
		}
	}
}

bool OnlinePaletteManager::GetReceivedPalette(uint16_t matchPlayerIndex, ReceivedPaletteView& out) const
{
	if (matchPlayerIndex > 1)
		return false;
	const ReceivedPalette& received = m_received[matchPlayerIndex];
	out.decided = received.decided;
	out.withheld = received.withheld;
	out.hash = received.hash;
	out.data = received.decided ? &received.data : nullptr;
	return received.decided;
}

bool OnlinePaletteManager::GetMatchPlayerIdentity(uint16_t matchPlayerIndex, uint64_t* steamId, std::string* name) const
{
	for (const IMPlayer& player : m_pRoomManager->GetIMPlayersInCurrentMatch())
	{
		if (player.matchPlayerIndex == (int)matchPlayerIndex)
		{
			*steamId = player.steamID.ConvertToUint64();
			*name = player.steamName;
			return *steamId != 0;
		}
	}
	return false;
}
