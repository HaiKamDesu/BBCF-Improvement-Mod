#pragma once

#include "Game/Room/Room.h"

#include <cstdint>
#include <string>

#include <steam_api.h>

// Copy/paste sharing for player-created BBCF rooms.
//
// Today the community shares rooms by hand: open the host's Steam profile, right-click
// Join Game, copy the link, paste it into Discord; the receiver pastes it into a browser
// URL bar so the Steam client hands it back to the game. This collapses both halves into
// hotkeys (Ctrl+C / Ctrl+V by default).
//
// Copy formats the room you are currently in as
//     steam://joinlobby/586140/<lobbyID>/<yourSteamID64>
// and puts it on the clipboard. Any member can produce a working link -- the third field
// is only used by the Steam client to resolve who to follow, and never reaches the game.
//
// Join parses such a link out of the clipboard and hands the URL to the Steam client,
// which posts GameLobbyJoinRequested_t to BBCF; the game then calls
// ISteamMatchmaking::JoinLobby itself and runs its own leave/join/error flow. We
// deliberately never call JoinLobby directly: entering a Steam lobby behind the game's
// back would leave its room state machine unaware of the lobby it is now in.
//
// The lobby ID can only be learned from Steam callbacks -- there is no "which lobby am I
// in" API -- so it is cached from LobbyEnter_t, with LobbyDataUpdate_t as a recovery path
// for the case where the enter event was missed (e.g. mod initialized late).
class LobbyLinkManager
{
public:
	static LobbyLinkManager& GetInstance();

	// Driven once per game frame from the GetFrameCounter hook. Handles hotkey edges.
	void Tick();

	// Called from SteamMatchmakingWrapper::LeaveLobby so the cache never outlives the room.
	void OnLeaveLobby(uint64_t lobbyId);

	uint64_t GetCurrentLobbyId() const { return m_currentLobbyId; }

	// True when there is a cached lobby and the current room is a player-created one
	// (lobby / free-for-all / training) rather than ranked, spectate or replay.
	bool CanShareCurrentRoom() const;

	// Hotkey actions. Public so a UI button can trigger them later.
	void CopyCurrentLobbyLink();
	void JoinLobbyFromClipboard();

private:
	LobbyLinkManager() = default;

	void CacheLobbyId(uint64_t lobbyId, const char* source);
	bool IsLocalPlayerInLobby(uint64_t lobbyId) const;

	STEAM_CALLBACK(LobbyLinkManager, OnLobbyEnter, LobbyEnter_t);
	STEAM_CALLBACK(LobbyLinkManager, OnLobbyDataUpdate, LobbyDataUpdate_t);

	uint64_t m_currentLobbyId = 0;

	// Set when we fire a steam:// join, so a LobbyEnter_t failure arriving shortly after
	// can be surfaced as a toast instead of leaving the user with the game's generic
	// popup. Cleared once consumed or once it ages out.
	unsigned long long m_pasteJoinTickMs = 0;

};

// Parsing helpers, exposed for reuse and so their edge cases are testable by inspection.
namespace LobbyLink
{
	// Formats a shareable steam://joinlobby URL. Returns an empty string if either ID is 0.
	std::string FormatJoinUrl(uint64_t lobbyId, uint64_t memberSteamId);

	// Scans arbitrary text (a whole Discord message, not just a bare URL) for the first
	// BBCF joinlobby link and returns its lobby ID, or 0 if none is present. Links for
	// other app IDs are rejected. When outMemberSteamId is given it receives the link's
	// third field -- a member of that lobby, which must be preserved rather than replaced
	// with our own ID, since we are not in the lobby we are asking to join.
	uint64_t ParseLobbyIdFromText(const std::string& text, uint64_t* outMemberSteamId = nullptr);

	// True for room types that are player-created and shareable over Steam.
	bool IsShareableRoomType(RoomType type);
}
