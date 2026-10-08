// Feeds the local input system's two player words into the game.
//
// FUN_004963F0 is each controller task's per-tick action-word builder (thiscall, ecx =
// GAME_KeyControler, plain ret, returns the effective word). Verified from its assembly
// 2026-10-07 (docs/Decompilation/Controllers.md):
//
//   prev     = [[ctrl+0x20]+0x0C]            effective word of the previous tick
//   [+0x30]  = 0; then for each action i < [ctrl+0x38]: bit = 1 << i
//              pressed   = vtbl+8(i)           (binding -> device game key)
//              [+0x30]  |= bit if pressed      physical word
//              effective |= bit if ([+0x50] ? [+0x4C] & bit : pressed)
//   Up+Down and Left+Right cancel, in both words.
//   [+0x28] |= newly set bits, [+0x2C] |= newly cleared bits (vs prev)
//   [+0x50] = 0; return effective
//
// The word's bit i is action index i, so Up = bit 0 ... Start = bit 26, Reset = bit 27 - the
// same values as LocalInput::GameButton.
//
// For pad slots 0/1 and the keyboard controllers (AASTEAM_SystemManager +0x0C..+0x28) this
// replacement computes exactly that, with "pressed" taken from the local input system instead
// of the device: slot 0 = Player 1, slot 1 = Player 2, keyboard controllers = nothing (every
// keyboard reaches the game through a player instead). The game's key config is bypassed for
// these controllers; the mod's per-device mapping replaces it. Every other controller runs the
// original.

#include "LocalInputManager.h"

#include "Core/logger.h"
#include "Core/utils.h"
#include "Hooks/HookManager.h"
#include "Hooks/RankedAutomationHarness.h"
#include "Hooks/hooks_system_input.h"
#include "Core/ControllerOverrideManager.h"

#include <cstring>

namespace LocalInput
{
	namespace
	{
		constexpr uintptr_t kBuildActionWordsRva = 0x000963F0;
		constexpr uintptr_t kSystemManagerPtrRva = 0x008929C8;   // [VA 0x00C929C8] = AASTEAM_SystemManager*

		DWORD g_buildJmpBack = 0;
		DWORD g_connectedJmpBack = 0;
		constexpr uintptr_t kIsControllerIdConnectedRva = 0x0007F480;
		constexpr uintptr_t kActiveControllerIdRva = 0x008929A0;   // engine+0x25F0, VA 0x00C929A0

		enum ControllerRole
		{
			Role_None,
			Role_KeyboardMenu,
			Role_KeyboardBattle,
			Role_Pad0Menu,
			Role_Pad1Menu,
			Role_Pad0Battle,
			Role_Pad1Battle,
		};

		ControllerRole RoleOf(void* controller)
		{
			uint8_t* const* const holder = reinterpret_cast<uint8_t* const*>(
				reinterpret_cast<uintptr_t>(GetBbcfBaseAdress()) + kSystemManagerPtrRva);
			const uint8_t* const sysMgr = *holder;
			if (!sysMgr || !controller)
				return Role_None;
			const auto at = [&](size_t offset) { return *reinterpret_cast<void* const*>(sysMgr + offset); };
			if (controller == at(0x10) || controller == at(0x1C)) return Role_Pad0Menu;
			if (controller == at(0x14) || controller == at(0x20)) return Role_Pad1Menu;
			if (controller == at(0x24)) return Role_Pad0Battle;
			if (controller == at(0x28)) return Role_Pad1Battle;
			if (controller == at(0x0C)) return Role_KeyboardMenu;
			if (controller == at(0x18)) return Role_KeyboardBattle;
			return Role_None;
		}

		SystemControllerSlot LegacySlot(ControllerRole role)
		{
			switch (role)
			{
			case Role_Pad0Menu: return SystemControllerSlot::MenuP1;
			case Role_Pad1Menu: return SystemControllerSlot::MenuP2;
			case Role_Pad0Battle: return SystemControllerSlot::CharP1;
			case Role_Pad1Battle: return SystemControllerSlot::CharP2;
			default: return SystemControllerSlot::None;
			}
		}

		uint32_t CancelOpposites(uint32_t word)
		{
			if ((word & 0x1) && (word & 0x4)) word &= ~0x5u;
			if ((word & 0x2) && (word & 0x8)) word &= ~0xAu;
			return word;
		}

		// Returns true and the word for one of our controllers.
		bool __cdecl TryOurWord(void* controller, uint32_t* word)
		{
			Manager& manager = Manager::Get();
			if (!manager.IsActive())
				return false;
			const ControllerRole role = RoleOf(controller);
			if (role == Role_None)
				return false;

			manager.Tick(true);

			// Player 1 is always "the" local player: menus, single-player modes and netplay all
			// follow the active controller id, so it stays 0 (pad slot 0 = Player 1). Without
			// this, a Player 2 device pressing start on the title would hand Player 1's side to
			// Player 2's devices. Nothing in the game depends on it being 1
			// (Controllers.md section 0, answer 3).
			int32_t* const activeId = reinterpret_cast<int32_t*>(
				reinterpret_cast<uintptr_t>(GetBbcfBaseAdress()) + kActiveControllerIdRva);
			if (*activeId != 0)
			{
				static int lastLogged = 0;
				if (lastLogged != *activeId)
				{
					lastLogged = *activeId;
					LOG(1, "[LocalInput] the game switched its active controller to id %d; put back to 0 (Player 1)\n", *activeId);
				}
				*activeId = 0;
			}

			uint32_t value = 0;
			switch (role)
			{
			case Role_Pad0Menu: value = manager.PlayerWord(Side_P1, false); break;
			case Role_Pad1Menu: value = manager.PlayerWord(Side_P2, false); break;
			case Role_Pad0Battle: value = manager.PlayerWord(Side_P1, true); break;
			case Role_Pad1Battle: value = manager.PlayerWord(Side_P2, true); break;
			default: value = 0; break; // keyboards arrive through a player
			}

			// The ranked automation harness drives menus by substituting these words; it keeps
			// working on top of the local input system.
			uint32_t automation = 0;
			if (RankedAutomationHarness::TryOverrideSystemInput(LegacySlot(role), value, &automation))
				value = automation;

			*word = value;
			return true;
		}

		uint32_t __fastcall BuildOurWord(uint8_t* ctrl, uint32_t word)
		{
			const uint32_t count = *reinterpret_cast<uint32_t*>(ctrl + 0x38);
			const uint32_t valid = count >= 32 ? 0xFFFFFFFFu : ((1u << count) - 1u);
			const uint32_t prev = *reinterpret_cast<uint32_t*>(*reinterpret_cast<uint8_t**>(ctrl + 0x20) + 0x0C);

			const uint32_t physical = CancelOpposites(word & valid);
			uint32_t effective = physical;
			if (*reinterpret_cast<int32_t*>(ctrl + 0x50) != 0)
				effective = CancelOpposites(*reinterpret_cast<uint32_t*>(ctrl + 0x4C) & valid);

			*reinterpret_cast<uint32_t*>(ctrl + 0x30) = physical;
			uint32_t& pressed = *reinterpret_cast<uint32_t*>(ctrl + 0x28);
			uint32_t& released = *reinterpret_cast<uint32_t*>(ctrl + 0x2C);
			for (uint32_t i = 0; i < count && i < 32; ++i)
			{
				const uint32_t bit = 1u << i;
				if (effective & bit)
				{
					if (!(prev & bit))
						pressed |= bit;
				}
				else if (prev & bit)
				{
					released |= bit;
				}
			}
			*reinterpret_cast<int32_t*>(ctrl + 0x50) = 0;
			NoteControllerForResetProbe(ctrl);
			return effective;
		}

		// IsControllerIdConnected (0x47F480, stdcall(id), ret 4): ids above 2 clamp to 0, id 2
		// shares wrapper 1 with id 1. Id 0 stays connected as in vanilla (the game force-opens
		// pause if an in-use id drops during online play); ids 1/2 are connected while Player 2
		// has a device, so pad-assign and join screens see exactly who is there. -1 = not ours.
		int __cdecl ConnectedFor(uint32_t id)
		{
			Manager& manager = Manager::Get();
			if (!manager.IsActive())
				return -1;
			const int wrapper = (id == 1 || id == 2) ? 1 : 0;
			const int answer = wrapper == 0 ? 1 : (manager.PlayerHasDevice(Side_P2) ? 1 : 0);
			// What the join and pad-assign screens are told, on change only.
			static int lastAnswer[2] = { -1, -1 };
			if (lastAnswer[wrapper] != answer)
			{
				lastAnswer[wrapper] = answer;
				LOG(1, "[LocalInput] game asked whether controller %s is connected: %s\n",
					wrapper == 0 ? "0 (Player 1)" : "1/2 (Player 2)", answer ? "yes" : "no");
			}
			return answer;
		}

		void __declspec(naked) IsControllerIdConnectedHook()
		{
			__asm
			{
				mov eax, [esp + 4]
				push ecx
				push edx
				push eax
				call ConnectedFor
				add esp, 4
				pop edx
				pop ecx
				cmp eax, -1
				je connected_original
				ret 4
			connected_original:
				push ebp
				mov ebp, esp
				mov eax, [ebp + 8]
				jmp[g_connectedJmpBack]
			}
		}

		void __declspec(naked) BuildActionWordsHook()
		{
			__asm
			{
				push ecx                // controller
				push edx
				sub esp, 4              // the word
				mov edx, esp
				push edx
				push ecx
				call TryOurWord
				add esp, 8
				test al, al
				jz run_original
				mov edx, [esp]          // word
				mov ecx, [esp + 8]      // controller
				call BuildOurWord       // fastcall(ecx, edx) -> eax
				add esp, 4
				pop edx
				pop ecx
				ret                     // the game's function returns with a plain ret too
			run_original:
				add esp, 4
				pop edx
				pop ecx
				// the 6 bytes the JMP replaced
				push ebp
				mov ebp, esp
				sub esp, 0Ch
				jmp[g_buildJmpBack]
			}
		}
	}

	bool InstallGameInputBridge()
	{
		const uintptr_t base = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
		// push ebp / mov ebp,esp / sub esp,0Ch / push ebx / push esi / mov esi,ecx / xor ebx,ebx
		static const uint8_t kExpected[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x53, 0x56, 0x8B, 0xF1, 0x33, 0xDB };
		const uint8_t* const site = reinterpret_cast<const uint8_t*>(base + kBuildActionWordsRva);
		if (IsBadReadPtr(site, sizeof(kExpected)) || std::memcmp(site, kExpected, sizeof(kExpected)) != 0)
		{
			LOG(1, "[LocalInput] action-word builder does not match the RE'd build; local input left off\n");
			return false;
		}
		g_buildJmpBack = HookManager::SetHook("LocalInputBuildActionWords",
			static_cast<DWORD>(base + kBuildActionWordsRva), 6, reinterpret_cast<void*>(BuildActionWordsHook));
		if (g_buildJmpBack == 0)
		{
			LOG(1, "[LocalInput] could not hook the action-word builder; local input left off\n");
			return false;
		}
		LOG(1, "[LocalInput] action-word builder replaced for pad slots and keyboard controllers\n");

		// push ebp / mov ebp,esp / mov eax,[ebp+8] / xor ecx,ecx
		static const uint8_t kConnectedExpected[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x33, 0xC9 };
		const uint8_t* const connectedSite = reinterpret_cast<const uint8_t*>(base + kIsControllerIdConnectedRva);
		if (!IsBadReadPtr(connectedSite, sizeof(kConnectedExpected)) &&
			std::memcmp(connectedSite, kConnectedExpected, sizeof(kConnectedExpected)) == 0)
		{
			g_connectedJmpBack = HookManager::SetHook("LocalInputControllerConnected",
				static_cast<DWORD>(base + kIsControllerIdConnectedRva), 6, reinterpret_cast<void*>(IsControllerIdConnectedHook));
		}
		if (g_connectedJmpBack == 0)
			LOG(1, "[LocalInput] controller-connected query not hooked; join screens use the game's own answer\n");
		return true;
	}

	bool ResolveControllerWord(void* controller, uint32_t* word)
	{
		return TryOurWord(controller, word);
	}
}
