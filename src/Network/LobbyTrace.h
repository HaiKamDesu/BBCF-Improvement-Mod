#pragma once
#include <steam_api.h>

#include <cstdint>

// Lobby trace: a running account of everything the online lobby system does, written to
// DEBUG.txt with the prefix [LobbyTrace], so a tester's session shows exactly where a join,
// a connection or a room went wrong - and whether Steam, the other player or the game itself
// was the cause. See docs/Decompilation/Lobby.md for the system this observes.
//
// It only watches: every Steam lobby call the game makes and its result (including results
// that never arrive), Steam's own lobby and P2P events checked against who is actually in
// our lobby and room, P2P traffic per peer and channel, the game's lobby messages by type,
// the P2P session state at the moments that matter, the network task's states and dialogs,
// the network server's state bytes, the room member table, and GGPO assertions. Nothing it
// does changes what the game does.
namespace LobbyTrace
{
	bool Enabled();

	// ISteamMatchmaking, called from SteamMatchmakingWrapper.
	void OnCreateLobby(int lobbyType, int maxMembers, uint64_t call, void* caller);
	void OnJoinLobby(uint64_t lobby, uint64_t owner, uint64_t call, void* caller);
	void OnLeaveLobby(uint64_t lobby, void* caller);
	void OnListFilter(const char* description);
	void OnRequestLobbyList(uint64_t call, void* caller);
	void OnSetLobbyData(uint64_t lobby, const char* key, const char* value);
	void OnLobbySetting(const char* what, uint64_t lobby, int value);

	// True when `id` owns a Steam lobby we are in (created or entered, not left since). The
	// membership is tracked while LobbyTrace or LobbyFixes is on.
	bool IsOwnerOfJoinedLobby(uint64_t id);

	// SteamAPI_RegisterCallResult / UnregisterCallResult: wraps the lobby call results
	// (create, join, list) so their outcome is logged before the game's handler gets it.
	CCallbackBase* WrapRegisterCallResult(CCallbackBase* callback, uint64_t call);
	CCallbackBase* MapUnregisterCallResult(CCallbackBase* callback, uint64_t call);

	// ISteamNetworking, called from SteamNetworkingWrapper.
	void OnSendP2P(uint64_t remote, const void* data, uint32_t size, int sendType, int channel, bool ok);
	void OnReadP2P(uint64_t remote, const void* data, uint32_t size, int channel);
	void OnAcceptP2P(uint64_t remote, bool ok, void* caller);
	void OnCloseP2P(uint64_t remote, int channel, void* caller);
	void OnAllowRelay(bool allow);

	// Game patches (GGPO assertion log). Byte-checked; skipped when the build differs.
	void InstallGameHooks();

	// Once per frame on the main thread, after Steam's callbacks have run.
	void Tick();
}
