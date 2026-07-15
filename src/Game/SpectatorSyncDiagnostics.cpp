#include "SpectatorSyncDiagnostics.h"

#include "Core/Settings.h"
#include "Core/logger.h"
#include "Core/utils.h"

#include <Windows.h>
#include <cstdint>

// Addresses below are RVAs (VA - module base 0x00400000). Traced statically
// from tools/bbcf_disasm_ascii.txt; narrative and disasm excerpts in
// docs/Research/SpectatorDesyncInvestigation.md.
namespace
{
	// FUN_0049A400: no-arg getter for the network session object.
	constexpr uintptr_t kGetNetworkSessionRva = 0x0009A400;
	// FUN_0049A340: thiscall on the session object, returns the session mode.
	// 1/2 = P2P player, 3 = spectator.
	constexpr uintptr_t kGetSessionModeRva = 0x0009A340;
	// FUN_0055C540: no-arg getter for the battle-scene base; the state object
	// FUN_0055EDB0 inspects lives at scene + kSceneStateObjectOffset.
	constexpr uintptr_t kGetBattleSceneRva = 0x0015C540;
	constexpr uintptr_t kSceneStateObjectOffset = 0x62B7C;
	// State object fields read by FUN_0055EDB0 (the advance-on-error gate).
	constexpr uintptr_t kSceneStateAOffset = 0x30;
	constexpr uintptr_t kSceneStateBOffset = 0x34;
	constexpr uintptr_t kSceneStateCOffset = 0x38;

	// SteamSpectatorBackend fields (Ghidra/BBCF.h layout, confirmed in disasm
	// at spectator SyncInput 0x77E340 and PollUdpProtocolEvents 0x77E110).
	constexpr uintptr_t kBackendNextInputToSendOffset = 0x5CD4;
	constexpr uintptr_t kBackendMaxReceivedFrameOffset = 0x5CD8;

	typedef void* (__cdecl* GetNetworkSession_t)();
	typedef int(__fastcall* GetSessionMode_t)(void* pSession, void* edx);
	typedef uint8_t* (__cdecl* GetBattleScene_t)();

	int g_playerModeErrors = 0;      // starvation hits in player modes (expected, quiet)
	int g_spectatorErrors = 0;       // starvation hits in spectator mode
	int g_zeroInputAdvances = 0;     // spectator errors where the game chose to advance
	int g_forcedStalls = 0;          // advances we converted to stalls (fix enabled)
	unsigned long g_lastLoggedBackend = 0;

	int ReadInt(uintptr_t addr)
	{
		return *reinterpret_cast<volatile int*>(addr);
	}
}

extern "C" unsigned long g_spectatorBackendPtrRaw = 0;
extern "C" unsigned long g_spectatorSyncInjectFramesRemaining = 0;

SpectatorSyncDiagnostics::Status SpectatorSyncDiagnostics::GetStatus()
{
	Status s = {};
	s.backendPtr = g_spectatorBackendPtrRaw;
	s.spectatorErrors = g_spectatorErrors;
	s.zeroInputAdvances = g_zeroInputAdvances;
	s.forcedStalls = g_forcedStalls;
	s.injectRemaining = static_cast<int>(g_spectatorSyncInjectFramesRemaining);
	s.nextInputToSend = -1;
	s.maxReceivedFrame = -1;
	if (g_spectatorBackendPtrRaw)
	{
		s.nextInputToSend = ReadInt(g_spectatorBackendPtrRaw + kBackendNextInputToSendOffset);
		s.maxReceivedFrame = ReadInt(g_spectatorBackendPtrRaw + kBackendMaxReceivedFrameOffset);
	}
	return s;
}

void SpectatorSyncDiagnostics::InjectDesync(int frames)
{
	if (frames < 0)
		frames = 0;
	LOG(1, "SpectatorSync: INJECTION ARMED for %d frames (fix %s, backend=0x%08lX)\n",
		frames, Settings::settingsIni.spectatorSyncFailStall ? "ON" : "OFF",
		g_spectatorBackendPtrRaw);
	g_spectatorSyncInjectFramesRemaining = static_cast<unsigned long>(frames);
}

extern "C" int __cdecl SpectatorSyncErrorThunk()
{
	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());

	// Fault-injection path: the entry hook already forced this SyncInput to
	// fail; only the spectator backend's SyncInput is hooked, so we are in
	// spectator mode by construction. Mirror the real bug (force advance) or
	// prove the fix (force stall), and log either way.
	if (g_spectatorSyncInjectFramesRemaining > 0)
	{
		g_spectatorSyncInjectFramesRemaining--;
		g_spectatorErrors++;
		const bool stall = Settings::settingsIni.spectatorSyncFailStall;
		if (stall)
			g_forcedStalls++;
		else
			g_zeroInputAdvances++;
		LOG(1, "SpectatorSync: INJECTED syncFail (%lu left) -> %s (zeroAdv total %d)\n",
			g_spectatorSyncInjectFramesRemaining,
			stall ? "FORCED STALL (fix active)" : "FORCED ZERO-INPUT ADVANCE",
			g_zeroInputAdvances);
		return stall ? 1 : 2;
	}

	void* pSession = reinterpret_cast<GetNetworkSession_t>(moduleBase + kGetNetworkSessionRva)();
	int mode = 0;
	if (pSession)
	{
		mode = reinterpret_cast<GetSessionMode_t>(moduleBase + kGetSessionModeRva)(pSession, nullptr);
	}

	if (mode == 1 || mode == 2)
	{
		// Player-side rollback starvation: the game already always stalls here.
		g_playerModeErrors++;
		return 0;
	}

	// Replicate FUN_0055EDB0's spectator gate so we can log the decision the
	// game is about to make (we run before it does).
	int stateA = -999, stateB = -999, stateC = -999;
	bool gameWillAdvance = false;
	uint8_t* sceneBase = reinterpret_cast<GetBattleScene_t>(moduleBase + kGetBattleSceneRva)();
	if (sceneBase)
	{
		const uintptr_t stateObj = reinterpret_cast<uintptr_t>(sceneBase) + kSceneStateObjectOffset;
		stateA = ReadInt(stateObj + kSceneStateAOffset);
		stateB = ReadInt(stateObj + kSceneStateBOffset);
		stateC = ReadInt(stateObj + kSceneStateCOffset);
		const bool statePairMatches =
			(stateA == 4 && stateB == 5) ||
			(stateA == 5 && stateB == 3) ||
			(stateB == 4);
		gameWillAdvance = statePairMatches && (stateC != -1);
	}

	g_spectatorErrors++;

	int nextInputToSend = -1;
	int maxReceivedFrame = -1;
	if (g_spectatorBackendPtrRaw)
	{
		nextInputToSend = ReadInt(g_spectatorBackendPtrRaw + kBackendNextInputToSendOffset);
		maxReceivedFrame = ReadInt(g_spectatorBackendPtrRaw + kBackendMaxReceivedFrameOffset);
	}

	const bool forceStall = gameWillAdvance && Settings::settingsIni.spectatorSyncFailStall;
	if (gameWillAdvance)
	{
		g_zeroInputAdvances++;
		if (forceStall)
			g_forcedStalls++;
	}

	// The zero-input advance is the smoking gun -- log every one. Plain
	// starvation stalls are normal; log them sparsely so onset context is
	// still visible in DEBUG.txt without spamming.
	const bool newSession = g_spectatorBackendPtrRaw != g_lastLoggedBackend;
	if (newSession)
	{
		g_lastLoggedBackend = g_spectatorBackendPtrRaw;
		g_playerModeErrors = 0;
		g_spectatorErrors = 0;
		g_zeroInputAdvances = 0;
		g_forcedStalls = 0;
		LOG(1, "SpectatorSync: new spectate session, backend=0x%08lX\n", g_spectatorBackendPtrRaw);
	}
	if (gameWillAdvance || g_spectatorErrors <= 20 || (g_spectatorErrors % 300) == 0)
	{
		LOG(1, "SpectatorSync: syncFail #%d scene=(%d,%d,%d) nextInput=%d maxRecv=%d lag=%d -> %s%s (zeroAdv total %d)\n",
			g_spectatorErrors, stateA, stateB, stateC,
			nextInputToSend, maxReceivedFrame,
			(maxReceivedFrame >= 0 && nextInputToSend >= 0) ? (maxReceivedFrame - nextInputToSend) : -1,
			gameWillAdvance ? "ADVANCE-WITH-ZERO-INPUTS" : "stall",
			forceStall ? " [FORCED STALL]" : "",
			g_zeroInputAdvances);
	}

	return forceStall ? 1 : 0;
}
