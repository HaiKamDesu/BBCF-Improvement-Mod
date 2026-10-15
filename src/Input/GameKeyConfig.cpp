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

			struct KeyboardCandidate
			{
				std::string id;
				GameBinds binds;
				HeldKeys held;
			};
			KeyboardCandidate g_kbCandidate[2];

			// The keyboard each side edits in Button Settings, with its keys as they are now.
			// An empty id: that side has no keyboard of its own and edits the game's own
			// keyboard config (g_gameKb), as in vanilla.
			//
			// The game keeps ONE set of keyboard records, shared by both players' keyboard
			// panels. So that both players can have a keyboard panel open at once, each panel
			// is handed its own side's keys only while its own code runs: its tick and confirm
			// are wrapped (LoadKeyboardRecords before, SaveKeyboardRecords after), and the item
			// drawer reads the side's keys directly instead of the records.
			KeyboardCandidate g_kbEdit[2];
			GameBinds g_gameKb;
			constexpr int kGameKeyboard = 2;
			int g_kbStaged = -1;               // whose keys are in the records: 0/1, kGameKeyboard, -1 unknown

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
				g_kbEdit[0] = KeyboardCandidate();
				g_kbEdit[1] = KeyboardCandidate();
				std::memcpy(g_gameKb.setB, opt + kKbSetB, 40);
				std::memcpy(g_gameKb.setA, opt + kKbSetA, 40);
				g_kbStaged = kGameKeyboard;
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

			// The 40 bytes of a keyboard's battle records (Type A, then Type B), for the log:
			// with these a report shows exactly which keys a screen started from.
			std::string RecordHex(const uint8_t* rec)
			{
				char text[96];
				int at = 0;
				for (int i = 0; i < 40; ++i)
					at += snprintf(text + at, sizeof(text) - at, i == 20 ? "|%02X" : "%02X", rec[i]);
				return text;
			}

			// The keys a side's keyboard panel shows and edits.
			GameBinds& KeyboardKeysFor(int side)
			{
				return g_kbEdit[side & 1].id.empty() ? g_gameKb : g_kbEdit[side & 1].binds;
			}

			// Puts a side's keyboard keys into the game's shared records, right before that side's
			// keyboard panel code runs.
			void LoadKeyboardRecords(int side)
			{
				uint8_t* const opt = OptData();
				if (!opt)
					return;
				const GameBinds& keys = KeyboardKeysFor(side);
				const int who = g_kbEdit[side & 1].id.empty() ? kGameKeyboard : (side & 1);
				if (who != g_kbStaged || std::memcmp(opt + kKbSetB, keys.setB, 40) != 0 ||
					std::memcmp(opt + kKbSetA, keys.setA, 40) != 0)
				{
					std::memcpy(opt + kKbSetB, keys.setB, 40);
					std::memcpy(opt + kKbSetA, keys.setA, 40);
					std::memcpy(g_staged.kbB, keys.setB, 40);
					std::memcpy(g_staged.kbA, keys.setA, 40);
				}
				g_kbStaged = who;
			}

			// Takes whatever changed in the records back to whose keys they are: a side's keyboard
			// (0/1) or the game's own (kGameKeyboard).
			void SaveKeyboardRecordsOf(int who)
			{
				uint8_t* const opt = OptData();
				if (!opt || who < 0)
					return;
				const int side = who == kGameKeyboard ? 0 : (who & 1);
				static const KeyboardCandidate kNobody;
				const KeyboardCandidate& kb = who == kGameKeyboard ? kNobody : g_kbEdit[side];
				GameBinds& keys = kb.id.empty() ? g_gameKb : g_kbEdit[side].binds;
				if (std::memcmp(opt + kKbSetB, keys.setB, 40) != 0 || std::memcmp(opt + kKbSetA, keys.setA, 40) != 0)
				{
					const std::string before = RecordHex(keys.setB[0]);
					std::memcpy(keys.setB, opt + kKbSetB, 40);
					std::memcpy(keys.setA, opt + kKbSetA, 40);
					if (!kb.id.empty())
					{
						Sanitize(keys, BindClass::Keyboard);
						g_pendingChanges.emplace_back(kb.id, keys);
					}
					else
					{
						// The game's own config is what keyboards without settings of their own use.
						for (GameBinds& vanilla : g_vanilla[1])
						{
							std::memcpy(vanilla.setB, keys.setB, 40);
							std::memcpy(vanilla.setA, keys.setA, 40);
							Sanitize(vanilla, BindClass::Keyboard);
						}
					}
					LOG(1, "[GameKeyConfig] %s: keyboard settings changed (P%d): %s (was %s)\n",
						kb.id.empty() ? "the game's own keyboard config" : kb.id.c_str(), side + 1,
						RecordHex(opt + kKbSetB).c_str(), before.c_str());
				}
				std::memcpy(g_staged.kbB, opt + kKbSetB, 40);
				std::memcpy(g_staged.kbA, opt + kKbSetA, 40);
			}

			// After a side's keyboard panel code ran: its edits go to that side's keys.
			void SaveKeyboardRecords(int side)
			{
				SaveKeyboardRecordsOf(g_kbEdit[side & 1].id.empty() ? kGameKeyboard : (side & 1));
			}

			// The side a keyboard panel belongs to (its editor slot's id), or -1.
			int KeyboardPanelSide(const uint8_t* panel, int family)
			{
				if (!panel)
					return -1;
				const int slot = *reinterpret_cast<const int32_t*>(panel + 0x1F8);
				return family == 0
					? EditorSlotSide(kSetBStateRva, kSetBStateGuardRva, slot)
					: EditorSlotSide(kSetAStateRva, kSetAStateGuardRva, slot);
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
			uint32_t g_lastKbCaptureCur[2][4] = {};
			ULONGLONG g_lastKbCaptureMs[2] = {};

			FakeRegistry* __cdecl FakeKeyboardRegistryFor(const uint8_t* panel, int family)
			{
				if (!g_loaded || !panel)
					return nullptr;
				const int side = KeyboardPanelSide(panel, family);
				// A side with no keyboard of its own captures from the game's keyboard, as in vanilla.
				if (side < 0 || g_kbEdit[side].id.empty())
					return nullptr;
				// Edge = held now and not at the previous call (the capture runs every frame
				// while listening). On the first call nothing counts as new, so the key that
				// started the capture is not bound by accident. Kept per side: both players can
				// be capturing at once.
				const ULONGLONG now = GetTickCount64();
				const bool recent = now - g_lastKbCaptureMs[side] < 100;
				for (int i = 0; i < 4; ++i)
				{
					const uint32_t cur = g_kbEdit[side].held.bits[i];
					g_fakeKeyboard.cur[i] = cur;
					g_fakeKeyboard.prev[i] = recent ? g_lastKbCaptureCur[side][i] : cur;
					g_lastKbCaptureCur[side][i] = cur;
				}
				g_lastKbCaptureMs[side] = now;
				return &g_fakeRegistry;
			}

			constexpr uintptr_t kKbPanelRva[2] = { 0x0121BB88, 0x00D9A0B8 };   // battle / menu, + slot*0x1FC

			// The pad-to-keyboard panel switch: vanilla opens the keyboard panel for id 0 only.
			int __cdecl AllowKeyboardPanel(int id, int family, int slot)
			{
				if (id != 0 && id != 1)
					return 0;
				// Both players may have a keyboard panel open at once: each panel's code is handed
				// its own side's keys while it runs (see g_kbEdit).
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
					g_kbEdit[0] = KeyboardCandidate();   // Player 1 edits the game's own keyboard config
					setPanelId();
					return 1;
				}
				if (g_kbEdit[id].id != g_kbCandidate[id].id)
				{
					g_kbEdit[id] = g_kbCandidate[id];
					LOG(1, "[GameKeyConfig] P%d keyboard panel edits %s\n", id + 1, g_kbEdit[id].id.c_str());
				}
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
				if (!g_kbEdit[id].id.empty())
				{
					g_kbEdit[id].binds.typeB = opt[kTypeBFlag + id] & 1;
					g_pendingChanges.emplace_back(g_kbEdit[id].id, g_kbEdit[id].binds);
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

			// ---------------------------------------------------------------- both keyboard panels at once

			// The keyboard panels' tick (browse, clear, capture) and confirm (defaults, Type B):
			// everything that edits the keyboard records. fastcall(panel), plain ret.
			using PanelFn = int(__fastcall*)(uint8_t* panel);
			PanelFn g_kbTick[2] = {};      // battle 0x006A98F0, menu 0x00516C30
			PanelFn g_kbConfirm[2] = {};   // battle 0x006A9AD0, menu 0x00516DC0

			int RunAsSide(uint8_t* panel, int family, PanelFn original)
			{
				const int side = g_loaded ? KeyboardPanelSide(panel, family) : -1;
				if (side < 0)
					return original(panel);
				LoadKeyboardRecords(side);
				const int result = original(panel);
				SaveKeyboardRecords(side);
				return result;
			}

			int __fastcall KbTickSetB(uint8_t* panel) { return RunAsSide(panel, 0, g_kbTick[0]); }
			int __fastcall KbTickSetA(uint8_t* panel) { return RunAsSide(panel, 1, g_kbTick[1]); }
			int __fastcall KbConfirmSetB(uint8_t* panel) { return RunAsSide(panel, 0, g_kbConfirm[0]); }
			int __fastcall KbConfirmSetA(uint8_t* panel) { return RunAsSide(panel, 1, g_kbConfirm[1]); }

			// Replaces the vtable entry holding `original` (searched for within `slots` entries
			// of the vtable) with `wrapper`; returns the original.
			PanelFn PatchVtableEntry(const char* label, uintptr_t vtable, int slots, uintptr_t original, void* wrapper)
			{
				uint32_t* const entries = reinterpret_cast<uint32_t*>(vtable);
				if (IsBadReadPtr(entries, slots * 4))
					return nullptr;
				for (int i = 0; i < slots; ++i)
				{
					if (entries[i] != original)
						continue;
					DWORD old = 0;
					if (!VirtualProtect(&entries[i], 4, PAGE_READWRITE, &old))
						return nullptr;
					entries[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(wrapper));
					VirtualProtect(&entries[i], 4, old, &old);
					return reinterpret_cast<PanelFn>(original);
				}
				LOG(1, "[GameKeyConfig] %s: not in its vtable, skipped\n", label);
				return nullptr;
			}

			// The item drawer reads a keyboard row's key straight from the records
			// (0x006BB1ED battle, 0x006BBDF3 menu: mov al,[eax+ecx*4+record], eax = optData +
			// row-3, ecx = profile*5, panel at [ebp-6180h]). Both panels draw after both have
			// ticked, so the records hold only the last one's keys: give each its own.
			int __cdecl KeyboardRowKeyForDraw(const uint8_t* panel, int family, const uint8_t* rowAddr, int profile5)
			{
				const uintptr_t recordOffset = family == 0 ? kKbSetB : kKbSetA;
				const uint8_t* const vanilla = rowAddr + profile5 * 4 + recordOffset;
				const uint8_t* const opt = OptData();
				const int side = g_loaded && opt ? KeyboardPanelSide(panel, family) : -1;
				const int index = opt ? static_cast<int>(rowAddr - opt) + profile5 * 4 : -1;
				if (side < 0 || index < 0 || index >= 40)
					return *vanilla;
				const GameBinds& keys = KeyboardKeysFor(side);
				return family == 0 ? keys.setB[index / 20][index % 20] : keys.setA[index / 20][index % 20];
			}

			DWORD g_drawBack[2] = {};

			void __declspec(naked) KeyboardRowDrawSetB()
			{
				__asm
				{
					push ecx
					push edx
					push ecx
					push eax
					push 0
					push dword ptr[ebp - 6180h]
					call KeyboardRowKeyForDraw
					add esp, 16
					pop edx
					pop ecx
					jmp[g_drawBack]
				}
			}

			void __declspec(naked) KeyboardRowDrawSetA()
			{
				__asm
				{
					push ecx
					push edx
					push ecx
					push eax
					push 1
					push dword ptr[ebp - 6180h]
					call KeyboardRowKeyForDraw
					add esp, 16
					pop edx
					pop ecx
					jmp[g_drawBack + 4]
				}
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
			// Both players' keyboard panels at once: per-side keys for the panel code and the drawer.
			g_kbTick[0] = PatchVtableEntry("keyboard panel tick (battle)", base + 0x005748D8, 24, base + 0x002A98F0, reinterpret_cast<void*>(KbTickSetB));
			g_kbConfirm[0] = PatchVtableEntry("keyboard panel confirm (battle)", base + 0x005748D8, 24, base + 0x002A9AD0, reinterpret_cast<void*>(KbConfirmSetB));
			g_kbTick[1] = PatchVtableEntry("keyboard panel tick (menu)", base + 0x0054BF4C, 24, base + 0x00116C30, reinterpret_cast<void*>(KbTickSetA));
			g_kbConfirm[1] = PatchVtableEntry("keyboard panel confirm (menu)", base + 0x0054BF4C, 24, base + 0x00116DC0, reinterpret_cast<void*>(KbConfirmSetA));
			ok += (g_kbTick[0] != nullptr) + (g_kbConfirm[0] != nullptr) + (g_kbTick[1] != nullptr) + (g_kbConfirm[1] != nullptr);
			static const uint8_t kDrawReadB[] = { 0x8A, 0x84, 0x88, 0x56, 0x42, 0x00, 0x00 };   // mov al,[eax+ecx*4+4256h]
			static const uint8_t kDrawReadA[] = { 0x8A, 0x84, 0x88, 0xA7, 0x4A, 0x05, 0x00 };   // mov al,[eax+ecx*4+54AA7h]
			if (Matches(base + 0x002BB1ED, kDrawReadB, sizeof(kDrawReadB)))
			{
				g_drawBack[0] = HookManager::SetHook("GameKeyConfigKbDrawSetB", static_cast<DWORD>(base + 0x002BB1ED), 7,
					reinterpret_cast<void*>(KeyboardRowDrawSetB));
				ok += g_drawBack[0] != 0;
			}
			if (Matches(base + 0x002BBDF3, kDrawReadA, sizeof(kDrawReadA)))
			{
				g_drawBack[1] = HookManager::SetHook("GameKeyConfigKbDrawSetA", static_cast<DWORD>(base + 0x002BBDF3), 7,
					reinterpret_cast<void*>(KeyboardRowDrawSetA));
				ok += g_drawBack[1] != 0;
			}
			if (!g_kbTick[0] || !g_kbConfirm[0] || !g_kbTick[1] || !g_kbConfirm[1] || !g_drawBack[0] || !g_drawBack[1])
				LOG(1, "[GameKeyConfig] not every per-side keyboard patch is in place; both keyboard panels at once may show or edit the other player's keys\n");
			LOG(1, "[GameKeyConfig] %d/16 patches in place\n", ok);
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

			// Which button-settings profile each side's battle input is read with, against what
			// the game set up for the match (Kakuy 2026-10-13/14: "my controls change when a
			// match starts, back gives barrier, fixed after the first round or by opening the
			// mod"; nothing in the log said which Type each side was on). Logged on change.
			{
				const int32_t* const setB = EngineInts(kEngineSetBProfile);
				const int32_t* const setA = EngineInts(kEngineSetAProfile);
				const int32_t* const sideId = EngineInts(0x25F4);
				const int now[8] = { setB[0], setB[1], setA[0], setA[1], sideId[0], sideId[1],
					opt[kTypeBFlag] & 1, opt[kTypeBFlag + 1] & 1 };
				static int last[8] = { -100, -100, -100, -100, -100, -100, -100, -100 };
				if (std::memcmp(now, last, sizeof(now)) != 0)
				{
					std::memcpy(last, now, sizeof(now));
					LOG(1, "[GameKeyConfig] battle profile (Type) in use: id0=%d id1=%d; menu set id0=%d id1=%d; "
						"controller id per side P1=%d P2=%d; Type B allowed P1=%d P2=%d\n",
						now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7]);
				}
			}

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
			for (KeyboardCandidate& kb : g_kbEdit)
			{
				if (kb.id.empty())
					continue;
				const DeviceRef* ref = find(kb.id);
				kb.held = ref ? HeldGameKeys(ref->kind, ref->controls) : HeldKeys();
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
				if (active && menuSet && !g_kbEdit[side].id.empty())
				{
					g_kbEdit[side].binds.menuSet = now.menuSet[side] & 1;
					record(g_kbEdit[side].id, g_kbEdit[side].binds);
				}
			}
			const bool anyOpen = open[0] || open[1] || g_editor[0].open || g_editor[1].open;
			// The keyboard records are normally edited only inside a wrapped panel tick or
			// confirm, which saves the change itself. Anything else that touches them is taken
			// as an edit to the keys last loaded.
			if (anyOpen && (g_kbStaged == 0 || g_kbStaged == 1 || g_kbStaged == kGameKeyboard) &&
				(std::memcmp(now.kbB, g_staged.kbB, 40) != 0 || std::memcmp(now.kbA, g_staged.kbA, 40) != 0))
			{
				SaveKeyboardRecordsOf(g_kbStaged);
				for (const auto& pending : g_pendingChanges)
					record(pending.first, pending.second);
				g_pendingChanges.clear();
				ReadImage(opt, now);
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
				if (!g_kbEdit[side].id.empty() && g_kbEdit[side].binds.menuSet != live)
				{
					g_kbEdit[side].binds.menuSet = live;
					record(g_kbEdit[side].id, g_kbEdit[side].binds);
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
						LOG(1, "[GameKeyConfig] P%d Button Settings opened%s; Type B flags %d/%d, keyboard records hold %s (%s)\n", side + 1,
							best ? "" : " - that player has no usable pad right now (keyboard or nothing)",
							opt[kTypeBFlag] & 1, opt[kTypeBFlag + 1] & 1, g_kbStaged == kGameKeyboard ? "the game's own keys" : g_kbStaged < 0 ? "unknown keys" : g_kbEdit[g_kbStaged].id.c_str(),
							RecordHex(opt + kKbSetB).c_str());
					editor.open = true;
					if (best)
					{
						editor.padOwner = best->id;
						editor.padBinds = best->binds;
						editor.padKind = best->kind;
						StagePad(side, editor.padBinds);
						LOG(1, "[GameKeyConfig] P%d editor: pad records -> %s\n", side + 1, best->id.c_str());
					}
					// The keyboard this player edits: their most recently used one. Its keys are
					// handed to their keyboard panel whenever that panel's code runs.
					const KeyboardCandidate& kb = g_kbCandidate[side];
					if (!kb.id.empty() && find(kb.id) && g_kbEdit[side].id != kb.id)
					{
						g_kbEdit[side] = kb;
						LOG(1, "[GameKeyConfig] P%d editor: keyboard -> %s\n", side + 1, kb.id.c_str());
					}
				}
				else if (!open[side] && editor.open)
				{
					LOG(1, "[GameKeyConfig] P%d Button Settings closed%s\n", side + 1,
						editor.padOwner.empty() ? " (no pad was being edited)" : (" (was editing " + editor.padOwner + ")").c_str());
					editor = SideEditor();
					g_kbEdit[side] = KeyboardCandidate();   // the next editor re-picks
				}
				if (editor.open && !editor.padOwner.empty())
				{
					const DeviceRef* pad = find(editor.padOwner);
					editor.padControls = pad ? pad->controls : ControlSet();
				}
			}

			// Every editor closed: the records hold the game's own keyboard config again, which is
			// what the game saves as its own, rather than the keys of whichever player's keyboard
			// panel ran last.
			const bool anyEditor = open[0] || open[1] || g_editor[0].open || g_editor[1].open;
			if (!anyEditor && g_kbStaged != kGameKeyboard && g_kbStaged >= 0)
			{
				std::memcpy(opt + kKbSetB, g_gameKb.setB, 40);
				std::memcpy(opt + kKbSetA, g_gameKb.setA, 40);
				std::memcpy(g_staged.kbB, g_gameKb.setB, 40);
				std::memcpy(g_staged.kbA, g_gameKb.setA, 40);
				g_kbStaged = kGameKeyboard;
				LOG(1, "[GameKeyConfig] Button Settings closed: keyboard records back to the game's own config\n");
			}

			// 3. Outside the editors, char select reads the Type B flag per id: allow Type B for
			// a side when any of its devices has it enabled.
			for (int side = 0; side < 2; ++side)
			{
				if (g_editor[side].open)
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
