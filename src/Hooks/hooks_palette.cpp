#include "hooks_palette.h"

#include "HookManager.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Game/characters.h"
#include "Core/Settings.h"

DWORD GetCharObjPointersJmpBackAddr = 0;
void __declspec(naked)GetCharObjPointers()
{
	static char* addr = nullptr;

	LOG_ASM(2, "GetCharObjPointers\n");

	__asm
	{
		pushad
		add eax, 25E8h
		mov addr, eax
	}

	g_interfaces.player1.SetCharDataPtr(addr);
	addr += 0x4;
	g_interfaces.player2.SetCharDataPtr(addr);

	__asm
	{
		popad
		mov[eax + edi * 4 + 25E8h], esi
		jmp[GetCharObjPointersJmpBackAddr]
	}
}

DWORD ForceBloomOnJmpBackAddr = 0;
int restoredForceBloomOffAddr = 0;
void __declspec(naked)ForceBloomOn()
{
	static CharData* pCharObj = nullptr;
	static CharPaletteHandle* pCharHandle = nullptr;

	LOG_ASM(7, "ForceBloomOn\n");

	__asm
	{
		mov [pCharObj], edi
		pushad
	}

	if (pCharObj == g_interfaces.player1.GetData())
	{
		pCharHandle = &g_interfaces.player1.GetPalHandle();
	}
	else
	{
		pCharHandle = &g_interfaces.player2.GetPalHandle();
	}

	if (pCharHandle->IsCurrentPalWithBloom())
	{
		__asm jmp TURN_BLOOM_ON
	}

	__asm
	{
		popad
		jmp[restoredForceBloomOffAddr]
TURN_BLOOM_ON:
		popad
		jmp[ForceBloomOnJmpBackAddr]
	}
}

DWORD GetIsP1CPUJmpBackAddr = 0;
void __declspec(naked)GetIsP1CPU()
{
	LOG_ASM(2, "GetIsP1CPU\n");

	__asm
	{
		mov[eax + 1688h], edi
		mov g_gameVals.isP1CPU, edi;
		jmp[GetIsP1CPUJmpBackAddr]
	}
}

DWORD GetGameStateCharacterSelectJmpBackAddr = 0;
void __declspec(naked)GetGameStateCharacterSelect()
{
	LOG_ASM(2, "GetGameStateCharacterSelect\n");

	//

	__asm
	{
		mov dword ptr[ebx + 10Ch], 6
		jmp[GetGameStateCharacterSelectJmpBackAddr]
	}
}

DWORD GetPalBaseAddressesJmpBackAddr = 0;
void __declspec(naked) GetPalBaseAddresses()
{
	static int counter = 0;
	static char* palPointer = 0;

	LOG_ASM(2, "GetPalBaseAddresses\n");

	__asm
	{
		pushad

		mov palPointer, eax
	}

	if (counter == 0)
	{
		g_interfaces.player1.GetPalHandle().SetPointerBasePal(palPointer);
	}
	else if (counter == 1)
	{
		g_interfaces.player2.GetPalHandle().SetPointerBasePal(palPointer);
	}
	else
	{
		counter = -1;
	}

	counter++;

	__asm
	{
		popad

		mov[ecx + 830h], eax
		jmp[GetPalBaseAddressesJmpBackAddr]
	}
}

// Platinum personality/voice (Sena/Luna) override - LOCAL AUDIO ONLY, fully client-side.
//
// The personality flag lives in the select-struct singleton (base = FUN_0047E860()) at
// slot*0x20 + 0x164C (1 = Sena, 0 = Luna; char index at +0x1648), rolled at char-select by a
// deterministic shared-seed PRNG. The flag ALSO feeds the deterministic BBScript VM, so it is
// synced/checksummed match state: WRITING a value that differs from the opponent desyncs the
// match (confirmed by live test). We therefore NEVER write it.
//
// The flag is read (never written) at two kinds of client-side sites, both reading from this
// same struct. We hook each read and substitute our chosen value IN-REGISTER (never touching
// memory), gated to Platinum only, so nothing synced changes:
//   1. Voice-bank LOAD (routine FUN_00555A20, runs at battle load): reads the personality and
//      uses it to index a .pac filename table, mounting vbtl_pt_<N>.pac / vbtldb_pt_<N>.pac
//      (N=0 -> "a"/Luna cues, N=1 -> "b"/Sena cues; the two personalities ship as SEPARATE
//      banks, only the rolled one is mounted). Slot-0 reads at +0x164C (0x5560D0/621D/640C),
//      slot-1 reads at +0x166C (0x556176/556319/556488). base = select-struct singleton.
//   2. Voice-file PLAY resolvers (FUN_005CFC80 / FUN_005D1440): read the personality to pick
//      the "<base><a/b/c/d>.wav" cue name (0x5CFF57 / 0x5D15E2, `mov eax,[eax+ecx+164Ch]`).
// Both MUST be overridden together: forcing only PLAY would request "b" cues from a bank that
// only mounted "a" -> lookup fails -> silence (this was the earlier bug). Forcing LOAD too
// mounts the "b" bank so the "b" cue resolves. Everything here (mounted XACT banks, voice
// handles) is per-client heap, never part of the GGPO checksum.
// See docs/Research/PlatinumVoiceChoiceInvestigation.md.

// choice enum for a given match slot: 0 = Default (leave the game's RNG pick), 1 = Luna, 2 = Sena.
static int PlatinumVoiceChoiceForSlot(int slot)
{
	const bool online = g_interfaces.pRoomManager && g_interfaces.pRoomManager->IsRoomFunctional();
	if (online)
	{
		const bool spectator = g_interfaces.pRoomManager->IsThisPlayerSpectator();
		const int localSlot = spectator ? -1 : (int)g_interfaces.pRoomManager->GetThisPlayerMatchPlayerIndex();

		if (!spectator && slot == localSlot)
			return Settings::settingsIni.platinumVoiceChoice; // our own Platinum -> our setting
		if (g_interfaces.pOnlinePaletteManager)
			return g_interfaces.pOnlinePaletteManager->GetPlayerVoiceChoice((uint16_t)slot); // their sent choice (0 if none/mod-less)
		return 0;
	}

	return (slot == 0) ? Settings::settingsIni.platinumVoiceChoice : 0; // offline: local player is P1
}

// Shared by every hook. base = select-struct singleton; slot = 0/1; kind = 0 for a voice-bank
// LOAD read, 1 for a PLAY-resolver read (used only for logging). Reads (never writes) the char
// index and the rolled personality, and returns the value the hooked instruction should yield:
// the chosen personality for the local override, or the original rolled value otherwise
// (non-Platinum, no override, bad slot) so behaviour is identical to vanilla in those cases.
static int BiasPlatinumPersonality(DWORD base, int slot, int kind)
{
	if (!base || (slot != 0 && slot != 1))
		return 0;

	const int* asInts = reinterpret_cast<const int*>(base + (DWORD)slot * 0x20 + 0x1648);
	const int original = asInts[1]; // +0x164C, exactly what the hooked mov reads

	if (asInts[0] != CharIndex_Platinum)
		return original;

	const int choice = PlatinumVoiceChoiceForSlot(slot);
	const int forced = (choice == 0) ? original : ((choice == 2) ? 1 : 0); // 1=Sena(b), 0=Luna(a)

	// Diagnostics for online tests. LOAD reads are bounded (a handful per match) so we always
	// log them - they prove which bank got mounted. PLAY reads fire per voice line, so dedup
	// them per slot to avoid spam. Rich context lets us diagnose slot mapping / packet receipt.
	const bool online = g_interfaces.pRoomManager && g_interfaces.pRoomManager->IsRoomFunctional();
	const int localSlot = (online && !g_interfaces.pRoomManager->IsThisPlayerSpectator())
		? (int)g_interfaces.pRoomManager->GetThisPlayerMatchPlayerIndex() : -1;
	const int oppSent = (online && g_interfaces.pOnlinePaletteManager)
		? g_interfaces.pOnlinePaletteManager->GetPlayerVoiceChoice((uint16_t)slot) : -1;
	const char* kindStr = (kind == 0) ? "load" : "play";
	const char* choiceStr = (choice == 2) ? "Sena" : (choice == 1) ? "Luna" : "default";

	static int sLastPlayLogged[2] = { -99, -99 };
	const bool shouldLog = (kind == 0) || (sLastPlayLogged[slot] != forced);
	if (kind == 1)
		sLastPlayLogged[slot] = forced;
	if (shouldLog)
		LOG(2, "[PlatVoice] %s slot%d orig=%d -> %d choice=%s online=%d localSlot=%d oppSent=%d\n",
			kindStr, slot, original, forced, choiceStr, (int)online, localSlot, oppSent);

	return forced;
}

// --- PLAY resolvers: `mov eax,[eax+ecx+164Ch]` (7 bytes). eax = slot<<5, ecx = base.
// Replace the read: put the biased personality in eax for the following suffix switch.
DWORD PlatVoiceResolverAJmpBackAddr = 0;
void __declspec(naked) PlatVoiceResolverAHook()
{
	static DWORD sBase; static DWORD sShift; static int sOut;
	__asm { mov sBase, ecx
		mov sShift, eax
		pushad }
	sOut = BiasPlatinumPersonality(sBase, (int)(sShift >> 5), 1);
	__asm { popad
		mov eax, sOut
		jmp [PlatVoiceResolverAJmpBackAddr] }
}

DWORD PlatVoiceResolverBJmpBackAddr = 0;
void __declspec(naked) PlatVoiceResolverBHook()
{
	static DWORD sBase; static DWORD sShift; static int sOut;
	__asm { mov sBase, ecx
		mov sShift, eax
		pushad }
	sOut = BiasPlatinumPersonality(sBase, (int)(sShift >> 5), 1);
	__asm { popad
		mov eax, sOut
		jmp [PlatVoiceResolverBJmpBackAddr] }
}

// --- LOAD sites: `mov esi,[<base>+imm]` (6 bytes). base = edi (or eax at 0x556488). imm is
// +0x164C for slot 0 / +0x166C for slot 1. Replace the read: put the biased personality in
// esi so the game mounts that personality's bank. The slot is baked into each function.
DWORD PlatVoiceLoadVbtlP1JmpBackAddr = 0;
void __declspec(naked) PlatVoiceLoadVbtlP1Hook() // 0x5560D0 vbtl_pt slot0 (edi)
{
	static DWORD sBase; static int sOut;
	__asm { mov sBase, edi
		pushad }
	sOut = BiasPlatinumPersonality(sBase, 0, 0);
	__asm { popad
		mov esi, sOut
		jmp [PlatVoiceLoadVbtlP1JmpBackAddr] }
}

DWORD PlatVoiceLoadVbtlP2JmpBackAddr = 0;
void __declspec(naked) PlatVoiceLoadVbtlP2Hook() // 0x556176 vbtl_pt slot1 (edi)
{
	static DWORD sBase; static int sOut;
	__asm { mov sBase, edi
		pushad }
	sOut = BiasPlatinumPersonality(sBase, 1, 0);
	__asm { popad
		mov esi, sOut
		jmp [PlatVoiceLoadVbtlP2JmpBackAddr] }
}

DWORD PlatVoiceLoadSubP1JmpBackAddr = 0;
void __declspec(naked) PlatVoiceLoadSubP1Hook() // 0x55621D subvoice slot0 (edi)
{
	static DWORD sBase; static int sOut;
	__asm { mov sBase, edi
		pushad }
	sOut = BiasPlatinumPersonality(sBase, 0, 0);
	__asm { popad
		mov esi, sOut
		jmp [PlatVoiceLoadSubP1JmpBackAddr] }
}

DWORD PlatVoiceLoadSubP2JmpBackAddr = 0;
void __declspec(naked) PlatVoiceLoadSubP2Hook() // 0x556319 subvoice slot1 (edi)
{
	static DWORD sBase; static int sOut;
	__asm { mov sBase, edi
		pushad }
	sOut = BiasPlatinumPersonality(sBase, 1, 0);
	__asm { popad
		mov esi, sOut
		jmp [PlatVoiceLoadSubP2JmpBackAddr] }
}

DWORD PlatVoiceLoadDbP1JmpBackAddr = 0;
void __declspec(naked) PlatVoiceLoadDbP1Hook() // 0x55640C vbtldb slot0 (edi)
{
	static DWORD sBase; static int sOut;
	__asm { mov sBase, edi
		pushad }
	sOut = BiasPlatinumPersonality(sBase, 0, 0);
	__asm { popad
		mov esi, sOut
		jmp [PlatVoiceLoadDbP1JmpBackAddr] }
}

DWORD PlatVoiceLoadDbP2JmpBackAddr = 0;
void __declspec(naked) PlatVoiceLoadDbP2Hook() // 0x556488 vbtldb slot1 (eax)
{
	static DWORD sBase; static int sOut;
	__asm { mov sBase, eax
		pushad }
	sOut = BiasPlatinumPersonality(sBase, 1, 0);
	__asm { popad
		mov esi, sOut
		jmp [PlatVoiceLoadDbP2JmpBackAddr] }
}

DWORD GetPaletteIndexPointersJmpBackAddr = 0;
void __declspec(naked) GetPaletteIndexPointers()
{
	static int* pPalIndex = nullptr;

	LOG_ASM(2, "GetPaletteIndexPointers\n");

	__asm
	{
		pushad
		add esi, 8h
		mov pPalIndex, esi
	}

	LOG_ASM(2, "\t- P1 palIndex: 0x%p\n", pPalIndex);
	g_interfaces.player1.GetPalHandle().SetPointerPalIndex(pPalIndex);

	__asm
	{
		add esi, 20h
		mov pPalIndex, esi
	}

	LOG_ASM(2, "\t- P2 palIndex: 0x%p\n", pPalIndex);
	g_interfaces.player2.GetPalHandle().SetPointerPalIndex(pPalIndex);

	__asm
	{
		popad
		lea edi, [edx + 24D8h]
		jmp[GetPaletteIndexPointersJmpBackAddr]
	}
}
// DWORD P1InputJmpBackAddr = 0;
//void __declspec(naked)P1Input()
//{
//	LOG_ASM(2, "P1Input\n");
//	
//	static char* addr = nullptr;
//	static int playerNr = -1;
//	__asm
//	{
//		movzx edi, ax
//		mov[esi], di
//		mov[addr], esi
		
//	}
//	g_gameVals.P1InputJumpBackAdress = P1InputJmpBackAddr;

//	__asm
//	{
//		jmp[P1InputJmpBackAddr]
//	}
	
//}]

bool placeHooks_palette()
{
	GetCharObjPointersJmpBackAddr = HookManager::SetHook("GetCharObjPointers", "\x89\xB4\x00\x00\x00\x00\x00\x8B\x45",
		"xx?????xx", 7, GetCharObjPointers);

	GetPalBaseAddressesJmpBackAddr = HookManager::SetHook("GetPalBaseAddresses", "\x89\x81\x30\x08\x00\x00\x8b\xc8\xe8\x00\x00\x00\x00\x5f",
		"xxxxxxxxx????x", 6, GetPalBaseAddresses);

	GetPaletteIndexPointersJmpBackAddr = HookManager::SetHook("GetPaletteIndexPointers", "\x8d\xba\xd8\x24\x00\x00\xb9\x00\x00\x00\x00",
		"xxxxxxx????", 6, GetPaletteIndexPointers);

	GetGameStateCharacterSelectJmpBackAddr = HookManager::SetHook("GetGameStateCharacterSelect", "\xc7\x83\x0c\x01\x00\x00\x06\x00\x00\x00\xe8",
		"xxxxxxxxxxx", 10, GetGameStateCharacterSelect);

	ForceBloomOnJmpBackAddr = HookManager::SetHook("ForceBloomOn", "\x83\xfe\x15\x75", "xxxx", 5, ForceBloomOn, false);
	restoredForceBloomOffAddr = ForceBloomOnJmpBackAddr + HookManager::GetBytesFromAddr("ForceBloomOn", 4, 1);
	HookManager::ActivateHook("ForceBloomOn");

	GetIsP1CPUJmpBackAddr = HookManager::SetHook("GetIsP1CPU", "\x89\xB8\x00\x00\x00\x00\x8B\x83",
		"xx????xx", 6, GetIsP1CPU);

	// Platinum voice LOCAL-audio override: hook both the voice-bank LOAD reads and the
	// voice-file PLAY reads of the personality flag, biasing the value in-register (never
	// writing memory), gated to Platinum. See BiasPlatinumPersonality / block comment above.
	// All addresses are direct (base + RVA). LOAD reads are 6-byte `mov esi,[..+imm]`; PLAY
	// reads are 7-byte `mov eax,[eax+ecx+164Ch]`.
	const DWORD bbcfBase = reinterpret_cast<DWORD>(GetBbcfBaseAdress());
	// PLAY resolvers (FUN_005CFC80 / FUN_005D1440)
	PlatVoiceResolverAJmpBackAddr    = HookManager::SetHook("PlatVoiceResolverA",  bbcfBase + 0x001CFF57, 7, PlatVoiceResolverAHook);
	PlatVoiceResolverBJmpBackAddr    = HookManager::SetHook("PlatVoiceResolverB",  bbcfBase + 0x001D15E2, 7, PlatVoiceResolverBHook);
	// LOAD reads (voice-bank mount routine FUN_00555A20): 3 categories x 2 player slots
	PlatVoiceLoadVbtlP1JmpBackAddr   = HookManager::SetHook("PlatVoiceLoadVbtlP1", bbcfBase + 0x001560D0, 6, PlatVoiceLoadVbtlP1Hook);
	PlatVoiceLoadVbtlP2JmpBackAddr   = HookManager::SetHook("PlatVoiceLoadVbtlP2", bbcfBase + 0x00156176, 6, PlatVoiceLoadVbtlP2Hook);
	PlatVoiceLoadSubP1JmpBackAddr    = HookManager::SetHook("PlatVoiceLoadSubP1",  bbcfBase + 0x0015621D, 6, PlatVoiceLoadSubP1Hook);
	PlatVoiceLoadSubP2JmpBackAddr    = HookManager::SetHook("PlatVoiceLoadSubP2",  bbcfBase + 0x00156319, 6, PlatVoiceLoadSubP2Hook);
	PlatVoiceLoadDbP1JmpBackAddr     = HookManager::SetHook("PlatVoiceLoadDbP1",   bbcfBase + 0x0015640C, 6, PlatVoiceLoadDbP1Hook);
	PlatVoiceLoadDbP2JmpBackAddr     = HookManager::SetHook("PlatVoiceLoadDbP2",   bbcfBase + 0x00156488, 6, PlatVoiceLoadDbP2Hook);


//	P1InputJmpBackAddr = HookManager::SetHook("P1Input", "\x0F\xB7\x00\x66\x89\x00\xE9\x00\x00\x00\x00\x53",
//		"xx?xx?x????x", 6, P1Input);

	return true;
}
