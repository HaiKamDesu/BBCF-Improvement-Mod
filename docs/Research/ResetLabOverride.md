# Training "reset positions": overriding what a held direction does

RE session 2026-09-27. Question: a user wants Down + reset to swap sides and Up + reset to go
to the middle (today Up = swap, the mod's own feature, and Down = middle, the game's). Can we
remap without the two fighting (e.g. Down doing *both* middle and swap)?

**Verdict: yes, cleanly.** The game funnels the whole "held direction -> where do we reset"
rule through one small function, and it does not touch positions at all: it only writes a
training-menu value. Hooking that function's entry gives complete control with a 6-byte JMP
and no epilogue gymnastics. Implemented behind `ResetLabUpAction` / `ResetLabDownAction`.

All addresses are static VAs (image base 0x400000). RVAs are in `src/Game/GhidraDefs.h`
under "Training reset positions".

## The game's path

```
MatchInfo::update_Fight   FUN_0055F780   (training: mode 6 -> FUN_00560FC0)
  FUN_00560FC0            training round-flow step; phase 0 asks
    FUN_0055EF40          "was reset pressed?"  (ADDR_TrainingResetRequested)
      for player 0,1: FUN_00496620(controllerId, 0x8000000, &zero)  -- reset bit, just-pressed
      if any:  FUN_006D0490(); FUN_006D2D40();   return 1
    -> phase 1: fade (FUN_004C97B0(0x1E)) then MatchInfo::next_round FUN_00562B40
```

`FUN_006D2D40` (`ADDR_TrainingResetDirectionToMenu`) is the rule, decompiled:

```c
if (gameMode != 6) return;
localPlayer = engine+0x1688;                     // FUN_0047E9C0
ctrl        = engine[0x25F4 + localPlayer*4];    // that player's controller id
left  = FUN_004966A0(ctrl,  9, keyconfigName);   // action ID 9  -> mask 0x8
right = FUN_004966A0(ctrl, 10, keyconfigName);   // action ID 10 -> mask 0x2
down  = FUN_004966A0(ctrl,  8, keyconfigName);   // action ID 8  -> mask 0x4
if (localPlayer == P2) != !FUN_0047E790())        // P2 side, or sides swapped: mirror
    swap(left, right);                            // (only when one of them is held)
if (left)       set("TRI_ResetPosition", 2);
else if (right) set("TRI_ResetPosition", 1);
else if (down)  set("TRI_ResetPosition", 0);      // 0 = centre
/* nothing held: no write -> the previous value stays (that is why a plain reset is sticky) */
```

`set` = `FUN_006CD5C0(nameHandle, value)`: walks the menu-item table (entries of 0x38 bytes,
0x17AE3BC..0x17B009B, C-string name at +0, `int` at -0x14 must be 1) and calls the setter
`FUN_006CD700` (thiscall, this = entry, one stack arg) which stores into
`optData(FUN_004B9700) + 0xA568 + entry->idx(+0x34) * 4` (or `+0xBBB8..` for four flagged
items). "TRI_ResetPosition" is the ordinary training-menu option; the held direction is just
a shortcut for changing it. `FUN_006CBFE0` (`ADDR_TrainingResetPositionApply`, called from
FUN_005C7CD0 / FUN_005585F0 during the reset) reads it back and turns it into a side.

Up (action ID 7, mask 0x1) is never read by the game. That is why "Up = swap" never collided
with anything: the game simply keeps the previous position and the mod swaps afterwards.

Action-ID -> mask table at 0x9DE0B0 (dumped from the exe): 5=0x4000000, 6=0x8000000,
7=0x1, 8=0x4, 9=0x8, 10=0x2. The earlier GhidraDefs comment had 8/9 as left/down; the
mirroring in FUN_006D2D40 proves 8 is the un-mirrored one (Down) and 9/10 the pair.

## Two corrections to older notes

- `ADDR_RoundRestart_VariantA/B/C` (0x58F390 / 0x58F4D0 / 0x58F610) were believed to be the
  training reset. They are `bbcmd_30014_EndTimestopEffect`, `bbcmd_30015_StopTimestopEffect`,
  `bbcmd_30013_TimestopEffect` (Tadatys' symbols), referenced only from the script-command
  vtables. Renamed `ADDR_TimestopEffect_*`; nothing used them.
- The mod's `hooks_system_input.cpp` comment on the action-ID order (fixed in place).

## The hook

Site: entry of FUN_006D2D40, first three instructions `push ebp / mov ebp,esp / sub esp,2Ch`
(6 bytes). Pre-flight per `docs/HookEpilogueContract.md`:

- Branch targets into 0x6D2D41..0x6D2D45: none (the only reference to the function is the
  `call` at 0x55EFCC).
- Prologue state: we sit *before* the prologue, so on the "skip" path the stack holds only
  the return address and a bare `ret` is the correct epilogue. The function takes no
  arguments (void(void), `ecx` is stored but never read).
- Not a `call` site; nothing to re-perform.

`TrainingResetDirection_Hook` (naked, `hooks_system_input.cpp`) saves flags/registers, calls
`TrainingResetDirection_Decide()`, restores, then either re-executes the 6 overwritten bytes
and jumps back to 0x6D2D46, or `ret`s.

## Where the direction comes from (second pass, 2026-09-27 evening)

The first build read the direction from the mod's packed battle-input word
(`GetLastObservedBattleInput(0)`, BattleInputWrite hook at 0x55A338). In-game every reset
press logged `held=0x0`: the word was **neutral at the hook** every single time, while the
same hook logged `observed=2` (Down) fine during play. So Down=swap never fired.

Headless-Ghidra pass (`ResetDirectionSourceGhidraReport.txt`, script
`ghidra_scripts/DecompileResetDirectionSource.py`):

- The battle-input writer is `FUN_00559FE0`, called only from `FUN_0056B1F0` <-
  `FUN_0056AFF0`, a task object (its only reference is the vtable slot at 0x94FC78). The
  reset step is in a different chain: `FUN_00561720` -> `MatchInfo::update_Fight
  FUN_0055F780` -> `FUN_00560FC0` -> `FUN_0055EF40` -> `FUN_006D2D40`. Two independent
  per-frame tasks; nothing ties the writer's output to the moment the reset rule runs, and
  what the hook saw says the rule runs while the word is neutral. A value the game itself
  never consults for this decision is the wrong thing to read here.
- What FUN_006D2D40 **does** consult, for Left/Right/Down: `FUN_004966A0(ctrlId, actionId,
  keyconfigName)` -> `FUN_0047E7B0(ctrlId)` = one of the two `GAME_SynthKeyControler`
  wrappers (`FUN_00464410`, index 0/1) -> `FUN_00464170(actionId)`, which for each of the
  wrapper's devices (vtable[0], vtable[1] when `wrapper+4 != 0`) computes `FUN_004964E0` =
  `maskTable[actionId] & [device+0x20]->+0xC` and ORs them. `ctrlId` is
  `engine[0x25F4 + local*4]`, `local` = `engine+0x1688` (FUN_0047E9C0), the training
  player's side. Mask table 0x9DE0B0: 7=Up 0x1, 8=Down 0x4, 9=Left 0x8, 10=Right 0x2 -
  and FUN_006D2D40 does treat 8 as centre and mirrors 9/10, confirming the labels.

The hook now calls exactly that chain (`ReadHeldDirectionsFromGame`) for IDs 7..10 at the
same instant the game is about to call it for 8/9/10. Whatever it returns for Down/Left/Right
is by construction what the game acts on for the same press, so the mod and the game cannot
disagree; Up is read from the same devices with the same call. The old objection to
"the held words" was about ORing *every captured controller object* (menu controllers,
second key-config set); this reads one wrapper, the one the game reads.

`ScrWindow::TickTrainingResetSwap` (EndScene) no longer reads a direction at all: it takes
the hook's verdict for the press (`ConsumeLastResetPressDecision`) and toggles the swap when
the reset executes. Without the hook (controller gate off) there is no verdict and no swap.

## Why "Reset to middle" / corners did nothing (third pass, 2026-09-27)

In-game, Down = Swap sides worked but Up = Reset to middle and Down = Left corner left the
reset where it was, although the log showed the right decision (`action=2 skipGame=1`,
`action=3 skipGame=1`) and no "item not found".

The mod's own table walk passed the wrong `this` to the setter. Disassembly:

```
FUN_006CD5C0 (the game's find-by-name + set):
  006CD5CB  mov esi,17AE3BCh            ; walks NAMES, 0x38 apart; edi = index
  006CD61F  lea eax,[edi*8]; sub eax,edi
  006CD628  lea ecx,[eax*8+017AE3A8h]   ; this = 0x17AE3A8 + i*0x38 = name - 0x14
  006CD62F  call 006CD700
FUN_006CD700 (setter, this = entry BASE):
  [esi+4] flags: 4 -> optData+BBB8, 8 -> +BBBC, 0x10 -> +BBC0, 0x20 -> +BBC4
  else          optData + [esi+34h]*4 + A568
FUN_006CBF90 -> FUN_006D3510 (sorted lookup) returns
  006D369B  lea eax,[ecx*8+017AE3A8h]   ; same base
FUN_006CC0F0 (reader): same flag/index offsets as the setter
```

So the entry base is 0x14 bytes before the name; flags live at `base+4` and the index at
`base+0x34`. The mod passed the **name** pointer. `[name+4]` is the bytes `"Rese"`, and
`'R'` = 0x52 has bit 0x10 set, so every "middle"/"corner" write went to `optData+0xBBC0` -
one of the four flagged options, not TRI_ResetPosition. The game's own reset then read the
untouched TRI_ResetPosition and used the previous position, which is exactly what was seen.
Swap worked because its path writes nothing.

Reader and setter are the same slot (FUN_006CC0F0 mirrors FUN_006CD700), so the reset-time
reader FUN_006CBFE0 -> FUN_006CBF90 sees whatever the setter wrote; there is no second copy.
FUN_006D2D40 does nothing else besides that one setter call (it returns void, and
FUN_0055EF40 ignores it), so skipping it and writing the value is equivalent to the game's
own path.

Fix: the mod no longer walks the table. It calls `FUN_006CD5C0("TRI_ResetPosition", v)`
(`__stdcall`, 32-byte zero-padded name, exactly the call at 0x6D2E13..0x6D2E1C) and reads
the value back with `FUN_006CBF90("TRI_ResetPosition")`, logging
`[ResetSwap] TRI_ResetPosition <before> -> wrote <v> -> reads back <v>`.

Side effect of the broken build: while it ran, a "middle" or "corner" press wrote 0 or 2
into whichever training option lives at optData+0xBBC0. If a training menu option looks
changed after testing that build, that is why; resetting it in the training menu fixes it.

## The four settings

`ResetLabUpAction`, `ResetLabDownAction`, `ResetLabLeftAction`, `ResetLabRightAction`
(Settings window, Training). All default to **Default** = today's behaviour: Up swaps sides
(mod), Down = middle, Left/Right = that corner (game). Options: Nothing (plain reset, the
previous position stays), Reset to middle, Left corner, Right corner, Swap sides.

Priority when several directions are held matches FUN_006D2D40: Left, then Right, then
Down, then Up (which the game never reads).

| setting for the held direction | game's function | mod |
|---|---|---|
| Default | runs | Up only: swap via TickTrainingResetSwap |
| Nothing | **skipped** | nothing (previous position stands) |
| Swap sides | **skipped** | swap via TickTrainingResetSwap |
| Reset to middle | **skipped** | writes TRI_ResetPosition = 0 through the game's setter |
| Left / Right corner | **skipped** | writes 2 (left) / 1 (right), swapped when the training player is the P2 side, as FUN_006D2D40 does |

Exactly one of {game, mod} handles any press. The log line per press is
`[ResetSwap] reset pressed: held=0x.. dir=0x.. action=N swap=0/1 skipGame=0/1`.

## Caveats

- Directions are read only for the training player's controller; a second pad holding a
  direction does nothing, which is also the game's behaviour.
- The remap only exists while the SystemInputWrite hook is installed (the reset press edge
  comes from it), i.e. under `IsControllerHooksRuntimeAllowed()`.
- The menu-item table lookup is by name at runtime; if "TRI_ResetPosition" is not found the
  hook logs `[ResetSwap] TRI_ResetPosition item not found` and the middle/corner write does
  nothing (the game was skipped for that press, which is the same as no direction).

## Reports

`TrainingResetPosition*GhidraReport.txt` (1-7) in this folder are the headless-Ghidra
decompiles this was read from; `run_ghidra_training_reset_position.cmd` +
`ghidra_scripts/DecompileTrainingResetPosition*.py` regenerate them. Headless Ghidra can be
run from WSL with `JAVA_TOOL_OPTIONS=-Duser.name=Usuario` (the project is owned by the
Windows user name) and `-readOnly`.
