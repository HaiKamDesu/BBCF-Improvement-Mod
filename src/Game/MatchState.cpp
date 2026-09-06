#include "MatchState.h"

#include "Audio/MusicManager.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Game/gamestates.h"
#include "Game/NetworkStallDiagnostics.h"
#include "Game/Playbacks/UnlimitedPlaybackManager.h"
#include "Game/ReplayFiles/ReplayFileManager.h"
#include "Network/RankedListConnectionFilter.h"
#include "Overlay/Window/PaletteEditorWindow.h"
#include "Overlay/Window/Ranked/RankedListFilterWindow.h"
#include "Overlay/Window/ReplayRewindWindow.h"
#include "Overlay/WindowContainer/WindowType.h"
#include "Overlay/WindowManager.h"

namespace
{
	// Every g_interfaces manager is built on the mod's own init thread, which runs
	// concurrently with the game's startup. The ones below the Steam wrappers in
	// InitManagers are built only once SteamAPI_Init has run and those wrappers
	// exist, so on a cold boot - Steam still starting while the game is already
	// walking scene transitions - any of them can legitimately still be null when a
	// transition hook fires.
	//
	// Measured on 2026-09-06: the game reached the menu screen 17 seconds in with no
	// SteamAPI_Init on record at all. InitManagers ran twice and created nothing
	// below pNetworkManager, because every guard in its cascade needs the wrappers.
	// OnMatchEnd then dereferenced a null pOnlinePaletteManager and took an access
	// violation reading 0x10, inside the deque assignment in
	// ClearSavedPalettePacketQueues. The relaunch had Steam up and was fine, which is
	// what makes this look like "crashes once on the first launch of the day".
	//
	// Deliberately NOT fixed by initializing anything earlier, or by making a
	// transition wait for init. The mod's earliest init is load-bearing: saved BGM
	// replacements must reach the filename table before the game's boot audio init or
	// Character Select's track can never be replaced at all (see the comment in
	// dllmain.cpp and commit f98a2a4). And blocking a game thread on a Steam
	// handshake would trade a crash for a hang. So the transitions skip only the
	// parts whose manager does not exist yet - exactly what MatchState::OnUpdate
	// already does on the per-frame path - and everything else still runs.
	bool g_loggedMissingPaletteManager = false;
	bool g_loggedMissingGameModeManager = false;
	bool g_loggedMissingRoomManager = false;
	bool g_loggedMissingOnlinePaletteManager = false;
	bool g_loggedMissingOnlineGameModeManager = false;
	bool g_loggedMissingReplayUploadManager = false;

	// One line per manager per process. A silent skip would hide a real ordering
	// regression; a line per transition would repeat for the life of the session.
	bool ManagerReady(const void* manager, const char* name, bool& alreadyLogged)
	{
		if (manager != nullptr)
		{
			return true;
		}

		if (!alreadyLogged)
		{
			alreadyLogged = true;
			LOG(1, "[MatchState] %s does not exist yet; skipping its part of this transition. "
			       "InitManagers builds it only once its dependencies are up.\n", name);
		}

		return false;
	}
}

void MatchState::OnMatchInit()
{
	if (!WindowManager::GetInstance().IsInitialized())
	{
		return;
	}

	LOG(2, "MatchState::OnMatchInit\n");

	RankedListConnectionFilter::GetInstance().OnMatchStarted();

	GetMusicManager().OnMatchInit();

	const bool paletteManagerReady = ManagerReady(
		g_interfaces.pPaletteManager, "pPaletteManager", g_loggedMissingPaletteManager);
	if (paletteManagerReady)
	{
		g_interfaces.pPaletteManager->LoadPaletteSettingsFile();
		g_interfaces.pPaletteManager->OnMatchInit(g_interfaces.player1, g_interfaces.player2);
	}

	// No room manager means there is no online session to reconcile against, which
	// is the same answer IsRoomFunctional would give offline.
	const bool roomFunctional =
		ManagerReady(g_interfaces.pRoomManager, "pRoomManager", g_loggedMissingRoomManager) &&
		g_interfaces.pRoomManager->IsRoomFunctional();

	if (roomFunctional)
	{
		// Prevent loading palettes.ini custom palette on opponent

		uint16_t thisPlayerMatchPlayerIndex = g_interfaces.pRoomManager->GetThisPlayerMatchPlayerIndex();

		if (paletteManagerReady && thisPlayerMatchPlayerIndex != 0)
		{
			g_interfaces.pPaletteManager->RestoreOrigPal(g_interfaces.player1.GetPalHandle());
		}

		if (paletteManagerReady && thisPlayerMatchPlayerIndex != 1)
		{
			g_interfaces.pPaletteManager->RestoreOrigPal(g_interfaces.player2.GetPalHandle());
		}

		// Send our custom palette and load opponent's. Still checked separately from
		// the room: this one additionally needs pPaletteManager, so an InitManagers
		// pass that ran before dllmain finished builds the room manager without it.
		if (ManagerReady(g_interfaces.pOnlinePaletteManager, "pOnlinePaletteManager",
			g_loggedMissingOnlinePaletteManager))
		{
			g_interfaces.pOnlinePaletteManager->OnMatchInit();
		}

		// Activate settled game mode
		if (ManagerReady(g_interfaces.pOnlineGameModeManager, "pOnlineGameModeManager",
			g_loggedMissingOnlineGameModeManager))
		{
			g_interfaces.pOnlineGameModeManager->OnMatchInit();
		}

		// Add players to steam's "recent games" list. pSteamFriendsWrapper is a
		// precondition of pRoomManager existing, so reaching here means it is up.
		for (const RoomMemberEntry* player : g_interfaces.pRoomManager->GetOtherRoomMemberEntriesInCurrentMatch())
		{
			g_interfaces.pSteamFriendsWrapper->SetPlayedWith(CSteamID(player->steamId));
		}

		// Send the broadcast to other players regarding telling if you have replay upload disabled or not.
		if (ManagerReady(g_interfaces.pReplayUploadManager, "pReplayUploadManager",
			g_loggedMissingReplayUploadManager))
		{
			g_interfaces.pReplayUploadManager->OnMatchInit();
		}


	}

	g_gameVals.isFrameFrozen = false;
	UnlimitedPlaybackManager::Instance().OnMatchInit();

	WindowManager::GetInstance().GetWindowContainer()->GetWindow<PaletteEditorWindow>(WindowType_PaletteEditor)->OnMatchInit();
}

void MatchState::OnMatchRematch()
{
	LOG(2, "MatchState::OnMatchRematch\n");

	// Backup BGM cleanup for flows that reach the rematch/summary screen without
	// hitting the primary one (UpdateMusicState clears on MatchState -> VictoryScreen,
	// while the audio engine is still alive). By here the engine is usually already
	// tearing down, so the bank ops no-op and only the game-facing state restore
	// applies. No-op unless the Jukebox took over BGM.
	GetMusicManager().RestoreNativeBgmForMatchEnd();

	if (ManagerReady(g_interfaces.pPaletteManager, "pPaletteManager", g_loggedMissingPaletteManager))
	{
		g_interfaces.pPaletteManager->OnMatchRematch(
			g_interfaces.player1,
			g_interfaces.player2
		);
	}

	if (ManagerReady(g_interfaces.pOnlinePaletteManager, "pOnlinePaletteManager",
		g_loggedMissingOnlinePaletteManager))
	{
		g_interfaces.pOnlinePaletteManager->ClearSavedPalettePacketQueues();
	}
}

void MatchState::OnMatchEnd()
{
	LOG(2, "MatchState::OnMatchEnd\n");

	// Clear the mod's custom BGM footprint at match end so the game's native
	// post-match transition (summary / Character Select / Main Menu) doesn't stall
	// on our direct-XACT state. Idempotent with the Character Select hook.
	if (GetMusicManager().IsControllingBgm())
		GetMusicManager().ClearBgmForSceneExit();

	if (ManagerReady(g_interfaces.pGameModeManager, "pGameModeManager", g_loggedMissingGameModeManager))
	{
		g_interfaces.pGameModeManager->EndGameMode();
	}

	if (ManagerReady(g_interfaces.pPaletteManager, "pPaletteManager", g_loggedMissingPaletteManager))
	{
		g_interfaces.pPaletteManager->OnMatchEnd(
			g_interfaces.player1.GetPalHandle(),
			g_interfaces.player2.GetPalHandle()
		);
	}

	// The crash of 2026-09-06 was this exact call with a null manager.
	if (ManagerReady(g_interfaces.pOnlinePaletteManager, "pOnlinePaletteManager",
		g_loggedMissingOnlinePaletteManager))
	{
		g_interfaces.pOnlinePaletteManager->ClearSavedPalettePacketQueues();
	}

	if (ManagerReady(g_interfaces.pOnlineGameModeManager, "pOnlineGameModeManager",
		g_loggedMissingOnlineGameModeManager))
	{
		g_interfaces.pOnlineGameModeManager->ClearPlayerGameModeChoices();
	}

	//resets the upload veto
	if (ManagerReady(g_interfaces.pReplayUploadManager, "pReplayUploadManager",
		g_loggedMissingReplayUploadManager))
	{
		g_interfaces.pReplayUploadManager->OnMatchEnd();
	}

}




void MatchState::OnUpdate()
{
	LOG(7, "MatchState::OnUpdate\n");

	NetworkStallDiagnostics::OnUpdate();

	if (WindowManager::GetInstance().IsInitialized())
	{
		RankedListFilterWindow* const filterWindow = WindowManager::GetInstance().GetWindowContainer()
			->GetWindow<RankedListFilterWindow>(WindowType_RankedListFilter);
		if (filterWindow != nullptr)
		{
			filterWindow->UpdateAutoVisibility();
		}
	}

	// The render hook can run during teardown or while managers are still being
	// assembled. Keep the per-frame path inert until its owned managers exist.
	if (g_interfaces.pPaletteManager != nullptr)
	{
		g_interfaces.pPaletteManager->OnUpdate(
			g_interfaces.player1.GetPalHandle(),
			g_interfaces.player2.GetPalHandle()
		);
		g_interfaces.pPaletteManager->ClearPlatinumItemPaletteLink(
			g_interfaces.player1,
			g_interfaces.player2
		);
	}
	if (g_interfaces.pOnlinePaletteManager != nullptr)
	{
		g_interfaces.pOnlinePaletteManager->OnUpdate();
	}
	if (g_interfaces.pReplayRewindManager != nullptr)
	{
		g_interfaces.pReplayRewindManager->OnUpdate();
	}
	g_rep_manager.check_and_load_replay_steam();
}

void MatchState::OnIntroPlaying() 
{
	LOG(7, "MatchState::OnIntroPlaying\n");

	if (*g_gameVals.pGameMode == GameMode_ReplayTheater) {
		WindowManager::GetInstance().GetWindowContainer()->GetWindow<ReplayRewindWindow>(WindowType_ReplayRewind)->Open();
	}
}
