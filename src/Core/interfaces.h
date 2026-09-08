#pragma once

#include "CustomGameMode/GameModeManager.h"
#include "D3D9EXWrapper/d3d9.h"
#include "D3D9EXWrapper/ID3D9EXWrapper_Device.h"
#include "Game/Player.h"
#include "Game/Room/Room.h"
#include "Game/ReplayRewind/ReplayRewind.h"

#include "Network/NetworkManager.h"
#include "Network/OnlineGameModeManager.h"
#include "Network/OnlinePaletteManager.h"
#include "Network/ReplayUploadManager.h"
#include "Network/RoomManager.h"
#include "Palette/PaletteManager.h"
#include "SteamApiWrapper/SteamApiHelper.h"
#include "SteamApiWrapper/SteamFriendsWrapper.h"
#include "SteamApiWrapper/SteamMatchmakingWrapper.h"
#include "SteamApiWrapper/SteamNetworkingWrapper.h"
#include "SteamApiWrapper/SteamUserStatsWrapper.h"
#include "SteamApiWrapper/SteamUserWrapper.h"
#include "SteamApiWrapper/SteamUtilsWrapper.h"

struct interfaces_t
{
	SteamFriendsWrapper* pSteamFriendsWrapper;
	SteamMatchmakingWrapper* pSteamMatchmakingWrapper;
	SteamNetworkingWrapper* pSteamNetworkingWrapper;
	SteamUserWrapper* pSteamUserWrapper;
	SteamUserStatsWrapper* pSteamUserStatsWrapper;
	SteamUtilsWrapper* pSteamUtilsWrapper;

	IDirect3DDevice9Ex* pD3D9ExWrapper;

	NetworkManager* pNetworkManager;
	RoomManager* pRoomManager;
	SteamApiHelper* pSteamApiHelper;

	PaletteManager* pPaletteManager;
	OnlinePaletteManager* pOnlinePaletteManager;

	GameModeManager* pGameModeManager;
	OnlineGameModeManager* pOnlineGameModeManager;

	ReplayUploadManager* pReplayUploadManager;
	ReplayRewind* pReplayRewindManager;

	Player player1;
	Player player2;
};

struct gameVals_t
{
	int* pGameState;
	int* pGameMoney;
	int* pGameMode;
	int* pMatchState;
	int* pMatchTimer;
	int* pMatchRounds;

	int playerAvatarBaseAddr;
	int* playerAvatarAddr;
	int* playerAvatarColAddr;
	byte* playerAvatarAcc1;
	byte* playerAvatarAcc2;

	int isP1CPU;
	//DWORD P1InputJumpBackAdress;
	unsigned char* stageListMemory;
	int *stageSelect_X;
	int *stageSelect_Y;
	int *musicSelect_X;
	int *musicSelect_Y;

	/////////////////
	// New fields below
	/////////////////

	// *pIsHUDHidden is a bitfield:
	// 0x00 - hud is visible
	// 0x01 - hud is hidden (intro)
	// 0x02 - hud is hidden (astral)
	// 0x04 - loading icon is shown
	int* pIsHUDHidden;

	bool isFrameFrozen;
	unsigned framesToReach;
	unsigned* pFrameCount;

	D3DXMATRIX* viewMatrix;
	D3DXMATRIX* projMatrix;

	int* pEntityList;
	int entityCount;


	Room* pRoom;
	
};

struct gameProc_t
{
	HWND hWndGameWindow;
};

// Records which window the mod should treat as the game's.
//
// This used to be guessed positionally - "the second window the process creates" - and on a
// cold boot that guess is wrong. Neptune's Report 3 caught it: the game's D3D device was
// created against 0x000500AA while the mod had latched 0x000A0682, a window that did not even
// exist when the device was made. Everything downstream then misbehaves at once - ImGui's
// backend is bound to it (so DisplaySize came out 0x0 and the overlay drew into nothing),
// PassKeyboardInputToGame compares it against GetForegroundWindow (so every keystroke was
// withheld from the game), and raw keyboard input is registered against it (so WM_INPUT never
// arrived). Gamepads use none of those, which is why they kept working.
//
// 'fromDevice' marks the authoritative source: the window D3D presents to IS the game window,
// by definition. Once one of those arrives the positional guess stops being allowed to
// overwrite it. Logs whenever the answer changes, so the next report says so outright.
void AdoptGameWindow(HWND hwnd, bool fromDevice, const char* source);

// True once AdoptGameWindow has taken a window from a D3D device.
bool IsGameWindowFromDevice();
struct modValues_t {
	bool enableForeignPalettes = true; 
	int allowPaletteDownloads = -1;
	// Hotkeys used to be mirrored here as virtual-key codes. They now live in
	// HotkeyManager, which owns modifiers and controller bindings too; ask it instead.
	int uploadReplayData;
	std::string uploadReplayDataHost; 
	std::string uploadReplayDataEndpoint;
	unsigned short uploadReplayDataPort;
	bool uploadReplayDataUseTls = false;
	bool uploadReplayDataVeto = false; //this refers to when other players disable replay upload
	float frame_history_width;
	float frame_history_height;
	float frame_history_spacing;
	bool frame_history_auto_reset;
};
//temporary placeholders until wrappers are created / final addresses updated
struct temps_t
{
	ISteamFriends** ppSteamFriends;
	ISteamMatchmaking** ppSteamMatchmaking;
	ISteamNetworking** ppSteamNetworking;
	ISteamUser** ppSteamUser;
	ISteamUserStats** ppSteamUserStats;
	ISteamUtils** ppSteamUtils;
};

extern interfaces_t g_interfaces;
extern gameProc_t g_gameProc;
extern gameVals_t g_gameVals;
extern temps_t g_tempVals;
extern modValues_t g_modVals;

int GetGameSceneStatus();
void InitManagers();
void CleanupInterfaces();
