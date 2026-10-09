# Controllers and input assignment

Boundary: everything between the OS input APIs and the point where battle or menu code asks
"did player X press action Y". Goal: a full override of device refresh (hot-plug) and of
how devices are assigned to players. Addresses are VAs at image base 0x00400000 (Steam build
11073075). Ghidra's `GetCharSelectStruct` (FUN_0047E860) returns the engine/game-state block at
0x00C903B0; this doc calls it `engine`.

First mapped 2026-10-07 (L1 pass). Second pass the same day raised the core to L2.

- **L2** rows had their convention, `ret N`, arguments, return value and the fields they touch
  checked in the assembly (`tools/bbcf_disasm_ascii.txt`) and, for tables, in the raw bytes of
  the archived `BBCF.exe`.
- **L1** rows are from the decompile plus spot checks. Treat Ghidra's register arguments with
  suspicion until a row is L2.

## 0. Answers for the rewrite

The mod's new input system will:

- substitute the physical action word of the controller tasks every tick, at the existing
  SystemInputWrite hook (base+0x96408);
- report the connection state of controller ids 0/1 itself;
- keep the controller-id model with the active id pinned to 0.

Everything below is confirmed from the assembly unless it is marked *hypothesis*.

### A1. Layout of the controller action word (`KeyControler+0x30`)

**One layout for both sets.**

- `InputDevice_BuildActionWords` (0x004963F0) sets bit `1 << i` for action index `i` when the
  controller's binding for `i` is down, for `i < +0x38` (always 0x1C = 28).
- Bits 28-31 can never be set. The game's own mask 0x20000000 (checked at 0x004AD231 on the
  keyboard set-A controller) is therefore dead.
- Set A (menus: pad controllers at sysMgr+0x10/+0x14 and the keyboard controller at +0x0C) and
  set B (battle: pads at +0x24/+0x28, keyboard at +0x18) use the same bit positions. They differ
  only in which bindings the key config writes, and in which bits the readers look at.
- Callers never test bits directly. They pass an **action ID** (0-30), which
  `ActionIdMaskTable` (0x009DE0B0, 31 dwords) turns into a mask. The table was re-read from the
  exe for this pass.

| Bit | Mask | Mask IDs that select it | XInput default | Keyboard default | Battle meaning (set B) | Menu meaning (set A) | Mod (`PackSystemInputWord`) |
|---|---|---|---|---|---|---|---|
| 0 | 0x00000001 | 7 | d-pad/left stick up | W, also Up arrow and Space (hardcoded) | Up | Up | dirs 0x01 Up: agrees |
| 1 | 0x00000002 | 10 | right | D, also Right arrow | Right | Right | dirs 0x02 Right: agrees |
| 2 | 0x00000004 | 8 | down | S, also Down arrow | Down | Down | dirs 0x04 Down: agrees |
| 3 | 0x00000008 | 9 | left | A, also Left arrow | Left | Left | dirs 0x08 Left: agrees |
| 4-7 | | none | unbound | unbound | not packed | no reader | never set |
| 8 / 9 / 10 / 11 | 0x100 / 0x200 / 0x400 / 0x800 | 21 / 24 / 22 / 23 | unbound | unbound | not packed | a second direction group (up/right/down/left by the same ID order as 7-10). Never bound by a default or by any key-config apply, so always 0 in vanilla. Read only as held IDs 21/22 at 0x006AE4E0/0x006AE503 | never set |
| 12 | 0x00001000 | 1, 3, 12 | A (game key 0) | J | **D** (packed 0x80) | **Confirm** (IDs 3 and 1 are the common confirm checks; pad assign, key config) | main 0x10 = D, Confirm: agrees |
| 13 | 0x00002000 | 2, 4, 13 | B (1) | O | **C** (0x40) | **Cancel/back** (ID 4) | main 0x20 = C, Return: agrees |
| 14 | 0x00004000 | 14 | X (2) | U | **A** (0x10) | extra button (ID 14) | main 0x40 = A, "PlayerInfo": agrees |
| 15 | 0x00008000 | 11 | Y (3) | I | **B** (0x20) | extra button (ID 11) | main 0x80 = B, "FriendFilter": agrees |
| 16 | 0x00010000 | 15, 27 | LB (4) | K | not packed. Set-B record slot +0xA. Read as mask 0x10000 by training (0x006D21F9, 0x006DD42E) | page/category (ID 15) | secondary 0x01 = Fn1: agrees |
| 17 | 0x00020000 | 16, 25 | RB (5) | L | **Taunt** (packed 0x100) | page/category (ID 16) | secondary 0x02 = Taunt: agrees |
| 18 | 0x00040000 | 17, 28 | LT (6) | P | not packed. Set-B record slot +0xB. Mask 0x40000 at 0x006D221B (training) | page/category (ID 17) | secondary 0x04 = Fn2: agrees |
| 19 | 0x00080000 | 18, 26 | RT (7) | `=` (VK_OEM_PLUS) | **Special** (packed 0x200) | page/category (ID 18) | secondary 0x08 = Special: agrees |
| 20 | 0x00100000 | 19 | left-stick click (10) | N | not packed, not in any key config | no caller found | never set |
| 21 | 0x00200000 | 20 | right-stick click (11) | M | same | no caller found | never set |
| 22 | 0x00400000 | none | unbound | unbound | macro **A+B** (packer ORs it into A and B). Set-B record +6 | not in set A | **disagrees**: the mod expands MacroAB into bits 14 and 15 |
| 23 | 0x00800000 | none | unbound | unbound | macro **B+C** (+7) | | expanded, as above |
| 24 | 0x01000000 | none | unbound | unbound | macro **A+B+C** (+8) | | expanded |
| 25 | 0x02000000 | none | unbound | unbound | macro **A+B+C+D** (+9) | | expanded |
| 26 | 0x04000000 | 5 | Start (8) | Enter | not packed | **Start** (title press-start mask 0x4000000, invite and join screens use ID 5) | system 0x04 = start: agrees, but no MenuAction produces it |
| 27 | 0x08000000 | 6, 29, 30 | Back (9) | Backspace | **Reset positions** (set-B record +0xC; training reset tests mask 0x8000000 just-pressed through `Input_MaskJustPressedForId`, 0x0055EFA3) | **Back/Select** (title quit, ID 6) | system 0x08 = select: agrees, but **disagrees** for `MacroResetPositions`, which the mod never encodes into bit 27 |

Where the "XInput default" and "Keyboard default" columns come from:

- **Defaults**: `BattleKeyControler_SetDefaults` (0x004699C0) for pads and
  `SystemKeyControler_SetDefaults` (0x00469AB0) for the keyboard. The key config overwrites
  them later, but only bits 12-19 for set A pads, plus bits 0-3 for the keyboards, plus bits
  22-25 and 27 for set B. **Bits 20, 21, 26 and 27 of set A keep these defaults forever.**
- **XInput game keys** (`XInputPad_Poll`): 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT (>30), 7 RT
  (>30), 8 Start, 9 Back, 10 LS click, 11 RS click. Directions 0x20-0x23 are up/right/down/left
  from the d-pad or the left stick, and 0x24-0x27 are the right stick.
- **Keyboard game keys**: the VK table that `Keyboard_ctor` builds (section 3). Letters A-Z are
  game keys 0-25.

How the packer and the key config line up with the mod:

- **Packed battle word** (`InputDevice_PackBattleWord`, 0x004968E0). Directions -> numpad digit,
  with left beating right and down beating up; SOCD was already neutralised.
  - 0x10 A = bits 14|22|24|25
  - 0x20 B = 15|22|23|24|25
  - 0x40 C = 13|23|24|25
  - 0x80 D = 12|25
  - 0x100 = bit 17
  - 0x200 = bit 19
  - Bits 16, 18, 20, 21, 26 and 27 never reach the battle word.
- **Key-config record order** proves the mod's enums. The set-B keyboard record
  (`ApplyKeyboardKeyConfig_SetB`) binds, byte by byte: Up, Down, Left, Right, A, B, C, D, bit 17,
  bit 19, AB, BC, ABC, ABCD, bit 16, bit 18, reset. That is exactly `BattleAction`'s order. The
  set-A keyboard record binds Up, Down, Left, Right, then bits 14, 15, 13, 12, 17, 19, 16, 18.
  That is exactly `MenuAction`'s order. The mod's menu labels (PlayerInfo, FriendFilter,
  ChangeCategory, ReplayControls) are context names; to the game they are just bits 14, 15, 17
  and 19.

Agreement summary for `ControllerOverrideManager`:

- Every bit the mod sets means what the mod thinks it means.
- `SystemInputBytes` is not four semantic bytes; it is the 32-bit action word cut into bytes.
  Byte +31 holds bits 8-15 (the mod only uses 12-15), byte +32 holds bits 16-23 (the mod uses
  16-19), byte +33 holds bits 24-31 (the mod uses 26-27).
- Gaps:
  - The battle macros are expanded into button bits instead of bits 22-25. That is equivalent
    for the packer and for every reader found.
  - `MacroResetPositions` is dropped. It should set bit 27 in the set-B word.
  - Nothing produces bit 26 (Start), so a mod-driven player cannot press start at the title or
    the invite screen, nor join as challenger or training enemy (all of those use ID 5).
- `SystemControllerSlot` resolves only the six pad pointers. The keyboard controllers
  (sysMgr+0x0C/+0x18) resolve to `None`.

### A2. Tick order, and whether substituting `+0x30` is enough

Per frame, task priorities run low to high:

1. GAME_InputTask (0x01001000) polls every device.
2. The two keyboard controllers (0x01002000) tick.
3. Pad slot 0 (0x01003000) and slot 1 (0x01003001) tick, each set-A and set-B controller once.

Each controller tick is `KeyControler_Update` (0x00496600) -> `KeyControler_Tick` (0x00496AF0).
`+0x48` is a freeze flag; the only writer is the ctor, which writes 0. `KeyControler_Tick` does
this, in order:

1. Load `node = [+0x20]` and clear `+0x28` (just pressed) and `+0x2C` (released).
2. `InputDevice_BuildActionWords` (0x004963F0):
   - `ebx = 0` (effective accumulator), `[ebp-0Ch] = node+0x0C` (previous effective word),
     `edi = 0`.
   - **0x00496408 `mov [esi+30h],ebx` / 0x0049640B `cmp [esi+38h],ebx`**. This is the mod's
     SystemInputWrite site: 6 bytes, `89 5E 30 39 5E 38`. ESI = controller, and **EBX is always 0
     here**.
   - Then `jbe 0x496456`, which skips the loop when count <= EBX.
   - The loop, for `i < +0x38`: physical = `vtbl+8(i)` (IsBindingDown). If `+0x50` is set, the
     effective bit = `+0x4C & (1<<i)`; otherwise it is the physical bit. `+0x30 |= physical`,
     `ebx |= effective`.
   - SOCD on both words: bits 0+2 both set -> both cleared, bits 1+3 both set -> both cleared.
   - Edges against the previous effective word: `+0x28 |= newly set`, `+0x2C |= newly cleared`.
   - `+0x50 = 0`. Return `ebx`.
3. If any edge, advance the history ring (`FUN_00405CB0` on the list at `+0x1C`) and zero the
   new node's `+0x10`.
4. `node+0x10++` (frames since the last edge of ANY bit), and `node+0x0C = ebx`.
5. `+0x44--` while > 0.

**The current hook does not substitute. It merges.** The naked stub writes the mod word `W` to
`+0x30` *and* leaves `EBX = W`. That has three effects:

- `W >= 0x1C` (any button bit set): the `jbe` skips the loop. Pure substitution, and the
  `+0x4C/+0x50` override is ignored.
- `1 <= W <= 0x1B` (directions only, or bit 4): the loop still runs, and the device's physical
  bits are **ORed in**. The keyboard and pad still leak through whenever the mod holds only
  directions.
- `W == 0`: identical to vanilla. Returning 0 cannot blank a controller.

**Correct substitution**: when the mod owns a controller, write `[esi+30h] = W`, set `ebx = W`,
and jump to **0x00496456** (the SOCD block after the loop) instead of back to 0x0049640E. Checks:

- Nothing else branches to 0x00496409..0x0049640D. 0x00496456 is reached only from the `jbe`
  at 0x0049640E and by falling out of the loop.
- Every register the tail uses is valid there: `[ebp-0Ch]` was set at 0x00496405, `edi` is
  pushed and reloaded at 0x0049649B, `ecx` is zeroed at 0x00496495.
- Use a separate "owned" flag, so that "owned, nothing held" (W = 0) is not confused with
  "pass through".
- This skips the IsBindingDown calls, which are pure reads (including the keyboard's hardcoded
  arrow keys), so nothing is lost.

With that change, one substituted `W` is everything every downstream reader sees:

| Reader | Reads | Follows a substituted `+0x30`? |
|---|---|---|
| Edges `+0x28` / `+0x2C` | computed from `ebx` vs the previous `node+0x0C` | yes (`ebx = W`) |
| History node `+0x0C` (effective), `+0x10` (frames) | `ebx` | yes |
| `KeyControler_ActionHeld` 0x004964E0 (wrapper 0x00464170 "RepeatPressed", actually held) | `node+0x0C` | yes |
| `KeyControler_ActionJustPressed` 0x00496520, `MaskJustPressed` 0x004963A0, `ActionReleased` 0x004965C0 | `+0x28` / `+0x2C` | yes |
| Repeat `ControllerWrapper_ActionPressedOrRepeat` 0x004641F0 -> `KeyControler_ActionPressedOrRepeat` 0x00496560 | `+0x28`, or held with `node+0x10 > 8` and `(node+0x10 & 3) == 0` | yes. The frame counter is shared by all bits, so any other edge restarts the repeat (vanilla behaviour) |
| `InputDevice_PackBattleWord` 0x004968E0 | **`+0x30` directly** (not the history node; this corrects the L1 note) | yes |
| `BattleInputWrite` 0x00559FE0 / `_Alt` 0x0055A3E0 | `GetBattleWrapperForSide` -> `BattleWrapper_PackBattleWord` -> `+0x30` of set B | yes |
| Online sampler 0x004E6000 | `GetBattleWrapperForActiveId` -> `BattleWrapper_PackBattleWord` -> `+0x30` of set B; forced to 5 (neutral) when `FUN_0069A980() != 0` | yes |
| Replay recorder 0x0069DB60 (online branch) | same as the online sampler | yes |
| Virtual keyboard 0x004744D0 | `KeyControler_ActionJustPressed(3/4)` on pad set A directly | yes |

Notes:

- Wrapper 0 merges two controllers: pad slot 0 and the keyboard. Menu queries OR them, and
  `BattleWrapper_PackBattleWord` vector-adds the two digits. To control wrapper 0 completely,
  substitute **both** the pad slot-0 controller and the keyboard controller of the same set,
  e.g. give the keyboard `W = 0`, owned.
- The hook fires for every controller task in the task list. That includes the stale tasks
  that the mod's redetect leaves alive, which nothing reads.
- Raw device readers that bypass the action word entirely: see A6.

### A3. Connection and count; pinning the active id to 0

| VA | Function | Convention | What it does |
|---|---|---|---|
| 0x0047F480 | IsControllerIdConnected | **stdcall(id), ret 4** (the L1 note said cdecl) | Unsigned clamp (`id > 2` -> 0, so negative ids also -> 0), then `GetMenuWrapper(id)` (id 0 -> wrapper 0, anything else -> wrapper 1), then `ControllerWrapper_IsConnected` |
| 0x004644E0 | ControllerWrapper_IsConnected | thiscall(wrapper), ret | pad `KeyControler_IsDeviceConnected`, ORed with the keyboard's only when wrapper+4 == 0. Only caller: 0x0047F480 |
| 0x00496A90 | KeyControler_IsDeviceConnected | thiscall(ctrl), ret | registry[*vtbl+0x14()]; out of range -> 0; else tail-jumps to device vtbl+0x10 (keyboard and DInput `ReturnTrue`, XInput `+0x2C & 1`) |
| 0x0047EB50 | GetControllerCount | no args, ret | `mov eax,2` |

To report connection state for ids 0/1, a hook at `IsControllerIdConnected` (or at
`ControllerWrapper_IsConnected`) is enough. Every game reader goes through 0x0047F480. **Vanilla
id 0 is always connected**, because the keyboard's IsConnected is `ReturnTrue`.

Callers of IsControllerIdConnected and GetControllerCount:

| Caller | Screen | Ids | Use |
|---|---|---|---|
| 0x004A6670 InviteSelectPad_Tick | online invite pad select | 0..1 | first connected id with Start (ID 5) -> `engine+0x25F0 = id` |
| 0x00515D80 / 0x006A8B70 | MenuKeyConfigMenu / KeyConfigMenu player join | 0..1 | connected id + Start claims an editor slot |
| 0x005F9450 | title/attract "any player pressed" | 0..Count-1 | connected id with Start or ID 1, after more than 0x1D frames -> 1 |
| 0x005F9A40 Title_PressStartTick | title | 0..1 | see A6 for the raw keyboard part |
| 0x00712580 / 0x007226A0 / 0x0071C970 | VS pad assign tick / icons / screen create | 0..1 | disconnected -> unassign / hide icon / count connected |
| 0x00697380 / 0x00699040 | lost-controller pause (UI manager 0x01500374) | **0..3** | see below |
| 0x006D2190 Training_EnemyControllerJoin | training | 0..Count-1 | skips the local player's id (`+0x25F4[+0x1688]`); Start -> dummy side id |

**Lost-controller pause.** `UiManager_Tick` (0x00699040) polls ids 0..3 every frame:

- `+0x00..+0x0C`: last connected state per id.
- `+0x10..+0x1C`: "lost" latch, set when an id that was connected reads disconnected.
- `+0x20..+0x2C`: `Engine_IsControllerIdInUse(id)` (0x0047F530).

`0x00697380` returns 1 when an id is in use, disconnected and latched. `0x0047F390`
(`Pause_ShouldOpen`) returns 1 when that holds *and* `FUN_004A77E0()+0x270 != 0` (*hypothesis*:
online session active). The pause menus (0x004DFF20 sparring, 0x006D04A0 training, 0x006D5BA0)
then open by themselves. Opening clears the latches (0x00697FC0).

In-use rule (`Engine_IsControllerIdInUse`, thiscall(engine, id), ret 4):

- Modes 1/5 (Arcade/Versus): the id is a human P1/P2.
- Mode 6 (Training): the id is the local side's id, or the enemy controller id.
- Every other mode: `id == engine+0x25F0`.

**So if the mod reports id 0 disconnected while it is in use, the game force-opens the pause
menu.** Keep id 0 connected while a local human exists.

**Every writer of `engine+0x25F0` (active id):**

| Writer | Context | Value |
|---|---|---|
| 0x0047CBC0 (engine ctor) | first use | 0 (and P1 = 0, P2 = 1) |
| 0x004830D0, 0x00483370 | boot/title resets | 0 |
| 0x005F9A40 | title press-start | the id that pressed |
| 0x004A6670 | online invite pad select | the id that pressed Start |
| 0x006C4990 case 0x16 | SaveSequence task | `task+0x20`, whose only writer (ctor 0x006C3A90) writes 0 |

**Every writer of `+0x25F4` / `+0x25F8` (P1/P2 ids):**

| Writer | Context | Value |
|---|---|---|
| 0x0047CBC0 | engine ctor | P1 = 0, P2 = 1 |
| 0x005505A0, 0x005F8790, 0x0066E5F0, 0x0072A3B0, 0x0072C5C0, 0x0072CB60, 0x007308C0 | mode starts (story, arcade, challenge, Abyss, main menu launches) | P1 = active, P2 = !active |
| 0x0072D060 | VS pad assign apply | as picked; one pick -> the other side gets !id; none -> 0/1 |
| 0x0049F0F0 Online_AssignControllerIds | online | local side = active, other = !active; fallback 0/1 |
| 0x00486AB0 Challenger_JoinTick | arcade/Abyss/versus join | `+0x25F4[side] = id` for an id other than the local id that presses Start |
| 0x006CFB10, 0x006CFEE0, 0x006D2190 | training enemy | dummy side = joining id |
| 0x006C2960 | reset | P1 = 0, P2 = 1 |
| 0x0047FD20 | restore from the saved copy | `+0x2534/+0x2538` -> P1/P2 (saved by 0x0047D910 CharSelectConfigCommit) |

**What pinning the active id to 0 changes:**

- Title, invite and SaveSequence keep writing ids. Their result is overwritten to 0. A player
  who pressed start on what the game calls "pad slot 1" then drives menus and netplay through
  wrapper 0. That is safe only if the mod routes the claiming device's word into pad slot 0
  (and the keyboard controller). The game has no other path.
- **Online**: `Online_AssignControllerIds` gives the local side id 0 and the remote side id 1.
  The remote side's input comes from the network; wrapper 1 is never sampled for it. The online
  sampler and the replay recorder read wrapper 0. `BattleLoad_ApplyPlayerKeyConfig` loads the
  local side's profile into slot 0 (and the keyboard). Consistent.
- **Story/arcade/training/tutorial**: P1 = 0, P2 = 1 (CPU or dummy). Challenger join and
  training enemy join test ids other than the local id (0), so id 1 can still join and becomes
  that side's id.
- **VS pad assign** does not use the active id. P1/P2 can still become 1/0, and the local side
  can become 1.
- **Replays (mode 11)**: `0x0069DE70` skips id assignment; playback uses recorded words. Theater
  menus use active-id queries, so wrapper 0 controls them.
- **P1/P2 ids can still be 1**, from mode starts, pad assign, challenger, training join, the
  online remote side and the restore. An id-1 side reads wrapper 1, which is pad slot 1 only,
  never the keyboard. Its readers: BattleInputWrite, BattleLoad key config (slot 1 profile),
  named queries with id 1, `GetMenuWrapperForSide`/`ForTrainingSide`, and the 0..3 connection
  polling. The mod therefore has to substitute pad slot 1's words for the second local human.
- Nothing found breaks. The one trap is the disconnect pause above.

### A4. Keyboard controllers (sysMgr+0x0C set A, +0x18 set B)

- **Zeroing their word removes the keyboard from wrapper 0 completely**, but only with the
  corrected hook (owned, `W = 0`, loop skipped). Then `+0x30 = 0`, the effective word is 0, and
  the keyboard never produces an edge. Every wrapper-0 query ORs in zero, and the battle and
  online packers add a neutral digit 5 (the digit merge in 0x00464340 treats 5 as identity).
- With the **current** hook, returning 0 is vanilla and the keyboard stays in.
- Side effects found: none on controller state.
- What zeroing does NOT do:
  - `ControllerWrapper_IsConnected` still ORs the keyboard's `ReturnTrue`, so id 0 still reads
    connected. That is good: see A3.
  - `Keyboard_Poll` still runs `GetKeyboardState`.
  - The key config still writes keyboard bindings. Harmless.
  - The raw readers in A6 still see the keyboard.
- **`+0x3C = 1`** is written only by `SysMgr_InitSystemKey`; the ctor writes 0. Its only
  readers are `KeyControler_ActionHeld/JustPressed/PressedOrRepeat/Released` and
  `AnyAttackHeld/JustPressed`. They use it to choose the argument of `FUN_0047EBB0(engine,
  +0x3C ? 0 : +0x54)`, whose result is **discarded**. `FUN_0047EBB0` (thiscall, ret 4) is a pure
  per-player block getter: `side <= 3 ? engine+0x118+side*0x710 : 0`. So `+0x3C` has no effect
  on behaviour; it is the leftover of a removed feature.

### A5. Window focus and the input-focus gate

**Application active flag.** `App_Get` (0x0043BAF0) is cdecl, takes no arguments and returns the
static app object 0x00A5A500. This is the same object as `[sysMgr+4]`; hwnd is at `+0x28`.
`app+0xD0` means **"application active"** (WM_ACTIVATEAPP), not "window focused".

| Writer | Convention | Effect |
|---|---|---|
| App_ctor 0x0043A7A0 | | `+0xD0 = 1` |
| App_OnDeactivate 0x0043C420 | thiscall(app), ret | SetFocus(NULL) if GetFocus() == hwnd; `+0xD0 = 0` |
| App_OnActivate 0x0043D110 | thiscall(app, int setFocus), ret 4 | optional SetFocus(hwnd); `+0xD0 = 1` |
| 0x0043B9A0 | thiscall(app), ret | runs only while `+0xC8 == 0`: window style, ShowWindow, SetFocus, `+0xD0 = 1`. *Hypothesis*: leave fullscreen / show window. Called by 16 menu functions at 0x006C3B30..0x006C4990 |

The WndProc (0x0044F350) handles WM_ACTIVATEAPP: wParam != 0 calls `App_OnActivate(0)`,
wParam == 0 calls `App_OnDeactivate`. It has no WM_KEYDOWN or WM_SYSKEYDOWN handling.

**The only reader of `app+0xD0` is `Keyboard_Poll`** (0x004194B8). All 144 callers of App_Get
were scanned, and there is no absolute reference to the field. While the app is inactive:

- **Stops**: the keyboard. GetKeyboardState and TranslateVkState are both skipped, so the
  current and previous key maps freeze. A key held at focus loss stays held, and no new edges
  happen until the app is active again. Every keyboard reader sees the frozen state: the keyboard
  controller tasks and the raw readers in A6.
- **Keeps running**: XInput and DInput pad polling (neither checks the flag, and DInput pads are
  opened `DISCL_EXCLUSIVE | DISCL_BACKGROUND`), GAME_InputTask, every controller tick, the
  battle input writers and the online sampler. A mod layer that writes `+0x30` itself is not
  focus-gated unless it adds its own gate. To match vanilla for the keyboard part, freeze or
  zero the mod keyboard state while `app+0xD0 == 0`.

**Input-focus name stack ("UI exclusive name").** This gates only the named query API
(`Input_*ForId`).

- Names are `char[32]` at 0x01415A28; the top index is at [0x013874B8]. Slot 0 is `""`.
- The override flag is [0x013874BC], with the override name at 0x01463DDC.
- `InputFocus_GetCurrentName` (0x00695AD0): no arguments, `ret`, returns `char*`.

| VA | Name | Convention | Behaviour |
|---|---|---|---|
| 0x00697B90 | InputFocus_Push | cdecl(const char name[32]), ret | index++, copy 32 bytes (logs "PushUIExclusiveName" past 8). 20 callers: virtual keyboard, key-config pages, 0x006C3B30.. menus, MsgWindow_Open, 0x0069B1D0, 0x00777620 |
| 0x00697B70 | InputFocus_Pop | no args, ret | returns the old top, index-- |
| 0x00697DB0 | InputFocus_Remove | cdecl(name), ret | removes a named entry (virtual keyboard only) |
| 0x00697A30 | MsgWindow_Open | thiscall(msgWin, params*), ret 4 | pushes "MsgWindow" (kind 0). Kinds 1/2 ("MsgWindowForInvite"/"ForRank") use the override instead |
| 0x00695510 | MsgWindow_Close | fastcall(msgWin), ret | index-- unless kind is 1/2 |
| 0x00698220 / 0x0068A7E0 | MsgWindow_Set/ClearOverrideFocus | fastcall, ret | set / clear the override |
| 0x006973D0 | MsgWindow_HasFocus | fastcall, ret | current name == this window's name |
| 0x006997F0 | SysOverlay_SetHidePopup | cdecl(on, ctrlId), ret | pushes / pops "WindowHide" |
| 0x0068A860 | UI reset | | index = 0, flag = 0, lock `+0x30 = 0` |

How the gate works:

- `Input_*ForId(id, action, focusName)` answers only if lock `[0x01500374]+0x30 == 0` and
  `strcmp(focusName, current) == 0`. Most callers pass `""`. While a popup owns the focus, every
  `""` query returns 0. `Input_ActionJustPressedForId_NoFocus` (0x004967A0) skips only the name
  check.
- The lock field has no nonzero writer (only the getter's init and 0x0068A860 write it, both
  0). *Hypothesis*: it is always 0 in this build.
- Direct wrapper/KeyControler calls, BattleInputWrite, the online sampler and the raw readers are
  not gated.
- **Substituting `+0x30` is compatible with the gate.** Edges still land on the controller,
  and an open popup only blocks the queries that pass `""`.

### A6. Readers that bypass the controller word

**Gameplay and menus never read a device directly.** Battle input goes through PackBattleWord on
`+0x30`, and menus go through the wrappers. The direct readers are all keyboard (plus pads on the
key-config pages) and none of them is gameplay:

| Reader | Device / keys | Effect | Survives a `+0x30` substitution? |
|---|---|---|---|
| 0x005F9A40 Title_PressStartTick | primary keyboard map, raw: **Space** (game key 0x41) edge, **Esc** (0x25) edge | Space claims the id and starts; Esc (or pad bit 27) takes the quit path (*hypothesis*: quit). The raw tests run on every pass, not only id 0 (corrects the L1 note), but id 0 is tested first and is always connected, so the keyboard claims id 0 | **yes**: Space/Esc still start/quit |
| 0x004D1F70 RawKeyMaskJustPressed | cdecl(mask), ret; mask -> game key via table 0x00EC2CB8 (built by 0x004D1BF0), just-pressed on the primary keyboard | every caller passes 0x10 = **Enter**, to skip caution/movie screens (0x006F6C40, 0x00707850, 0x007081F0); fallback is the wrapper bit 12 for the training-side id | **yes**: Enter still skips |
| 0x0050CB00 Keyboard_AnyModifierHeld | no args, ret; primary keyboard Shift/Ctrl/Alt | suppresses title and pause presses while a modifier is held (Alt+Enter guard): 0x005F9450, 0x0050E3E0, battle pause 0x00699C50, 0x007116FD, 0x00759980 | yes (it only suppresses) |
| 0x00699040 tail, via 0x00460990 Keyboard_KeyJustPressed (thiscall(kbd, gameKey), ret 4) | Alt+F4 (0x29), Alt+Enter (0x24) | quit (*hypothesis*), toggle fullscreen (*hypothesis*, app method 0x0043AEA0) | yes |
| 0x00515750 / 0x006A8650 | primary keyboard map scan | key-config capture (rebind) | yes, rebind UI only |
| 0x005153D0 / 0x006A8430 | pad device of `[sysMgr+0x10+4*slot]` via GetDeviceIndexPtr + registry, device IsPressed | key-config capture of pad buttons | yes, rebind UI only |
| 0x004671A0 / 0x00467480 / 0x00417450 | GetKeyState / GetAsyncKeyState on WM_KEYDOWN | DXUT edit box / IME controls | text entry only |
| WndProc WM_CHAR -> TextInput_PushChar 0x00418F70 | WM_CHAR | text-entry ring | text entry only |

Blocking these needs the device layer (the mod's existing GetKeyboardState hook at
`Keyboard_Poll`, or zeroing the keyboard map). For the rewrite, the realistic choices are:

- leave them alone (they are menu conveniences); or
- also zero the keyboard device map when the mod owns the keyboard. That kills Space/Enter/Esc at
  the title and the caution screens. Mod-owned keyboards then need another way to press start,
  which bit 26 provides.

## 1. Boundary

The subsystem is a five-layer stack:

1. **Devices** (`AA_CInput` family, vtable slot 2 = Poll, slot 3 = IsPressed(gameKey),
   slot 4 = IsConnected): `AASTEAM_CInputKeyBoard` (GetKeyboardState),
   `AASTEAM_CInputPad` (DirectInput8), `AASTEAM_CInputXInputPad` (XInput 1.3), and the arcade
   `AAJVS_CInputPad`. They live in one global **device registry** (capacity 5) and are polled
   once per frame by `GAME_InputTask`.
2. **Controller tasks** (`GAME_KeyControler` -> `GAMESTEAM_BattleKeyControler` /
   `GAMESTEAM_SystemKeyControler`): one task per (device, key-config set). Each one maps up to
   28 logical actions to device game keys through a binding table, and builds the held,
   just-pressed and released action words plus a 50-frame history ring.
3. **SystemManager slots**: the six pad-controller pointers and two keyboard-controller pointers
   at `[0x00C929C8]+0x0C..+0x28`.
4. **Wrappers**: two static `GAME_SynthKeyControler` objects (menus, set A) and two
   `GAME_BattleSynthKeyControler` objects (battle, set B). Wrapper 0 = pad slot 0 plus the
   keyboard. Wrapper 1 = pad slot 1 only.
5. **Controller ids and sides**: `engine+0x25F0` (active id), `engine+0x25F4/+0x25F8` (P1/P2
   ids), `engine+0x1688` (local side). The id -> wrapper getters, the named query API
   (`Input_*ForId`, gated by input lock and input focus), and every function that writes the ids.

Also inside: key-config application (bbsave.dat profiles -> binding tables) and the boot
wiring in WinMain.

**Where I stopped, and why:**

- *Consumers.* The ~250 callers of `Input_ActionJustPressedForId` / `Input_ActionRepeatForId`
  / the wrapper checks are menus, training, replay and battle logic. The boundary ends at the
  query API. On the battle side it ends at `BattleInputWrite` (FUN_00559FE0) and the online
  sampler (FUN_004E6000), which turn a side's controller id into the packed battle word. What
  happens after that (side routing for CPU/replay/dummy, GGPO) belongs to Battle and Netplay.
- *Key-config menus* (`CKeyConfigMenu_Keyboard`, `MenuKeyConfigMenu`, FUN_006A77D0..,
  FUN_005147D0..) are UI that edits the option blob. Only the apply functions are in scope.
- *Option/save blob* (`GetOptionData` / `GetOptionManager`, bbsave.dat) is the Settings
  subsystem. Its keyconfig offsets are in section 3.
- *Task framework* (FUN_00407110 spawn-by-priority, AA_TaskNode, list helpers
  FUN_00405B20/00405CB0) is the engine core.
- *Text entry* (`GAME_VirtualKeyboard`/`GAMESTEAM_VirtualKeyboard`, FUN_004744D0..; the
  WM_CHAR ring FUN_00418F70) uses the query API but is not game input.
- *JVS I/O library* (FUN_0044DA80..0044DE20, 0x813500..) is arcade hardware. The JVS classes
  are listed because they share the slots and gate pad creation at boot.
- Shared stubs fill many vtable slots and are not listed. All checked in asm:
  - `FUN_006A9DA0`: `xor eax,eax; ret`.
  - `FUN_006A9DB0`: `ret`.
  - `FUN_00516A20`: `xor eax,eax; ret 4`.
  - `FUN_00429DC0`: `ret 4`.
- *Added in the L2 pass, at the edge:* the UI/system-overlay manager (0x01500374: input lock,
  focus-name stack, lost-controller pause), the app-active flag, and the raw keyboard readers
  (title, caution skip, Alt+F4/Alt+Enter, modifier guard). They are listed because they gate
  or bypass the query API, which a rewrite must know. Their bodies (pause menus, MsgWindow
  drawing, fullscreen switching) are out of scope.

## 2. Functions

Counts: 195 functions (169 from the L1 pass, plus 26 added in the L2 pass: the focus stack, the UI manager and lost-controller pause, the in-use test, the engine ctor, the app-active flag and the raw keyboard readers). L2 114, L1 81, L0 0.

### Boot wiring (AASTEAM_SystemManager input slots, vtable 0x0089DBC4, singleton [0x00C929C8])

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x00482C00 | WinMain_InitSystems | 1228 | L1 | Boot sequence. Calls sysMgr vtbl+0x14 InitInput, +0x18 InitSystemKey, +0x20 InitInputTask, +0x38 InitNetwork, then JVS mgr vtbl+0x1C and, only if that returns 0, sysMgr vtbl+0x1C InitBattleKey (pads). Error strings name each step. Only caller of the device creators. |
| 0x00471A80 | SystemManager_ctor | 113 | L1 | new(0xA04) -> [0x00C929C8]. +0x30 = 0, +0x34 = -1 (keyboard device index, filled by InitSystemKey). General system manager, input is a few slots of it. |
| 0x00471B90 | SystemManager_dtor_body | 296 | L1 | Clears the device registry (FUN_00409660) on shutdown, CoUninitialize when +0x30. |
| 0x00472550 | SysMgr_InitInput | 44 | L2 | thiscall(sysMgr), ret. Registry reset to capacity 5 (deletes every device), then DInputContext_Create(app+0x18 hInstance, app+0x1C hwnd), app=[sysMgr+4]. Returns bool. Old IDirectInput8 is NOT released (overwritten) - leaks one per mod redetect. The mod calls this as _call_DirectInput8Create. |
| 0x00473EF0 | SysMgr_InitSystemKey | 197 | L2 | thiscall(sysMgr), ret; returns 1, or 0 when the registry is full. Registry Add with the keyboard factory (vtbl 0x0089DBA8, kind 1, so it also becomes registry+0xC primaryKeyboard) -> index. Calls ReturnTrue on the device (no effect). Spawns two SystemKeyControler tasks (factory vtbl 0x0089DBBC) at prio 0x01002000 through FUN_00407110(app+4, &factory, prio), each KeyControler_Init(&index, 0x1C) and +0x3C = 1. sysMgr+0x0C = first (set A), +0x34 = index, +0x18 = second (set B). |
| 0x004722C0 | SysMgr_InitBattleKey | 472 | L2 | thiscall(sysMgr), ret, returns 1. Creates exactly 2 pad slots: XInput first (XInputGetState on user index 0 and 1 only), DInput placeholders for the rest. Per slot n: device -> two GAMESTEAM_BattleKeyControler tasks (prio 0x01003000+n), first stored at sysMgr+0x10+4n and +0x1C+4n, second at +0x24+4n; ctrl+0x54 = n. Bugs: XInput device gets user index n (slot), not the index it was found at; DInput slot n = n-th attached game controller. |
| 0x00472580 | SysMgr_InitInputTask | 40 | L2 | thiscall(sysMgr), ret, returns 1. FUN_00407110(app+4, &{vtbl 0x0089DBA0}, 0x01001000): GAME_InputTask runs before every controller task. |
| 0x00474360 | GAME_InputTaskFactory_Create | 143 | L2 | InputTask factory vtbl+0: new(0x18), task base ctor FUN_00405960, vtbl 0x0084EC04 then 0x0089DB94. Returns 0 on alloc failure. |
| 0x004829C0 | GAME_InputTask_Update | 15 | L2 | GAME_InputTask vtbl+4. cdecl, no args (ignores ECX), returns 0. InputDeviceRegistry_PollAll(registry). No focus gate. |
| 0x004D1550 | GAME_InputTask_scalar_dtor | 88 | L1 | vtbl+0. |

### Device registry and DirectInput context

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x00409500 | InputDeviceRegistry_Get | 138 | L2 | cdecl, no args, returns 0x00A1291C. Lazy static (guard 0x00A1292C bit 0): {AA_CInput** devices, capacity, count, primaryKeyboard} zeroed, atexit. |
| 0x00409590 | InputDeviceRegistry_Reset | 86 | L2 | thiscall(reg, capacity), ret 4, returns 1. Clear, then malloc(capacity*4) zero-filled; +4 = capacity, +8 = 0. Called with 5. |
| 0x00409660 | InputDeviceRegistry_Clear | 93 | L2 | thiscall(reg), ret, returns 1. Deletes devices count-1..0 via vtbl+0(1) (null-checked), frees the array, zeroes +0/+4/+8/+0xC. |
| 0x004095F0 | InputDeviceRegistry_Add | 103 | L2 | thiscall(reg, factory*, int* outIndex), ret 8. Returns 0 if either pointer is null or count >= capacity. device = factory vtbl+0(); devices[count] = device (no null check); *outIndex = count; if factory vtbl+4() == 1 (keyboard kind) and +0xC == 0 -> +0xC = device; count++; returns 1. Kinds: keyboard 1, DInput pad 0 (0x006A9DA0), XInput 3 (0x00538FF0). |
| 0x004094E0 | InputDeviceRegistry_GetByIndex | 28 | L2 | thiscall(reg, uint* index), ret 4. Returns devices[*index] or 0 when *index >= count. |
| 0x004096C0 | InputDeviceRegistry_PollAll | 37 | L2 | thiscall(reg), ret, returns 1. vtbl+8 (Poll) on every device, highest index first. No null check, no focus gate. |
| 0x00418FC0 | DInputContext_Get | 108 | L1 | cdecl, returns 0x00A255B4 {hInstance, hwnd, IDirectInput8W*}. |
| 0x00419030 | DInputContext_Create | 49 | L2 | thiscall(ctx, hInstance, hwnd), ret 8. ctx+0 = hInstance, +4 = hwnd, DirectInput8Create(hInstance, 0x800, IID_IDirectInput8W @0x997FC4, &ctx+8, 0). Returns hr >= 0. |

### AA_CInput devices (keyboard, DirectInput pad, XInput pad)

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x00419370 | AA_CInput_scalar_dtor | 34 | L1 | Base class vtbl 0x0084FC64 slot 0. |
| 0x004192B0 | AA_CInput_dtor_unwind_a | 7 | L1 | SEH unwind funclet target, resets vtable to AA_CInput. |
| 0x004192C0 | AA_CInput_dtor_unwind_b | 66 | L1 | Same, second unwind path. |
| 0x0041DB90 | ReturnTrue | 6 | L2 | `mov eax,1; ret`, no stack args. As device vtbl+0x10 it is IsConnected = always 1 (keyboard, DInput pads, base classes); as factory vtbl+4 it is the keyboard kind. |
| 0x004197B0 | InputDevice_QueryIs6 | 16 | L2 | thiscall(dev, n), ret 4: returns n == 6. Keyboard vtbl+0x14 and +0x18, DInput vtbl+0x14. No caller found; purpose unknown. |
| 0x004199E0 | DInputPad_Query18 | 21 | L2 | thiscall(dev, n), ret 4: returns n == 6 ? 2 : 0. DInput vtbl+0x18. No caller found. |
| 0x00419430 | KeyboardFactory_Create | 115 | L2 | Keyboard factory vtbl+0: new(0x128) + Keyboard_ctor; 0 on alloc failure. |
| 0x00419070 | Keyboard_ctor | 570 | L2 | thiscall(kbd), ret, returns kbd. vtbl 0x0084FC64 -> 0x0084FC84 -> 0x0084FCA8; +0x08..+0x27 (both key maps) = 0, +0x28..+0x127 (GetKeyboardState buffer) = 0, +4 kind = 0. Refills the global 256-byte VK -> game-key table at 0x00A255C8 (0x6A = unmapped). Full table in section 3. |
| 0x004193A0 | KeyboardBase_scalar_dtor | 89 | L1 | AA_CInputKeyboardBase vtbl slot 0. |
| 0x00419310 | Keyboard_scalar_dtor | 95 | L1 | vtbl slot 0. |
| 0x004194B0 | Keyboard_Poll | 45 | L2 | vtbl+8, fastcall(kbd), returns 1. Only when [FUN_0043BAF0()+0xD0] != 0 (app object 0x00A5A500; window active, hypothesis): GetKeyboardState(kbd+0x28) via IAT 0x0084A49C, then vtbl+0x1C. The mod's DenyKeyboardInputFromGame hook replaces this GetKeyboardState call. |
| 0x004194E0 | Keyboard_TranslateVkState | 117 | L2 | vtbl+0x1C (GhidraDefs name ADDR_Keyboard_Poll). Copies +0x08..+0x17 (128-bit game-key map) to +0x18..+0x27, clears it, sets bit table[vk] for each VK byte with 0x80 in +0x28. Does not call GetKeyboardState itself. |
| 0x00419400 | Keyboard_IsPressed | 39 | L2 | vtbl+0xC, thiscall(kbd, gameKey), ret 4: bit gameKey of the current map at +0x08, as 0/1. |
| 0x00419B70 | DInputPadFactory_Create | 115 | L2 | DInput pad factory vtbl+0: new(0x294) + DInputPad_ctor; 0 on alloc failure. |
| 0x00419560 | DInputPad_ctor | 155 | L2 | thiscall(pad), ret, returns pad. vtbl 0x0084FC64 -> 0x0084FCCC; +8 = 0, +0x268..+0x27C = 0, +4 kind = 1. |
| 0x00419600 | DInputPad_scalar_dtor | 131 | L1 | Unacquire + Release of +8, then base. |
| 0x00419BF0 | DInputPad_ReleaseDevice | 34 | L2 | vtbl+4, thiscall(pad), ret. If +8: Unacquire (vtbl+0x20), Release (vtbl+8), +8 = 0. |
| 0x00419A00 | DInputPad_Open | 240 | L2 | thiscall(pad, enumIndex), ret 4; returns 1 on full success, else 0 (ignored by the creator). ctx = DInputContext_Get; +0x260 = enumIndex, +0x264 = 0; EnumDevices(4 = DI8DEVCLASS_GAMECTRL, DInputPad_EnumCallback, pad, 1 = ATTACHEDONLY). If +8 was created: SetDataFormat(c_dfDIJoystick @0x00997FAC), SetCooperativeLevel(ctx hwnd, **9 = DISCL_EXCLUSIVE \| DISCL_BACKGROUND**; the L1 note said non-exclusive), EnumObjects(DInputPad_EnumAxesCallback, device, 3 = DIDFT_AXIS), SetProperty(DIPROP_AXISMODE, DIPH_DEVICE, ABS), Acquire. |
| 0x00419890 | DInputPad_EnumCallback | 236 | L1 | stdcall(DIDEVICEINSTANCEW*, pad). Skips (counting) until +0x264 == +0x260; at the target position an XInput device (IsXInputDevice_WMI) is skipped WITHOUT counting; the first non-XInput device there is created (CreateDevice), caps -> +0xC, guidInstance -> +0x38, guidProduct -> +0x48, product name -> +0x58, button layout -> +0x280. Verified from asm. |
| 0x0041A0A0 | IsXInputDevice_WMI | 795 | L1 | cdecl(guidProduct*) -> bool. Microsoft's WMI sample: CoCreateInstance(WbemLocator), Win32_PNPEntity DeviceIDs containing IG_, VID/PID match. Runs twice per created DInput device (once more for the layout pick); slow. |
| 0x00419AF0 | DInputPad_SetXInputLayout | 51 | L2 | thiscall(pad), ret: writes the XInput-style layout bytes at +0x280..+0x293. Unreachable, because the enum callback already rejects XInput devices. |
| 0x00419B30 | DInputPad_SetGenericLayout | 51 | L1 | Default map: buttons 0-11 at +0x284.., dpad buttons none, axes X/Y at +0x290. |
| 0x00419810 | DInputPad_EnumAxesCallback | 120 | L1 | DIPROP_RANGE -1000..1000 on every axis. |
| 0x00419C20 | DInputPad_Poll | 1149 | L1 | vtbl+8. Poll(); on failure Acquire() and skip the frame. GetDeviceState(80); hat -> dir bits, 16 buttons, 6 axes with +-600 threshold, two sticks through StickToDirectionBits. Out: +0x268 buttons (game keys 0-31), +0x26C directions (game keys 32+), +0x270..+0x27C axes. No reconnect: a lost device just retries Acquire forever. |
| 0x00419980 | DInputPad_SelectAxis | 70 | L1 | Returns DIJOYSTATE axis 0-5 by selector byte. |
| 0x00419690 | StickToDirectionBits | 279 | L1 | cdecl(x, y, up, left, right, down masks). Deadzone radius 0.6, 8 sectors (tan 22.5 = 0.3826). Shared by DInput and XInput. |
| 0x004197C0 | DInputPad_IsPressed | 74 | L2 | vtbl+0xC, thiscall(pad, gameKey), ret 4: key < 32 -> +0x268 bit, else +0x26C bit (key-32); 0/1. |
| 0x0041A550 | XInputPadFactory_Create | 112 | L2 | XInput factory vtbl+0: new(0x3C) + XInputPad_ctor; 0 on alloc failure. |
| 0x00538FF0 | Return3 | 6 | L2 | `mov eax,3; ret`. As the XInput factory vtbl+4 it is kind 3. |
| 0x0041A3D0 | XInputPad_ctor | 152 | L2 | thiscall(pad), ret, returns pad. vtbl 0x0084FC64 -> 0x0084FDB4; zeroes +0x08 (0x14 bytes) and +0x1C (0xC bytes), +0x28..+0x38 = 0, +4 kind = 2. |
| 0x0041A470 | XInputPad_scalar_dtor | 95 | L1 | vtbl slot 0. |
| 0x0041A520 | XInputPad_SetUserIndex | 32 | L2 | thiscall(pad, userIndex), ret 4, returns 1: +0x28 = userIndex, +0x2C = +0x30 = 0. The creator passes the slot number, not the index it probed. |
| 0x0041A5C0 | XInputPad_Poll | 524 | L2 | vtbl+8, thiscall(pad), ret; returns 0 when disconnected, else 1. XInputGetState(+0x28). +0x2C: bit0 connected now, bit1 just lost, bit2 just connected (then XInputGetCapabilities(+0x28, 1, +8), +0x2C = 5, +0x30 = 0). Disconnected: +0x30/+0x34/+0x38 = 0. Connected: XINPUT_GAMEPAD -> +0x1C..+0x27. +0x30 = wButtons, \| 0x1000000 if LT > 30, \| 0x2000000 if RT > 30. +0x34 = d-pad and left stick (up 1, right 2, down 4, left 8) \| right stick (0x10/0x20/0x40/0x80), sticks through StickToDirectionBits. +0x38 = game keys A 0, B 1, X 2, Y 3, LB 4, RB 5, LT 6, RT 7, Start 8, Back 9, LS 10, RS 11. Reconnect at the SAME user index works. No focus gate. |
| 0x0041A4D0 | XInputPad_IsPressed | 68 | L2 | vtbl+0xC, thiscall(pad, gameKey), ret 4: key < 32 -> +0x38 bit, else +0x34 bit (key-32); 0/1. |
| 0x0041A540 | XInputPad_IsConnected | 7 | L2 | vtbl+0x10, thiscall(pad), ret: +0x2C & 1. The only device class with a real connected state. |

### Controller tasks (GAME_KeyControler, GAMESTEAM_Battle/SystemKeyControler)

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x004698A0 | BattleKeyControlerFactory_Create | 133 | L2 | Factory vtbl+0 (0x0089DBB4): new(0x58), KeyControler_ctor, vtbl 0x0089C61C; 0 on alloc failure. |
| 0x00469930 | SystemKeyControlerFactory_Create | 133 | L2 | Factory vtbl+0 (0x0089DBBC): new(0x58), KeyControler_ctor, vtbl 0x0089C658; 0 on alloc failure. |
| 0x004960C0 | KeyControler_ctor | 186 | L2 | thiscall(ctrl), ret, returns ctrl. Task base ctor FUN_00405960, vtbl 0x0084EC04 -> 0x008A91D4; +0x18 = 0; history list at +0x1C initialised (FUN_004059A0); +0x28, +0x2C, +0x34, +0x38, +0x3C, +0x40, +0x44, +0x48, +0x50 = 0; +0x54 = -1. Leaves +0x30 and +0x4C uninitialised (+0x30 is rewritten every tick; +0x4C is read only while +0x50 != 0). |
| 0x00496180 | KeyControler_dtor_body | 156 | L2 | thiscall(ctrl), ret. vtbl = 0x008A91D4; pops every history node off the +0x1C list (FUN_00405E00) and deletes it via vtbl+0(1); frees +0x34 and zeroes it; list dtor FUN_004059E0; task base dtor FUN_004059C0. |
| 0x00496220 | GAME_KeyControler_scalar_dtor | 33 | L2 | Base vtbl slot 0. thiscall(ctrl, flags), ret 4: dtor body, then operator delete if flags & 1. |
| 0x004D1260 | GAMESTEAM_KeyControler_scalar_dtor | 88 | L2 | Slot 0 of both GAMESTEAM controller vtables (and JVS). Same as 0x00496220 inside an SEH frame. |
| 0x00496990 | KeyControler_Init | 244 | L2 | thiscall(ctrl, int* deviceIndex, count), ret 8, returns 1. +0x18 = *deviceIndex (a registry INDEX); +0x38 = count; +0x34 = malloc(count*4) filled with -1; 50 (0x32) history nodes new(0x14) (vtbl 0x008A91CC, +0x0C = +0x10 = 0) appended to the +0x1C list (FUN_00405B20); then vtbl+0xC SetDefaults. |
| 0x004699C0 | BattleKeyControler_SetDefaults | 228 | L2 | vtbl+0xC, thiscall(ctrl), ret, returns 1. SetBinding(action, gameKey): 0-3 -> 0x20-0x23 (up/right/down/left), 12-19 -> buttons 0-7, 20 -> 10, 21 -> 11, 22-25 -> -1, 26 -> 8 (Start), 27 -> 9 (Back). 4-11 stay -1 from Init. See A1. |
| 0x00469AB0 | SystemKeyControler_SetDefaults | 228 | L2 | vtbl+0xC, thiscall(ctrl), ret, returns 1. Keyboard game keys: 0 W, 1 D, 2 S, 3 A, 12 J, 13 O, 14 U, 15 I, 16 K, 17 L, 18 P, 19 `=`, 20 N, 21 M, 22-25 -1, 26 Enter, 27 Backspace. |
| 0x00496600 | KeyControler_Update | 17 | L2 | vtbl+4 (task tick), thiscall(ctrl), ret: +0x48 != 0 -> return 1 without ticking; else tail-jump to KeyControler_Tick. +0x48 is only ever written 0 (ctor), so controllers never freeze. |
| 0x00496AF0 | KeyControler_Tick | 84 | L2 | thiscall(ctrl), ret, returns 1. Clears +0x28/+0x2C; ebx = BuildActionWords(); if any edge, advance the history ring (FUN_00405CB0 on +0x1C) and zero the new node's +0x10; node+0x10++; node+0x0C = ebx; +0x44-- if > 0. Exact order in A2. |
| 0x004963F0 | InputDevice_BuildActionWords | 236 | L2 | thiscall(ctrl) (ECX only), ret, returns the effective held word. +0x30 = 0, then OR of `1<<i` per bound-and-down action; effective = +0x50 ? (+0x4C & bit) : physical bit; SOCD cancel (bits 0+2, 1+3) on both; +0x28/+0x2C edges against the previous node+0x0C; +0x50 = 0. The SystemInputWrite hook is at 0x00496408, where EBX is always 0 (A2). Name is historical: the words live in the controller task. |
| 0x00469700 | BattleKeyControler_IsBindingDown | 74 | L2 | vtbl+8, thiscall(ctrl, action), ret 4. binding = vtbl+0x10(action); -1 -> 0; device = registry[*vtbl+0x14()]; device vtbl+0xC(binding). No null check on the device. |
| 0x00469750 | SystemKeyControler_IsBindingDown | 195 | L2 | vtbl+8, thiscall(ctrl, action), ret 4 (/GS cookie). Up to three game keys: the binding (vtbl+0x10) plus hardcoded extras through the jump table at 0x00469814: action 0 -> Up arrow (0x5E) and Space (0x41), 1 -> Right arrow (0x5F), 2 -> Down (0x60), 3 -> Left (0x61). Tests them in order on registry[*vtbl+0x14()] via device vtbl+0xC; stops at the first -1 or hit. Both keyboard controllers (sets A and B) have it. |
| 0x00469850 | KeyControler_GetBinding | 16 | L2 | vtbl+0x10, thiscall(ctrl, action), ret 4: [+0x34][action]. No bounds check. |
| 0x00469840 | KeyControler_GetDeviceIndexPtr | 4 | L2 | vtbl+0x14, thiscall(ctrl), ret: returns &ctrl+0x18. |
| 0x00496AD0 | KeyControler_SetBinding | 23 | L2 | thiscall(ctrl, action, gameKey), ret 8: if +0x34 != 0, [+0x34][action] = gameKey. No bounds check. |
| 0x00469830 | KeyControler_Stub0 | 5 | L2 | vtbl+0x1C/+0x20: `xor eax,eax; ret 10h` (4 stack args). |
| 0x00469860 | KeyControler_StubZero2 | 25 | L2 | vtbl+0x28/+0x2C: thiscall(ctrl, int* a, int* b), ret 8: *a = *b = 0. |
| 0x00469880 | KeyControler_StubZero2Ret0 | 27 | L2 | vtbl+0x34: as 0x00469860, returns 0. |
| 0x004963A0 | KeyControler_MaskJustPressed | 13 | L2 | thiscall(ctrl, mask), ret 4: +0x28 & mask. Raw mask, no table, no FUN_0047EBB0 call. |
| 0x004963B0 | KeyControler_MaskPressedOrRepeat | 55 | L2 | thiscall(ctrl, mask, delay, period), ret 0xC: 1 if +0x28 & mask, or if node+0x0C & mask and node+0x10 > delay and (node+0x10 - delay) % period == 0. |
| 0x004964E0 | KeyControler_ActionHeld | 53 | L2 | thiscall(ctrl, actionId), ret 4: maskTable[id] & node+0x0C (effective held). First calls FUN_0047EBB0(engine, +0x3C ? 0 : +0x54) and discards the result (A4). |
| 0x00496520 | KeyControler_ActionJustPressed | 50 | L2 | thiscall(ctrl, actionId), ret 4: maskTable[id] & +0x28. Same discarded FUN_0047EBB0 call. |
| 0x00496560 | KeyControler_ActionPressedOrRepeat | 83 | L2 | thiscall(ctrl, actionId), ret 4: 1 if maskTable[id] & +0x28, or if held (node+0x0C) with node+0x10 > 8 and (node+0x10 & 3) == 0. Discarded FUN_0047EBB0 call. Menu cursor repeat; the frame counter restarts on an edge of ANY bit. |
| 0x004965C0 | KeyControler_ActionReleased | 50 | L2 | thiscall(ctrl, actionId), ret 4: maskTable[id] & +0x2C. Discarded FUN_0047EBB0 call. |
| 0x00496250 | KeyControler_AnyAttackHeld | 165 | L2 | thiscall(ctrl), ret: node+0x0C & (mask[11] \| mask[12] \| mask[13] \| mask[14]) = bits 15, 12, 13, 14 (B, D, C, A). Four discarded FUN_0047EBB0 calls. |
| 0x00496300 | KeyControler_AnyAttackJustPressed | 153 | L2 | thiscall(ctrl), ret: the same on +0x28. |
| 0x00496A90 | KeyControler_IsDeviceConnected | 42 | L2 | thiscall(ctrl), ret. registry[*vtbl+0x14()] via 0x004094E0; out of range -> 0; else tail-jump to device vtbl+0x10 (IsConnected). Only caller: ControllerWrapper_IsConnected. |
| 0x004968E0 | InputDevice_PackBattleWord | 172 | L2 | thiscall(ctrl, unused), ret 4, returns the 16-bit battle word. Reads **+0x30** (the physical word, not the history node; corrects the L1 note). Digit: left (bit 3) -> 1/7/4 (down tested before up), right (bit 1) -> 3/9/6, neither -> 2/8/5. 0x10 if bits 14\|22\|24\|25, 0x20 if 15\|22\|23\|24\|25, 0x40 if 13\|23\|24\|25, 0x80 if 12\|25, 0x100 if bit 17, 0x200 if bit 19. |
| 0x00496AC0 | ActionIdToMask | 15 | L2 | cdecl(id), ret: [0x009DE0B0 + id*4]. No bounds check. |
| 0x0047EBB0 | Engine_GetPlayerBlock | 32 | L2 | thiscall(engine, side), ret 4: side <= 3 ? engine+0x118+side*0x710 : 0. Pure. Called (result discarded) by every KeyControler action query. |

### Per-player wrappers (GAME_SynthKeyControler / GAME_BattleSynthKeyControler)

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x00464410 | GetMenuWrapper | 175 | L2 | cdecl(id), ret. Lazy statics (guard 0x00A5C66C bits 0/1): {vtbl 0x0089B13C, 0} at 0x00A5C664 and {vtbl 0x0089B13C, 1} at 0x00A5C670. id == 0 -> wrapper 0, ANY other value -> wrapper 1 (setne). |
| 0x00464270 | GetBattleWrapper | 175 | L2 | cdecl(id), ret. Same pattern (guard 0x00A5C680): {vtbl 0x0089B148, 0} at 0x00A5C678 and {.., 1} at 0x00A5C684. |
| 0x00464330 | MenuWrapper_GetPad | 13 | L2 | vtbl+0, thiscall(wrapper), ret: [[0x00C929C8]+0x10+4*(wrapper+4)] (pad set A). |
| 0x004644D0 | MenuWrapper_GetKeyboard | 9 | L2 | vtbl+4: [[0x00C929C8]+0x0C] (keyboard set A). |
| 0x00464320 | BattleWrapper_GetPad | 15 | L2 | vtbl+0: SysMgr_GetBattlePadController(wrapper+4) (pad set B). |
| 0x004644C0 | BattleWrapper_GetKeyboard | 9 | L2 | vtbl+4: [[0x00C929C8]+0x18] (keyboard set B). |
| 0x004C1380 | SysMgr_GetBattlePadController | 14 | L2 | thiscall(sysMgr, slot), ret 4: [sysMgr+0x24+4*slot]. Also AA_SystemManager vtbl slot 13. |
| 0x004641B0 | ControllerWrapper_ActionJustPressed | 54 | L2 | thiscall(wrapper, actionId), ret 4. Pad result, ORed with the keyboard's only when wrapper+4 == 0. So the keyboard always belongs to controller id 0. |
| 0x00464170 | ControllerWrapper_ActionRepeatPressed | 54 | L2 | thiscall(wrapper, actionId), ret 4. **Held** (KeyControler_ActionHeld) on the pad, ORed with the keyboard's when wrapper+4 == 0. The GhidraDefs name 'ActionRepeatPressed' is wrong. |
| 0x004641F0 | ControllerWrapper_ActionPressedOrRepeat | 54 | L2 | thiscall(wrapper, actionId), ret 4. The real menu-repeat check (KeyControler_ActionPressedOrRepeat), keyboard ORed in for wrapper 0. |
| 0x00464230 | ControllerWrapper_ActionReleased | 54 | L2 | thiscall(wrapper, actionId), ret 4. Released (+0x2C), keyboard ORed in for wrapper 0. |
| 0x004640E0 | ControllerWrapper_MaskJustPressed | 54 | L2 | thiscall(wrapper, mask), ret 4. Raw mask on +0x28, keyboard ORed in for wrapper 0. |
| 0x00464120 | ControllerWrapper_MaskPressedOrRepeat | 66 | L2 | thiscall(wrapper, mask, delay, period), ret 0xC. KeyControler_MaskPressedOrRepeat on both. |
| 0x00464080 | ControllerWrapper_AnyAttackHeld | 42 | L2 | thiscall(wrapper), ret. KeyControler_AnyAttackHeld on both. |
| 0x004640B0 | ControllerWrapper_AnyAttackJustPressed | 42 | L2 | thiscall(wrapper), ret. KeyControler_AnyAttackJustPressed on both. 19 callers (menus, name entry, continue). |
| 0x004644E0 | ControllerWrapper_IsConnected | 42 | L2 | thiscall(wrapper), ret. KeyControler_IsDeviceConnected of the pad, ORed with the keyboard's only for wrapper 0 (keyboard always 1, so id 0 is always connected). Only caller: 0x0047F480. |
| 0x00464340 | BattleWrapper_PackBattleWord | 196 | L2 | thiscall(wrapper, unused), ret 4, returns a 16-bit word. pad = PackBattleWord(vtbl+0()). Wrapper 1: returns that. Wrapper 0: kbd = PackBattleWord(vtbl+4()); buttons = (pad \| kbd) & 0xFFF0; digits are vector-added around 5 (both up -> pad-3, both down -> pad+3, both left -> pad+1, both right -> pad-1, then + kbd - 5), so opposite directions cancel and 5 is the identity. |
| 0x00464510 | ControllerWrapper_SetVibration | 23 | L2 | thiscall(wrapper, a, b), ret 8: the pad controller's (vtbl+0) +0x40 = a, +0x44 = b. Callers, all on the active id's menu wrapper: online flow 0x004AA820 (4, 25), 0x004AA890/0x004ABA50/0x004ACECF/0x004AE6D0 (0, 0), 0x0076F2B0 (clamp(a, 0..5), b), 0x00777620 (0, 0). *Hypothesis*: vibration request (strength 0-5, frames; option 'SYOP_Vibration'). Dead on Steam: XInputSetState is not imported and nothing reads +0x40. |

### Controller ids, sides and the named query API

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x0047E7B0 | GetControllerWrapperById | 23 | L2 | cdecl(id): unsigned clamp (id > 2, including negatives, -> 0; 2 stays and maps to wrapper 1), tail-jump to GetMenuWrapper. |
| 0x0047E7D0 | GetMenuWrapperForSide | 130 | L2 | cdecl(side), ret. Ensures the engine static (guard 0x00C929C0); id = [0x00C929A4 + side*4] with the unsigned clamp; GetMenuWrapper(id). No side bounds check. |
| 0x0047E9D0 | GetMenuWrapperForTrainingSide | 189 | L2 | cdecl, no args. side = [0x00C91A38] (engine+0x1688); then as GetMenuWrapperForSide. |
| 0x0047EA90 | GetMenuWrapperForActiveId | 125 | L2 | cdecl, no args. id = [0x00C929A0] with the clamp -> GetMenuWrapper. Also reached through the thunk 0x00759F90. |
| 0x0047E5C0 | GetBattleWrapperForSide | 130 | L2 | cdecl(side), ret: GetBattleWrapper([0x00C929A4 + side*4] clamped). Used by BattleInputWrite. |
| 0x0047E650 | GetBattleWrapperForActiveId | 125 | L2 | cdecl, no args: GetBattleWrapper([0x00C929A0] clamped). Used by the online sampler and the replay recorder. |
| 0x0047E9B0 | GetLocalPlayerControllerId | 14 | L2 | thiscall(engine), ret: engine+0x25F4[engine+0x1688]. dumpbin mis-disassembles it; checked from the bytes `8B 81 88 16 00 00 8B 84 81 F4 25 00 00 C3`. |
| 0x0047E9C0 | GetLocalPlayerSide | 7 | L2 | thiscall(engine), ret: engine+0x1688 (same dword as ADDR_TrainingHumanSide). |
| 0x0047EB50 | GetControllerCount | 6 | L2 | No args, `mov eax,2; ret`. Hardcoded. |
| 0x0047F480 | IsControllerIdConnected | 34 | L2 | **stdcall(id), ret 4** (ECX ignored). Unsigned clamp, GetMenuWrapper, ControllerWrapper_IsConnected. Every connection query in the game goes through here (A3). |
| 0x00496620 | Input_MaskJustPressedForId | 113 | L2 | cdecl(id, mask, focusName), ret. Gate: lock [0x01500374]+0x30 != 0 -> 0; strcmp(focusName, current focus) != 0 -> 0. id == -100 (0xFFFFFF9C) -> engine+0x25F0. GetBattleWrapper(id) (no clamp; setne still gives 0/1) -> ControllerWrapper_MaskJustPressed. **Set B.** Callers: training reset 0x0055EFA3 (mask 0x8000000), training fn1/fn2 masks 0x10000/0x40000 (0x006D21F9, 0x006D221B, 0x006DD42E). |
| 0x004966A0 | Input_ActionHeldForId | 113 | L2 | cdecl(id, actionId, focusName), ret. Same gate; GetControllerWrapperById (clamp) -> menu wrapper (set A) -> ControllerWrapper_ActionRepeatPressed (= held). 21 callers (training reset direction etc.). |
| 0x00496720 | Input_ActionJustPressedForId | 113 | L2 | cdecl(id, actionId, focusName), ret. Same gate -> menu wrapper (set A) -> ActionJustPressed. 121 callers; IDs used: 3 (99 sites), 4 (94), 16 (62), 15 (56), 11 (55), 14 (53), 5 (47), 18 (46), 7/8 (45 each), 17 (39), 9/10 (26 each), 6 (20), 2 (5), 12 (4), 1 (1). |
| 0x004967A0 | Input_ActionJustPressedForId_NoFocus | 63 | L2 | cdecl(id, actionId), ret. Lock gate only, no focus check -> menu wrapper -> ActionJustPressed. |
| 0x004967E0 | Input_ActionRepeatForId | 113 | L2 | cdecl(id, actionId, focusName), ret. Same gate -> menu wrapper -> ActionPressedOrRepeat. 90 callers, nearly all IDs 7-10. |
| 0x00496860 | Input_ActionReleasedForId | 113 | L2 | cdecl(id, actionId, focusName), ret. Same gate -> menu wrapper -> ActionReleased. |
| 0x006956F0 | InputLock_Get_thunk | 5 | L2 | `jmp 0x00695700`. |
| 0x00695700 | UiManager_Get | 229 | L2 | cdecl, no args, returns 0x01500374 (lazy static, guard 0x015003B0; zeroes +0x00..+0x34 on first use). Despite the old name this is the UI/system-overlay manager (A3, A5). +0x30 is the lock; nothing writes it nonzero. |
| 0x00731600 | Object_GetField30 | 4 | L2 | Shared getter, thiscall(obj), ret: obj+0x30. Here: the lock flag. |
| 0x00695AD0 | InputFocus_GetCurrentName | 29 | L2 | No args, ret, returns char*: [0x013874BC] != 0 ? 0x01463DDC : 0x01415A28 + [0x013874B8]*0x20. Writers in A5. |
| 0x00697B90 | InputFocus_Push | | L2 | cdecl(const char name[32]), ret. [0x013874B8]++, copies 32 bytes into 0x01415A28 + idx*0x20; logs 'PushUIExclusiveName' past index 8. 20 callers. |
| 0x00697B70 | InputFocus_Pop | | L2 | No args, ret. Returns the old top pointer, [0x013874B8]--. 9 callers. |
| 0x00697DB0 | InputFocus_Remove | | L1 | cdecl(name), ret. Removes a named entry and compacts the stack. Only caller: virtual keyboard 0x004744D0. |
| 0x00697A30 | MsgWindow_Open | | L1 | thiscall(msgWin, params*), ret 4. Saves the current focus at +0x278; kind 0 pushes 'MsgWindow' (name table 0x00A043E4); kinds 1/2 ('MsgWindowForInvite'/'ForRank') use the override instead. |
| 0x00695510 | MsgWindow_Close | | L1 | fastcall(msgWin), ret. If +0x264: clears it; index-- unless kind 1/2. |
| 0x00698220 | MsgWindow_SetOverrideFocus | | L1 | fastcall(msgWin), ret. Copies the kind's name to 0x01463DDC, [0x013874BC] = 1. |
| 0x0068A7E0 | MsgWindow_ClearOverrideFocus | | L1 | fastcall(msgWin), ret. Clears the flag if the current name is this window's. |
| 0x006973D0 | MsgWindow_HasFocus | | L1 | fastcall(msgWin), ret. Current name == this window's name. |
| 0x006997F0 | SysOverlay_SetHidePopup | | L1 | cdecl(on, ctrlId), ret. On change: [0x013874C0] = on, [0x013874C4] = ctrlId; pushes 'WindowHide' when on, index-- when off. |
| 0x0068A860 | UiManager_Reset | | L1 | Focus index = 0, override flag = 0, lock +0x30 = 0. Called from 0x0047F130 at mode start. |
| 0x0047F530 | Engine_IsControllerIdInUse | 154 | L2 | thiscall(engine, id), ret 4. Side = the one whose +0x25F4/+0x25F8 == id. Modes 1/5: side found and engine+0x118+side*0x710 == 1 (human). Mode 6: side == +0x1688, or training menu item == 5 and training task+0x1AC40 == id. Other modes: id == +0x25F0. |
| 0x00699040 | UiManager_Tick | | L1 | fastcall(obj 0x01500374), from 0x0074CCA0. Per id 0..3: +0x00 last connected, +0x10 lost latch (set on connected -> disconnected), +0x20 in-use (change -> 0x0049A400, 0x006A9DB0). Also the 'WindowHide' popup (polls IDs 3/6/4/5 on [0x013874C4]), Alt+F4 / Alt+Enter (A6), and ticks 0x006A8B70, 0x00515D80, 0x006C4880. |
| 0x00697380 | Pause_AnyInUseControllerLost | 67 | L1 | thiscall(obj), ret: 1 if some id 0..3 is in use, disconnected and latched. |
| 0x0047F390 | Pause_ShouldOpen | 214 | L1 | No args, ret. 1 if FUN_004A77E0()+0x270 (*hypothesis*: online session) and 0x00697380(); else menu-layer and MsgWindow checks. Callers: pause menus 0x004DFF20, 0x006D04A0, 0x006D5BA0, and 0x0068A650. |
| 0x00697FC0 | UiManager_ClearLostLatches | | L1 | fastcall(obj): zeroes +0x10..+0x1C. Called when a pause menu opens (0x004DE260, 0x0069ABB0, 0x006CC740, 0x006D4120). |
| 0x0068A650 | Pause_OwnerControllerId | | L1 | Returns which id may drive the pause/system menu: -1 nobody, else the local id, the first in-use id, a P1/P2 id, or the active id (online; modes 4/7/11 with Start). Caller 0x00699C50. |
| 0x005F9450 | Title_AnyPlayerPressed | | L1 | For id < GetControllerCount(): connected and Start (5) or ID 1 (bit 12) after more than 0x1D frames -> 1. Suppressed while Keyboard_AnyModifierHeld. Callers 0x005F9510/9610/98B0/9F90. |

### Assignment writers (who sets engine+0x25F0 / +0x25F4 / +0x25F8)

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x00483370 | Boot_ResetActiveId | 503 | L1 | After InitGameSystem: engine+0x25F0 = 0. |
| 0x004830D0 | Boot_ResetActiveId_Alt | 417 | L1 | Second boot/title path, same write. |
| 0x005F9A40 | Title_PressStartTick | 906 | L1 | Title screen, ids 0..1, pad set-A controllers sysMgr+0x10/+0x14. Quit path: pad MaskJustPressed(0x8000000) (bit 27) or a RAW Esc (game key 0x25) edge on the primary keyboard. Claim path: pad MaskJustPressed(0x4000000) (bit 26), MenuWrapper(id).ActionJustPressed(12), or a RAW Space (0x41) edge -> engine+0x25F0 = id (write at 0x005F9D07). The raw keyboard test runs on both passes (corrects the L1 note), but id 0 is tested first and is always connected, so the keyboard claims id 0. |
| 0x004A6670 | InviteSelectPad_Tick | 1679 | L1 | Online invite: state 7; first connected id 0..1 with Input_ActionJustPressedForId(id, 5 = Start) -> engine+0x25F0 (write at 0x004A6AEC). Skipped (current active id reused) when task[0x37] == 0 && task[0x3B] != 0. |
| 0x006C4990 | MenuTask_SetActiveId | 9120 | L1 | SaveSequence task (static 0x01704BF8), case 0x16: engine+0x25F0 = task+0x20, whose only writer is the ctor 0x006C3A90 (0). *Hypothesis*: effectively writes 0. Ticked from 0x00699040 through 0x006C4880. |
| 0x005505A0 | ModeStart_AssignFromActive | 1857 | L1 | Mode setup (story/arcade variants 4/5/6): side 0 local, P1 = active id, P2 = (active == 0). Also re-applies key config. |
| 0x005F8790 | ModeStart_AssignFromActive_8 | 399 | L1 | Mode 8 variant of the same pattern. |
| 0x0066E5F0 | ModeStart_AssignFromActive_16 | 439 | L1 | Mode 0x10: P1 = active, P2 = !active, local side = arg. |
| 0x0072A3B0 | MainMenu_StartMode | 1555 | L1 | Main menu mode launch (modes 0x11, 0x12, 9, 10...): local side 0, P1 = active, P2 = !active. |
| 0x0072C5C0 | MainMenu_StartMode_8 | 410 | L1 | Mode 8 variant. |
| 0x0072CB60 | MainMenu_StartMode_7 | 413 | L1 | Mode 7 variant. |
| 0x007308C0 | MainMenu_StartMode_4 | 769 | L1 | Mode 4 variant. |
| 0x00712580 | MainMenu_PadAssignTick | 545 | L1 | VS pad-assign screen (CTRL_MMPadAsign). For id 0..1: disconnected -> unassign; Left (9) -> P1 slot task+0x6C00, Right (10) -> P2 slot +0x6C04 (-1 = none); action 3 confirms, 4 cancels. |
| 0x007226A0 | MainMenu_PadAssignIconTick | 773 | L1 | Per-controller icon state on that screen; hides icons of disconnected ids. |
| 0x0072D060 | MainMenu_ApplyPadAssignment | 300 | L1 | Writes the screen's picks: both set -> as picked; one set -> other side gets !id; none -> 0/1. local side = 1 only if just P2 was picked. |
| 0x0047CBC0 | Engine_ctor | | L1 | Lazy static engine init: +0x25F0 = 0, +0x25F4 = 0, +0x25F8 = 1 (0x0047CD91..9D). |
| 0x0047D910 | CharSelectConfigCommit | | L1 | Saves P1/P2 ids into +0x2534/+0x2538 (read back by 0x0047FD20). |
| 0x0049F0F0 | Online_AssignControllerIds | 313 | L1 | Net obj (0x004A0FE0)+0x23218 -> +0x2C: bit 1 set -> local side = bit 0; local side = active id, other side = !active; fallback P1 0 / P2 1. Callers: 0x0049F2A0 (online flow), 0x0054C6E0, 0x0069DE70 (skipped in replay mode 0xB). |
| 0x0047FD20 | Engine_RestoreSavedSetup | 264 | L1 | Copies a saved block, including engine+0x2534/+0x2538 -> +0x25F4/+0x25F8 (rematch/return paths, hypothesis). |
| 0x00486AB0 | Challenger_JoinTick | 893 | L1 | Modes 1/16/5 'new challenger': an id 0..1 other than the local id (+0x25F4[+0x1688]) pressing Start (5) on GetControllerWrapperById(id) -> engine+0x25F4[side] = id (0x00486DC6). |
| 0x006C2960 | Assign_ResetToDefault | 322 | L1 | P1 = 0, P2 = 1 (with side resets). |
| 0x006D2190 | Training_EnemyControllerJoin | 1957 | L1 | Training: for id < GetControllerCount() (2) other than GetLocalPlayerControllerId (not the active id), Start (5) -> task+0x1AC40 = id, TRI_Enemy = 1, and the dummy side's +0x25F4/+0x25F8 = id (writes 0x006D240E..0x006D284E). |
| 0x006CFB10 | Training_SetEnemyMode | 927 | L1 | TRI_Enemy setter; mode 1 copies task+0x1AC40 into the dummy side's controller id. |
| 0x006CFEE0 | Training_SetEnemyMode_Alt | 403 | L1 | Same, second menu path. |

### Key config application

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x00483F10 | ApplyAllKeyConfig | 495 | L2 | thiscall(task), ret, returns 1. Not only key config: a boot/title state step. Sets FUN_004A77E0()+0xE8 = 1, resets the focus stack via 0x00695830(0)->0x00695510, then applies: pad set B slot 0/1 with optData+0x4254/+0x4255, keyboard set B with +0x4254 (the SAME profile byte as pad slot 0), pad set A slot 0/1 with +0x54AA5/+0x54AA6, keyboard set A with +0x54AA5; engine+0x25D8/+0x25DC = the set-A profile bytes. Then sound/option setup and two task spawns (prio 0x08000001, 0x01006001). Sole caller 0x004832B4. |
| 0x004BB6E0 | ApplyPadKeyConfig_SetB | 370 | L2 | thiscall(optMgr, slot, profile byte), ret 8 (ECX only used by the static guard 0x00EBFE18 / 0x00CF85D8 init). Controller = SysMgr_GetBattlePadController(slot). Record = 0x00CFC7E4 + 20*(slot*2 + profile). Byte -> action: +0 14 (A), +1 15 (B), +2 13 (C), +3 12 (D), +4 17 (taunt), +5 19 (special), +6 22 (AB), +7 23 (BC), +8 24 (ABC), +9 25 (ABCD), +0xA 16, +0xB 18, +0xC 27 (reset). Directions are not configurable on pads. Profiles are keyed by SLOT, not by device. |
| 0x004BB860 | ApplyKeyboardKeyConfig_SetB | 430 | L2 | thiscall(optMgr, profile byte), ret 4. Keyboard set B (sysMgr+0x18), record 0x00CFC836 + 20*profile: +0 0 (Up), +1 2 (Down), +2 3 (Left), +3 1 (Right), +4 14, +5 15, +6 13, +7 12, +8 17, +9 19, +0xA 22, +0xB 23, +0xC 24, +0xD 25, +0xE 16, +0xF 18, +0x10 27. Exactly the mod's BattleAction order. |
| 0x004BBA10 | ApplyPadKeyConfig_SetA | 275 | L2 | thiscall(optMgr, slot, profile byte), ret 8. Pad set A ([sysMgr+0x10+4*slot]), record 0x00D4D035 + 20*(slot*2 + profile): +0 14, +1 15, +2 13, +3 12, +4 17, +5 19, +6 16, +7 18. Bits 20, 21, 26 and 27 keep their defaults. |
| 0x004BBB30 | ApplyKeyboardKeyConfig_SetA | 340 | L2 | thiscall(optMgr, profile byte), ret 4. Keyboard set A (sysMgr+0x0C), record 0x00D4D087 + 20*profile: +0 0, +1 2, +2 3, +3 1, +4 14, +5 15, +6 13, +7 12, +8 17, +9 19, +0xA 16, +0xB 18. Exactly the mod's MenuAction order. |
| 0x00612530 | BattleLoad_ApplyPlayerKeyConfig | 1911 | L1 | Before a match, for each side: the pad set B of slot = engine+0x25F4[side] gets that player's profile; keyboard set B only when that id is 0 (checked in asm at 0x0061270C / 0x0061279D); engine+0x25E0[id] = profile. Online: only the local side (FUN_0049A340 == 2 -> side 1). |
| 0x006A8AD0 | KeyConfigMenu_ApplyIfInUse | 151 | L2 | No args, ret. Ticks both key-config editor instances (0x006A7B20/0x006A7A20, 0x006A7BD0/0x006A7AC0). For each editor (id [0x015D3950], profile [0x015D3960]; id [0x015D3B50], profile [0x015D3B60]): if id >= 0 and engine+0x25E0[id] == profile -> ApplyPadKeyConfig_SetB(id, profile), and ApplyKeyboardKeyConfig_SetB(profile) when id == 0. |

### Battle-side consumers (the boundary's end)

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x00559FE0 | BattleInputWrite | 1001 | L1 | Offline per-frame battle input (task 0x56AFF0). Per side: id = engine+0x25F4[side], route by side kind, BattleWrapper_PackBattleWord -> battleScene+0x62D8C[side]. Mod hook site (hooks_battle_input). |
| 0x0055A3E0 | BattleInputWrite_Alt | 897 | L1 | Copy of the above used by FUN_004E6130 (network path). |
| 0x004E6000 | Online_SampleLocalInput | 289 | L2 | thiscall(netObj), no stack args, ret. For side 0..1: if FUN_0049A400()->FUN_0049A340() == side+1 (local): word = BattleWrapper_PackBattleWord(GetBattleWrapperForActiveId()). Default 5. Forced to 5 when FUN_0069A980() != 0. Then 0x00783370 (add local input on 0x00A5BD08) and, on success paths, FUN_004E6130. Online always reads the ACTIVE id. |
| 0x0069DB60 | ReplayRecorder_SampleInput | 680 | L1 | Replay recorder (not a 'mode 5 sampler'). Writes 16-bit words into this+0x8D4 [round][side][frame < 0x3840]. Mode 5: copies battleScene+0x62D8C per side. Online: network words, or the local sample from the ACTIVE-id battle wrapper (same as the online sampler). |

### Arcade JVS input (dead on Steam: JVS init fails, so its pad creator returns 0)

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x004D1460 | AAJVS_SystemManager_ctor | 112 | L1 | new(0x40) -> [0x00C929CC]; vtbl 0x00942C1C. |
| 0x004D14D0 | AAJVS_SystemManager_scalar_dtor | 127 | L1 |  |
| 0x004D15B0 | AAJVS_Init | 168 | L1 | vtbl+4, called at boot. Opens the JVS I/O; on success polls up to 120 x 16 ms and sets +0x30 = 1. |
| 0x004D1660 | AAJVS_CreatePads | 18 | L1 | vtbl+0x1C. Returns 0 when +0x30 == 0 (always on Steam), which lets SysMgr_InitBattleKey run. Body continues in FUN_004D1672 (Ghidra split). |
| 0x004D1672 | AAJVS_CreatePads_body | 394 | L1 | JVS pads + GAMEJVS_BattleKeyControler into sysMgr+0x10/+0x14. |
| 0x0044DFB0 | AAJVS_PadFactory_Create | 112 | L1 |  |
| 0x0044DF00 | AAJVS_Pad_scalar_dtor | 95 | L1 |  |
| 0x0044E020 | AAJVS_Pad_Poll | 238 | L1 | vtbl+8. |
| 0x0044DF60 | AAJVS_Pad_IsPressed | 40 | L1 | vtbl+0xC. |
| 0x004D1300 | GAMEJVS_BattleKeyControlerFactory_Create | 133 | L1 |  |
| 0x004D12C0 | GAMEJVS_BattleKeyControler_IsBindingDown | 64 | L1 | vtbl+8. |
| 0x004D1390 | GAMEJVS_BattleKeyControler_SetDefaults | 206 | L1 | vtbl+0xC. |

### Edges kept for reference

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x0044F350 | WndProc_KeyMessage | 164 | L1 | Game WndProc, thiscall(wndObj, msg, wParam, lParam), ret 0xC. WM_DESTROY PostQuitMessage; WM_ACTIVATE wndObj+0x22C; WM_CLOSE DestroyWindow; WM_ACTIVATEAPP App_OnActivate(0) / App_OnDeactivate (app+0xD0); WM_CHAR TextInput_PushChar. No WM_KEYDOWN handling. Mod hook PassKeyboardInputToGame at 0x0044F359. |
| 0x00418F70 | TextInput_PushChar | 72 | L1 | thiscall(ring, wchar), ret 4. WM_CHAR ring buffer (256 wchar) for text entry; not part of game input. |
| 0x0043BAF0 | App_Get | | L1 | cdecl, no args: lazy static app object 0x00A5A500 (= [sysMgr+4]; hwnd at +0x28). +0xD0 = application active (A5). |
| 0x0043C420 | App_OnDeactivate | | L1 | thiscall(app), ret: SetFocus(NULL) if focused; +0xD0 = 0. From WM_ACTIVATEAPP(0). |
| 0x0043D110 | App_OnActivate | | L1 | thiscall(app, setFocus), ret 4: optional SetFocus(hwnd); +0xD0 = 1. From WM_ACTIVATEAPP(1). |
| 0x0050CB00 | Keyboard_AnyModifierHeld | | L1 | No args, ret. Primary keyboard map: Shift/Ctrl/Alt game keys (0x3A-0x3F, 0x44, 0x45) -> 1. Raw reader (A6). |
| 0x00460990 | Keyboard_KeyJustPressed | | L1 | thiscall(kbd, gameKey), ret 4: cur & ~prev on the key maps. Raw reader (A6). |
| 0x004D1F70 | RawKeyMaskJustPressed | | L1 | cdecl(mask), ret. Mask -> game key via table 0x00EC2CB8 (built by 0x004D1BF0), just pressed on the primary keyboard. All callers pass 0x10 = Enter (skip caution/movie screens). Raw reader (A6). |

## 3. Data

### Globals

| VA | What | Notes |
|---|---|---|
| 0x00C929C8 | `AASTEAM_SystemManager*` | Set in WinMain. Input slots listed below. |
| 0x00C929CC | `AAJVS_SystemManager*` | Arcade I/O. `+0x30` = JVS present (0 on Steam). |
| 0x00A1291C | device registry | `{AA_CInput** devices, capacity, count, AA_CInput* primaryKeyboard}`, guard 0x00A1292C. Capacity 5. In practice always 3 devices: keyboard, pad 0, pad 1 (order depends on who created them). |
| 0x00A255B4 | DirectInput context | `{HINSTANCE, HWND, IDirectInput8W*}`. |
| 0x00A255C8 | VK -> game key table | 256 bytes in BSS, rebuilt by every keyboard ctor. Full mapping below. |
| 0x00A25754.. | DInput hat/axis direction masks | Lazy-filled from bit constants at 0x00A256D4... |
| 0x00A5C664 / 0x00A5C670 | menu wrappers 0 / 1 | `{vtbl 0x0089B13C, int index}`, guard 0x00A5C66C |
| 0x00A5C678 / 0x00A5C684 | battle wrappers 0 / 1 | `{vtbl 0x0089B148, int index}`, guard 0x00A5C680 |
| 0x009DE0B0 | action id -> mask table | 31 dwords (GhidraDefs `ADDR_ActionIdMaskTable`). Full table below. |
| 0x01500374 | UI/system-overlay manager (old name "input lock object") | `+0x00..+0x0C` last connected per id 0..3, `+0x10..+0x1C` lost latch, `+0x20..+0x2C` in-use, `+0x30` lock read by the named queries (never written nonzero), `+0x38` cleared with it. Guard 0x015003B0. |
| 0x01415A28 / 0x01463DDC | input focus names | 32-byte names; current index at 0x013874B8, override flag 0x013874BC. "WindowHide" popup state at 0x013874C0 (on) / 0x013874C4 (controller id). |
| 0x00A5A500 | app object (`App_Get`) | `+0x28` hwnd, `+0xD0` application active (A5), `+0xC8`/`+0xCC` window-mode flags. |
| 0x00CFC7E4 / 0x00CFC836 | keyconfig set B records | pad 20 bytes x (slot*2 + profile) / keyboard 20 bytes x profile. Inside optData 0x00CF85E0. Byte maps in the ApplyKeyConfig rows. |
| 0x00D4D035 / 0x00D4D087 | keyconfig set A records | Same shape, menu set. |
| 0x00EC2CB8 | raw-key mask table | 29 (mask, game key) pairs for `RawKeyMaskJustPressed`, built by 0x004D1BF0. |

**ActionIdMaskTable (0x009DE0B0), read from the exe:**

| IDs | Mask (bit) |
|---|---|
| 0 | 0 |
| 1, 3, 12 | 0x1000 (12) |
| 2, 4, 13 | 0x2000 (13) |
| 5 | 0x4000000 (26) |
| 6, 29, 30 | 0x8000000 (27) |
| 7 / 8 / 9 / 10 | 0x1 (0) / 0x4 (2) / 0x8 (3) / 0x2 (1) |
| 11 | 0x8000 (15) |
| 14 | 0x4000 (14) |
| 15, 27 | 0x10000 (16) |
| 16, 25 | 0x20000 (17) |
| 17, 28 | 0x40000 (18) |
| 18, 26 | 0x80000 (19) |
| 19 / 20 | 0x100000 (20) / 0x200000 (21) |
| 21 / 22 / 23 / 24 | 0x100 (8) / 0x400 (10) / 0x800 (11) / 0x200 (9) |

**Keyboard game keys (VK -> game key, emulated from Keyboard_ctor):**

- **Letters and digits**:
  - A-Z: VK 0x41-0x5A -> 0x00-0x19.
  - Number row 1-9: VK 0x31-0x39 -> 0x1A-0x22; 0 -> 0x23.
  - Numpad 0-9: VK 0x60-0x69 -> 0x54-0x5D.
  - Numpad keys: `*` 0x4F, `+` 0x51, `-` 0x50, `.` 0x53, `/` 0x4E.
- **Function keys**: F1-F12 -> 0x26-0x31.
- **Editing and navigation**:
  - Enter 0x24, Esc 0x25, Space 0x41, Backspace 0x42, Tab 0x38, Caps 0x39.
  - Insert 0x48, Delete 0x49, Home 0x4A, End 0x4B, PgUp 0x4C, PgDn 0x4D.
  - Arrows: Up 0x5E, Right 0x5F, Down 0x60, Left 0x61.
- **Modifiers**: LShift 0x3A, RShift 0x44, LCtrl 0x3B, RCtrl 0x45, LAlt 0x3C, RAlt 0x3D.
- **Other keys**:
  - PrtSc 0x35, LWin 0x46, Apps 0x40.
  - OEM keys: `=` (VK_OEM_PLUS) 0x32, `;` 0x69, VK 0x2F 0x34.
  - JP IME keys: VK 0x15 -> 0x66, 0x19 -> 0x62, 0x1C -> 0x65, 0x1D -> 0x64.
- **Unmapped**: everything else -> 0x6A, a shared "unmapped" bit that no binding uses.

### engine (0x00C903B0, FUN_0047E860)

| Offset | Abs | Meaning |
|---|---|---|
| +0x0108 | 0x00C904B8 | game mode (1 Arcade, 4 Story, 5 Versus, 6 Training, 7 Tutorial, 8 Challenge, 11 Replay theater, 15 Online, 16 Abyss) |
| +0x0118 + side*0x710 | | per-player block (`Engine_GetPlayerBlock` 0x0047EBB0, side <= 3); `+0` = 1 when human |
| +0x1688 | 0x00C91A38 | local player side (= GhidraDefs `ADDR_TrainingHumanSide`) |
| +0x25D8 / +0x25DC | | set-A key-config profile per id (written by ApplyAllKeyConfig) |
| +0x25E0 + id*4 | 0x00C92990 | set-B key-config profile currently applied to controller id |
| +0x25F0 | 0x00C929A0 | **active controller id**: whoever pressed start at the title or picked the invite pad. Writers in A3 |
| +0x25F4 | 0x00C929A4 | **P1 controller id** |
| +0x25F8 | 0x00C929A8 | **P2 controller id** |
| +0x2534/+0x2538 | | saved copy of the P1/P2 ids (0x0047D910), restored by 0x0047FD20 |

Controller ids are 0 or 1 everywhere, except that the UI manager polls 0..3. The id getters use
an UNSIGNED clamp (`cmp eax,2; cmova eax,0`): ids above 2 and negative ids become 0, and id 2
maps to wrapper 1. `GetMenuWrapper`/`GetBattleWrapper` themselves only test `id != 0`.

### AASTEAM_SystemManager input slots

| Offset | Content |
|---|---|
| +0x04 | app object 0x00A5A500 (`+0x18` hInstance, `+0x1C` hwnd as used by InitInput) |
| +0x0C | keyboard SystemKeyControler, set A (menus) |
| +0x10, +0x14 | pad BattleKeyControler slot 0/1, set A (menus) |
| +0x18 | keyboard SystemKeyControler, set B (battle) |
| +0x1C, +0x20 | the same objects as +0x10/+0x14, written by InitBattleKey and **never read** (all 117 references to the sysMgr global were checked) |
| +0x24, +0x28 | pad BattleKeyControler slot 0/1, set B (battle) |
| +0x34 | registry index of the keyboard device |

Who reads these slots without going through a wrapper:

- the virtual keyboard 0x004744D0 (pad set A, KeyControler_ActionJustPressed(3/4));
- the key-config capture pages 0x005153D0 / 0x006A8430 (pad set A -> device);
- the title 0x005F9A40 (pad set A, raw masks);
- 0x004ACECF (keyboard set A, mask 0x20000000, which is dead);
- 0x0073DA40 (keyboard set A present?).

The mod's `SetControllerPosSwap` labels (+0x10 menu, +0x1C unknown, +0x24 char) match this.
Swapping +0x10/+0x14 and +0x24/+0x28 swaps both pad slots between the wrappers. The keyboard
stays on wrapper 0 no matter what.

### Device objects (verified offsets)

Common: `+0x00` vtbl, `+0x04` kind (keyboard 0, DInput pad 1, XInput pad 2).

| Class (vtbl, size) | Fields |
|---|---|
| AASTEAM_CInputKeyBoard (0x0084FCA8, 0x128) | `+0x08..+0x17` current 128-bit game-key map, `+0x18..+0x27` previous, `+0x28` 256-byte GetKeyboardState buffer |
| AASTEAM_CInputPad (0x0084FCCC, 0x294) | `+0x08` IDirectInputDevice8W* (exclusive + background), `+0x0C` DIDEVCAPS, `+0x38` guidInstance, `+0x48` guidProduct, `+0x58` product name (WCHAR[260]), `+0x260` target enum index, `+0x264` enum counter, `+0x268` button bits (game keys 0-31), `+0x26C` direction bits (game keys 32+), `+0x270..+0x27C` axes, `+0x280..+0x283` dpad-as-button map, `+0x284..+0x28F` button map, `+0x290..+0x293` axis selectors |
| AASTEAM_CInputXInputPad (0x0084FDB4, 0x3C) | `+0x08` XINPUT_CAPABILITIES, `+0x1C` XINPUT_GAMEPAD, `+0x28` user index, `+0x2C` flags (1 connected, 2 just lost, 4 just found), `+0x30` raw buttons (+LT/RT bits 24/25), `+0x34` direction bits, `+0x38` mapped buttons (game keys 0-11) |

Device vtable slots, read from the exe:

| Slot | Keyboard | DInput pad | XInput pad |
|---|---|---|---|
| +0x00 | scalar dtor 0x00419310 | 0x00419600 | 0x0041A470 |
| +0x04 | `ret` 0x006A9DB0 | ReleaseDevice 0x00419BF0 | `ret` |
| +0x08 | Poll 0x004194B0 | 0x00419C20 | 0x0041A5C0 |
| +0x0C | IsPressed 0x00419400 | 0x004197C0 | 0x0041A4D0 |
| +0x10 | IsConnected `ReturnTrue` | `ReturnTrue` | 0x0041A540 |
| +0x14 | 0x004197B0 (== 6) | 0x004197B0 | 0x00516A20 (0) |
| +0x18 | 0x004197B0 | 0x004199E0 | 0x00516A20 |
| +0x1C | TranslateVkState 0x004194E0 | | |

Factory kinds (vtbl+4): keyboard 1, DInput 0, XInput 3.

### Controller task (GAME_KeyControler, size 0x58)

| Offset | Meaning |
|---|---|
| +0x00 | vtbl (Battle 0x0089C61C, System 0x0089C658, base 0x008A91D4, JVS 0x00942BDC) |
| +0x0C / +0x10 | task id / priority (task framework) |
| +0x18 | **device registry index** (not a pointer) |
| +0x1C | history ring list (50 nodes of 0x14 bytes, vtbl 0x008A91CC) |
| +0x20 | current history node: `+0x0C` effective held word, `+0x10` frames since the last edge of any bit |
| +0x24 | list count (the dtor pops while it is nonzero) |
| +0x28 | actions just pressed (edge vs. previous effective word) |
| +0x2C | actions released |
| +0x30 | **physical held word** (A1 layout); zeroed and rebuilt every tick; what the battle packer reads |
| +0x34 | `int bindings[+0x38]` (game key per action, -1 unbound) |
| +0x38 | action count (0x1C) |
| +0x3C | 1 for keyboard controllers; only selects the argument of a discarded call (A4) |
| +0x40 / +0x44 | set by ControllerWrapper_SetVibration (0x00464510); +0x44 counts down per tick, +0x40 has no reader. *Hypothesis*: vibration strength / frames, dead on Steam |
| +0x48 | nonzero = tick skipped; only ever written 0 |
| +0x4C / +0x50 | override word / override-active (one tick, cleared by BuildActionWords). No writer besides the ctor and BuildActionWords was found in the boundary |
| +0x54 | pad slot index (-1 for keyboards) |

Controller vtable (all four classes share slots 4-13):

| Slot | Function |
|---|---|
| +0x00 | scalar dtor |
| +0x04 | Update 0x00496600 |
| +0x08 | IsBindingDown (Battle 0x00469700, System 0x00469750, JVS 0x004D12C0; base `_purecall`) |
| +0x0C | SetDefaults |
| +0x10 | GetBinding 0x00469850 |
| +0x14 | GetDeviceIndexPtr 0x00469840 |
| +0x18 | `ret 4` 0x00429DC0 |
| +0x1C / +0x20 | 0x00469830 |
| +0x24 | 0x00516A20 |
| +0x28 / +0x2C | 0x00469860 |
| +0x30 | 0x006A9DA0 |
| +0x34 | 0x00469880 |

### Corrections to existing notes (GhidraDefs.h and the mod)

- The +0x28/+0x2C/+0x30 action words belong to the **controller task**, not to an
  `AA_CInput` device. The "controller pointer" the SystemInputWrite hook sees in ESI is a
  KeyControler.
- `KeyControler+0x18` is a registry **index**, not an `AA_CInput*`.
- `+0x30` is the physical word. The effective held word, which honours the +0x4C/+0x50
  override, is in the history node at `[+0x20]+0x0C`. `ActionHeld` reads the node, but
  **`PackBattleWord` reads `+0x30`** (the L1 pass said the node).
- The GhidraDefs comment "actionCount typically 30" is wrong. It is 0x1C (28) for every
  controller.
- `hooks_system_input.cpp`:
  - "EBX = packed system input word from the game" is wrong. EBX is always 0 at the hook.
  - The stub ORs physical input in whenever the override word is < 0x1C (A2).
- `FUN_00464170` ("ActionRepeatPressed") is a plain **held** check. The repeat check is
  `FUN_004641F0` (just pressed, or held more than 8 frames and on every 4th frame).
- `FUN_004194E0` ("Keyboard_Poll") only translates. The GetKeyboardState call is in
  vtbl+8, `FUN_004194B0`.
- `IsControllerIdConnected` is stdcall (ret 4), not cdecl.
- `DInputPad_Open` opens pads `DISCL_EXCLUSIVE | DISCL_BACKGROUND` (9), not non-exclusive.
- The title's raw keyboard test runs on both id passes, not only id 0.
- `0x01500374` is the UI/system-overlay manager, not just an "input lock".
- `ControllerOverrideManager.cpp`'s `BBCF_PAD_SLOT0_PTR_OFFSET = 0x104FA298` points outside
  the image (base+0x104FA298), so `DebugLogPadSlot0` reads nothing meaningful. DInput device
  pointers live at `pad+0x08` inside registry entries.

### Vtables (RTTI-confirmed, COL -> vtable scan)

| Class | vtable |
|---|---|
| AA_CInput / AA_CInputKeyboardBase | 0x0084FC64 / 0x0084FC84 |
| AASTEAM_CInputKeyBoard / Pad / XInputPad | 0x0084FCA8 / 0x0084FCCC / 0x0084FDB4 |
| AAJVS_CInputPad | 0x00898DB8 |
| AASTEAM_CInputKeyBoardFactory / PadFactory / XInputPadFactory | 0x0089DBA8 / 0x0089DB7C / 0x0089DB88 |
| GAME_KeyControler / GAMESTEAM_Battle / System / GAMEJVS_Battle | 0x008A91D4 / 0x0089C61C / 0x0089C658 / 0x00942BDC |
| GAMESTEAM_Battle/SystemKeyControlerFactory / GAMEJVS | 0x0089DBB4 / 0x0089DBBC / 0x00942C70 |
| GAME_SynthKeyControler / GAME_BattleSynthKeyControler | 0x0089B13C / 0x0089B148 |
| GAME_InputTask / Factory | 0x0089DB94 / 0x0089DBA0 |
| History ring node (GAME_CKeyRingBuffer, *hypothesis*) | 0x008A91CC |
| AASTEAM_SystemManager / AAJVS_SystemManager | 0x0089DBC4 / 0x00942C1C |

## 4. Flows

### Boot (confirmed)

`FUN_00482C00` (WinMain init), in order:

1. `SystemManager_ctor` -> [0x00C929C8]. JVS manager -> [0x00C929CC]. `AAJVS_Init` runs at
   once (fails fast on Steam).
2. **InitInput** `SysMgr_InitInput`: registry reset to 5 slots, `DirectInput8Create` (W).
3. **InitSystemKey** `SysMgr_InitSystemKey`: keyboard device (registry index 0, also the
   registry's primary keyboard); two SystemKeyControler tasks on it (sysMgr+0x0C set A, +0x18
   set B), both at priority 0x01002000 with `+0x3C = 1`. Defaults bound.
4. **InitInputTask**: GAME_InputTask at priority 0x01001000.
5. InitNetwork.
6. **InitBattleKey**: `AAJVS_CreatePads` returns 0, so `SysMgr_InitBattleKey` runs:
   - XInputGetState(0), XInputGetState(1). For each success: an XInput device with **user index
     = number of slots created so far**, and two BattleKeyControlers (set A at +0x10/+0x1C,
     set B at +0x24) at priority 0x01003000 + slot.
   - The remaining slots (up to 2) get DInput pads. `DInputPad_Open(slot)` enumerates attached
     game controllers and takes the first non-XInput device at enumeration position `slot`;
     the positions before it count XInput devices too. Slot objects are created even when no
     device is found.
7. Later, `ApplyAllKeyConfig` (FUN_00483F10) loads the saved profiles into sets A and B, and
   the title resets `engine+0x25F0 = 0`.

Per frame (task priorities, low first):

1. GAME_InputTask polls every device. Only the keyboard is gated, by app+0xD0.
2. SystemKeyControlers tick.
3. BattleKeyControlers tick (BuildActionWords; the mod's SystemInputWrite hook runs here).
4. Scene, menu and battle code query through the wrappers, and BattleInputWrite / the online
   sampler pack `+0x30`.

### Hot-plug (confirmed: there is none)

There is no WM_DEVICECHANGE handling: the WndProc handles only WM_DESTROY, WM_ACTIVATE,
WM_CLOSE, WM_ACTIVATEAPP and WM_CHAR. Nothing calls InitInput/InitSystemKey/InitBattleKey
outside WinMain (functions.tsv shows 0 direct callers; the only calls are the vtable calls in
FUN_00482C00). What survives a re-plug:

- **XInput pad present at boot, re-plugged on the same user index**: works. Poll flags bit 2
  and refreshes capabilities.
- **XInput pad at boot, comes back on another user index**: dead until restart.
- **Any pad plugged after boot**: invisible. Its slot is either an XInput device polling
  another index or a DInput placeholder with no device.
- **DInput pad unplugged and re-plugged**: Poll fails, so Acquire is retried every frame.
  Recovery depends on DirectInput giving the same instance back (often it does not), and the
  device is never re-enumerated.

The mod's `RedetectControllers_Internal` replays steps 2, 6 and 3 (in that order), then
`ReapplySavedKeyConfig`. Side effects found here:

- The old controller tasks are never destroyed. 6 per redetect stay in the task list, tick
  every frame, and also pass through the SystemInputWrite hook.
- One `IDirectInput8W` leaks per call.
- The keyboard ends up at registry index 2 instead of 0. Harmless, because the old tasks'
  indexes stay below the count of 3.

### Claiming a controller (confirmed)

- **Title / press start** `Title_PressStartTick`, ids 0..1, `engine+0x25F0 = id` on any of:
  - Start (bit 26) on the pad's set-A controller;
  - action 12 on the menu wrapper;
  - raw Space.
- **Mode start** (arcade, story, training, tutorial, ...: FUN_005505A0, 0072A3B0,
  0072C5C0, 0072CB60, 007308C0, 005F8790, 0066E5F0): local side 0, P1 = active id,
  P2 = (active == 0 ? 1 : 0).
- **VS pad assign** `MainMenu_PadAssignTick` + `MainMenu_ApplyPadAssignment`: each connected
  id pushes Left (P1) or Right (P2). The result is written to +0x25F4/+0x25F8; an unpicked side
  gets the other id.
- **Challenger** `Challenger_JoinTick`: an id other than the local one presses Start -> that
  side's id.
- **Training** `Training_EnemyControllerJoin`: an id other than the local one presses Start ->
  TRI_Enemy = 1, and the dummy side's id = that id.
- **Online** `InviteSelectPad_Tick` picks the active id; `Online_AssignControllerIds` gives the
  local side the active id. Every frame `Online_SampleLocalInput` reads
  **GetBattleWrapperForActiveId**, so netplay always uses the active id.
- **Before each match** `BattleLoad_ApplyPlayerKeyConfig` loads each player's chosen profile
  into set B of pad slot = their id. The keyboard profile is only applied when the id is 0.

### Why refresh and assignment behave badly (evidence)

Confirmed from code:

1. **Devices are fixed at boot** (see Hot-plug). Nothing ever re-enumerates.
2. **Exactly two pads, ever.** Only XInput user indexes 0 and 1 are probed, and
   `GetControllerCount` returns 2. A pad that Windows put on XInput index 2 or 3 is skipped by
   the XInput loop *and* by the DInput path (IsXInputDevice rejects it), so it is invisible.
3. **XInput user-index bug**: the device polls the slot number, not the index where it was
   found. A lone pad on index 1 gets slot 0 polling index 0 = dead.
4. **DInput slot = enumeration position**, and that count includes XInput devices. With a
   DInput stick enumerated before an XInput pad, slot 1 lands on the XInput pad, which is
   rejected, so slot 1 ends up empty. The order depends on USB enumeration, so it changes
   between boots.
5. **DInput slots are always "connected"** (IsConnected = `ReturnTrue`), even with no device.
   Pad-assign screens and `IsControllerIdConnected` cannot tell an empty DInput slot from a
   real one.
6. **The keyboard is welded to controller id 0**: the wrappers OR it in only for index 0, and
   its key config is applied only when id 0 plays (and uses pad slot 0's profile byte at boot).
   A keyboard player and a pad-0 player always share inputs, and a keyboard cannot be P2 on
   its own.
7. **Key config follows the slot, not the device** (record = slot*2 + profile). If pads swap
   slots between boots, players get each other's binds.
8. **Two fixed ids, one "active" id.** Menus read the active id, battles read the P1/P2 ids,
   netplay reads the active id. Changing a device after the title screen has no path back into
   these ids.
9. **Slow enumeration**: each DInput slot runs a WMI query (CoCreateInstance + full
   Win32_PNPEntity scan) up to twice. That is a boot cost, and a redetect hitch.
10. **Focus loss freezes only the keyboard** (A5). Pads keep reading in the background, and a
    key held at focus loss stays held until the app is active again.

Hypotheses (not verified):

- `+0x40/+0x44` is a vibration request that never reaches hardware on Steam.
- `FUN_004A77E0()+0x270` is "online session active" (it gates the lost-controller pause).
- The UI-manager lock `+0x30` is always 0, so the named-query gate is the focus name alone.

## 5. Open questions and next L2 targets

Open:

- `+0x4C/+0x50` (override word): no writer outside the ctor and BuildActionWords was found
  inside the boundary. A writer through some other base register elsewhere in the binary was
  not ruled out (a whole-binary scan for `[reg+4Ch]` writes on controllers is the next step).
- `FUN_004197B0` / `FUN_004199E0` (device vtbl+0x14/+0x18, "== 6"): no caller found. *Hypothesis*:
  a remnant of the arcade build.
- Bits 8-11 (IDs 21-24) are read as held at 0x006AE4E0/0x006AE503 but never bound. Which menu
  is that?
- `FUN_004A77E0` (the net/session object 0x00CF7958?) and its `+0x270`: who writes it.
- Strings `MainPadID`, `SubPadID`, `Pad1Available..Pad4Available`, `Controller1..4` (data
  table 0x009E5DB4) have no code references in the export. Leftover config keys?

Answered in this pass (were open):

- Who writes `+0x48`: only the ctor (0). `+0x40/+0x44`: ControllerWrapper_SetVibration.
- Who reads sysMgr `+0x1C/+0x20`: nobody.
- The discarded `FUN_0047EBB0` call: a pure getter whose result is unused.
- `FUN_0043BAF0()+0xD0`: application active, and only the keyboard reads it.
- The focus stack: pushers and poppers are in A5.
- `FUN_006C4990` writes the active id from a field that is always 0. `FUN_0069DB60` is the
  replay recorder; mode 5 is Versus.

Remaining L1 rows, in the recommended order for the next pass:

1. `DInputPad_Poll` (0x00419C20), `DInputPad_EnumCallback` (0x00419890), `StickToDirectionBits`
   (0x00419690), `DInputPad_SelectAxis`, `DInputPad_EnumAxesCallback`, `IsXInputDevice_WMI`.
   Needed only if the mod keeps the game's DInput devices. A mod-owned device layer replaces
   them.
2. `SysMgr_InitInput` is L2; `SystemManager_ctor`/`_dtor_body`, `WinMain_InitSystems`: boot
   context, low value.
3. `BattleInputWrite` / `_Alt` (0x00559FE0 / 0x0055A3E0): only the packer call sites were
   verified. The side routing belongs to Battle.
4. The id writers (`ModeStart_*`, `MainMenu_*`, pad assign, challenger, training join,
   `Online_AssignControllerIds`): the write sites were located; full L2 only matters if the
   rewrite patches them instead of pinning the id.
5. The focus-stack and UI-manager rows added in this pass (MsgWindow_*, UiManager_Tick, pause
   helpers).
6. The JVS rows: dead on Steam, last.

## 6. Key config screens and per-device bindings

Mapped 2026-10-07 (third pass), for the goal: players configure buttons in the GAME's own
screens, and the mod remembers the result per physical device and applies it wherever that
device plays. **Confirmed** = read in the assembly (or in the raw bytes of the archived exe /
`bbsave.dat`). *Hypothesis* = from the decompile or inferred, not verified.

### 6.1 Recommended design (summary)

1. **optData is a staging window, the mod's per-device store is the truth.** Keep, per
   physical device, the four records the game would hold for it (set B Type A/B, set A Type A/B,
   in the record format of its class, 20 bytes each) plus its "Type B enabled" flag, in
   `BBCF_IM\controllers.ini`. Mirror the owning device's records into optData (the
   `bbsave.dat` image) whenever the device that owns a record changes, and **detect edits by
   diffing optData against the last image the mod wrote, once per frame**. No hook is needed to
   detect an edit: every game screen writes optData directly (6.4), and only the screens do.
2. **Apply in the bridge, per device.** `GameInputBridge` already knows whether it is building
   a menu (set A) or battle (set B) word for pad slot 0/1. Build each device's effective binding
   table from the class defaults plus its record for the profile the game selected
   (`engine+0x25D8[slot]` for set A, `engine+0x25E0[slot]` for set B; 6.3), and OR the devices of
   the side. A keyboard on Player 2 then gets its own keyboard config with no game change.
3. **Redirect the two capture reads** so the game's rebind screens listen to the device being
   edited, not to the game's slot device or the merged keyboard map: the pad-capture device fetch
   (`call 004094E0` at 0x006A851E / 0x005154BE) and the keyboard-capture map fetch
   (`call 00409500` at 0x006A89A0 / 0x00515AA2). 6.6 has the patch details.
4. **Optional patches** for a keyboard on Player 2: open the pad-to-keyboard panel switch for
   id 1 (0x006A9C16 / 0x00516EB6), and fix the battle keyboard panel's profile source
   (0x006A8024). Allow one keyboard editor at a time: the keyboard records are not per id.
5. **Never write `bbsave.dat` yourself.** Write only valid values into optData (6.7) and let the
   game save. Vanilla then simply sees the bindings of the device that last owned each record.

### 6.2 Where key configs live (confirmed)

`GetOptionManager` (0x004B9770) returns the CSaveDataManager at 0x00CF85D8. `GetOptionData`
(0x004B9700) returns `0x00CF85E0` = manager+8 = the `BBSave` block. Both are cdecl with no
arguments, and both lazily construct the manager (guard 0x00EBFE18 bit 0, ctor 0x004B9350). The
block is 0x588B8 = 362680 bytes, which is exactly the size of `Save\bbsave.dat`. The archived
save holds the key-config records at the same offsets as optData, in plain bytes, so the file is
an image of the block. The first 16 bytes of the file carry a header: "Save" at +8 and the size
at +0xC. *Hypothesis*: +0..+7 is a checksum.

| optData offset | VA | Size | What |
|---|---|---|---|
| +0x4204 + 20*(profile + slot*2) | 0x00CFC7E4 | 4 x 20 | **set B pad records** (battle), per pad slot 0/1 x profile 0/1 (Type A/B). 13 bytes used |
| +0x4254 + id | 0x00CFC834 | 2 x 1 | **"Type B enabled" flag** per controller id, 0 or 1. Also used as the *profile* for the boot apply (6.3) |
| +0x4256 + 20*profile | 0x00CFC836 | 2 x 20 | **set B keyboard records**, per profile only (no id). 17 bytes used |
| +0x54A55 + 20*(profile + slot*2) | 0x00D4D035 | 4 x 20 | **set A pad records** (menus). The apply reads 8 bytes |
| +0x54AA5 + id | 0x00D4D085 | 2 x 1 | **set A profile** per id (the menu "Current Set", 0/1) |
| +0x54AA7 + 20*profile | 0x00D4D087 | 2 x 20 | **set A keyboard records**, per profile only. 12 bytes used |

`record = profile + slot*2` is confirmed in `ApplyPadKeyConfig_SetB` (0x004BB73A:
`lea eax,[profile+slot*2]`, record base 0x00CFC7E4 + 20*that). The profile byte is used
unmasked, so a value other than 0/1 indexes another slot's record.

Runtime fields (not saved):

- `engine+0x25D8[id]`: set A profile currently in use.
- `engine+0x25E0[id]`: set B profile currently applied (the battle "Current Set").
- Char select `charSel+0xA1C[side]`: 1 = Type A, 2 = Type B. `CharSel_IsTypeB` (0x0060A270,
  thiscall(charSel, side), ret 4) returns `[+0xA1C+side*4] == 2`.
- Char select `charSel+0xA24[side]`: a second per-side option, 0/1. `FUN_0060A290` returns
  `[+0xA24+side*4] == 1` into player block +0x538. *Hypothesis*: Stylish/Technical.

Record byte maps, editor row = byte + 3 (rows 0 Exit, 1 Default, 2 ChangeKeyType). The action is
the binding index passed to `KeyControler_SetBinding`. Defaults come from the reset functions:
`ResetPadKeyConfig_SetB` copies 0x009DFDEC, `ResetKeyboardKeyConfig_SetB` copies 0x009DFE00,
set A uses 0x009DFE14 (pad) / 0x009DFE28 (keyboard).

| Byte | Row label (`KYCF_*`) | Action (bit) | Pad set B default | Keyboard set B: action / default |
|---|---|---|---|---|
| +0 | AttackA (keyboard: UP) | 14 A | 2 (X) | 0 Up / W (0x16) |
| +1 | AttackB (DOWN) | 15 B | 3 (Y) | 2 Down / S (0x12) |
| +2 | AttackC (LEFT) | 13 C | 1 (B) | 3 Left / A (0x00) |
| +3 | AttackD (RIGHT) | 12 D | 0 (A) | 1 Right / D (0x03) |
| +4 | AttackE | 17 taunt | 5 (RB) | 14 A / U (0x14) |
| +5 | AttackF | 19 special | 7 (RT) | 15 B / I (0x08) |
| +6 | AttackAB | 22 | -1 | 13 C / O (0x0E) |
| +7 | AttackBC | 23 | -1 | 12 D / J (0x09) |
| +8 | AttackABC | 24 | -1 | 17 / L (0x0B) |
| +9 | AttackABCD | 25 | -1 | 19 / `=` (0x32) |
| +0xA | Memory | 16 | 4 (LB) | 22 / -1 |
| +0xB | Play | 18 | 6 (LT) | 23 / -1 |
| +0xC | QuickReset | 27 | 9 (Back) | 24 / -1 |
| +0xD..+0x10 | | | | 25 -1, 16 K (0x0A), 18 P (0x0F), 27 Backspace (0x42) |

The keyboard row labels (table 0x00A052A8) are UP, DOWN, LEFT, RIGHT, AttackA..F, AB, BC, ABC,
ABCD, Memory, Play, QuickReset. The pad table is 0x00A05268. Set A records:

- pad bytes +0..+7 -> actions 14, 15, 13, 12, 17, 19, 16, 18; defaults 2, 3, 1, 0, 5, 7, 4, 6;
- keyboard bytes +0..+0xB -> 0, 2, 3, 1, 14, 15, 13, 12, 17, 19, 16, 18; defaults W S A D U I O
  J L = K P.

### 6.3 What writes the binding tables, and when (confirmed unless marked)

All apply functions are `thiscall(optMgr, ...)`: ECX is only used for the lazy-init guard.
They resolve the controller themselves and call `KeyControler_SetBinding` per byte, so they
write `controller+0x34[action]` and nothing else. Bindings they do not touch keep the class
defaults (section 3).

| Function | Writes | Profile source |
|---|---|---|
| `ApplyPadKeyConfig_SetB(slot, p)` 0x004BB6E0, ret 8 | `SysMgr_GetBattlePadController(slot)`+0x34, actions 12-19, 22-25, 27 | caller |
| `ApplyKeyboardKeyConfig_SetB(p)` 0x004BB860, ret 4 | sysMgr+0x18, actions 0-3, 12-19, 22-25, 27 | caller |
| `ApplyPadKeyConfig_SetA(slot, p)` 0x004BBA10, ret 8 | [sysMgr+0x10+4*slot], actions 12-19 | caller |
| `ApplyKeyboardKeyConfig_SetA(p)` 0x004BBB30, ret 4 | sysMgr+0x0C, actions 0-3, 12-19 | caller |

Who calls them:

- **Boot** `ApplyAllKeyConfig` (0x00483F10):
  - set B pads 0/1 with `optData[0x4254]` / `[0x4255]` (the Type B flags);
  - set B keyboard with `[0x4254]` (Player 1's flag);
  - set A pads 0/1 and the set A keyboard with `[0x54AA5]` / `[0x54AA6]`;
  - then `engine+0x25D8/+0x25DC` = the set A profile bytes.
- **Before each match** `BattleLoad_ApplyPlayerKeyConfig` (0x00612530):
  - per side, `p = CharSel_IsTypeB(side)`, id = `engine+0x25F4[side]`;
  - pad set B of slot id gets profile p;
  - the keyboard set B only when id == 0 (0x0061270C / 0x0061279D);
  - `engine+0x25E0[id] = p` (0x00612726 / 0x006127B1);
  - online: only the local side (`FUN_0049A340() == 2` -> side 1).
- **Mode start** `ModeStart_AssignFromActive` (0x00550AE3..0x00550B3C): the same pad/keyboard
  pair with `p = byte [FUN_004A77E0()+0x2A8]` (*hypothesis*: a mode/session copy), then
  `engine+0x25E0[id] = p`.
- **Battle key-config editor closed** `KeyConfigMenu_ApplyIfInUse` (0x006A8AD0): for each
  editor, only if `engine+0x25E0[id] ==` the editor's profile.
- **Menu key config**: it applies **live**. Every capture, clear and Type toggle calls
  `ApplyPadKeyConfig_SetA` / `ApplyKeyboardKeyConfig_SetA` at once (0x0051566D, 0x00515725,
  0x00515CB9, 0x005165BE...). On close, `MenuKeyConfigMenu_ApplyOnClose` (0x00515CD0) re-applies
  and saves the profile: `engine+0x25D8[id]` (0x00515D1E) and `optData[0x54AA5+id]`
  (0x00515D2A).

The "Current Set" choice:

- **Battle.** Char select `CharSel_ButtonSettingTick` (0x0060CD50, *decompile*): Up (ID 7)
  -> `+0xA1C[side] = 1` (Type A) and Down (ID 8) -> 2 (Type B). Both only when
  `optData[0x4254 + engine+0x25F4[side]] != 0`. `CharSel_TypeBDisabled` (0x0060B510,
  thiscall(charSel, side), ret 4) returns that byte == 0. So a player can only pick Type B
  after enabling it in the battle editor (row 2 confirm, 6.4).
- **Menu.** Row 2 Left/Right in the menu editor writes `engine+0x25D8[id]` and applies at once
  (0x005165A7). Close writes `optData[0x54AA5+id]`.

### 6.4 The game's editors

There are two editor families, each with a pad panel and a keyboard panel per editor slot:

| | Battle set B: `CKeyConfigMenu` / `_Keyboard` | Menu set A: `CMenuKeyConfigMenu` / `_Keyboard` |
|---|---|---|
| Focus name | "KeyConfigMenu" | "MenuKeyConfigMenu" |
| vtable (pad / kbd) | 0x00974870 / 0x009748E0 | 0x0094BEC4 / 0x0094BF4C |
| Open | `KeyConfigMenu_Open(id)` 0x006A8FB0, cdecl, ret | `MenuKeyConfigMenu_Open(id)` 0x005161C0, cdecl |
| Editor state block | `FUN_006A7B20()` = 0x0161BF80: `int id[2]` (-1 free), +8 closed, +0xC.. timers | `FUN_00514B20()` = 0x0119A4B0, same shape |
| Pad panel objects | 0x015D3790 + slot*0x200 | 0x01165B30 + slot*0x200 |
| Keyboard panel objects | 0x0161BB88 + slot*0x1FC | 0x0119A0B8 + slot*0x1FC |
| Join tick (2nd player) | `KeyConfigMenu_JoinTick` 0x006A8B70 | `MenuKeyConfigMenu_JoinTick` 0x00515D80 |
| Capture pad / keyboard | 0x006A8430 / 0x006A8650 | 0x005153D0 / 0x00515750 |

Panel fields:

- `+0x1C0` = controller id. The menu framework uses it for every navigation query.
- `+0x1D0` = profile being viewed/edited (0 = Type A, 1 = Type B; row 2 shows
  `KYCF_ChangeKeyType_A/_B`).
- `+0x58` = cursor row.
- `+0x1EC/+0x1F0/+0x1F4` = state / next state / delay: 0 browse, 1 capture, 2 delay of 8
  frames.
- Pad panel: `+0x1F8` = release-wait mask, `+0x1FC` = editor slot.
- Keyboard panel: `+0x1F8` = editor slot.

**Which id and device an editor edits** (confirmed):

- `KeyConfigMenu_Open(id)` callers and what they pass:
  - char select 0x0060C4D0: `engine+0x25F4[side]`;
  - main-menu options 0x006B5294 (MOP_KeyConfig) and 0x0071AB0B: the active id. 0x0071A5A0
    snapshots every set B record and both flags to 0x0181E784.. first. *Hypothesis*: 0x00711720
    compares them later to detect a change;
  - battle pause 0x0069B5F5: the pause menu's `[+0x1C0]`;
  - training 0x006D1175 (TRI_KeyConfig);
  - 0x005458D8: the active id.
- Inside `KeyConfigMenu_Open`:
  - slot = `(engine+0x25F8 == id)`, so the P2 id gets slot 1 and anything else slot 0;
  - `state.id[slot] = id`, the other slot = -1;
  - `panel[slot]+0x1C0 = id`, `+0x1D0 = engine+0x25E0[id]`;
  - both pad panels are shown and both keyboard panels hidden.
- A second player joins in the JoinTick: a connected id 0..1 (`IsControllerIdConnected`) that
  presses Start (ID 5) claims a free slot. That path does not go through Open.
- Navigation: `MenuItemManager_InputDispatch` (0x006C14C0) reads `id = [panel+0x1C0]` and maps
  `Input_ActionJustPressedForId(id, ...)`:
  - ID 3 -> vtbl+8 (confirm);
  - ID 4 / 6 -> +0xC (cancel);
  - ID 0xE -> +0x14;
  - ID 0xF / 0x10 (bits 16 / 17, LB / RB) -> **+0x18(0/1), the pad<->keyboard panel switch**;
  - 0x11 -> +0x1C, 0x12 -> +0x20;
  - ID 5 (Start) -> +0x24 (leave).
- **The keyboard panel is reachable only for id 0.** `KeyConfigMenu_PadPanel_OnPage`
  (0x006A9BF0, thiscall(panel, dir), ret 4) and `MenuKeyConfigMenu_PadPanel_OnPage`
  (0x00516E90) test `cmp dword ptr [state+slot*4],0 / jne` (0x006A9C12/0x006A9C16,
  0x00516EB2/0x00516EB6, bytes `83 3C 88 00 75 2C`). Only then do they hide the pad panel and
  show keyboard panel `0x0161BB88 + (slot != 0)*0x1FC`.
- **Vanilla quirk** (asm 0x006A8024): the battle keyboard panel's setup (0x006A7E50) always
  takes its profile from `[0x015D3960]`, editor slot 0's pad panel, whatever its own slot. Its
  row-2 toggle (0x006A95B3) also writes there. The menu keyboard panel uses
  `[0x01165D00 + slot*0x200]` correctly (0x00515044).

**How a press is captured** (pad: confirmed in asm 0x006A8430; keyboard: decompile plus the
call site in asm):

- **Enter capture.** In browse, D just pressed (ID 0xC) on a binding row (`row-3 < 15` battle
  pad, `< 19` battle keyboard, `< 11` menu pad) -> state 2 for 8 frames, then state 1. The pad
  panel also arms `+0x1F8 = 1 << record[+3]`, the bit of the game key bound to D in the record
  being edited. The menu pad panel uses its own record (+0x54A58). Capture then waits until no
  scanned key is held.
- **Pad capture** (0x006A8430 / 0x005153D0):
  - `id = state.id[panel+0x1FC]`, with -1 -> 0;
  - `device = InputDeviceRegistry_GetByIndex(reg, [sysMgr+0x10+id*4]->vtbl+0x14())`, i.e. the
    set A pad controller's device of slot id (call at 0x006A851E / 0x005154BE);
  - it scans **game keys {0,1,2,3,4,5,6,7,9,10,11}** with `device->vtbl+0xC(gameKey)`
    (IsPressed). Start (8) and every direction (0x20+) can never be bound;
  - the first key held is written to `optData + (p + id*2 + 0x34D)*20 + row-3`. 0x34D*20 =
    0x4204; the menu pad uses +0x54A55;
  - if another byte of the same 20-byte record already held that key, it gets the old value
    (swap);
  - the battle editor writes **optData only**. The menu editor also applies set A at once. The
    menu pad refuses an empty critical row: it keeps confirm/cancel and directions unique,
    0x00515577...
- **Keyboard capture** (0x006A8650 / 0x00515750):
  - device = `[InputDeviceRegistry_Get()+0xC]`, the **primary keyboard**, independent of the
    id (call 0x006A89A0, then `mov edx,[eax+0Ch]` at 0x006A89A5);
  - it reads the device's maps **directly**: an edge is `cur(+0x08) & ~prev(+0x18)` over a fixed
    79-key list. The list excludes Enter, Esc, F1-F12, both Alts, Space, Ins/Del/Home/End/PgUp/
    PgDn, the arrows and 0x6A;
  - the key is written to `optData + 0x4256 + p*20 + row-3` (menu: +0x54AA7), with the same
    swap;
  - the menu keyboard refuses to put a key that is bound to a direction, confirm or cancel into
    an empty other row.
- **Clear.** Holding ID 10 (bit 1, Right) for 15 frames on a binding row writes 0xFF (battle:
  0x006A9310; menu: 0x00516520, which also re-applies).
- **Row 2.** Left/Right (repeat IDs 9/10) toggles `panel+0x1D0` (the viewed profile; the menu
  editor also writes `engine+0x25D8[id]` and applies). Confirm on row 2 in the battle pad panel
  toggles `optData[0x4254+id]` bit 0, but only while viewing Type B (0x006A9A66..0x006A9AB6).
  The battle keyboard panel toggles `optData[0x4254]` with no id (0x006A9B9B..).
- **Default** (row 1): `ResetPadKeyConfig_SetB(optMgr, id)` 0x004BC9B0 (ret 4) resets **both**
  profiles of slot id. `ResetKeyboardKeyConfig_SetB` 0x004BCAA0 (ret) resets both keyboard
  profiles. Set A keyboard: `0x004BCC70`. The set A pad reset was not located.
- **Greyed rows.** The battle editor draws binding rows grey when viewing Type B and
  `optData[0x4254+id] == 0` (0x006A8143, 0x006BAF12, 0x006BB21A).

**How bindings are displayed** (confirmed):

- One giant item drawer, `FUN_006B5BE0`, reads the record byte (pad 0x006BAB5B, keyboard
  0x006BB1ED, menu pad 0x006BB9A8, menu keyboard 0x006BBDF3).
- Pad bytes go through a jump table (0x006BAB71, index 0x006BD72C, cases 0x006BD6EC):
  - 0 South, 1 East, 2 West, 3 North;
  - 4 L1, 5 R1, 6 L2, 7 R2;
  - 9 Select, 10 L3, 11 R3;
  - 0x24-0x27 RSUp/RSRight/RSDown/RSLeft;
  - 8 and 0x0C-0x23 draw nothing.
  The name goes to `Glyph_Resolve(name, 1)` (0x00695B00, cdecl).
- Keyboard bytes go to `KeyboardGameKeyToGlyph(gameKey)` (0x00697470, cdecl; 999 = none).
- **There is no per-device icon set.** The glyph style comes from `FUN_0042C8C0`, which is
  `mov eax,6; ret` on Steam. Pad glyphs are positional names of the game key index, so a DInput
  device's button k is drawn as XInput game key k.
- *Hypothesis*: the second argument 1 of `Glyph_Resolve` sends the prompt names
  (West/North/"East "/NG/South/OK/R1/R2/L1/L2) through `PromptRemapPad` (0x00695930: set A pad
  record of the ACTIVE id, profile `engine+0x25D8[active]`). That is how menu prompts show the
  player's bound button. The row names "East" and "Select" are not in that list.

### 6.5 Physical control -> game key -> action, per device class (confirmed)

| Class | Game key space | Notes |
|---|---|---|
| Keyboard (`AASTEAM_CInputKeyBoard`) | 0..0x69 = `table[VK]` (0x00A255C8, section 3), 0x6A unmapped | IsPressed = bit of the 128-bit map at +0x08. Directions are rebindable (keyboard records). SystemKeyControler adds Up arrow + Space (action 0), Right, Down, Left arrows (actions 1-3) on top of any binding |
| XInput (`AASTEAM_CInputXInputPad`) | 0..11 = A B X Y LB RB LT RT Start Back LS RS; 0x20-0x23 = d-pad/left stick up/right/down/left; 0x24-0x27 = right stick | keys < 32 -> +0x38, else +0x34 bit (key-32) |
| DInput (`AASTEAM_CInputPad`) | 0..11 = DIJOYSTATE button `[+0x284+k]` (generic layout: button k; loop 0x0041A028..0x0041A068); 0x20.. = directions from hat/axes (+0x290 axis selectors) and the d-pad-as-button map +0x280 | +0x280 bytes map raw buttons onto up/left/right/down (0x00419F80..0x0041A015); the generic layout points them at buttons 16-19 (`0x13121110`). *L1*: whether bits 16-19 can ever be set |

The **effective binding table** a device would get in vanilla (what the bridge must rebuild):

- **Pad class** (`BattleKeyControler_SetDefaults`): 0-3 -> 0x20-0x23, 12-19 -> 0-7, 20 -> 10,
  21 -> 11, 26 -> 8, 27 -> 9, others -1. Then overlay the pad record: set B bytes -> 12-19,
  22-25, 27; set A bytes -> 12-19.
- **Keyboard class** (`SystemKeyControler_SetDefaults`): 0 W, 1 D, 2 S, 3 A, 12 J, 13 O, 14 U,
  15 I, 16 K, 17 L, 18 P, 19 `=`, 20 N, 21 M, 26 Enter, 27 Backspace. Then overlay: set B bytes
  -> 0-3, 12-19, 22-25, 27; set A bytes -> 0-3, 12-19. Plus the hardcoded arrow/Space extras.
- Bit i of the word is set when `binding[i] != -1` and the device's game key is held.
- The bits that always keep the class default are 20, 21 and 26 in both sets, plus 27 in
  set A.

### 6.6 Recommended override points

**(a) Detect edits: frame diff, no hook.**

- Every edit path writes optData directly: capture, clear, swap, Default, the flag toggle and
  the set A profile on close. Nothing else in the binary writes these ranges besides the boot
  init (0x004BBF00) and the resets (all callers listed in 6.4).
- Once per frame (the bridge tick), compare the image the mod staged last against optData:
  - the four set B pad records (+0x4204, 80 bytes);
  - the flags (+0x4254, 2);
  - the keyboard set B records (+0x4256, 40);
  - and the same for set A (+0x54A55 80, +0x54AA5 2, +0x54AA7 40).
- Hand any changed record to the device that owns it (b), then refresh the staged image.
- If the mod wants the editor session boundaries anyway, they are in plain globals:
  - editor slot claimed/freed: `0x0161BF80[2]` (set B) / `0x0119A4B0[2]` (set A), valid once
    the guard bit 0 of 0x0161BFA0 / 0x0119A4D0 is set; -1 = free;
  - the slot's id and profile: `panel+0x1C0` / `+0x1D0` (0x015D3950 / 0x015D3960 + slot*0x200;
    set A 0x01165CF0 / 0x01165D00 + slot*0x200).
  - Hook-based alternatives: `KeyConfigMenu_Open` 0x006A8FB0 and `MenuKeyConfigMenu_Open`
    0x005161C0 (cdecl(int id), plain ret, prologue `55 8B EC 83 EC 24`). Note that the JoinTick
    claims slots without calling Open.

**(b) Store per physical device.**

- Ownership:
  - pad records of slot s belong to the **editing device of side s**;
  - the keyboard records belong to the keyboard being edited. With no editor open, keep P1's
    primary keyboard there, the vanilla meaning.
- Editing device = the side's most recently active device when the editor slot is claimed:
  `DeviceView::lastInputMs` among `side == s`. Lock it until the slot is freed. Prefer a pad
  while a pad panel is visible and a keyboard while a keyboard panel is visible. Panel
  visibility: the panel's shown flag, set through 0x006BDB40 (its offset is not mapped yet).
- Store per device: setB[2][20], setA[2][20] and typeBEnabled. A pad-class device stores
  pad-format records, and a keyboard stores keyboard-format records.
- Staging rules:
  - when the owning device of a record changes, copy its records into optData before any
    editor or char-select logic reads them. Do it on assignment changes and editor claims; the
    main menu snapshots at 0x0071AA5A just before Open, so stage earlier, not in an Open hook,
    or the game sees a spurious change;
  - write the device's typeBEnabled to `optData[0x4254+side]` so char select lets that player
    pick Type B.

**(c) Apply when the device plays (including a keyboard on Player 2).**

- In `GameInputBridge`, for a Role_PadNBattle controller use set B profile
  `engine+0x25E0[N]`, and for a Role_PadNMenu controller set A profile `engine+0x25D8[N]`.
  The mod pins P1 = id 0 and P2 = id 1, so slot N = id N = side N.
- Per device on side N: binding table = class defaults + device record (6.5), with the device's
  controls translated into its class game keys:
  - keyboard: VK -> 0x00A255C8;
  - XInput: the order in 6.5;
  - DInput: button k -> k (k < 12), hat/stick -> 0x20-0x23.
- OR the per-device words. SOCD-clean as `BuildActionWords` does.
- The game's apply functions can keep running: they only write the binding tables of the six
  controllers whose words the bridge replaces. Leave them alone, so the vanilla-off switch keeps
  working.
- This needs no game patch for a keyboard on P2: vanilla's "keyboard config only for id 0"
  (BattleLoad 0x0061270C, ApplyIfInUse, boot) never matters, because the bridge never reads the
  game's keyboard controllers.

**(d) Make capture and display follow the mod's device.**

1. **Pad capture device.** Replace the `call 004094E0` at **0x006A851E** (set B) and
   **0x005154BE** (set A) with a call to a mod thunk:
   - bytes `E8 BD 0F D6 FF` / `E8 1D 40 EF FF`;
   - convention `thiscall(registry, int* index)`, **ret 4**, returns `AA_CInput*`;
   - it returns a static fake device for that editor's side. The id is in EDI at 0x006A851E
     (`[ebp-44h]`) and in EBX at 0x005154BE;
   - the fake's vtbl+0xC (`thiscall(dev, gameKey)`, ret 4) answers from the editing device in
     pad game-key space (6.5);
   - it must report the D-bound key consistently, or the release-wait (+0x1F8) never ends.
   - A keyboard as editing device: answer 0, since the pad panel is pad-only.
2. **Keyboard capture map.** Replace the `call 00409500` at **0x006A89A0** / **0x00515AA2**
   (5 bytes, `E8 5B 0B D6 FF` / `E8 59 3A EF FF`):
   - the thunk returns a fake registry whose +0xC points to a fake keyboard;
   - layout as `AASTEAM_CInputKeyBoard`: +0x08 current and +0x18 previous 128-bit game-key maps,
     both fed from the editing keyboard's VKs via the VK table;
   - the capture reads only registry+0xC and those two maps.
   - Without this, every keyboard on the machine (merged by GetKeyboardState, or nothing if the
     mod zeroes it) can rebind.
3. **Keyboard panel for id 1.** NOP the `jne` (`75 2C`) at **0x006A9C16** and **0x00516EB6**,
   conditionally: only when the editing device of that slot is a keyboard and no other keyboard
   panel is open. The keyboard records have no id, so two keyboard editors at once would write
   the same 20 bytes.
4. **Battle keyboard panel profile.** At **0x006A8024** `A1 60 39 5D 01` (`mov eax,[015D3960h]`):
   - read `[0x015D3960 + slot*0x200]` instead (slot = `[esi+1F8h]`). Otherwise a slot-1
     keyboard edits slot 0's viewed profile.
   - The row-2 toggle at 0x006A95B3 writes the same global and needs the same fix.
5. **Display needs nothing.** Glyphs depend only on the record bytes and the class of panel
   (pad vs keyboard), not on the device. Mislabels for DInput pads are cosmetic: the button
   index is drawn as the XInput glyph at that index.
6. Navigation inside the editors already goes through the ids, so it follows the bridge words
   of the side.

- Branch-target scan (direct jumps and calls in `tools/bbcf_disasm_ascii.txt`): nothing lands
  inside any of the patch sites above.

### 6.7 Risks

- **`bbsave.dat` validity for vanilla.** The file is the optData image, so anything the mod
  stages is saved. Keep every byte in the range vanilla can produce:
  - pad records: -1 or {0..7, 9..11}. The defaults also use 9; never 8 or >= 0x20 from a
    capture;
  - keyboard records: -1, the 79-key list, or the defaults (0x32 `=`, 0x42 Backspace);
  - flags `0x4254/0x4255` and `0x54AA5/0x54AA6`: exactly 0 or 1. They are used unmasked as
    profile indexes (`record = p + slot*2`), and 2 would select the other slot's record.
- Vanilla `SetBinding` and `IsPressed` have **no bounds checks**. A keyboard game key > 127 or
  a pad record byte >= 64 reads past the device maps.
- Never write the file directly. The header (+0..+7, *hypothesis* checksum) is the game's job.
- **Bindings for vanilla.** After the mod, vanilla sees the records of whichever device last
  owned them. The file stays valid; it is the expected trade-off.
- **Shared keyboard records.** Only one keyboard can be staged into optData at a time. The
  per-device store keeps them apart, but two simultaneous keyboard editors cannot (6.6 d3).
- **Spurious "changed" detection.** The main menu snapshots the records before Open
  (0x0071A5A0) and compares them later (0x00711720, *hypothesis*). Stage before the snapshot.
- **Online.** `BattleLoad_ApplyPlayerKeyConfig` applies only the local side, and
  `engine+0x25E0` of the remote id is stale. The bridge only builds local words, so only the
  local side's profile is read.

### 6.7.1 Implemented (2026-10-07, `src/Input/GameKeyConfig.cpp`)

The 6.6 design is in, all nine patches byte-checked, and verified in-game with a virtual
Xbox pad on P1 and the keyboard on P2 (rebinds stored per device, P2 keyboard panel, battle
input from set B). One thing 6.6 missed, **confirmed**:

- **Keyboard panels are built with controller id 0.** The constructor at 0x006A79B8 (menu:
  0x005149B8) writes `panel+0x1C0 = 0`, while the pad panels get their slot (0x006A788D). The
  menu framework navigates a panel with the inputs of its `+0x1C0` id, so a keyboard panel opened
  for id 1 was driven by Player 1. The panel-switch thunk now writes the editor's id into
  `kbPanel[slot]+0x1C0` (battle 0x0161BB88, menu 0x0119A0B8, + slot*0x1FC) before showing it.
- Save-loaded signal: `ApplyAllKeyConfig` (0x00483F10) entry hook; the records at that moment are
  the "inherited" config of every device that was never configured.
- Both capture functions keep their panel at `[ebp-158h]`; the keyboard-capture thunk reads it to
  find the slot (`panel+0x1F8`) and from it the side.

### 6.7.2 Both players' keyboard panels at once (2026-10-09)

The keyboard records (+0x4256 set B, +0x54AA7 set A) exist once, not per id, so two keyboard
panels open together would edit and show the same 40 bytes. Every writer of those records is a
keyboard panel method that gets its panel in ECX, and only through the vtable (no direct callers):

| | Battle | Menu |
|---|---|---|
| Tick (browse, clear, capture) | 0x006A98F0 | 0x00516C30 |
| Confirm (Restore Defaults, Type B) | 0x006A9AD0 | 0x00516DC0 |
| Drawer read of a row's key | 0x006BB1ED `8A 84 88 56 42 00 00` | 0x006BBDF3 `8A 84 88 A7 4A 05 00` |

All four methods are fastcall(panel) with a plain `ret`. In the drawer, `eax` = optData + row-3,
`ecx` = profile*5 and the panel is at `[ebp-6180h]`; no branch lands inside either 7-byte read.

**Correction:** the battle keyboard panel's vtable starts at 0x009748DC, not 0x009748E0 as listed
above (0x009748E0 holds slot 1). Read from 0x009748E0, the menu twin's offsets look off by one.

The mod wraps the four methods to load that side's keys into the records first and save them
back afterwards, and the two drawer reads return the side's keys. A side with no keyboard of
its own edits the game's own config. When every editor is closed, the records go back to the
game's own config. `GameKeyConfig.cpp`, `g_kbEdit`. Verified in-game with two keyboards (a
HIDMaestro steam-controller-2 keyboard on P1 and injected input on P2): simultaneous rebinds,
Restore Defaults on one side only, reopen, battle and menu editors.

### 6.8 Functions added in this pass

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x004B9700 | GetOptionData | 108 | L2 | cdecl, no args, returns 0x00CF85E0 (CSaveDataManager+8, the bbsave.dat image). Lazy ctor 0x004B9350 under guard 0x00EBFE18. |
| 0x004B9770 | GetOptionManager | 108 | L2 | Same, returns 0x00CF85D8. `this` of every Apply/Reset. |
| 0x004BC9B0 | ResetPadKeyConfig_SetB | 226 | L2 | thiscall(optMgr, id), ret 4: copies 20 bytes of 0x009DFDEC into BOTH profiles of slot id (0x00CFC7E4 + id*40 and +20). Editor row 1. |
| 0x004BCAA0 | ResetKeyboardKeyConfig_SetB | 209 | L2 | thiscall(optMgr), ret: 0x009DFE00 into both keyboard set B records (0x00CFC836, 0x00CFC84A). |
| 0x004BCC70 | ResetKeyboardKeyConfig_SetA | 209 | L1 | 0x009DFE28 into 0x00D4D087 and 0x00D4D09B. |
| 0x006A8FB0 | KeyConfigMenu_Open | 415 | L2 | cdecl(int id), ret. Slot = (engine+0x25F8 == id). state.id[slot] = id, other = -1; panel[slot]+0x1C0 = id, +0x1D0 = engine+0x25E0[id]; shows both pad panels (0x015D3790/0x015D3990), hides keyboard panels (0x0161BB88/0x0161BD84) and writes their +0x1C0 (id / -1). |
| 0x006A8430 | KeyConfigMenu_CapturePad | 539 | L2 | fastcall(panel), ret, returns 0/1. Device = registry[[sysMgr+0x10+id*4]->GetDeviceIndexPtr] (0x006A851E). Release-wait on +0x1F8, then first held of game keys {0-7,9,10,11} -> optData+(p+id*2+0x34D)*20+row-3 with swap; state 2/8 frames. optData only. |
| 0x006A8650 | KeyConfigMenu_CaptureKeyboard | 1133 | L1 | fastcall(panel), ret. Primary keyboard map edge (cur +0x08 & ~prev +0x18) over a 79-key list -> optData+0x4256+p*20+row-3 with swap. Registry fetch at 0x006A89A0. |
| 0x006A8040 | KeyConfigMenu_PadPanel_SetupRow | 455 | L2 | thiscall(panel, items, row), ret 8. Row 2 label `KYCF_ChangeKeyType_%s` (A when +0x1D0 == 0, else B); rows 3+ label from 0x00A05268, disabled when +0x1D0 != 0 and optData[0x4254+id] == 0. |
| 0x006A8210 | KeyConfigMenu_KbdPanel_SetupRow | 455 | L1 | Keyboard twin (labels 0x00A052A8, 17 binding rows). |
| 0x006A7C80 | KeyConfigMenu_PadPanel_Setup | 457 | L1 | 16 rows, layout, glyph item type. |
| 0x006A7E50 | KeyConfigMenu_KbdPanel_Setup | 486 | L1 | 20 rows. Ends with +0x1D0 = [0x015D3960] for ANY slot (asm 0x006A8024, vanilla quirk). |
| 0x006A77D0 | CKeyConfigMenu_ctor | 293 | L1 | vtbl 0x00974870; +0x1FC = instance counter 0x0161BFA4 (editor slot). |
| 0x006A7900 | CKeyConfigMenu_Keyboard_ctor | 282 | L1 | vtbl 0x009748E0; +0x1F8 = counter 0x0161BFC4. |
| 0x006A7B20 | KeyConfigMenu_State | 165 | L1 | Lazy static 0x0161BF80 (guard 0x0161BFA0): id[2] = -1, +8 closed, +0xC..+0x18 per-id join cooldowns, +0x1C. |
| 0x006A7BD0 | KeyConfigMenu_KbdState | 135 | L1 | Lazy static 0x0161BFA8 (guard 0x0161BFC0). |
| 0x006A7A20 | KeyConfigMenu_CloseIfFocused | 151 | L1 | Focus "KeyConfigMenu" and not closed: ids = -1, closed = 1, hide panels, pop focus (0x00697B70). |
| 0x006A7AC0 | KeyConfigMenu_KbdClose | 89 | L1 | Keyboard-state twin. |
| 0x006A83E0 | IsKeyConfigMenuFocused | 67 | L1 | Focus name == "KeyConfigMenu". 7 callers (challenger, char select, VS menus) use it to suspend their own input. |
| 0x006A8B70 | KeyConfigMenu_JoinTick | 1081 | L1 | Ticked by UiManager 0x00699040. Per connected id 0..1 not in cooldown: Start (ID 5) claims a free slot (panel +0x1C0 = id, +0x1D0 = engine+0x25E0[id]). Calls KeyConfigMenu_ApplyIfInUse when both slots are free. |
| 0x006A9310 | KeyConfigMenu_PadPanel_BrowseTick | 534 | L1 | Row 2 Left/Right toggles +0x1D0 bit 0 (asm 0x006A9379). Hold ID 10 15 frames clears a byte to 0xFF. D (ID 0xC) enters capture with +0x1F8 = 1 << record[+3] (0x006A94CD). |
| 0x006A9530 | KeyConfigMenu_KbdPanel_BrowseTick | 464 | L1 | Same for the keyboard record; the row-2 toggle also writes [0x015D3960] (0x006A95B3). No release-wait. |
| 0x006A98A0 | KeyConfigMenu_PadPanel_Tick | 80 | L1 | vtbl+0x34: state 0 browse, 1 capture, 2 delay (+0x1F4) then +0x1EC = +0x1F0. |
| 0x006A98F0 | KeyConfigMenu_KbdPanel_Tick | 80 | L1 | Keyboard twin. |
| 0x006A99C0 | KeyConfigMenu_PadPanel_OnConfirm | 259 | L2 | vtbl+8, thiscall(panel), ret. Row 0: leave (id = -1). Row 1: ResetPadKeyConfig_SetB(id). Row 2 with +0x1D0 != 0: toggle optData[0x4254+id] bit 0. |
| 0x006A9AD0 | KeyConfigMenu_KbdPanel_OnConfirm | 274 | L2 | Row 0 leave, row 1 ResetKeyboardKeyConfig_SetB, row 2 toggles optData[0x4254] (no id, 0x006A9B9B). |
| 0x006A9940 | KeyConfigMenu_PadPanel_OnCancel | 57 | L1 | vtbl+0xC: in browse, state.id[slot] = -1. |
| 0x006A9980 | KeyConfigMenu_KbdPanel_OnCancel | 57 | L1 | Twin. |
| 0x006A9BF0 | KeyConfigMenu_PadPanel_OnPage | 90 | L2 | vtbl+0x18, thiscall(panel, dir), ret 4. Only if state.id[slot] == 0 (0x006A9C12): hide self, show keyboard panel 0x0161BB88 + (slot != 0)*0x1FC. |
| 0x006A9C50 | KeyConfigMenu_KbdPanel_OnPage | 65 | L2 | vtbl+0x18, ret 4: back to pad panel 0x015D3790 + slot*0x200. |
| 0x006A9CA0 | KeyConfigMenu_PadPanel_OnStart | 107 | L2 | vtbl+0x24, ret. In capture: abort. Else join cooldown[id] = 10, state.id[slot] = -1 (leave). |
| 0x006A9D10 | KeyConfigMenu_KbdPanel_OnStart | 137 | L2 | Keyboard twin; re-shows the pad panel if the other slot is in use. |
| 0x006A9150 | KeyConfigMenu_DrawAssignExplain | 439 | L1 | "KYCF_AssignExplain" box while capturing. |
| 0x006A9700 | KeyConfigMenu_PadPanel_Draw | 94 | L1 | vtbl+0x50: explain box when state == 1. |
| 0x006A9760 | KeyConfigMenu_KbdPanel_Draw | 94 | L1 | Twin. |
| 0x005161C0 | MenuKeyConfigMenu_Open | 415 | L1 | cdecl(int id). Same as KeyConfigMenu_Open with set A globals (state 0x0119A4B0, pad panels 0x01165B30 + slot*0x200, profile engine+0x25D8[id]). |
| 0x005153D0 | MenuKeyConfigMenu_CapturePad | 883 | L1 | As 0x006A8430 on +0x54A55, plus confirm/cancel protection, and applies set A at once. Device fetch at 0x005154BE. |
| 0x00515750 | MenuKeyConfigMenu_CaptureKeyboard | 1395 | L1 | As 0x006A8650 on +0x54AA7, refuses critical keys into empty rows, applies set A at once. Registry fetch at 0x00515AA2. |
| 0x00515CD0 | MenuKeyConfigMenu_ApplyOnClose | 171 | L1 | Per editor: ApplyPadKeyConfig_SetA(id, p), keyboard set A if id == 0, engine+0x25D8[id] = p (0x00515D1E), optData[0x54AA5+id] = p (0x00515D2A). Also called from char select 0x0060E508 / 0x0060F1BC. |
| 0x00515D80 | MenuKeyConfigMenu_JoinTick | 1081 | L1 | Set A twin of 0x006A8B70. |
| 0x00516520 | MenuKeyConfigMenu_PadPanel_BrowseTick | 627 | L1 | Row 2 Left/Right: +0x1D0 ^= 1, engine+0x25D8[id] = it, apply pad + keyboard set A. Clear by hold; D enters capture. |
| 0x005167A0 | MenuKeyConfigMenu_KbdPanel_BrowseTick | 640 | L1 | Keyboard twin. |
| 0x00516E90 | MenuKeyConfigMenu_PadPanel_OnPage | 90 | L2 | vtbl+0x18, ret 4. Only id 0 (0x00516EB2): show keyboard panel 0x0119A0B8 + (slot != 0)*0x1FC. |
| 0x00516EF0 | MenuKeyConfigMenu_KbdPanel_OnPage | 65 | L2 | Back to pad panel 0x01165B30 + slot*0x200. |
| 0x00514B20 | MenuKeyConfigMenu_State | 165 | L1 | Lazy static 0x0119A4B0 (guard 0x0119A4D0), same shape as 0x006A7B20. |
| 0x00514BD0 | MenuKeyConfigMenu_KbdState | 135 | L1 |  |
| 0x00514C80 | MenuKeyConfigMenu_PadPanel_Setup | 466 | L1 |  |
| 0x00514E60 | MenuKeyConfigMenu_KbdPanel_Setup | 502 | L1 | +0x1D0 = [0x01165D00 + slot*0x200] (asm 0x00515044), correct per slot. |
| 0x0060CD50 | CharSel_ButtonSettingTick | 1237 | L1 | thiscall(charSel, ?, side). Up (ID 7) -> +0xA1C[side] = 1, Down (ID 8) -> 2, both only if optData[0x4254+P id] != 0; Left/Right -> +0xA24[side] 0..1. |
| 0x0060A270 | CharSel_IsTypeB | 23 | L2 | thiscall(charSel, side), ret 4: [+0xA1C+side*4] == 2. Profile for BattleLoad. |
| 0x0060A290 | CharSel_GetOptionA24 | 29 | L2 | thiscall(charSel, side), ret 4: [+0xA24+side*4] == 1 (-> player block +0x538). |
| 0x0060B510 | CharSel_TypeBDisabled | 43 | L2 | thiscall(?, side), ret 4: optData[0x4254 + engine+0x25F4[side]] == 0. |
| 0x006C14C0 | MenuItemManager_InputDispatch | 1826 | L1 | id = [menu+0x1C0]; action IDs 3/4/6/0xB/0xE/0xF/0x10/0x11/0x12/5 -> vtbl+8/+0xC/+0x10/+0x14/+0x18(0/1)/+0x1C/+0x20/+0x24 (asm 0x006C19AE..0x006C1B4A). |
| 0x00695B00 | Glyph_Resolve | 5582 | L1 | cdecl(name, translate). Named glyph -> sprite; translate = 1 remaps prompt names through the active id's set A record. |
| 0x00695930 | PromptRemapPad | 65 | L2 | cdecl(logical), tail-jumps 0x006977F0 with optData[0x54A55 + (engine+0x25D8[active] + active*2)*20 + logical]. |
| 0x006957F0 | PromptRemapKeyboard | 48 | L2 | cdecl(logical), tail-jumps 0x00697470 with the set A keyboard byte of profile engine+0x25D8 (id 0). |
| 0x006977F0 | PadGameKeyToGlyph | 467 | L1 | Game key 0..0x27 -> South/East/West/North/L1/R1/L2/R2/Select/L3/R3/RS*; 999 none. |
| 0x00697470 | KeyboardGameKeyToGlyph | 616 | L1 | Keyboard game key -> sprite; 999 none. |
| 0x0042C8C0 | GetGlyphStyle | 6 | L2 | `mov eax,6; ret`. Fixed on Steam: no per-device icon set. |
| 0x0071A5A0 | MainMenu_OpenKeyConfig | 1585 | L1 | Snapshots the set B records + flags (0x0181E784..) or set A (0x0071AB29..) and opens the battle or menu editor with the active id. |
| 0x00711720 | MainMenu_KeyConfigChangedCheck | 3650 | L1 | *Hypothesis*: compares optData with the 0x0071A5A0 snapshot (0x00711BBC..0x00711C9D). |
