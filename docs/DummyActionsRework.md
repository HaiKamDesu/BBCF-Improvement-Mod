# Dummy actions — how the training dummy is told what to do

The training page's "Dummy actions" list, the playback libraries behind it, and the pieces
that exchange playback files. Written for whoever touches this next.

Started 2026-09-08 as a rework of three overlapping features; the history and the reasoning
behind each decision are kept at the bottom, because most of it is the kind of thing that
looks arbitrary until you know why.

## What it replaced, and why it is one system now

Three features did overlapping jobs and none could be combined:

| | What it could do | What stopped it |
|---|---|---|
| Dummy actions panel | force one of the dummy's own script states | four hard-coded triggers, animations only |
| Recording slots | play a CF slot back | no triggers at all, manual |
| Unlimited Playback | fire a recorded playback on any of 7 triggers | **one trigger at a time** — its settings said "Selects the single trigger type that can fire library playback", and picking one cleared the rest |

Unlimited Playback's internals were the most complete (entry weights, three picking modes,
per-trigger cooldowns, compatibility probing, playback caching, the borrowed CF slot with its
pending restore), so it was generalised and the other two deleted rather than a fourth thing
added beside them.

## The model

Every one of the seven **triggers** independently holds a **list of actions** (since
2026-09-27; before that, exactly one). An action names a *source* — where its inputs come
from:

| Source | Payload | How it is delivered |
|---|---|---|
| From Input Notation | numpad text, parsed by `TasManager::TryParseCommand` | frames into a borrowed CF slot |
| From File | one `.playback` file | ditto |
| From CF Recording Slot | slot 1-4, read live when the trigger fires | the slot already holds it |
| Recording | a copy of a CF slot taken when the action was added (hex in the ini) | frames into a borrowed CF slot |
| From Animation | one or more of the dummy's script states, with per-entry delays | script-pointer write, no slot |
| Burst | On Hitstun only | action-override name, no slot |

So a wakeup DP, an on-hit burst and a looping blockstring can all be armed at once, which
neither of the old systems allowed.

A trigger with several actions **picks** one when it fires — random, in order, or random
without repeats (`DummyActionManager::Pick`, per-trigger `selectionMode`) — over the actions
that are switched on and runnable. Each action has an `enabled` switch (only shown, and only
consulted, once the trigger holds more than one) and each trigger has one of its own; a
switched-off trigger fades in the UI and `IsRunnable` is false for it. Removing a trigger's
last action removes the trigger.

That is what the **Playback Library** source used to provide, one detour further away: a
library was a file of recorded playbacks with per-entry switches and a picking order, and a
trigger could name one. A trigger's action list is the same thing without the file, and it
can mix recordings with notation, animations and anything else. So the library source is
gone from the menu (`Source_Library` survives only as a load-time value): an old file that
names one is unpacked into `Source_Recording` actions on that trigger, switches and picking
order carried over. The Playback Library *window* still exists for its own purposes (the
working set, capture, export); it is simply no longer how a trigger is fed.

The action rows keep everything the Playback Library's rows could do: ctrl/shift-click
selection (per trigger), drag to reorder (within a trigger), a right-click menu with Send to
CF Slot, Save to File, Rename (recordings), Move up/down, Set index, and Turn ALL on/off, and
a multi-row remove behind a confirmation. "From File" also accepts a library file (`*.upl`)
and unpacks it into recording actions, so a big library built under the old system moves
over in one pick. Rows past `DummyActionMaxRows` (default 6) scroll inside the trigger.

`UnlimitedPlaybackManager::ResolveTriggerAction(trigger, out, actionIndex)` picks (or takes
the named action) and resolves it; `ExecuteResolvedAction` runs it. A row's ">" button goes
through `PlayActionNow(trigger, index)`, deferred to the next pre-tick like the library's
play-now for the same one-frame-alignment reason.

## Where the code lives

| File | Responsibility |
|---|---|
| `Game/Playbacks/DummyActionManager.*` | the trigger table: what is assigned, its summary text, save/load |
| `Game/Playbacks/UnlimitedPlaybackManager.*` | firing: trigger conditions, resolving an action to something playable, the CF slot borrow, the loop, libraries |
| `Overlay/Window/DummyActionsPanel.*` | the rows, `+ Add Action`, `Clear All`, and the five source modals + the trigger settings modal |
| `Overlay/Window/UnlimitedPlaybackWindow.*` | the Playback Library window, and the **shared** library panel/popups both it and the modals draw |
| `Game/Scr/ScrStateNames.*` | turns `NmlAtkAIR2C` into `j.2C` for the move picker |
| `Game/Playbacks/PlaybackManager.*` | slot memory and `.playback` file IO |

### The one path that fires anything

`UnlimitedPlaybackManager::ResolveTriggerAction(trigger, ResolvedAction*)` is the single place
an action becomes something executable. Every source ends up either as a frame buffer for
`StartRuntimePlayback` or as a `scrState*` to force. `StartResolvedAction` then executes it.
Add a source by extending those two and the modal that configures it — nothing else needs to
know.

### Two flags that gate everything

- `DummyActionManager::IsRunnable(trigger)` — is this action configured enough to do
  anything.
- `UnlimitedPlaybackManager::TriggerConfig::enabled` — the first gate in `TryFireTrigger`.
  It means **armed**, for every source. `DummyActionManager::SyncTriggerEnable()` keeps it in
  step with the table and is called **every frame** from `Tick()`, not on edit: a modal that
  finishes configuring an action changes the answer, and hanging that off the UI is how a
  feature ends up only working while its menu is open.

## Rules worth knowing before you change this

### 1. Frame phase: never build a SnapshotApparatus from the game hook

`UnlimitedPlaybackManager::Tick()` and `RunPreTick()` are called from
`void __declspec(naked) GetFrameCounter()` in `hooks_bbcf.cpp`, between its `pushfd`/`pushad`
and the matching restore — i.e. from the middle of the game's own frame update.

`SnapshotApparatus`'s constructor NOPs three sites in the game's code and calls the game's
`maybe_network_stuff_init`. Doing that from the hook phase **crashes the game**, which is how
the playback loop died twice. `ScrWindow` carries the same warning for the save states.

The guard: `HookPhaseGuard` sets `m_inHookTick` for the whole of both functions (RAII, because
`Tick` has a dozen early returns), `CanBuildSnapshotApparatusHere()` refuses while it is set
and leaves a note, and `RunDeferredSetup()` — called from `WindowManager` right after
`DrawAllWindows()` and `RunPendingSaveStateRequests()` — does the work. Both construction
sites consult the guard, so a new path reached from the hook is covered without anyone having
to remember this.

`RunDeferredSetup` is also where the saved table is read from disk, for the same reason.

### 2. A trigger's start hotkey cannot live inside its running-loop path

The loop's start/stop key used to be polled at the top of `ProcessLoopTick`. Once that was
correctly gated on the loop actually running, the loop could no longer be started at all.
`ProcessLoopHotkey` is separate and polled whenever a loop is configured.

### 3. Triggers are independent

`Tick()` once read `if (m_triggers[Trigger_OnLoop].enabled) { ProcessLoopTick(); return; }`.
That flag means *armed*, not *running*, so merely configuring a loop skipped all six other
triggers. The loop is just another trigger; it only takes over while it is physically
resetting the lab (`LoopPhase_PositionSetup` / `LoopPhase_Ending`), where another trigger
would fight it for the CF slot.

### 4. UI that edits live state must copy on open and restore on cancel

The library editor and the trigger settings modal both *have* to work on the live object —
the firing code reads the config every frame, and the library panel is shared with the
Playback Library window so the two cannot be allowed to drift. So both snapshot on open and
restore on Cancel, and dismissing by clicking away takes the same path. Saving or loading a
**file** is not undone, because that already happened on disk. Three separate bugs came from
this shape; assume the next one will too.

### 5. Frame layouts: know which of the three you are holding

- A **playback slot in memory** is two bytes per frame: input, then an aux byte.
  `load_raw_into_slot` takes the frame count as `size() / 2`.
- A **`.playback` file** is one facing byte, then one byte per frame.
  `PlaybackManager::raw_to_trimmed` converts.
- `TasManager::TryParseCommand` returns `uint16_t` per frame only because its notation has a
  taunt bit above the byte. Playback cannot express taunt; the notation modal says so rather
  than dropping it silently.

Notation actions get a **4-frame neutral tail**, so a button is never the buffer's last frame
where it is at the mercy of the run being torn down.

### 6. Mirroring is the game's, not ours

The game flips a buffer whose stored facing differs from the side being played on. So
"mirror" means *leave the stored facing alone* and "do not mirror" means *overwrite it with
the current side*. Notation is authored as if facing right. `MirrorPlaybackInputsInPlace` is
pre-existing dead code — do not assume it is what mirrors anything. Auto-mirror is per
trigger, since a corner-specific setup and a side-agnostic reversal want different answers.

### 7. `scrState*` cannot outlive a character

Animation actions therefore hold their moves **by name** (`Action::animationNames`), which is
also what is saved to disk. `InvalidateAnimations` drops only the pointers;
`ResolveAnimations(states, ownerCharData)` rebuilds them, dropping a move the new character
does not have along with its delay. That is what makes a saved `NmlAtk5B` resolve to whoever
is loaded now.

It is reconciled **per frame**, gated on `NeedsAnimationResolve(ownerCharData)`, not only on
a character swap: actions loaded from disk arrive *after* the script was parsed, so there is
no swap left to hang the work on. The marker remembers which character was matched against,
so one that genuinely lacks the move is not retried every frame.

`Set()` derives the names from the pointers — but only when there **are** pointers, or it
wipes the names an action was just loaded with.

## Trigger conditions

All in `UnlimitedPlaybackManager`, all reading player 2. The numbers came from testing in the
panel this replaced; they are copied verbatim, comments included.

| Trigger | Condition |
|---|---|
| Wakeup | `currentAction == state && actionTime == n && lastAction != state`, over six states |
| Block Gap | `blockstun == 1` and a Guard state — its last frame, so the action lands on the first actionable one |
| On Block | rising edge of `blockstun > 0` |
| On Hit | `hitstun > 0` as a **level**, latched once per combo, refused while bursting or teching |
| Throw Tech | `timeAfterTechIsPerformed == 29` in a `LockReject` state |
| On Hotkey / On Loop | you press the key |

Two subtleties that are easy to undo by accident:

- **Wakeup's `lastAction != state` test is load-bearing.** Forcing an animation moves the
  script pointer *without changing* `currentAction`, so `actionTime` restarts and walks past
  the same number again — without that test the trigger re-fires forever. Match `==`, not a
  substring, or one state matches several.
- **On Hit is a level test, not an edge.** A hit landing on a dummy already in hitstun still
  arms it, which an edge would miss. It fires at the *start* of being hit for every source,
  because that is what lets a reversal or a burst come out of hitstun at all — inputs
  delivered then are buffered by the game.

### Negative delay ("fire early")

`TriggerConfig::delayFrames` is one signed field. Positive waits; **negative fires early**,
which is how a reversal's motion is delivered before the dummy is actionable so the attack
lands on the first frame it can (about `-3` for a `623C`).

Only three triggers can do it, because only three are conditions the game counts *toward*:
the block gap (blockstun counts down), wakeup and throw tech (both count up to an actionable
frame). The UI refuses a negative value on the others.

## Persistence

`BBCF_IM/dummy_actions.ini`, rewritten whenever an action changes, read once from
`RunDeferredSetup`. `[N]` is trigger N: its `enabled`, `selection` (picking order), `delay`,
`cooldown` and `mirror`. `[N.M]` is action M of trigger N: `enabled`, `source`, then the
payload (`notation`, `file`, `slot`, `moves`/`movedelays`/`naoto`, or for a recording
`slot`/`name`/`facing`/`frames` as hex). A `[loop]` section holds the loop's own settings; a
`[sizes]` section the modal sizes people have settled on; an optional `[training]` section
holds key=value extras the caller owns — the training page uses it for positions, wake-up
timing and input delay when it exports the page as a **training file** (`*.bbcflab`, see
`PageTraining.cpp`). `ExportTo`/`ImportFrom` write and read that same format at any path;
`ResetToDefaults` wipes everything.

Old files (one action per `[N]` section, `source=` in the trigger section itself) still load:
`source=` starts an action wherever it appears, and a `Source_Library` action is unpacked as
described above.

Two things are deliberately not stored as-is:

- **Notation is saved as text and re-parsed on load**, so a change to the parser or the frame
  layout cannot leave a stale byte buffer behind.
- **Animations are saved by name** — see rule 7.

Loading logs a line per trigger and per action (`source`, `enabled`, `runnable`, `summary`)
so a round trip that drops something says which part went missing.

## Which side is the dummy

Nothing may read `g_interfaces.player2` and mean "the dummy". `TrainingSide::Dummy()` /
`Human()` (`src/Game/TrainingSide.h`) resolve from the game's own training-side index
(BBCF.exe+0x891A38, 0 = P1 is the human), so picking Player 2 on the training character
select or taking a replay over as P2 keeps every trigger, animation and script parse on the
right character. Before this every dummy feature silently acted on the human when P2 was
chosen.

## Libraries are per trigger (historical, removed 2026-09-27)

`m_triggerLibraries` and `SetEditTarget`-based sharing of the library UI with the trigger
modal are gone with the library source. `SetEditTarget()` remains for the Playback Library
window's own working set. The lesson that outlived it: UI that edits live state must copy on
open and restore on cancel (rule 4).

## Modal sizing

Modals measure their own body (`EndModalBodyAndSeparate` records it) and fit to it on the
next frame, re-centring as they resize — appearing-only centring positions a window for the
provisional size and leaves it off-centre after it grows. The outer window carries
`NoScrollbar`, since the footer is pinned and the body child is the scroller. A size the user
settles on beats the fit, and is saved.

`ImGuiChildFlags_AutoResizeY` is a **child** flag; passing `ImGuiWindowFlags_AlwaysAutoResize`
to a child does nothing (it silently left a blank row inside the Add Playback Entry box).

## Playback files as a shared currency

`PlaybackManager::save_playback_to_path` / `load_playback_from_path` / `raw_to_trimmed` exist
because the originals hardcode `./slots/` and append `.playback`, which is no use once a file
picker chooses the location.

Three features exchange playbacks through that format:

- **Import / Export Playback** (Training page) — import reads and *validates* the file before
  asking which slot, so an unreadable file is refused rather than becoming a slot choice that
  then fails.
- **Capture playback from replay** (Replays page) — replaces the "Add from replay" button
  removed from the library panel. `StopReplayRecordingToBuffer` hands the capture back rather
  than filing it, deliberately: the recording must end when the user says so, because the
  replay keeps playing while a file dialog is up and the captured range would grow by however
  long they spent naming the file.
- Libraries and dummy actions, which can load such a file directly.

## What this made obsolete

- `PlaybackEntry::triggerEnabled[7]` — which entries a trigger may use is now "which library
  is assigned". Old files still parse; the field is read and discarded.
- The library window's trigger-type combo, which wrote `enabled` across all seven triggers.
- `UnlimitedPlaybackManager::Mode` (`m_mode`) — forced to `Mode_Unlimited`, read by nothing.
- `ScrWindow::DrawDummyActionsBody` and `DrawRecordingSlotsBody`, deleted with the members
  only they used (~735 lines).
- Burst-on-hit and the Naoto EN toggle became a source and part of the animation payload
  respectively, rather than loose checkboxes.
- `m_autoMirrorOnSideSwap` is vestigial: auto-mirror is per trigger now. It is still read from
  library files for compatibility, but nothing sets it.

## Known gaps

- **Hotkey rebinds are not undone by Cancel** in the trigger settings modal. They are global
  bindings rather than part of the trigger, and the modal says so.
- **The library window still shows a trigger-config block** for the first library-backed
  trigger, now duplicated by the row's gear menu. It should go.
- **The replay-capture modal in `UnlimitedPlaybackWindow`** is dead UI kept only because its
  recording logic is shared. Worth deleting once the Replays page version is settled.
- Animation actions pick at random when several are assigned, matching the old panel. There
  is no weighting, unlike libraries.

## History, for context

- Stages 1-4 (extract `PlaybackLibrary`; several libraries at once; the trigger table and
  modals; trim the library window and reorder the training page) all landed together.
- Committed and tested separately beforehand: `ScrWindow::TickDummyActions` moving the
  dummy's per-frame work out of the draw path so it runs with the menu closed, plus
  `EnsureDummyScriptFresh` to stop a stale `scrState*` surviving a character swap, and the
  save-state setup countdown drawing with the menu closed (`0de73cc`).
- Bugs whose causes are already written up as rules above: the notation frame layout, the
  `enabled` flag meaning, the loop's frame phase, the loop's monopoly on triggers, burst
  needing a parsed script, the library popups, wakeup's re-trigger loop, and the three faults
  that made a saved animation action come back empty.
