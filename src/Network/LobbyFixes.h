#pragma once
#include <cstdint>

// Lobby fixes: client-side corrections to the online lobby system (ranked, player rooms, the
// join handshake). Each one fixes a bug found in docs/Decompilation/Lobby.md; the doc's
// "Fixes in the mod" section lists them with the evidence. Setting LobbyFixes=0 restores the
// vanilla behaviour for all of them. None changes what goes over the wire, so a player with
// these fixes and a vanilla player still play together.
namespace LobbyFixes
{
	bool Enabled();

	// Byte-checked game patches, applied once at hook time. Skipped when the build differs.
	void InstallGamePatches();

	// SteamMatchmakingWrapper::RequestLobbyList, just before the real call: the extra search
	// filters the game never sets.
	void BeforeRequestLobbyList();

	// SteamNetworkingWrapper::ReadP2PPacket, after a successful read: true when the packet must
	// not reach the game (it is then skipped and the next one read in its place).
	bool DropIncoming(uint64_t remote, const void* data, uint32_t size, int channel);

	// Messages the mod hands the game as if a peer had sent them (see OnSteamMemberLeft).
	// IsP2PPacketAvailable / ReadP2PPacket serve these before anything real on channel 0.
	bool PeekInjected(uint32_t* size, int channel);
	bool TakeInjected(void* dest, uint32_t capacity, uint32_t* size, uint64_t* remote, int channel);

	// Steam says `user` left `lobby` (left, disconnected, kicked or banned); `owner` is who
	// owned the lobby before the change. The game never listens to this: if the peer's own
	// leave message never arrived, the mod delivers it (0x36 to a host, 0x37 to a guest).
	void OnSteamMemberLeft(uint64_t lobby, uint64_t user, uint64_t owner, const char* how);

	// A JoinLobby came back with a failure: say why, since ranked shows every failure as
	// "failed to connect".
	void OnJoinFailed(uint32_t enterResponse);

	// Once per frame on the main thread, after Steam's callbacks have run.
	void Tick();
}
