#pragma once

#include "RoomManager.h"

#include "Palette/PaletteManager.h"

#include <queue>

class OnlinePaletteManager
{
public:
	// Whether an opponent lets us save their custom palette. Unknown means no
	// permission packet arrived (mod version without the feature, or no mod).
	enum class PaletteDownloadPermission : uint8_t
	{
		Unknown,
		Denied,
		Granted,
	};

	OnlinePaletteManager(PaletteManager* pPaletteManager, CharPaletteHandle* pP1CharPalHandle,
		CharPaletteHandle* pP2CharPalHandle, RoomManager* pRoomManager);
	void SendPalettePackets();
	void RecvPaletteDataPacket(Packet* packet);
	void RecvPaletteInfoPacket(Packet* packet);
	void RecvPaletteDownloadPermissionPacket(Packet* packet);
	void RecvPlatinumVoiceChoicePacket(Packet* packet);
	// Opponent's forced Platinum voice choice (settings enum: 0 = Default/leave RNG,
	// 1 = Luna, 2 = Sena). Returns 0 when nothing was received for that slot (opponent
	// on vanilla RNG or without the mod). Used by the per-frame voice force.
	int GetPlayerVoiceChoice(uint16_t matchPlayerIndex) const;
	void ProcessSavedPalettePackets();
	void ClearSavedPalettePacketQueues();
	void OnMatchInit();
	void OnUpdate();
	bool CanDownloadPalette(uint16_t matchPlayerIndex) const;
	PaletteDownloadPermission GetDownloadPermission(uint16_t matchPlayerIndex) const;

private:
	bool IsPaletteHandleReady(const CharPaletteHandle& charPalHandle) const;
	void SendPaletteDownloadPermissionPacket(uint16_t roomMemberIndex);
	void SendPlatinumVoiceChoicePacket(uint16_t roomMemberIndex);
	void SendPaletteInfoPacket(CharPaletteHandle& charPalHandle, uint16_t roomMemberIndex);
	void SendPaletteDataPackets(CharPaletteHandle& charPalHandle, uint16_t roomMemberIndex);
	void ProcessSavedPaletteInfoPackets();
	void ProcessSavedPaletteDataPackets();
	CharPaletteHandle& GetPlayerCharPaletteHandle(uint16_t matchPlayerIndex);

	struct UnprocessedPaletteInfo
	{
		uint16_t matchPlayerIndex;
		IMPL_info_t palInfo;

		UnprocessedPaletteInfo(uint16_t matchPlayerIndex_, IMPL_info_t* pPalInfo)
			: matchPlayerIndex(matchPlayerIndex_)
		{
			memcpy_s(&palInfo, sizeof(IMPL_info_t), pPalInfo, sizeof(IMPL_info_t));
		}
	};

	struct UnprocessedPaletteFile
	{
		uint16_t matchPlayerIndex;
		PaletteFile palFile;
		char palData[IMPL_PALETTE_DATALEN];

		UnprocessedPaletteFile(uint16_t matchPlayerIndex_, PaletteFile palFile_, char* pPalSrc)
			: matchPlayerIndex(matchPlayerIndex_), palFile(palFile_)
		{
			memcpy_s(palData, IMPL_PALETTE_DATALEN, pPalSrc, IMPL_PALETTE_DATALEN);
		}
	};

	std::queue<UnprocessedPaletteInfo> m_unprocessedPaletteInfos;
	std::queue<UnprocessedPaletteFile> m_unprocessedPaletteFiles;

	CharPaletteHandle* m_pP1CharPalHandle;
	CharPaletteHandle* m_pP2CharPalHandle;

	// Interfaces
	PaletteManager* m_pPaletteManager;
	RoomManager* m_pRoomManager;
	bool m_matchInitPending = false;
	bool m_loggedMatchInitWait = false;
	PaletteDownloadPermission m_playerPaletteDownloadPermissions[2] = {};

	// Received per-slot Platinum voice choice; -1 = none received yet (leave RNG).
	int8_t m_playerVoiceChoices[2] = { -1, -1 };
};
