#include "SpectatorSyncDiagnostics.h"

#include "Core/Settings.h"
#include "Core/logger.h"
#include "Core/utils.h"

#include <Windows.h>
#include <cstdint>

// RVAs (VA - module base 0x00400000), traced from tools/bbcf_disasm_ascii.txt.
namespace
{
	// SteamSpectatorBackend fields.
	constexpr uintptr_t kNextInputToSendOffset = 0x5CD4;
	constexpr uintptr_t kMaxReceivedFrameOffset = 0x5CD8;

	// Vanilla advance-vs-stall gate inputs (mirrors FUN_0055EDB0).
	constexpr uintptr_t kGetNetworkSessionRva = 0x0009A400; // FUN_0049A400 -> session
	constexpr uintptr_t kGetSessionModeRva = 0x0009A340;    // FUN_0049A340(session) -> mode (1/2=player, 3=spectator)
	constexpr uintptr_t kGetBattleSceneRva = 0x0015C540;    // FUN_0055C540 -> battle scene base
	constexpr uintptr_t kSceneStateObjectOffset = 0x62B7C;
	constexpr uintptr_t kSceneStateAOffset = 0x30;
	constexpr uintptr_t kSceneStateBOffset = 0x34;
	constexpr uintptr_t kSceneStateCOffset = 0x38;

	typedef void* (__cdecl* GetNetworkSession_t)();
	typedef int(__thiscall* GetSessionMode_t)(void* session);
	typedef uint8_t* (__cdecl* GetBattleScene_t)();

	int32_t ReadI32(uintptr_t addr) { return *reinterpret_cast<volatile int32_t*>(addr); }

	int g_starvationCount = 0;
	int g_zeroInputAdvances = 0;
	int g_stalls = 0;
	int g_sceneA = -999, g_sceneB = -999, g_sceneC = -999;
	unsigned long g_lastLoggedBackend = 0;

	// Replicates FUN_0055EDB0's spectator advance-with-zero gate, for logging. Reads the
	// live scene-state triple into g_sceneA/B/C as a side effect.
	bool VanillaWillAdvanceWithZero(uintptr_t moduleBase)
	{
		g_sceneA = g_sceneB = g_sceneC = -999;
		void* session = reinterpret_cast<GetNetworkSession_t>(moduleBase + kGetNetworkSessionRva)();
		if (!session)
			return false;
		int mode = reinterpret_cast<GetSessionMode_t>(moduleBase + kGetSessionModeRva)(session);
		if (mode != 3)
			return false; // only spectator mode ever advances-with-zero
		uint8_t* scene = reinterpret_cast<GetBattleScene_t>(moduleBase + kGetBattleSceneRva)();
		if (!scene)
			return false;
		const uintptr_t obj = reinterpret_cast<uintptr_t>(scene) + kSceneStateObjectOffset;
		g_sceneA = ReadI32(obj + kSceneStateAOffset);
		g_sceneB = ReadI32(obj + kSceneStateBOffset);
		g_sceneC = ReadI32(obj + kSceneStateCOffset);
		const bool statePair =
			(g_sceneA == 4 && g_sceneB == 5) ||
			(g_sceneA == 5 && g_sceneB == 3) ||
			(g_sceneB == 4);
		return statePair && (g_sceneC != -1);
	}
}

extern "C" unsigned long g_spectatorBackendPtrRaw = 0;
extern "C" unsigned long g_spectatorSyncInjectFramesRemaining = 0;

extern "C" int __cdecl SpectatorSyncOnStarvationThunk()
{
	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());

	bool advanceWithZero = false;
	int32_t nextInput = -1, maxRecv = -1;
	__try
	{
		advanceWithZero = VanillaWillAdvanceWithZero(moduleBase);
		const uintptr_t backend = g_spectatorBackendPtrRaw;
		if (backend)
		{
			nextInput = ReadI32(backend + kNextInputToSendOffset);
			maxRecv = ReadI32(backend + kMaxReceivedFrameOffset);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0; // never let instrumentation crash the game; behave like vanilla
	}

	if (g_spectatorBackendPtrRaw != g_lastLoggedBackend)
	{
		g_lastLoggedBackend = g_spectatorBackendPtrRaw;
		g_starvationCount = g_zeroInputAdvances = g_stalls = 0;
		LOG(1, "SpectatorSync: new spectate session, backend=0x%08lX\n", g_spectatorBackendPtrRaw);
	}

	g_starvationCount++;
	if (advanceWithZero)
		g_zeroInputAdvances++;
	else
		g_stalls++;

	// Every advance-with-zero is the smoking gun -- log all of them. Log stalls sparsely.
	if (advanceWithZero || g_starvationCount <= 20 || (g_starvationCount % 300) == 0)
	{
		LOG(1, "SpectatorSync: starvation #%d scene=(%d,%d,%d) nextInput=%d maxRecv=%d lag=%d -> vanilla %s (zeroAdv total %d)\n",
			g_starvationCount, g_sceneA, g_sceneB, g_sceneC, nextInput, maxRecv,
			(nextInput >= 0 && maxRecv >= 0) ? (maxRecv - nextInput) : -1,
			advanceWithZero ? "ADVANCE-WITH-ZERO-INPUT (desync-prone)" : "stall (safe wait)",
			g_zeroInputAdvances);
	}

	return 0; // instrumentation only -- vanilla decides the advance
}

SpectatorSyncDiagnostics::Status SpectatorSyncDiagnostics::GetStatus()
{
	Status s = {};
	s.backendPtr = g_spectatorBackendPtrRaw;
	s.starvationCount = g_starvationCount;
	s.zeroInputAdvances = g_zeroInputAdvances;
	s.stalls = g_stalls;
	s.injectRemaining = static_cast<int>(g_spectatorSyncInjectFramesRemaining);
	s.sceneA = g_sceneA; s.sceneB = g_sceneB; s.sceneC = g_sceneC;
	s.nextInputToSend = -1;
	s.maxReceivedFrame = -1;
	const uintptr_t backend = g_spectatorBackendPtrRaw;
	if (backend)
	{
		__try
		{
			s.nextInputToSend = ReadI32(backend + kNextInputToSendOffset);
			s.maxReceivedFrame = ReadI32(backend + kMaxReceivedFrameOffset);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			s.nextInputToSend = -1;
			s.maxReceivedFrame = -1;
		}
	}
	return s;
}

void SpectatorSyncDiagnostics::InjectDesync(int frames)
{
	if (frames < 0)
		frames = 0;
	LOG(1, "SpectatorSync: INJECTION ARMED for %d simulated starvations (backend=0x%08lX)\n",
		frames, g_spectatorBackendPtrRaw);
	g_spectatorSyncInjectFramesRemaining = static_cast<unsigned long>(frames);
}
