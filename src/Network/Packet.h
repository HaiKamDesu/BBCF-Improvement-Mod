#pragma once
#include <cstdint>
#include <cstring>

#define IM_PACKET_VERSION 0x95
#define IM_PACKET_VERSION_ASM 95h

constexpr int MAX_DATA_SIZE = 1200;

enum PacketType : uint16_t
{
	PacketType_IMID_Announce,
	PacketType_IMID_Acknowledge,
	PacketType_PaletteInfo,
	PacketType_PaletteData,
	PacketType_GameMode,
	PacketType_UploadReplayEnabled_Broadcast,
	PacketType_UploadReplayEnabled_Check,
	PacketType_UploadReplayEnabled_Response,
	PacketType_PaletteDownloadPermission,
	PacketType_PlatinumVoiceChoice,
	// 8.9.2+: PerformanceMonitor::Report, player to player only, at most 1 Hz and only while
	// one of the two has the online window's performance section open. Only ever sent to a
	// peer whose ModVersionInfo advertised kModCap_PerformanceReport, so an older build never
	// receives it. Append new types below.
	PacketType_PerformanceReport,
	// 8.9.2+: PerformanceMonitor ping location (see PerformanceMonitor.h "ROUTE CHECK"). Sent
	// ONLY to the opponent, ONLY while the game's P2P session with them is direct (not relayed),
	// once per match and on change, and only while one of the two has the stats window open.
	PacketType_PingLocation,
};

// 8.9.2+: rides as the DATA of PacketType_IMID_Acknowledge, which used to be empty. Every
// older build's AcceptAcknowledge/SendAcknowledge ignores packet data, so they still parse the
// packet exactly as before. The announce itself stays empty: it is the one packet a vanilla
// client receives (see RoomManager::SendAnnounce), and it must not change shape.
//
// Exchange: B answers A's announce with an ack carrying B's info and kModInfo_ReplyWanted;
// A (if new) answers that with an ack carrying A's info and no ReplyWanted, so it stops there.
// An old A ignores the data and never replies, which is how B tells "older than 8.9.2".
constexpr uint32_t kModInfoMagic = 0x49564D49; // "IMVI"
constexpr uint8_t kModInfo_ReplyWanted = 1 << 0;
constexpr uint16_t kModCap_PerformanceReport = 1 << 0;

#pragma pack(push, 1)
struct ModVersionInfo
{
	uint32_t magic;       // kModInfoMagic
	uint8_t infoVersion;  // 1
	uint8_t flags;        // kModInfo_*
	uint16_t caps;        // kModCap_*
	char version[24];     // MOD_VERSION, NUL-terminated
};
#pragma pack(pop)
static_assert(sizeof(ModVersionInfo) == 32, "ModVersionInfo is a wire format");

// BBCF packets' first two fields must be the packet size
struct Packet
{
	uint16_t __packetSize; // BBCF packets' first two fields must be the packet size, the game performs validation on them upon receiving
	uint16_t packetSize;
	uint16_t version = IM_PACKET_VERSION;
	PacketType packetType;
	uint64_t steamID;
	uint16_t part;
	uint16_t roomMemberIndex;
	uint32_t dataSize;
	unsigned char data[MAX_DATA_SIZE];

	Packet(void* dataSrc, uint16_t dataSize, PacketType packetType, uint16_t roomMemberIndex, uint16_t part = 0)
		: dataSize(dataSize), packetType(packetType), roomMemberIndex(roomMemberIndex), part(part)
	{
		// HeaderSize + dataSize
		packetSize = sizeof(Packet) - MAX_DATA_SIZE + dataSize;
		__packetSize = packetSize;

		if (dataSrc && dataSize)
		{
			memcpy_s(data, MAX_DATA_SIZE, dataSrc, dataSize);
		}
	}
};
