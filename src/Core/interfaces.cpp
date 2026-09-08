#include "interfaces.h"

#include "logger.h"
#include "utils.h"

interfaces_t g_interfaces = {};
gameProc_t g_gameProc = {};
temps_t g_tempVals = {};
gameVals_t g_gameVals = {};
modValues_t g_modVals = {};

namespace
{
	bool g_gameWindowFromDevice = false;
}

void AdoptGameWindow(HWND hwnd, bool fromDevice, const char* source)
{
	if (hwnd == nullptr)
	{
		return;
	}

	// A guess never overrides an answer.
	if (!fromDevice && g_gameWindowFromDevice)
	{
		if (hwnd != g_gameProc.hWndGameWindow)
		{
			LOG(1, "[GameWindow] ignoring %s guess 0x%p; keeping the D3D device window 0x%p\n",
				source, hwnd, g_gameProc.hWndGameWindow);
		}
		return;
	}

	const HWND previous = g_gameProc.hWndGameWindow;

	if (fromDevice)
	{
		g_gameWindowFromDevice = true;
	}

	if (previous == hwnd)
	{
		return;
	}

	g_gameProc.hWndGameWindow = hwnd;

	if (previous != nullptr)
	{
		// The interesting case. If this ever fires it means something downstream was pointed
		// at the wrong window until now.
		LOG(1, "[GameWindow] corrected from 0x%p to 0x%p (source: %s)\n", previous, hwnd, source);
	}
	else
	{
		LOG(1, "[GameWindow] set to 0x%p (source: %s)\n", hwnd, source);
	}
}

bool IsGameWindowFromDevice()
{
	return g_gameWindowFromDevice;
}

void InitManagers()
{
	LOG(1, "InitManagers\n");
	
	if (g_interfaces.pSteamNetworkingWrapper &&
		g_interfaces.pSteamUserWrapper &&
		!g_interfaces.pNetworkManager)
	{
		g_interfaces.pNetworkManager = new NetworkManager(
			g_interfaces.pSteamNetworkingWrapper,
			g_interfaces.pSteamUserWrapper->GetSteamID()
		);
	}

	if (g_interfaces.pNetworkManager &&
		g_interfaces.pSteamUserWrapper &&
		g_interfaces.pSteamFriendsWrapper &&
		!g_interfaces.pRoomManager)
	{
		g_interfaces.pRoomManager = new RoomManager(
			g_interfaces.pNetworkManager,
			g_interfaces.pSteamFriendsWrapper,
			g_interfaces.pSteamUserWrapper->GetSteamID()
		);
	}

	if (g_interfaces.pPaletteManager &&
		g_interfaces.pRoomManager &&
		!g_interfaces.pOnlinePaletteManager)
	{
		g_interfaces.pOnlinePaletteManager = new OnlinePaletteManager(
			g_interfaces.pPaletteManager,
			&g_interfaces.player1.GetPalHandle(),
			&g_interfaces.player2.GetPalHandle(),
			g_interfaces.pRoomManager
		);
	}

	if (!g_interfaces.pGameModeManager)
	{
		g_interfaces.pGameModeManager = new GameModeManager();
	}

	if (g_interfaces.pGameModeManager &&
		g_interfaces.pRoomManager &&
		!g_interfaces.pOnlineGameModeManager)
	{
		g_interfaces.pOnlineGameModeManager = new OnlineGameModeManager(
			g_interfaces.pGameModeManager,
			g_interfaces.pRoomManager
		);
	}
	if (g_interfaces.pRoomManager &&
		!g_interfaces.pReplayUploadManager)
	{
		g_interfaces.pReplayUploadManager = new ReplayUploadManager(g_interfaces.pRoomManager);
	}
	if (!g_interfaces.pReplayRewindManager)
	{
		g_interfaces.pReplayRewindManager =  new ReplayRewind();

	}
}

void CleanupInterfaces()
{
	LOG(1, "CleanupInterfaces\n");

	SAFE_DELETE(g_interfaces.pNetworkManager);
	SAFE_DELETE(g_interfaces.pPaletteManager);
	SAFE_DELETE(g_interfaces.pRoomManager);
	SAFE_DELETE(g_interfaces.pOnlinePaletteManager);
	SAFE_DELETE(g_interfaces.pOnlineGameModeManager);
	SAFE_DELETE(g_interfaces.pGameModeManager);

	SAFE_DELETE(g_interfaces.pD3D9ExWrapper);

	SAFE_DELETE(g_interfaces.pSteamFriendsWrapper);
	SAFE_DELETE(g_interfaces.pSteamMatchmakingWrapper);
	SAFE_DELETE(g_interfaces.pSteamNetworkingWrapper);
	SAFE_DELETE(g_interfaces.pSteamUserStatsWrapper);
	SAFE_DELETE(g_interfaces.pSteamUserWrapper);
	SAFE_DELETE(g_interfaces.pSteamUtilsWrapper);
	SAFE_DELETE(g_interfaces.pSteamApiHelper);
}

int GetGameSceneStatus() {
	auto base = GetBbcfBaseAdress();
	int* pGameSceneStatus = (int*)(base + 0x8903b0 + 0x2600);
	return SafeDereferencePtr(pGameSceneStatus);
}