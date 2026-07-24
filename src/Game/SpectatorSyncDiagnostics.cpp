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

	// SteamSpectatorBackend::DoPoll (Udp::Poll + PollUdpProtocolEvents = the network
	// drain). __thiscall, one (unused) stack arg, cleans its own stack (ret 4).
	constexpr uintptr_t kSpectatorDoPollRva = 0x37E090;
	typedef int(__thiscall* SpectatorDoPoll_t)(void* thisptr, int arg);

	// Vanilla advance-vs-stall gate inputs (mirrors FUN_0055EDB0).
	constexpr uintptr_t kGetNetworkSessionRva = 0x0009A400;
	constexpr uintptr_t kGetSessionModeRva = 0x0009A340;
	constexpr uintptr_t kGetBattleSceneRva = 0x0015C540;
	constexpr uintptr_t kSceneStateObjectOffset = 0x62B7C;
	constexpr uintptr_t kSceneStateAOffset = 0x30;
	constexpr uintptr_t kSceneStateBOffset = 0x34;
	constexpr uintptr_t kSceneStateCOffset = 0x38;

	typedef void* (__cdecl* GetNetworkSession_t)();
	typedef int(__thiscall* GetSessionMode_t)(void* session);
	typedef uint8_t* (__cdecl* GetBattleScene_t)();

	// Max consecutive frames to freeze while absorbing a stall. Observed real stalls were
	// <= 18 frames; 90 (~1.5s) covers them with margin while staying well under the game's
	// 5000ms (~300f) spectator disconnect timeout, so match-end resumes vanilla and reaches
	// the victory screen before the connection drops.
	constexpr int kMaxAbsorbFrames = 90;

	int32_t ReadI32(uintptr_t addr) { return *reinterpret_cast<volatile int32_t*>(addr); }

	int g_starvationCount = 0;
	int g_framesAbsorbed = 0;
	int g_zeroInputAdvancesLeaked = 0;
	int g_stalls = 0;
	int g_maxLagSeen = 0;
	int g_freezeStreak = 0;
	int32_t g_lastNextInput = -1;
	int g_sceneA = -999, g_sceneB = -999, g_sceneC = -999;
	unsigned long g_lastLoggedBackend = 0;

	// Mirrors FUN_0055EDB0's spectator advance-with-zero gate. Reads scene-state triple.
	bool VanillaWillAdvanceWithZero(uintptr_t moduleBase)
	{
		g_sceneA = g_sceneB = g_sceneC = -999;
		void* session = reinterpret_cast<GetNetworkSession_t>(moduleBase + kGetNetworkSessionRva)();
		if (!session)
			return false;
		if (reinterpret_cast<GetSessionMode_t>(moduleBase + kGetSessionModeRva)(session) != 3)
			return false;
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
extern "C" unsigned long g_spectatorInjectingThisFrame = 0;
extern "C" unsigned long g_spectatorInjectBypassFix = 0;

extern "C" int __cdecl SpectatorSyncOnStarvationThunk()
{
	const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	const uintptr_t backend = g_spectatorBackendPtrRaw;

	const bool injecting = (g_spectatorInjectingThisFrame != 0);
	g_spectatorInjectingThisFrame = 0;

	bool advanceWithZero = false;
	int32_t nextInput = -1, maxRecv = -1;
	int decision = 0;

	__try
	{
		advanceWithZero = VanillaWillAdvanceWithZero(moduleBase);
		// Injection forces the desync-prone situation on demand, regardless of the real
		// scene state, so the fix can be A/B tested anywhere.
		if (injecting)
			advanceWithZero = true;
		if (backend)
		{
			nextInput = ReadI32(backend + kNextInputToSendOffset);
			maxRecv = ReadI32(backend + kMaxReceivedFrameOffset);
		}

		if (injecting && g_spectatorInjectBypassFix)
		{
			// TEST: bypass the fix -> force the raw phantom-frame advance to reproduce
			// the desync (what vanilla does; what the fix is meant to prevent).
			decision = 2;
		}
		else if (advanceWithZero && backend)
		{
			// Pump the network drain so the awaited input can arrive and the receive
			// event queue can't overflow while we hold the fight.
			reinterpret_cast<SpectatorDoPoll_t>(moduleBase + kSpectatorDoPollRva)(reinterpret_cast<void*>(backend), 0);

			// Reset the absorb window whenever a real input got consumed (cursor moved).
			// (Skip during injection: the real input is present, so the cursor would
			// advance every frame and the window would never accumulate.)
			if (!injecting && nextInput != g_lastNextInput)
			{
				g_freezeStreak = 0;
				g_lastNextInput = nextInput;
			}
			g_freezeStreak++;
			// Freeze (prevent the phantom frame) within the absorb window; past it, let
			// vanilla advance-with-zero so match-end / dead connections still complete.
			decision = (g_freezeStreak <= kMaxAbsorbFrames) ? 1 : 0;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0; // never let the fix crash the game; fall back to vanilla
	}

	if (g_spectatorBackendPtrRaw != g_lastLoggedBackend)
	{
		g_lastLoggedBackend = g_spectatorBackendPtrRaw;
		g_starvationCount = g_framesAbsorbed = g_zeroInputAdvancesLeaked = g_stalls = 0;
		g_maxLagSeen = 0;
		LOG(1, "SpectatorSync: new spectate session, backend=0x%08lX\n", g_spectatorBackendPtrRaw);
	}

	g_starvationCount++;
	const int lag = (nextInput >= 0 && maxRecv >= 0) ? (maxRecv - nextInput) : -1;
	if (lag > g_maxLagSeen) g_maxLagSeen = lag;

	const char* outcome;
	if (decision == 2)
	{
		g_zeroInputAdvancesLeaked++;
		outcome = "FORCED advance-with-zero (TEST: fix bypassed, reproducing desync)";
	}
	else if (!advanceWithZero)
	{
		g_stalls++;
		outcome = "stall (vanilla, safe)";
	}
	else if (decision == 1)
	{
		g_framesAbsorbed++;
		outcome = "FREEZE (absorb stall, desync prevented)";
	}
	else
	{
		g_zeroInputAdvancesLeaked++;
		outcome = "advance-with-zero LEAKED (absorb window exceeded)";
	}

	// Log every advance-with-zero-state frame (frozen or leaked); stalls sparsely.
	if (advanceWithZero || g_starvationCount <= 20 || (g_starvationCount % 300) == 0)
	{
		LOG(1, "SpectatorSync: starvation #%d scene=(%d,%d,%d) nextInput=%d maxRecv=%d lag=%d streak=%d -> %s (absorbed %d, leaked %d)\n",
			g_starvationCount, g_sceneA, g_sceneB, g_sceneC, nextInput, maxRecv, lag,
			g_freezeStreak, outcome, g_framesAbsorbed, g_zeroInputAdvancesLeaked);
	}

	return decision;
}

SpectatorSyncDiagnostics::Status SpectatorSyncDiagnostics::GetStatus()
{
	Status s = {};
	s.backendPtr = g_spectatorBackendPtrRaw;
	s.starvationCount = g_starvationCount;
	s.framesAbsorbed = g_framesAbsorbed;
	s.zeroInputAdvancesLeaked = g_zeroInputAdvancesLeaked;
	s.stalls = g_stalls;
	s.currentFreezeStreak = g_freezeStreak;
	s.maxLagSeen = g_maxLagSeen;
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
