#include "OnlinePaletteManager.h"

#include "Palette/impl_format.h"

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

	uint16_t matchPlayerIndex = m_pRoomManager->GetPlayerMatchPlayerIndexByRoomMemberIndex(packet->roomMemberIndex);
	CharPaletteHandle& charPalHandle = GetPlayerCharPaletteHandle(matchPlayerIndex);

	if (!IsPaletteHandleReady(charPalHandle))
	{
		LOG(1, "[OnlinePalette] Queueing palette data until handle is ready (matchPlayerIndex=%u, part=%u)\n",
			matchPlayerIndex, packet->part);
		m_unprocessedPaletteFiles.push(UnprocessedPaletteFile(matchPlayerIndex, (PaletteFile)packet->part, (char*)packet->data));
		return;
	}

	m_pPaletteManager->ReplacePaletteFile((const char*)packet->data, (PaletteFile)packet->part, charPalHandle);
}

void OnlinePaletteManager::RecvPaletteInfoPacket(Packet* packet)
{
	LOG(2, "OnlinePaletteManager::RecvPaletteInfoPacket\n");

	if (!g_modVals.enableForeignPalettes)
		return;

	uint16_t matchPlayerIndex = m_pRoomManager->GetPlayerMatchPlayerIndexByRoomMemberIndex(packet->roomMemberIndex);
	CharPaletteHandle& charPalHandle = GetPlayerCharPaletteHandle(matchPlayerIndex);

	if (!IsPaletteHandleReady(charPalHandle))
	{
		LOG(1, "[OnlinePalette] Queueing palette info until handle is ready (matchPlayerIndex=%u)\n",
			matchPlayerIndex);
		m_unprocessedPaletteInfos.push(UnprocessedPaletteInfo(matchPlayerIndex, (IMPL_info_t*)packet->data));
		return;
	}

	m_pPaletteManager->SetCurrentPalInfo(charPalHandle, *(IMPL_info_t*)packet->data);
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

	ProcessSavedPaletteInfoPackets();
	ProcessSavedPaletteDataPackets();
}

void OnlinePaletteManager::ClearSavedPalettePacketQueues()
{
	LOG(2, "OnlinePaletteManager::ClearSavedPalettePacketQueues\n");

	m_unprocessedPaletteInfos = {};
	m_unprocessedPaletteFiles = {};
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

	if (m_pRoomManager->IsThisPlayerSpectator())
	{
		m_matchInitPending = false;
		m_loggedMatchInitWait = false;
		return;
	}

	if (!m_matchInitPending &&
		m_unprocessedPaletteInfos.empty() &&
		m_unprocessedPaletteFiles.empty())
	{
		return;
	}

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

void OnlinePaletteManager::ProcessSavedPaletteInfoPackets()
{
	LOG(2, "OnlinePaletteManager::ProcessSavedPaletteInfoPackets\n");

	const size_t pendingCount = m_unprocessedPaletteInfos.size();
	for (size_t i = 0; i < pendingCount; ++i)
	{
		UnprocessedPaletteInfo& palInfo = m_unprocessedPaletteInfos.front();

		CharPaletteHandle& charPalHandle = GetPlayerCharPaletteHandle(palInfo.matchPlayerIndex);

		if (IsPaletteHandleReady(charPalHandle))
		{
			if (g_modVals.enableForeignPalettes)
			{
				m_pPaletteManager->SetCurrentPalInfo(charPalHandle, palInfo.palInfo);
			}
		}
		else
		{
			m_unprocessedPaletteInfos.push(palInfo);
		}

		m_unprocessedPaletteInfos.pop();
	}
}

void OnlinePaletteManager::ProcessSavedPaletteDataPackets()
{
	LOG(2, "OnlinePaletteManager::ProcessSavedPaletteDataPackets\n");

	const size_t pendingCount = m_unprocessedPaletteFiles.size();
	for (size_t i = 0; i < pendingCount; ++i)
	{
		UnprocessedPaletteFile& palfile = m_unprocessedPaletteFiles.front();

		CharPaletteHandle& charPalHandle = GetPlayerCharPaletteHandle(palfile.matchPlayerIndex);

		if (IsPaletteHandleReady(charPalHandle))
		{
			if (g_modVals.enableForeignPalettes)
			{
				m_pPaletteManager->ReplacePaletteFile(palfile.palData, palfile.palFile, charPalHandle);
			}
		}
		else
		{
			m_unprocessedPaletteFiles.push(palfile);
		}

		m_unprocessedPaletteFiles.pop();
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
