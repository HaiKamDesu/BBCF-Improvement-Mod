// See GameKeyConfig.h. Every address below is from docs/Decompilation/Controllers.md section 6
// and was read in the assembly of the Steam build.

#include "GameKeyConfig.h"

#include "Core/logger.h"
#include "Core/utils.h"
#include "Hooks/HookManager.h"

#include <cstddef>
#include <cstring>

namespace LocalInput
{
	namespace GameKeyConfig
	{
		namespace
		{
			// ---------------------------------------------------------------- game data

			constexpr uintptr_t kOptDataRva = 0x008F85E0;        // VA 0x00CF85E0, the bbsave.dat image
			constexpr uintptr_t kOptDataGuardRva = 0x00ABFE18;   // bit 0 = constructed
			constexpr uintptr_t kEngineRva = 0x008903B0;         // VA 0x00C903B0
			constexpr uintptr_t kSetBStateRva = 0x0121BF80;      // battle editor state: int id[2]
			constexpr uintptr_t kSetBStateGuardRva = 0x0121BFA0;
			constexpr uintptr_t kSetAStateRva = 0x00D9A4B0;      // menu editor state
			constexpr uintptr_t kSetAStateGuardRva = 0x00D9A4D0;
			constexpr uintptr_t kPadPanelProfileRva = 0x011D3960; // battle pad panel slot 0 +0x1D0, stride 0x200

			// optData offsets
			constexpr size_t kPadSetB = 0x4204;    // + 40*slot + 20*profile
			constexpr size_t kTypeBFlag = 0x4254;  // + id
			constexpr size_t kKbSetB = 0x4256;     // + 20*profile
			constexpr size_t kPadSetA = 0x54A55;
			constexpr size_t kMenuSet = 0x54AA5;   // + id
			constexpr size_t kKbSetA = 0x54AA7;

			// engine offsets
			constexpr size_t kEngineSetAProfile = 0x25D8;   // int[2]
			constexpr size_t kEngineSetBProfile = 0x25E0;   // int[2]

			uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetBbcfBaseAdress()); }

			uint8_t* OptData()
			{
				const uint32_t guard = *reinterpret_cast<const uint32_t*>(Base() + kOptDataGuardRva);
				return (guard & 1) ? reinterpret_cast<uint8_t*>(Base() + kOptDataRva) : nullptr;
			}

			int32_t* EngineInts(size_t offset) { return reinterpret_cast<int32_t*>(Base() + kEngineRva + offset); }

			// The side (= controller id, the mod pins P1 = 0 and P2 = 1) an editor slot edits, or -1.
			int EditorSlotSide(uintptr_t stateRva, uintptr_t guardRva, int slot)
			{
				if (!(*reinterpret_cast<const uint32_t*>(Base() + guardRva) & 1))
					return -1;
				const int32_t id = reinterpret_cast<const int32_t*>(Base() + stateRva)[slot & 1];
				return (id == 0 || id == 1) ? id : -1;
			}

			// ---------------------------------------------------------------- record formats

			// record byte -> action (binding index = word bit)
			const int kPadSetBActions[] = { 14, 15, 13, 12, 17, 19, 22, 23, 24, 25, 16, 18, 27 };
			const int kPadSetAActions[] = { 14, 15, 13, 12, 17, 19, 16, 18 };
			const int kKbSetBActions[] = { 0, 2, 3, 1, 14, 15, 13, 12, 17, 19, 22, 23, 24, 25, 16, 18, 27 };
			const int kKbSetAActions[] = { 0, 2, 3, 1, 14, 15, 13, 12, 17, 19, 16, 18 };

			// ResetPadKeyConfig_SetB 0x009DFDEC, ResetKeyboardKeyConfig_SetB 0x009DFE00, set A
			// 0x009DFE14 / 0x009DFE28.
			const uint8_t kDefPadSetB[20] = { 2, 3, 1, 0, 5, 7, 0xFF, 0xFF, 0xFF, 0xFF, 4, 6, 9,
				0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
			const uint8_t kDefKbSetB[20] = { 0x16, 0x12, 0x00, 0x03, 0x14, 0x08, 0x0E, 0x09, 0x0B, 0x32,
				0xFF, 0xFF, 0xFF, 0xFF, 0x0A, 0x0F, 0x42, 0xFF, 0xFF, 0xFF };
			const uint8_t kDefPadSetA[20] = { 2, 3, 1, 0, 5, 7, 4, 6,
				0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
			const uint8_t kDefKbSetA[20] = { 0x16, 0x12, 0x00, 0x03, 0x14, 0x08, 0x0E, 0x09, 0x0B, 0x32, 0x0A, 0x0F,
				0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

			constexpr int kActionCount = 28;

			// Class defaults of the binding tables (BattleKeyControler / SystemKeyControler).
			void ClassDefaults(BindClass cls, int16_t table[kActionCount])
			{
				for (int i = 0; i < kActionCount; ++i)
					table[i] = -1;
				if (cls == BindClass::Pad)
				{
					for (int i = 0; i < 4; ++i) table[i] = static_cast<int16_t>(0x20 + i);
					for (int i = 0; i < 8; ++i) table[12 + i] = static_cast<int16_t>(i);
					table[20] = 10; table[21] = 11; table[26] = 8; table[27] = 9;
				}
				else
				{
					table[0] = 0x16; table[1] = 0x03; table[2] = 0x12; table[3] = 0x00;
					table[12] = 0x09; table[13] = 0x0E; table[14] = 0x14; table[15] = 0x08;
					table[16] = 0x0A; table[17] = 0x0B; table[18] = 0x0F; table[19] = 0x32;
					table[20] = 0x0D; table[21] = 0x0C; table[26] = 0x24; table[27] = 0x42;
				}
			}

			void RecordActions(BindClass cls, bool battle, const int** actions, int* count)
			{
				if (cls == BindClass::Pad)
				{
					*actions = battle ? kPadSetBActions : kPadSetAActions;
					*count = battle ? 13 : 8;
				}
				else
				{
					*actions = battle ? kKbSetBActions : kKbSetAActions;
					*count = battle ? 17 : 12;
				}
			}

			void BuildTable(BindClass cls, const uint8_t* record, bool battle, int16_t table[kActionCount])
			{
				ClassDefaults(cls, table);
				const int* actions = nullptr;
				int count = 0;
				RecordActions(cls, battle, &actions, &count);
				for (int j = 0; j < count; ++j)
					table[actions[j]] = record[j] == 0xFF ? -1 : record[j];
			}

			int BattleProfile(const GameBinds& binds, int side)
			{
				if (!binds.typeB)
					return 0;
				return (EngineInts(kEngineSetBProfile)[side & 1] & 1);
			}

			// ---------------------------------------------------------------- game keys

			// Keyboard: VK -> game key. The game's own table (0x00A255C8, filled by every keyboard
			// constructor) when it has been built, this copy of it otherwise.
			int StaticVkToGameKey(int vk)
			{
				if (vk >= 'A' && vk <= 'Z') return vk - 'A';
				if (vk >= '1' && vk <= '9') return 0x1A + (vk - '1');
				if (vk == '0') return 0x23;
				if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return 0x54 + (vk - VK_NUMPAD0);
				if (vk >= VK_F1 && vk <= VK_F12) return 0x26 + (vk - VK_F1);
				switch (vk)
				{
				case VK_MULTIPLY: return 0x4F; case VK_ADD: return 0x51; case VK_SUBTRACT: return 0x50;
				case VK_DECIMAL: return 0x53; case VK_DIVIDE: return 0x4E;
				case VK_RETURN: return 0x24; case VK_ESCAPE: return 0x25; case VK_SPACE: return 0x41;
				case VK_BACK: return 0x42; case VK_TAB: return 0x38; case VK_CAPITAL: return 0x39;
				case VK_INSERT: return 0x48; case VK_DELETE: return 0x49; case VK_HOME: return 0x4A;
				case VK_END: return 0x4B; case VK_PRIOR: return 0x4C; case VK_NEXT: return 0x4D;
				case VK_UP: return 0x5E; case VK_RIGHT: return 0x5F; case VK_DOWN: return 0x60; case VK_LEFT: return 0x61;
				case VK_LSHIFT: return 0x3A; case VK_RSHIFT: return 0x44; case VK_LCONTROL: return 0x3B;
				case VK_RCONTROL: return 0x45; case VK_LMENU: return 0x3C; case VK_RMENU: return 0x3D;
				case VK_SNAPSHOT: return 0x35; case VK_LWIN: return 0x46; case VK_APPS: return 0x40;
				case VK_OEM_PLUS: return 0x32; case VK_OEM_1: return 0x69; case 0x2F: return 0x34;
				case 0x15: return 0x66; case 0x19: return 0x62; case 0x1C: return 0x65; case 0x1D: return 0x64;
				}
				return 0x6A;
			}

			int VkToGameKey(int vk)
			{
				if (vk <= 0 || vk > 255)
					return 0x6A;
				const uint8_t* const table = reinterpret_cast<const uint8_t*>(Base() + 0x006255C8);
				// Built once a keyboard device exists: 'A' -> 0, 'B' -> 1, Enter -> 0x24.
				if (table['A'] == 0x00 && table['B'] == 0x01 && table[VK_RETURN] == 0x24)
				{
					const int gk = table[vk];
					return gk < 0x6A ? gk : 0x6A;
				}
				return StaticVkToGameKey(vk);
			}

			int XInputControlToGameKey(int control)
			{
				// LocalInputTypes order: A B X Y LB RB Back Start L3 R3, d-pad U D L R, LT RT,
				// left stick U D L R, right stick U D L R.
				static const int8_t map[kXInputControlCount] = {
					0, 1, 2, 3, 4, 5, 9, 8, 10, 11,
					0x20, 0x22, 0x23, 0x21, 6, 7,
					0x20, 0x22, 0x23, 0x21,
					0x24, 0x26, 0x27, 0x25,
				};
				return control >= 0 && control < kXInputControlCount ? map[control] : -1;
			}

			int DInputControlToGameKey(int control)
			{
				if (control >= 0 && control < 12)
					return control;
				switch (control)
				{
				case kDInputPovBase + 0: return 0x20;
				case kDInputPovBase + 1: return 0x21;
				case kDInputPovBase + 2: return 0x22;
				case kDInputPovBase + 3: return 0x23;
				case kDInputAxisBase + 0: return 0x23;   // X-
				case kDInputAxisBase + 1: return 0x21;   // X+
				case kDInputAxisBase + 2: return 0x20;   // Y-
				case kDInputAxisBase + 3: return 0x22;   // Y+
				}
				return -1;
			}

			int ControlToGameKey(DeviceKind kind, int control)
			{
				switch (kind)
				{
				case DeviceKind::Keyboard: return VkToGameKey(control);
				case DeviceKind::XInputPad: return XInputControlToGameKey(control);
				case DeviceKind::DInputPad: return DInputControlToGameKey(control);
				}
				return -1;
			}

			void SetBit(HeldKeys& held, int gk)
			{
				if (gk >= 0 && gk < 128)
					held.bits[gk >> 5] |= 1u << (gk & 31);
			}

			// ---------------------------------------------------------------- state

			bool g_installed = false;
			bool g_loaded = false;
			GameBinds g_vanilla[2][2];        // [class][side], records at load

			// What the mod last wrote into (or accepted from) optData.
			struct Image
			{
				uint8_t padB[80];
				uint8_t flags[2];
				uint8_t kbB[40];
				uint8_t padA[80];
				uint8_t menuSet[2];
				uint8_t kbA[40];
			};
			Image g_staged = {};

			void ReadImage(const uint8_t* opt, Image& image)
			{
				std::memcpy(image.padB, opt + kPadSetB, 80);
				std::memcpy(image.flags, opt + kTypeBFlag, 2);
				std::memcpy(image.kbB, opt + kKbSetB, 40);
				std::memcpy(image.padA, opt + kPadSetA, 80);
				std::memcpy(image.menuSet, opt + kMenuSet, 2);
				std::memcpy(image.kbA, opt + kKbSetA, 40);
			}

			// Editors and who they edit.
			struct SideEditor
			{
				bool open = false;
				std::string padOwner;          // pad records of slot side belong to it
				GameBinds padBinds;
				ControlSet padControls;
				DeviceKind padKind = DeviceKind::XInputPad;
			};
			SideEditor g_editor[2];

			std::string g_kbOwner;             // the keyboard whose records are staged
			int g_kbOwnerSide = -1;
			GameBinds g_kbOwnerBinds;
			HeldKeys g_kbOwnerHeld;

			struct KeyboardCandidate
			{
				std::string id;
				GameBinds binds;
				HeldKeys held;
			};
			KeyboardCandidate g_kbCandidate[2];

			std::vector<std::pair<std::string, GameBinds>> g_pendingChanges;

			void SnapshotVanilla(const uint8_t* opt)
			{
				for (int side = 0; side < 2; ++side)
				{
					GameBinds& pad = g_vanilla[0][side];
					std::memcpy(pad.setB, opt + kPadSetB + 40 * side, 40);
					std::memcpy(pad.setA, opt + kPadSetA + 40 * side, 40);
					pad.typeB = opt[kTypeBFlag + side] & 1;
					pad.menuSet = opt[kMenuSet + side] & 1;
					Sanitize(pad, BindClass::Pad);

					GameBinds& kb = g_vanilla[1][side];
					std::memcpy(kb.setB, opt + kKbSetB, 40);
					std::memcpy(kb.setA, opt + kKbSetA, 40);
					kb.typeB = opt[kTypeBFlag] & 1;   // vanilla: the keyboard follows id 0's flag
					kb.menuSet = opt[kMenuSet + side] & 1;
					Sanitize(kb, BindClass::Keyboard);
				}
			}

			void MarkLoaded(const char* why)
			{
				uint8_t* const opt = OptData();
				if (!opt)
					return;
				if (!g_loaded)
				{
					SnapshotVanilla(opt);
					LOG(1, "[GameKeyConfig] save data loaded (%s): per-device button settings live\n", why);
				}
				g_loaded = true;
				ReadImage(opt, g_staged);
				g_editor[0] = SideEditor();
				g_editor[1] = SideEditor();
				g_kbOwner.clear();
				g_kbOwnerSide = -1;
			}

			void __cdecl NoteSaveLoaded()
			{
				MarkLoaded("key config applied");
			}

			// ---------------------------------------------------------------- staging

			void StagePad(int side, const GameBinds& binds)
			{
				uint8_t* const opt = OptData();
				if (!opt)
					return;
				std::memcpy(opt + kPadSetB + 40 * side, binds.setB, 40);
				std::memcpy(opt + kPadSetA + 40 * side, binds.setA, 40);
				opt[kTypeBFlag + side] = binds.typeB & 1;
				opt[kMenuSet + side] = binds.menuSet & 1;
				EngineInts(kEngineSetAProfile)[side] = binds.menuSet & 1;
				std::memcpy(g_staged.padB + 40 * side, binds.setB, 40);
				std::memcpy(g_staged.padA + 40 * side, binds.setA, 40);
				g_staged.flags[side] = binds.typeB & 1;
				g_staged.menuSet[side] = binds.menuSet & 1;
			}

			void StageKeyboard(int side, const KeyboardCandidate& kb)
			{
				uint8_t* const opt = OptData();
				if (!opt || kb.id.empty())
					return;
				g_kbOwner = kb.id;
				g_kbOwnerSide = side;
				g_kbOwnerBinds = kb.binds;
				g_kbOwnerHeld = kb.held;
				std::memcpy(opt + kKbSetB, kb.binds.setB, 40);
				std::memcpy(opt + kKbSetA, kb.binds.setA, 40);
				std::memcpy(g_staged.kbB, kb.binds.setB, 40);
				std::memcpy(g_staged.kbA, kb.binds.setA, 40);
				LOG(1, "[GameKeyConfig] keyboard records -> %s (P%d)\n", kb.id.c_str(), side + 1);
			}

			// ---------------------------------------------------------------- patches

			// Fake pad for the pad capture: only vtbl+0x0C (IsPressed, thiscall(dev, gameKey),
			// ret 4) is ever called on it.
			struct FakePad
			{
				void** vtbl;
				int side;
			};

			int __fastcall FakePadIsPressed(FakePad* self, void*, int gameKey)
			{
				const SideEditor& editor = g_editor[self->side & 1];
				if (editor.padOwner.empty() || gameKey < 0 || gameKey >= 128)
					return 0;
				const HeldKeys held = HeldGameKeys(editor.padKind, editor.padControls);
				return held.Test(gameKey) ? 1 : 0;
			}

			void* g_fakePadVtbl[8];
			FakePad g_fakePads[2] = { { g_fakePadVtbl, 0 }, { g_fakePadVtbl, 1 } };

			FakePad* __cdecl FakePadFor(int id)
			{
				if (!g_loaded || (id != 0 && id != 1))
					return nullptr;
				return &g_fakePads[id];
			}

			// Fake device registry for the keyboard capture: it reads registry+0x0C (the
			// keyboard) and that keyboard's current (+0x08) and previous (+0x18) game-key maps.
			struct FakeKeyboard
			{
				void* vtbl;
				int kind;
				uint32_t cur[4];
				uint32_t prev[4];
			};
			struct FakeRegistry
			{
				void** devices;
				int capacity;
				int count;
				FakeKeyboard* keyboard;
			};
			static_assert(offsetof(FakeKeyboard, cur) == 0x08 && offsetof(FakeKeyboard, prev) == 0x18, "keyboard layout");
			static_assert(offsetof(FakeRegistry, keyboard) == 0x0C, "registry layout");

			FakeKeyboard g_fakeKeyboard = {};
			FakeRegistry g_fakeRegistry = { nullptr, 0, 0, &g_fakeKeyboard };
			uint32_t g_lastKbCaptureCur[4] = {};
			ULONGLONG g_lastKbCaptureMs = 0;

			FakeRegistry* __cdecl FakeKeyboardRegistryFor(const uint8_t* panel, int family)
			{
				if (!g_loaded || !panel)
					return nullptr;
				const int slot = *reinterpret_cast<const int32_t*>(panel + 0x1F8);
				const int side = family == 0
					? EditorSlotSide(kSetBStateRva, kSetBStateGuardRva, slot)
					: EditorSlotSide(kSetAStateRva, kSetAStateGuardRva, slot);
				if (side < 0)
					return nullptr;
				if (g_kbOwnerSide != side && !g_kbCandidate[side].id.empty())
					StageKeyboard(side, g_kbCandidate[side]);
				// Edge = held now and not at the previous call (the capture runs every frame
				// while listening). On the first call nothing counts as new, so the key that
				// started the capture is not bound by accident.
				const ULONGLONG now = GetTickCount64();
				const bool recent = now - g_lastKbCaptureMs < 100;
				for (int i = 0; i < 4; ++i)
				{
					const uint32_t cur = g_kbOwnerSide == side ? g_kbOwnerHeld.bits[i] : 0;
					g_fakeKeyboard.cur[i] = cur;
					g_fakeKeyboard.prev[i] = recent ? g_lastKbCaptureCur[i] : cur;
					g_lastKbCaptureCur[i] = cur;
				}
				g_lastKbCaptureMs = now;
				return &g_fakeRegistry;
			}

			constexpr uintptr_t kKbPanelRva[2] = { 0x0121BB88, 0x00D9A0B8 };   // battle / menu, + slot*0x1FC

			// The pad-to-keyboard panel switch: vanilla opens the keyboard panel for id 0 only.
			int __cdecl AllowKeyboardPanel(int id, int family, int slot)
			{
				if (id != 0 && id != 1)
					return 0;
				// The keyboard records are shared (there is one per profile, not per id), so only
				// one keyboard panel may be open at a time: the drawer and the screen would show
				// and edit the other player's keys. +0x08 is the panel's shown flag (0x006BDB95).
				const uint8_t* const otherPanel = reinterpret_cast<const uint8_t*>(
					Base() + kKbPanelRva[family & 1] + ((slot & 1) ^ 1) * 0x1FC);
				if (*reinterpret_cast<const int32_t*>(otherPanel + 0x08) != 0)
					return 0;
				// The keyboard panels are built with controller id 0 (0x006A79B8), and the menu
				// framework navigates a panel with the inputs of its +0x1C0 id - so a keyboard
				// panel opened for Player 2 would follow Player 1. Give it its editor's id.
				int32_t* const panelId = reinterpret_cast<int32_t*>(Base() + kKbPanelRva[family & 1] + (slot & 1) * 0x1FC + 0x1C0);
				const auto setPanelId = [&]() { *panelId = id; };
				if (!g_loaded)
				{
					if (id != 0)
						return 0;
					setPanelId();
					return 1;
				}
				if (g_kbCandidate[id].id.empty())
				{
					if (id != 0)
						return 0;             // id 1 needs a keyboard on Player 2
					g_kbOwner.clear();        // nobody to store Player 1's keyboard edits in
					g_kbOwnerSide = -1;
					setPanelId();
					return 1;
				}
				StageKeyboard(id, g_kbCandidate[id]);
				setPanelId();
				return 1;
			}

			// The battle keyboard panel's Type B toggle (0x006A9B95) flips optData[0x4254], id 0's
			// flag, whatever player the panel belongs to, while its rows are greyed by
			// optData[0x4254 + panel id]. Flip the panel's own id instead, and store it on the
			// keyboard being edited (not on that player's pad).
			void __cdecl KeyboardPanelToggleTypeB(const uint8_t* panel)
			{
				uint8_t* const opt = OptData();
				if (!opt || !panel)
					return;
				const int id = *reinterpret_cast<const int32_t*>(panel + 0x1C0) == 1 ? 1 : 0;
				opt[kTypeBFlag + id] ^= 1;
				if (!g_loaded)
					return;
				g_staged.flags[id] = opt[kTypeBFlag + id];
				if (!g_kbOwner.empty() && g_kbOwnerSide == id)
				{
					g_kbOwnerBinds.typeB = opt[kTypeBFlag + id] & 1;
					g_pendingChanges.emplace_back(g_kbOwner, g_kbOwnerBinds);
				}
			}

			DWORD g_kbToggleBack = 0;   // 0x006A9BDE

			void __declspec(naked) KeyboardPanelTypeBToggleHook()
			{
				__asm
				{
					pushad
					push esi
					call KeyboardPanelToggleTypeB
					add esp, 4
					popad
					jmp[g_kbToggleBack]
				}
			}

			DWORD g_origRegistryGetByIndex = 0;   // 0x004094E0
			DWORD g_origRegistryGet = 0;          // 0x00409500
			DWORD g_kbPanelAllow[2] = {};         // battle 0x006A9C18, menu 0x00516EB8
			DWORD g_kbPanelDeny[2] = {};          // battle 0x006A9C44, menu 0x00516EE4
			DWORD g_padPanelProfileBase = 0;      // VA 0x015D3960
			DWORD g_profileReadBack = 0;
			DWORD g_profileWriteBack = 0;
			DWORD g_loadedBack = 0;

			// call 004094E0 at 0x006A851E: thiscall(registry, int index), ret 4; id in EDI.
			void __declspec(naked) PadCaptureDeviceSetB()
			{
				__asm
				{
					push ecx
					push edi
					call FakePadFor
					add esp, 4
					pop ecx
					test eax, eax
					jz original
					ret 4
				original:
					jmp[g_origRegistryGetByIndex]
				}
			}

			// call 004094E0 at 0x005154BE: same, id in EBX.
			void __declspec(naked) PadCaptureDeviceSetA()
			{
				__asm
				{
					push ecx
					push ebx
					call FakePadFor
					add esp, 4
					pop ecx
					test eax, eax
					jz original
					ret 4
				original:
					jmp[g_origRegistryGetByIndex]
				}
			}

			// call 00409500 at 0x006A89A0 / 0x00515AA2 (cdecl, no args). The capture function
			// keeps its panel at [ebp-158h].
			void __declspec(naked) KeyboardCaptureRegistrySetB()
			{
				__asm
				{
					push 0
					push dword ptr[ebp - 158h]
					call FakeKeyboardRegistryFor
					add esp, 8
					test eax, eax
					jz original
					ret
				original:
					jmp[g_origRegistryGet]
				}
			}

			void __declspec(naked) KeyboardCaptureRegistrySetA()
			{
				__asm
				{
					push 1
					push dword ptr[ebp - 158h]
					call FakeKeyboardRegistryFor
					add esp, 8
					test eax, eax
					jz original
					ret
				original:
					jmp[g_origRegistryGet]
				}
			}

			// Replaces `cmp dword ptr [eax+ecx*4],0 / jne` (eax = editor state, ecx = slot).
			void __declspec(naked) KeyboardPanelSwitchSetB()
			{
				__asm
				{
					mov edx, [eax + ecx * 4]
					push ecx
					push 0
					push edx
					call AllowKeyboardPanel
					add esp, 12
					test eax, eax
					jz deny
					jmp[g_kbPanelAllow]
				deny:
					jmp[g_kbPanelDeny]
				}
			}

			void __declspec(naked) KeyboardPanelSwitchSetA()
			{
				__asm
				{
					mov edx, [eax + ecx * 4]
					push ecx
					push 1
					push edx
					call AllowKeyboardPanel
					add esp, 12
					test eax, eax
					jz deny
					jmp[g_kbPanelAllow + 4]
				deny:
					jmp[g_kbPanelDeny + 4]
				}
			}

			// 0x006A8024 `mov eax,[015D3960h]` in the battle keyboard panel setup (esi = panel):
			// read its own slot's pad panel profile instead of slot 0's.
			void __declspec(naked) KeyboardPanelProfileRead()
			{
				__asm
				{
					mov eax, [esi + 1F8h]
					and eax, 1
					shl eax, 9
					add eax, [g_padPanelProfileBase]
					mov eax, [eax]
					jmp[g_profileReadBack]
				}
			}

			// 0x006A95B3 `mov [015D3960h],eax` in its Type A/B toggle: same fix for the write.
			void __declspec(naked) KeyboardPanelProfileWrite()
			{
				__asm
				{
					push ecx
					mov ecx, [esi + 1F8h]
					and ecx, 1
					shl ecx, 9
					add ecx, [g_padPanelProfileBase]
					mov[ecx], eax
					pop ecx
					jmp[g_profileWriteBack]
				}
			}

			// ApplyAllKeyConfig 0x00483F10 entry: the save has just been loaded.
			void __declspec(naked) ApplyAllKeyConfigHook()
			{
				__asm
				{
					pushad
					pushfd
					call NoteSaveLoaded
					popfd
					popad
					push ebp
					mov ebp, esp
					sub esp, 8
					jmp[g_loadedBack]
				}
			}

			bool Matches(uintptr_t va, const uint8_t* bytes, size_t len)
			{
				return !IsBadReadPtr(reinterpret_cast<const void*>(va), len) &&
					std::memcmp(reinterpret_cast<const void*>(va), bytes, len) == 0;
			}

			// Replaces a `call rel32` whose target is `expectedTarget` with a call to `target`.
			bool PatchCall(const char* label, uintptr_t site, uintptr_t expectedTarget, void* target)
			{
				const uint8_t* const p = reinterpret_cast<const uint8_t*>(site);
				if (IsBadReadPtr(p, 5) || p[0] != 0xE8 ||
					site + 5 + *reinterpret_cast<const int32_t*>(p + 1) != expectedTarget)
				{
					LOG(1, "[GameKeyConfig] %s: call site does not match, skipped\n", label);
					return false;
				}
				DWORD old = 0;
				if (!VirtualProtect(reinterpret_cast<void*>(site), 5, PAGE_EXECUTE_READWRITE, &old))
					return false;
				*reinterpret_cast<int32_t*>(site + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
				VirtualProtect(reinterpret_cast<void*>(site), 5, old, &old);
				FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);
				return true;
			}

			// mov eax,[abs] (A1) / mov [abs],eax (A3) with a relocated absolute address.
			bool MatchesAbs(uintptr_t site, uint8_t opcode, uintptr_t abs)
			{
				uint8_t bytes[5] = { opcode };
				std::memcpy(bytes + 1, &abs, 4);
				return Matches(site, bytes, 5);
			}
		}

		// ---------------------------------------------------------------- public

		void Install()
		{
			if (g_installed)
				return;
			g_installed = true;
			const uintptr_t base = Base();
			for (void*& entry : g_fakePadVtbl)
				entry = reinterpret_cast<void*>(FakePadIsPressed);

			g_origRegistryGetByIndex = static_cast<DWORD>(base + 0x000094E0);
			g_origRegistryGet = static_cast<DWORD>(base + 0x00009500);
			int ok = 0;
			ok += PatchCall("pad capture (battle)", base + 0x002A851E, base + 0x000094E0, reinterpret_cast<void*>(PadCaptureDeviceSetB));
			ok += PatchCall("pad capture (menu)", base + 0x001154BE, base + 0x000094E0, reinterpret_cast<void*>(PadCaptureDeviceSetA));
			ok += PatchCall("keyboard capture (battle)", base + 0x002A89A0, base + 0x00009500, reinterpret_cast<void*>(KeyboardCaptureRegistrySetB));
			ok += PatchCall("keyboard capture (menu)", base + 0x00115AA2, base + 0x00009500, reinterpret_cast<void*>(KeyboardCaptureRegistrySetA));

			static const uint8_t kSwitch[] = { 0x83, 0x3C, 0x88, 0x00, 0x75, 0x2C };
			if (Matches(base + 0x002A9C12, kSwitch, sizeof(kSwitch)))
			{
				g_kbPanelAllow[0] = static_cast<DWORD>(base + 0x002A9C18);
				g_kbPanelDeny[0] = static_cast<DWORD>(base + 0x002A9C44);
				ok += HookManager::SetHook("GameKeyConfigKbPanelSetB", static_cast<DWORD>(base + 0x002A9C12), 6,
					reinterpret_cast<void*>(KeyboardPanelSwitchSetB)) != 0;
			}
			if (Matches(base + 0x00116EB2, kSwitch, sizeof(kSwitch)))
			{
				g_kbPanelAllow[1] = static_cast<DWORD>(base + 0x00116EB8);
				g_kbPanelDeny[1] = static_cast<DWORD>(base + 0x00116EE4);
				ok += HookManager::SetHook("GameKeyConfigKbPanelSetA", static_cast<DWORD>(base + 0x00116EB2), 6,
					reinterpret_cast<void*>(KeyboardPanelSwitchSetA)) != 0;
			}

			g_padPanelProfileBase = static_cast<DWORD>(base + kPadPanelProfileRva);
			if (MatchesAbs(base + 0x002A8024, 0xA1, g_padPanelProfileBase))
			{
				g_profileReadBack = HookManager::SetHook("GameKeyConfigKbProfileRead", static_cast<DWORD>(base + 0x002A8024), 5,
					reinterpret_cast<void*>(KeyboardPanelProfileRead));
				ok += g_profileReadBack != 0;
			}
			if (MatchesAbs(base + 0x002A95B3, 0xA3, g_padPanelProfileBase))
			{
				g_profileWriteBack = HookManager::SetHook("GameKeyConfigKbProfileWrite", static_cast<DWORD>(base + 0x002A95B3), 5,
					reinterpret_cast<void*>(KeyboardPanelProfileWrite));
				ok += g_profileWriteBack != 0;
			}

			// push ebx / call 004B9700 (GetOptionData)
			const uintptr_t toggleSite = base + 0x002A9B95;
			const uint8_t* const t = reinterpret_cast<const uint8_t*>(toggleSite);
			if (!IsBadReadPtr(t, 6) && t[0] == 0x53 && t[1] == 0xE8 &&
				toggleSite + 6 + *reinterpret_cast<const int32_t*>(t + 2) == base + 0x000B9700)
			{
				g_kbToggleBack = static_cast<DWORD>(base + 0x002A9BDE);
				ok += HookManager::SetHook("GameKeyConfigKbTypeBToggle", static_cast<DWORD>(toggleSite), 6,
					reinterpret_cast<void*>(KeyboardPanelTypeBToggleHook)) != 0;
			}

			static const uint8_t kApplyAll[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 };
			if (Matches(base + 0x00083F10, kApplyAll, sizeof(kApplyAll)))
			{
				g_loadedBack = HookManager::SetHook("GameKeyConfigSaveLoaded", static_cast<DWORD>(base + 0x00083F10), 6,
					reinterpret_cast<void*>(ApplyAllKeyConfigHook));
				ok += g_loadedBack != 0;
			}
			LOG(1, "[GameKeyConfig] %d/10 patches in place\n", ok);
		}

		bool SaveLoaded() { return g_loaded; }

		GameBinds Defaults(BindClass cls)
		{
			GameBinds b;
			const bool pad = cls == BindClass::Pad;
			for (int p = 0; p < 2; ++p)
			{
				std::memcpy(b.setB[p], pad ? kDefPadSetB : kDefKbSetB, 20);
				std::memcpy(b.setA[p], pad ? kDefPadSetA : kDefKbSetA, 20);
			}
			return b;
		}

		GameBinds Inherited(BindClass cls, int side)
		{
			return g_loaded ? g_vanilla[cls == BindClass::Pad ? 0 : 1][side & 1] : Defaults(cls);
		}

		bool Sanitize(GameBinds& binds, BindClass cls)
		{
			bool changed = false;
			const GameBinds def = Defaults(cls);
			for (int set = 0; set < 2; ++set)
			{
				const int* actions = nullptr;
				int count = 0;
				RecordActions(cls, set == 0, &actions, &count);
				for (int p = 0; p < 2; ++p)
				{
					uint8_t* const rec = set == 0 ? binds.setB[p] : binds.setA[p];
					const uint8_t* const defRec = set == 0 ? def.setB[p] : def.setA[p];
					for (int j = 0; j < count; ++j)
					{
						const uint8_t v = rec[j];
						const bool valid = v == 0xFF || (cls == BindClass::Pad ? (v <= 11 && v != 8) : v < 0x6A);
						if (!valid)
						{
							rec[j] = defRec[j];
							changed = true;
						}
					}
				}
			}
			if (binds.typeB > 1) { binds.typeB &= 1; changed = true; }
			if (binds.menuSet > 1) { binds.menuSet &= 1; changed = true; }
			return changed;
		}

		HeldKeys HeldGameKeys(DeviceKind kind, const ControlSet& controls)
		{
			HeldKeys held;
			if (controls.none())
				return held;
			const int limit = kind == DeviceKind::Keyboard ? 256 : kind == DeviceKind::XInputPad ? kXInputControlCount : kDInputAxisBase + 4;
			for (int c = 0; c < limit && c < kMaxControls; ++c)
			{
				if (controls[c])
					SetBit(held, ControlToGameKey(kind, c));
			}
			return held;
		}

		uint32_t Word(BindClass cls, const HeldKeys& held, const GameBinds& binds, bool battle, int side)
		{
			const int profile = battle ? BattleProfile(binds, side) : (binds.menuSet & 1);
			int16_t table[kActionCount];
			BuildTable(cls, battle ? binds.setB[profile] : binds.setA[profile], battle, table);
			uint32_t word = 0;
			for (int i = 0; i < kActionCount; ++i)
			{
				if (table[i] >= 0 && held.Test(table[i]))
					word |= 1u << i;
			}
			if (cls == BindClass::Keyboard)
			{
				// SystemKeyControler's hard-wired arrows (and Space as Up) on top of any binding.
				if (held.Test(0x5E) || held.Test(0x41)) word |= 0x1;
				if (held.Test(0x5F)) word |= 0x2;
				if (held.Test(0x60)) word |= 0x4;
				if (held.Test(0x61)) word |= 0x8;
			}
			return word;
		}

		std::map<int, uint32_t> EffectiveMapping(DeviceKind kind, const GameBinds& binds, int side)
		{
			std::map<int, uint32_t> mapping;
			const BindClass cls = ClassOf(kind);
			const int limit = kind == DeviceKind::Keyboard ? 256 : kind == DeviceKind::XInputPad ? kXInputControlCount : kDInputAxisBase + 4;
			for (int c = 1; c < limit; ++c)
			{
				ControlSet one;
				one.set(c);
				const uint32_t word = Word(cls, HeldGameKeys(kind, one), binds, true, side);
				if (word)
					mapping[c] = word;
			}
			if (kind != DeviceKind::Keyboard)
			{
				ControlSet zero;
				zero.set(0);
				const uint32_t word = Word(cls, HeldGameKeys(kind, zero), binds, true, side);
				if (word)
					mapping[0] = word;
			}
			return mapping;
		}

		bool EditorOpen(int side)
		{
			return g_editor[side & 1].open;
		}

		void Frame(const std::vector<DeviceRef>& devices, std::vector<std::pair<std::string, GameBinds>>& changed)
		{
			if (!g_installed)
				return;
			uint8_t* const opt = OptData();
			if (!opt)
				return;

			// Which sides have an editor open, in either family.
			bool open[2] = {};
			for (int slot = 0; slot < 2; ++slot)
			{
				const int b = EditorSlotSide(kSetBStateRva, kSetBStateGuardRva, slot);
				const int a = EditorSlotSide(kSetAStateRva, kSetAStateGuardRva, slot);
				if (b >= 0) open[b] = true;
				if (a >= 0) open[a] = true;
			}
			if (!g_loaded)
			{
				if (!open[0] && !open[1])
					return;
				MarkLoaded("key-config editor opened");
			}

			auto find = [&](const std::string& id) -> const DeviceRef*
			{
				for (const DeviceRef& d : devices)
					if (d.id == id)
						return &d;
				return nullptr;
			};
			auto record = [&](const std::string& id, const GameBinds& binds)
			{
				for (auto& entry : changed)
				{
					if (entry.first == id)
					{
						entry.second = binds;
						return;
					}
				}
				changed.emplace_back(id, binds);
			};

			// Keyboard candidates per side: the most recently used keyboard there. Keyboards
			// nobody has typed on (media keys, receivers) never own a config.
			for (int side = 0; side < 2; ++side)
			{
				const DeviceRef* best = nullptr;
				for (const DeviceRef& d : devices)
				{
					if (!d.usable || d.idle || d.kind != DeviceKind::Keyboard || d.side != side)
						continue;
					if (!best || d.lastInputMs > best->lastInputMs)
						best = &d;
				}
				KeyboardCandidate& c = g_kbCandidate[side];
				if (best)
				{
					c.id = best->id;
					c.binds = best->binds;
					c.held = HeldGameKeys(best->kind, best->controls);
				}
				else
				{
					c = KeyboardCandidate();
				}
			}
			if (!g_kbOwner.empty())
			{
				const DeviceRef* owner = find(g_kbOwner);
				g_kbOwnerHeld = owner ? HeldGameKeys(owner->kind, owner->controls) : HeldKeys();
			}

			for (const auto& pending : g_pendingChanges)
				record(pending.first, pending.second);
			g_pendingChanges.clear();

			// 1. Absorb what the screens changed since the last frame.
			Image now;
			ReadImage(opt, now);
			for (int side = 0; side < 2; ++side)
			{
				SideEditor& editor = g_editor[side];
				const bool active = editor.open || open[side];
				const bool padB = std::memcmp(now.padB + 40 * side, g_staged.padB + 40 * side, 40) != 0;
				const bool padA = std::memcmp(now.padA + 40 * side, g_staged.padA + 40 * side, 40) != 0;
				const bool flag = now.flags[side] != g_staged.flags[side];
				const bool menuSet = now.menuSet[side] != g_staged.menuSet[side];
				if (active && !editor.padOwner.empty() && (padB || padA || flag || menuSet))
				{
					GameBinds& b = editor.padBinds;
					std::memcpy(b.setB, now.padB + 40 * side, 40);
					std::memcpy(b.setA, now.padA + 40 * side, 40);
					b.typeB = now.flags[side] & 1;
					b.menuSet = now.menuSet[side] & 1;
					Sanitize(b, BindClass::Pad);
					record(editor.padOwner, b);
					LOG(1, "[GameKeyConfig] %s: button settings changed (P%d)\n", editor.padOwner.c_str(), side + 1);
				}
				// The menu set also belongs to the keyboard being edited for that side (the
				// keyboard panel's Type B toggle is handled by KeyboardPanelToggleTypeB).
				if (active && menuSet && !g_kbOwner.empty() && g_kbOwnerSide == side)
				{
					g_kbOwnerBinds.menuSet = now.menuSet[side] & 1;
					record(g_kbOwner, g_kbOwnerBinds);
				}
			}
			const bool anyOpen = open[0] || open[1] || g_editor[0].open || g_editor[1].open;
			if (anyOpen && !g_kbOwner.empty() &&
				(std::memcmp(now.kbB, g_staged.kbB, 40) != 0 || std::memcmp(now.kbA, g_staged.kbA, 40) != 0))
			{
				std::memcpy(g_kbOwnerBinds.setB, now.kbB, 40);
				std::memcpy(g_kbOwnerBinds.setA, now.kbA, 40);
				Sanitize(g_kbOwnerBinds, BindClass::Keyboard);
				record(g_kbOwner, g_kbOwnerBinds);
				LOG(1, "[GameKeyConfig] %s: keyboard settings changed (P%d)\n", g_kbOwner.c_str(), g_kbOwnerSide + 1);
			}
			// The menu editor's Current Set applies live through engine+0x25D8 and only reaches
			// optData on close; follow it while the editor is open.
			for (int side = 0; side < 2; ++side)
			{
				const bool menuEditor = EditorSlotSide(kSetAStateRva, kSetAStateGuardRva, 0) == side ||
					EditorSlotSide(kSetAStateRva, kSetAStateGuardRva, 1) == side;
				if (!menuEditor)
					continue;
				const uint8_t live = static_cast<uint8_t>(EngineInts(kEngineSetAProfile)[side] & 1);
				SideEditor& editor = g_editor[side];
				if (!editor.padOwner.empty() && editor.padBinds.menuSet != live)
				{
					editor.padBinds.menuSet = live;
					record(editor.padOwner, editor.padBinds);
				}
				if (!g_kbOwner.empty() && g_kbOwnerSide == side && g_kbOwnerBinds.menuSet != live)
				{
					g_kbOwnerBinds.menuSet = live;
					record(g_kbOwner, g_kbOwnerBinds);
				}
			}
			g_staged = now;

			// 2. Editors that opened or closed.
			for (int side = 0; side < 2; ++side)
			{
				SideEditor& editor = g_editor[side];
				if (open[side] && (!editor.open || editor.padOwner.empty()))
				{
					// The pad this player edits with: their most recently used pad. Picked once
					// per editor session (retried until the side has a pad).
					const DeviceRef* best = nullptr;
					for (const DeviceRef& d : devices)
					{
						if (!d.usable || d.kind == DeviceKind::Keyboard || d.side != side)
							continue;
						if (!best || d.lastInputMs > best->lastInputMs)
							best = &d;
					}
					if (!editor.open)
						LOG(1, "[GameKeyConfig] P%d Button Settings opened%s\n", side + 1,
							best ? "" : " - that player has no usable pad right now (keyboard or nothing)");
					editor.open = true;
					if (best)
					{
						editor.padOwner = best->id;
						editor.padBinds = best->binds;
						editor.padKind = best->kind;
						StagePad(side, editor.padBinds);
						LOG(1, "[GameKeyConfig] P%d editor: pad records -> %s\n", side + 1, best->id.c_str());
					}
					// A keyboard player: stage their keyboard right away, so the keyboard panel
					// shows their keys even before the switch hook runs.
					const KeyboardCandidate& kb = g_kbCandidate[side];
					const DeviceRef* kbRef = kb.id.empty() ? nullptr : find(kb.id);
					if (kbRef && (!best || kbRef->lastInputMs > best->lastInputMs) && g_kbOwner != kb.id)
						StageKeyboard(side, kb);
				}
				else if (!open[side] && editor.open)
				{
					LOG(1, "[GameKeyConfig] P%d Button Settings closed%s\n", side + 1,
						editor.padOwner.empty() ? " (no pad was being edited)" : (" (was editing " + editor.padOwner + ")").c_str());
					editor = SideEditor();
					if (g_kbOwnerSide == side)
						g_kbOwnerSide = -1;   // the records stay staged; the next editor re-picks
				}
				if (editor.open && !editor.padOwner.empty())
				{
					const DeviceRef* pad = find(editor.padOwner);
					editor.padControls = pad ? pad->controls : ControlSet();
				}
			}

			// 3. Outside the editors, char select reads the Type B flag per id: allow Type B for
			// a side when any of its devices has it enabled.
			for (int side = 0; side < 2; ++side)
			{
				if (g_editor[side].open || (side == 0 && g_kbOwnerSide == 1 && g_editor[1].open))
					continue;
				bool any = false;
				bool hasDevice = false;
				for (const DeviceRef& d : devices)
				{
					if (!d.usable || d.side != side)
						continue;
					hasDevice = true;
					any = any || d.binds.typeB != 0;
				}
				if (!hasDevice)
					continue;
				const uint8_t want = any ? 1 : 0;
				if (opt[kTypeBFlag + side] != want)
				{
					opt[kTypeBFlag + side] = want;
					g_staged.flags[side] = want;
				}
			}
		}
	}
}
