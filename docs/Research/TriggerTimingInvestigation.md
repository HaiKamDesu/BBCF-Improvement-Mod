# Dummy-action trigger timing: where each window begins and ends

Investigated 2026-09-27, for the report that the wakeup trigger's negative delay "works on
some wakeups and not others". Everything here is from the shipped scripts (`data/Char/
char_XX_scr.pac`, decoded with `tools/bbscript_dump.py`) and the fields the mod already reads
off the dummy; no new game hooks were needed.

## The bug, in one table

The old `ShouldTriggerWakeup` fired when `actionTime` hit a hand-tuned constant per state.
The states are fully committed animations whose length is **per character**:

| state | constant | real length (frames of sprites) |
|---|---|---|
| CmnActFDown2Stand (face-down getup) | 14 | 16 (Amane) … 62 (Tager); Ragna 22 |
| CmnActBDown2Stand (face-up getup) | 14 | 22 (Bullet) … 81 (Tager); Ragna 28 |
| CmnActUkemiLandN (neutral tech) | via Landing=1 | 16 … 33 |
| CmnActUkemiLandF / B (rolls) | 30 | not in the script (see below) |
| CmnActUkemiStagger | 7 | 8 … 15 |
| CmnActLockReject (throw escape) | 29 (`timeAfterTechIsPerformed`) | 24 (Noel) … 48 (Jin) |

So with delay 0 the face-down constant fired 8 frames before Ragna could act, 2 frames
before Amane could, and 48 frames before Tager could. A negative delay only shifted that
already-wrong point. The throw-tech "29" was 4 frames **late** for Ragna and 19 frames early
for Jin.

Full per-character table (frames of sprite time; `*` = the state contains commands other
than sprites, e.g. a sound or a flag, none of which change its length):

```
tag  FDown2Stand  BDown2Stand  UkemiLandN  UkemiLandNLanding  UkemiLandF  UkemiLandB  UkemiStagger  LockReject  AirLockReject
rg   22           28           20          15                 220         224         8             25          23
jn   39           27           27          12                 227         224         8             48          35
no   18           30*          27          12                 223         224         8             24          18*
rc   34           51*          20          15                 223         232         15            32          34
tk   23           28*          24          15                 220         234         13            30          26
tg   62           81           33          12                 221         222         10            33          19
lc   30*          36*          31          21                 220         224         8             29          29
ar   20*          26*          31*         39*                220*        224*        8             29*         29*
bn   24           28           24          15                 220         224         8             24          29
ca   31           31           20          15                 224         224         10            24          20
ha   22           28           31          15                 222         227         8             29          29
ny   20*          28*          18          15                 222         224         8             29          29
tb   22           28           16          15                 220         224         8             27          21
hz   36           38           18          18                 224         224         8             35          33
mu   20           28           18          15                 222         224         8             29          29
mk   22           28           20          15                 216         224         13            25          21
vh   36           28           20          15                 224         224         10            25          23
pt   22           26           18          18                 222         230         13            30          20
rl   27           31           20          15                 227         227         10            27          18
iz   20           28           20          15                 220         224         8             24          29
am   16           24           20          15                 220         224         8             30          30
bl   22           22           20          15                 220         220         8             28          29
az   22           26           20          15                 222         222         8             29          29
kg   22           28           20          15                 220         224         8             25          28
kk   24           28           20          15                 220         227         8             27          29
tm   36           38           18          18                 224         224         8             35          33
ce   22*          27*          18*         15*                220*        224*        9*            30*         28*
rm   20*          28*          18          15                 222         224         8             29          29
hb   20           26           18          15                 218         224         8             27*         29*
ph   22           26           28          15                 220         220         8             30          30
nt   24           28           20          15                 220         220         8             25          23
mi   30*          31*          24*         15                 227*        227*        8*            29          29
su   22           28           16          15                 220         224         8             29*         29*
es   27           28           32*         15                 218         218         8             29          30
ma   27           28           18          15                 222         222         8             29          29
jb   22           28           22          15                 230         230         8             29          29*
```

The tags are the `char_XX` archive tags (see the palette notes: Nu=ny, Lambda=rm,
Izanami=mi, Nine=ph, Mai=ma).

## What the scripts look like

A getup is nothing but sprites and a sound:

```
_State(CmnActFDown2Stand)
sprite(rg064_00,2) … sprite(rg064_10,2)
PrivateSE(rgse_00)
endState()
```

No cancel windows, no `upon`, no branches. The dummy is committed until `endState`, then the
engine drops it into `CmnActStand`. The same is true of `CmnActBDown2Stand`,
`CmnActUkemiLandN`, `CmnActUkemiStagger`, `CmnActLockReject` and `CmnActAirLockReject`.

The rolls are different:

```
_State(CmnActUkemiLandF)
sprite(rg110_00,2) … sprite(rg110_07,2)      14 frames of rolling out
sprite(rg110_08,200)                         held until the engine lands the roll
sprite(rg110_09,2) sprite(rg110_10,2) sprite(rg110_11,2)
endState()
```

The 200-frame hold is a wait, not a length: the roll is an air arc and the engine advances
past the held sprite on landing. So the script cannot tell us how long a roll is, and the
mod's parser (which sums sprite lengths into `scrState::frames`) reports 220-234 for it. The
old constant 30 was a guess at where that landing falls.

`CmnActUkemiLandNLanding` (12-39 frames) is in `PlayerExtendedData`'s idle list and its own
comment says it is actionable from its **second** frame. So the neutral tech's actionable
frame is the landing's first frame, not the end of the landing.

## The first-actionable rule (proven from the disassembly, 2026-09-27, third pass)

This replaces the "script end" and "learned" guesses of the earlier passes. Everything below
was read from BBCF.exe with headless Ghidra (`docs/Research/ghidra_scripts/
DecompileActionableGate.py`, `DumpVtableSlots.py`, `DumpRefsTo.py`, `FindDwordAnywhere.py`,
`FindStringRefs.py`) and `tools/bbcf_disasm_ascii.txt`. Every character runs on the one
class `OBJ_CCharBase` (vtable 0x9565F4; no other vtable holds its move check 0x5D11F0 or
its getup handlers), so the rule is the same for the whole cast - only script lengths differ.

### Where a move gets allowed

1. **The gate.** `FUN_005D0690(this = CharData, move entry)` (move entries 0x2B4 bytes at
   CharData+0x5B28) accepts a move from a non-neutral state only through
   `(CharData+0x2264 & mask(type)) != 0`, where the type is the entry's +0x44 set by
   `Move_Register` (`FUN_0059C050`, vtable +0xB28: `Move_Register(name, 2)` -> type 2,
   normals 0x1xx..0x4xx -> type 1, distortions 0x606 and astral 0x607 -> type 2) and
   the switch at the end of the gate maps type -> bit:

   | type | what | bit in +0x2264 | setter (bbcmd, VA) |
   |---|---|---|---|
   | 1 | normals | 0x1000 | 13014 WhiffNormalCancel, 0x5CE6D0 |
   | 2 | specials, distortions, astral | 0x2000 | 13015 WhiffSpecialCancel, 0x5CE760 |
   | 4 / 5 | walk forward / back | 0x4 / 0x20 | 13002 0x5CE620 / 13005 0x5CE450 |
   | 6 / 7 | dash forward / back | 0x8 / 0x40 | 13003 0x5CE600 / 13006 0x5CE430 |
   | 8..13 | jumps | 0x100 | 13008 WhiffJumpCancel, 0x5CE670 |
   | - | overdrive | 0x80000000 | 13031 WhiffOverdriveCancel, 0x5CE4D0 |

   The neutral path (move flag 0x400, taken only when CharData+0x260 bit 0x1000000 is clear)
   is closed for the whole knockdown-to-getup sequence: the knockdown and tech handlers set
   0x260 |= 0x1000000 at state entry (`FUN_005BE340`/`FUN_005C0640` phase 0), and only a
   state entry clears it (`FUN_005975C0`: `0x260 &= 0xFEFD9787`). So for every state in
   `kTimedStates` the mask +0x2264 is the whole story. (The trace's f260=01000000 all the way
   through the getup is exactly that.)

2. **Reset at every state entry.** `FUN_005CF1E0` (state entry, vtable +0x4C) -> `FUN_005975C0`
   -> vtable +0xADC = `FUN_005D2790` (13038 EnableAll) with 0: +0x2264 = +0x2268 = 0. The
   previous value is kept in +0x226C (used only for gatling/cancel-level paths).

3. **Set by the state's common-action handler.** `FUN_00582670` (vtable +0x44, "run upon
   event n") -> `FUN_005CE050`: on event 0 it looks the state name up in the 125-entry table
   at 0x123C670 (copied at static init 0x8408B0 from the CmnAct* name strings) into
   CharData+0x5B24, then `FUN_005D18D0` calls vtable +0x10C0.. per common action, passing the
   event number as the phase (0 = entry, 3 = every frame).

4. **Order inside one frame** (`FUN_005746D0`, vtable +0x14, the character's frame update):
   vtable +0x1C = `FUN_00581B50` (script step `FUN_0058B440`, then actionTime (+0x160) += 1,
   then event 3 -> the handler), then vtable +0x24 = `FUN_005D11F0` (the move checks, which
   end in `FUN_005B6DE0` -> `FUN_005C55C0` -> the gate), then, while a state is queued in
   +0x2040, the step and the check again (up to 10 times). So a bit a handler sets "at
   actionTime K" lets a move start ON frame K: the move's own frame 1 replaces the state's
   frame K, which is what the trace shows (AntiAirC at=1 on the frame the getup's frame 21
   would have been).

5. **End of a script.** `FUN_0058B440` fires event 8 and vtable +0x60 when the next command
   is `endState` and the current sprite's counter is on its last frame, so a state of L
   sprite-frames ends at the start of frame L+1, where the next state (CmnActStand, whose
   handler `FUN_005BD2A0` enables every bit at entry) is entered and the move check runs
   in the same frame. A state whose handler enables nothing is therefore actionable on
   frame L+1 exactly.

### The handlers, per timed state

| state (id, vtable slot) | handler | what it enables, when |
|---|---|---|
| CmnActFDown2Stand (0x33, +0x1158), CmnActBDown2Stand (0x2C, +0x113C) | `FUN_005BE340` (phase, face) via 0x5BB630 / 0x5BA730 | entry: 0x225C \|= 0x60000, 0x260 \|= 0x1000000, 0x25C \|= 0x1000. **actionTime == 14** (`cmp [ebx+160h],0Eh`, 0x5BE3D3): WhiffSpecialCancel (0x2000), 0x10000, and clears 0x225C bit 0x20000 (the trace's f225C 00060008 -> 00040008). **actionTime == 20** (0x5BE40B): FDash, BDash, normals, specials, overdrive, jump |
| CmnActUkemiLandN (0x46, +0x11A4) | `FUN_005C0640(phase, 0)` | nothing (entry only). Physics hop: ends on landing |
| CmnActUkemiLandNLanding (0x47, +0x11A8) | `FUN_005BD580` | **actionTime == 2**: overdrive, 0x100000, 0x10000, jump, specials, normals, FDash, BDash; actionTime > 5: walks |
| CmnActUkemiLandF / B (0x44 / 0x45) | `FUN_005C0640(phase, +1 / -1)` | no move bit, ever. Every frame: `if (sprite length > 199) sprite length = 25 - actionTime` - the script's 200-frame hold is cut so the next sprite starts on frame 25 (frame s+1 if the hold starts on s > 24) |
| CmnActUkemiStagger (0x40, +0x118C) | `FUN_005BD6C0` | entry: overdrive and 0x10000 only (no phase 3) |
| CmnActLockReject / CmnActAirLockReject (0x61 / 0x63) | `FUN_005BFF10(phase, air)` | nothing; phase 3 only feeds a countdown (29 - actionTime) to vtable +0x830 |
| *GuardLoop (e.g. 0x49 +0x11B0) | `FUN_005BF1D0` | every frame: walks, dashes, normals, specials, jump = (blockstun +0x2274 == 0) |

Bit 0x10000 (13019) is not a move type in the gate; it opens `FUN_005B87D0`'s input-0xDD
state change, not a move-table entry. Bit 0x100000 (13021) is read by `FUN_005B71C0` (turning).

### So: the first frame a move can start

| state | first special | first normal / dash / jump | game data it needs |
|---|---|---|---|
| CmnActFDown2Stand, CmnActBDown2Stand | **14** | **20** (L+1 if the getup is shorter: Amane face-down, L=16 -> 17) | none (engine constants); L caps it |
| CmnActUkemiLandN (neutral tech) | landing frame 2 | landing frame 2 | the hop's length (physics: measured 31, then this character's own) |
| CmnActUkemiLandNLanding | 2 | 2 (walk 6) | none |
| CmnActUkemiLandF / B | L+1 | L+1 | L = max(25, pre+2) - 1 + post, pre/post = script frames before / after the held sprite |
| CmnActUkemiStagger | L+1 | L+1 | script L |
| CmnActLockReject, CmnActAirLockReject | L+1 | L+1 | script L |
| block gap | first frame blockstun is 0 | same | blockstun b -> b frames on (window frame b+1) |
| hit recovery | unchanged model (first frame hitstun is 0); not proven: the hit handler `FUN_005C0280` changes state when actionTime > CharData+0x938, which is its own counter | | |

Roll lengths from the shipped scripts (L; first actionable L+1): 30 for almost everyone,
except LandF: mk 26, hb 28; LandB: rc 33, tk 35, pt 31; jb 31 both. The old constant 30 was
right for most of the cast by luck.

**Checking hypotheses (a)/(b):** (a) is right for specials - the frame the 0x20000 bit
clears is the frame WhiffSpecialCancel is set, and a special can start ON frame 14 (the
same handler call, same frame). (b) is wrong: nothing happens at the final sprite. Normals
come at frame 20, which for Ragna's 22-frame face-down getup is 2 frames before the end, but
that is a coincidence of Ragna's length: it is 20 for everyone (Tager's 62-frame getup lets
normals out 42 frames before the animation ends).

**Against the trace:** 4 on frame 19, 2 on 20, 1+C on 21; specials allowed since 14, so the DP
starts the frame its C arrives: AntiAirC at=1 on game frame 527 = getup frame 21. The
delay-0 run of the old code (first input "6" on frame 23 after a 22-frame getup) came out as
CmnActFWalk: walks are never enabled by the getup handler, so the only way to walk was the
script end -> CmnActStand on frame 23. Both consistent.

### The input buffer

`FUN_005CA700` (called from the battle-input writer `FUN_00559FE0`) rebuilds a byte table of
input codes every frame at CharData+0x1E79C (+0x10A per facing); moves test codes through
vtable +0x1284 (`FUN_005CA1E0`). Per button (A..D, 0x1000..0x8000 in the raw input) there is
an age counter at CharData+0x1E738 + 4*i: 1 on the press edge, +1 each frame the button stays
held, **0 the frame it is released**. The codes:

- press edge this frame (C: code 0x14);
- `FUN_0057E860`-style "pressed within 5 frames": `0 < age <= 5` (C: code 0x16 at
  +0x1E7B2) - Ragna's AntiAirC is `Move_Input_(173)` + `Move_Input_(22 = 0x16)`.

So a button **held** from its press is valid for 5 frames (the press frame and 4 more). A
one-frame tap - what a notation like `623C` plays, since the playback's next frame has no C -
resets the counter on the next frame and is **not buffered at all**. The motion part is more
forgiving: code 173 is motion 1 of the table at 0x9F5A00 (`FUN_005532F0`), most recent step
first: 3 within 7, 2 within 12, 6 within 8 frames of input history.

What that means for a negative delay: the button of the move must land ON or AFTER the first
actionable frame (or be held into it). With `623C` (3 frames) the only delay that puts the
DP on the first special frame is **-2**; -4 puts the C two frames before it, where it is
thrown away, and the DP does not come out on that wakeup at all (unless the button is held).

### How the mod uses it

`kTimedStates` carries a `TimingRule` per state; `PredictActionableAt(dummy, state, src,
normals)` applies the table above (normals=false: the earliest move of any kind, which is
what the trigger and the actionable marker use; normals=true: the normal/dash/jump frame, for
the log and the probe). `PredictStateFrames` gives L from the script (rolls with the held
sprite cut, using `scrState::longest_sprite_offset`), the neutral tech from measurement.
Observation is only a cross-check now: every exit from a timed state logs

```
[UP][TIMING] left <state> after its frame K into <next> on game frame F (a move|neutral)
[UP][TIMING] <move> started on frame K+1 of <state> (char C); rule: first move possible on frame P via <rule>
[UP][TIMING] MISMATCH: <move> started out of <state> (char C) on its frame K+1; the rule says no move before frame P via <rule>
[UP][TIMING] MISMATCH: <state> (char C) ran N frames, the rule says L via <source>
```

The "left X after its frame 1 into X" line of the first run was a bookkeeping bug (the exit
was checked after the previous state had already been overwritten with the new one); fixed,
and the exit into AntiAirC is now what gets checked.

`[UP][TRACE]` also prints `f2264`, the mask itself, so the frame the bits appear is visible.

### The probe (settings.ini `TriggerTimingProbe=1`, Debug; off by default)

On every timed-state window of the dummy (and the block gap) it plays A, B, A, B... through
the same runtime playback the triggers use (`StartRuntimePlayback`, dedicated slot 4), so
every frame is a new press edge, and logs

```
[UP][PROBE] started on <state> (char N) at game frame F: ... rule predicts a normal on frame Y (specials from frame Z, which A/B cannot show)
[UP][PROBE] <state> (char N): first move started on frame X of the state, rule predicted Y - OK|MISMATCH (move NmlAtk5A at=1)
```

A/B are normals, so for a getup it measures the **normal** frame (20); the special frame (14)
is checked with the wakeup trigger (below). The first press lands on the window's frame 2
(the playback is consumed from the next tick), which is no limit for any state here. Armed
triggers are refused while the probe is on (`[UP][GATE] ...: the trigger timing probe is on`).
Hit recovery is not probed: button presses in hitstun are techs and would change what is
being measured.

## Definitions the code now uses

Three moments per trigger, exposed per frame as `UnlimitedPlaybackManager::TriggerFrameEvents`
(`GetTriggerFrameEvents()`, with `GetTriggerFrameEventsFrame()` saying which game frame they
describe). They are computed every frame of a training match whether or not the trigger is
armed, so the frame history bar can show them.

- **windowStart** – the state or countdown began this frame. This is the earliest frame any
  negative delay can fire on; a lead longer than the window fires here.
- **actionable** – the first frame a move can START (see "The first-actionable rule"; the
  older "last frame the dummy cannot act" convention is gone). Delay 0 puts the first
  input on it.
- **detected** / **fired** – the condition (with the configured delay) was met, and the
  action actually started. They differ only with a positive delay.
- **framesToActionable** – the prediction a negative delay is measured against: `N` means
  the actionable frame is `N` frames away, `0` on it, `-1` outside a window or when the
  length is unknown. A negative delay `-L` fires when this equals `L + 1`, so the first
  input lands `L` frames before the actionable frame.

Per trigger (superseding the second pass; the rule column is the table above):

| trigger | window | actionable | prediction source |
|---|---|---|---|
| Wakeup: F/BDown2Stand | state entry | frame 14 (the first special; normals 20) | engine constants in `FUN_005BE340` |
| Wakeup: UkemiLandN | state entry | UkemiLandNLanding frame 2 | measured hop, exact once it lands |
| Wakeup: UkemiLandF / B (rolls) | state entry | L+1 | script + the handler's cut of the held sprite |
| Wakeup: UkemiStagger | state entry | L+1 | script |
| Throw tech: LockReject / AirLockReject | state entry | L+1 | script |
| Block gap | blockstun rising (or topped up) | first frame blockstun is 0 | blockstun (the guard handler re-enables moves at 0) |
| Hitstun end | hitstun rising (or topped up), not knocked down / teching | first frame hitstun is 0 | hitstun (unchanged, not proven) |
| On Block, On Hit | the rising edge itself | same frame | n/a; negative delay is meaningless, as documented |
| Key press, Loop | – | – | only `fired` is stamped |

Entry is detected as a change of `currentAction`, never a rewind of `actionTime`: forcing an
animation restarts `actionTime` without changing the state name, and reopening the window
on that is exactly how the old table fired forever after its own reversal came out. The
`lastAction != currentAction` guard from the old code is kept for the same reason.

### Frame ordering (proven 2026-09-27, replaces the earlier assumption)

Where `Tick()` sits in a game tick, read from `tools/bbcf_disasm_ascii.txt`:

```
FUN_0056B1F0   per-tick Update (the RunPreTick hook is on its entry)
  0x56B52D  call FUN_005521B0
              walks the object list from the battle scene, FUN_005DA7A0 per object
              (0x5522C0 loop) - the players' scripts advance here: currentAction,
              actionTime, hitstun, blockstun are frame N+1's values from this point on
              0x552322  call FUN_004D27D0   task-container step, its ONLY caller
                FUN_004D27D0: for each of 2 child tasks -> call FUN_004D2400 (0x4D27E6, ONLY caller)
                  FUN_004D2400:  mov eax,[esi] ; inc [esi+0Ch]     <- GetFrameCounter hook here
                                 call [eax+10h]                    <- the task's update
```

The hook pattern `8B 06 FF 46 0C` is those first two instructions (VA 0x4D2409), so
`UnlimitedPlaybackManager::Tick()` runs with the task's counter still at N while the
players already hold frame N+1's state; the counter becomes N+1 right after, and the task
update (`[vtbl+0x10]` -> `FUN_005613E0` <-> `MatchInfo::update_Fight FUN_0055F780`, each the
other's only caller, so reachable only through that vtable) runs the round flow. The render
pass (frame history) then sees counter N+1 with that same state.

Consequences:

- `Tick()` sees the state of the frame being simulated, not the previous one: `actionTime`
  is 1 on a state's first frame from the trigger code's point of view too. No off-by-one to
  absorb.
- Trigger events stamped with counter N describe the history column sampled at counter N+1.
  `FrameHistory::updateHistory` matches on `eventsFrame == currentFrame - 1` (and logs
  `[FrameHistory] trigger events run 1 frame(s) behind the counter` once).
- `g_gameVals.pFrameCount` is `esi+0xC` of whichever child task last went through
  `FUN_004D2400`; the "reset when the frame number moves" logic in `UpdateTriggerTiming`
  keeps a second child harmless.

### Marker convention in the frame history bar

The bar's newest frame is on the **left** by default (older frames shift right as they
arrive, as it has always been); `FrameHistoryTimeDirection=1` pins the newest frame to the
right edge instead so it reads oldest-first. Counting squares between markers works either
way; only the direction flips. Triangles sit above the dummy's block, **centred on the square of the frame they
belong to**. White = window began, yellow = first actionable frame (the first frame a move
can start), green = the frame the trigger's code decided to go, cyan = the first frame the
action's inputs are applied (the frame after green). With a -N delay the cyan marker is on a
square N steps to the left of the yellow one, and the move's first frame is the yellow square
when it comes out as early as the game allows.

Checked against the trace: the window opens on game frame 507 (getup at=1), so with the
rule's frame 14 the yellow marker is on the column of events frame 520 (at=14), and with the
old X=21 it would have been 527 - the column where AntiAirC is at=1, i.e. the DP's first
column. The trigger fired on 524 (green), its first input (`in=0x004`) is on 525 (cyan), and
the log line reports the columns between them. No FrameHistory change was needed for the
rule: the markers follow `m_timedActionableFrame`.

### Self-verifying log lines

```
[UP][TIMING] window opened: <state> (char C) frame=F actionTime=1 length=L via <src> firstActionable=frame A of the state (game frame G) via <rule>; normals from frame N
[UP][TIMING] Wakeup fires: state=<state> actionTime=T on game frame F; first input on game frame F+1; first actionable frame A of the state (game frame G) via <rule>; lead=N (delay -N); ...
[UP][TRACE] f=<frame> <dummy state> at=<actionTime> pbPos=<playback cursor> in=<input> f2264=<move mask> f25C=.. f260=.. f225C=.. f516C=.. f24974=..
[UP][TIMING] first actionable frame of <state>: game frame G, dummy now in <state> (actionTime n)
[UP][TIMING] left <state> after its frame K into <next> on game frame F (a move)
[UP][TIMING] <move> started on frame K+1 of <state> (char C); rule: first move possible on frame A via <rule>
[FrameHistory] marker fired (first input applied): trigger 0 on column X (game frame F+1, ...)
[FrameHistory] marker actionable (first frame a move can start): trigger 0 on column Y (...) - N column(s) after the fired marker
```

### Neutral tech is physics, not sprites

`CmnActUkemiLandN` is a hop; the engine leaves the state on landing, and its handler
(`FUN_005C0640` with direction 0) enables nothing. Ragna's script sums to 20 frames, Tager's
to 33, and both measured 31 in game. `PredictStateFrames` uses 31 until the character's own
hop has been seen, and the landing (`CmnActUkemiLandNLanding`, actionable on its frame 2)
re-anchors the actionable frame exactly when it happens.

## Verdict on negative delay, per trigger

- **Wakeup** – the first actionable frame is now the engine's, the same for everyone: a
  special on frame 14 of either getup, a normal on 20. Rolls, staggers and techs use the
  script-end or landing rule. Because a one-frame button tap is not buffered, the delay that
  lands a motion + button on the first frame is exactly -(motion length - 1): -2 for 623C.
- **Throw tech** – script end, exact.
- **Block gap / hitstun end** – the counters are the game's own countdown. Both pause during
  hitstop, so a lead is in countdown steps.
- **On Block / On Hit** – instantaneous edges; a negative delay cannot mean anything.

## What is deliberately not a wakeup

Air techs (`CmnActUkemiAirF/B/N`, 6-15 frames) are not in the wakeup list; a trigger that
fired on every air tech in a combo lab would be a different feature ("On Air Tech"). They
would slot into `kTimedStates` in one line each if wanted.

## Files

- `src/Game/Playbacks/UnlimitedPlaybackManager.*` – `kTimedStates`, `UpdateTriggerTiming`,
  `PredictStateFrames`, the rewritten `ShouldTriggerWakeup` / `ShouldTriggerThrowTech`,
  and the `TriggerFrameEvents` API. `DummyData()` is the one place the dummy is read from.
- `src/Game/Scr/ScrStateEntry.h`, `ScrStateReader.cpp` – `scrState::longest_sprite` and
  `longest_sprite_offset`, so a held-sprite state can be told from a fixed one and a roll's
  real length computed.
- `src/Core/settings.def`, `src/Overlay/Window/SettingsIniWindow.cpp` – `TriggerTimingProbe`.
- `docs/Research/ghidra_scripts/DecompileActionableGate.py`, `DumpVtableSlots.py`,
  `DumpRefsTo.py`, `FindDwordAnywhere.py`, `FindStringRefs.py` – the headless Ghidra scripts
  this pass used (decompile a list of addresses; vtable slots; refs; raw dword search).
- `src/Overlay/Window/ScrWindow.cpp` – `DummyFeaturesInUse` also asks
  `NeedsDummyScriptForTiming()`, so arming a wakeup or throw-tech action gets the dummy's
  script parsed (previously only animation/burst did). A consumer that wants the events with
  nothing armed calls `SetTimingEventsWanted(true)`.
- `tools/bbscript_dump.py` – the offline decoder used for the tables above.
