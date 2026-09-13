# Why a paused replay hides the game's own interface

Pausing a replay makes BBCF hide its input display — the scrolling A/B/C/D columns down both
edges and the two "Player 1 / Player 2" stick-and-button panels. The health bars, timer,
portraits and the replay seek bar stay. This is the record of how that was found, because five
plausible explanations were wrong first and a sixth was wrong in a way that passed its own test.

## What it is not

Each of these was measured in-game, not reasoned about:

* **`pIsHUDHidden`** — the mod's own HUD toggle. Unrelated; it never moves on a replay pause.
* **The element "visible" bit, and alpha.** A hook on the real per-element draw at
  `BBCF.exe+0x237E10` counted 150 elements reaching the draw every frame, paused or running,
  with the same number of them fully transparent either way. The elements are still processed.
* **`0x015C0358`** (`replayManager+0x64EE8`). This *is* read by the draw path, and for a while
  looked like the answer. It is the viewer's own manual hide-UI toggle, bound to a button; it
  stays 0 across a pause.
* **The `+0x2778` system draw flags** (`0x012F1ED0+0x2778`), bits 0 and 1, and the app mode at
  `0x00C903B0+0x108`. Both are read by the same early-out. Neither changes across a pause.
* **"The draws stop".** They do — D3D draw calls fall about 30% while the primitive count holds,
  which is exactly what many small quads disappearing looks like — but the call stack cannot say
  which ones. Everything in the game submits geometry through one `ID3DXEffect` pass loop at
  `0x004491CB`, fed by a deferred queue flushed in `0x00447680`, so a return-address histogram
  collapses to a single chain no matter how deep it is captured. Do not repeat that experiment.

The measurement that finally mattered was not a hook at all: **two screenshots**, the same replay
running and paused, taken well clear of the round-start animation. That is what named the missing
UI. Before that, everything measured was consistent with half a dozen stories.

## What it is

A memory diff over `.data` across a pause (three snapshots: two running, to learn what churns on
its own, then one paused) left two bytes that changed and were not churn:

```
0x015C0350  (replayManager + 0x64EE0)  0 -> 1      playback state; 1 == paused
0x015C0349                             0 -> 2
```

`replayManager+0x64EE0` is the playback state, read back by the getter at `0x0069D370`. It is
consulted by a single predicate at **`0x004D1FC0`**, which answers "should the interface be
hidden right now" for every piece of UI that can vanish:

```
004D1FC0  call 0069D2E0              ; GetReplayManager (singleton at 0x0155B470)
004D1FC5  mov  ecx,eax
004D1FC7  call 006CC4A0              ; app mode == 11 (Replay Theater)?
004D1FCC  test eax,eax
004D1FCE  je   004D1FE7              ; not Replay Theater -> other reasons
004D1FD0  call 0069D2E0
004D1FD5  mov  ecx,eax
004D1FD7  call 0069D370              ; [replayManager+0x64EE0], the playback state
004D1FDC  cmp  eax,1                 ; 1 == paused
004D1FDF  jne  004D1FE7              ; <-- the byte the mod flips
004D1FE1  mov  eax,1                 ; hide the interface
004D1FE6  ret
004D1FE7  ...                        ; several other reasons to hide it
```

Its callers include the input-info panel at `0x006CFA30` (which pushes the resource name
`TRI_InputInfo` immediately after the check), a second panel at `0x006D25AD`, and four small
"is this display on" getters at `0x00699BC0`/`BE0`/`C00`/`C20`.

## The fix, and the fix that looked like one

`0x004D1FC0` is not "should the interface be hidden". It is **"is the game paused"**, and the
world update asks it too:

```
005505D1  call 004D1FC0
005505D6  test eax,eax
005505D8  jne  005505DF      ; paused -> skip the update
005505DA  call 006A9DB0
```

Turning its `jne` at `0x004D1FDF` into a `jmp` does leave the input display up. It does so by
stopping the pause from ever happening: press pause, nothing pauses. **This was shipped to a test
run and passed**, because the test only took screenshots, and a running game photographs exactly
like a paused game with its interface restored. Assert the pause, never infer it.

What actually works is patching the branches that *consume* the answer to hide a display, and
leaving the answer alone:

| Site | Branch | What it gates |
|---|---|---|
| `0x00699BD0` | `jne` rel8 | display on/off getter, global `0x00A04B50` |
| `0x00699BF0` | `jne` rel8 | display on/off getter, global `0x00A04B44` |
| `0x00699C10` | `jne` rel8 | display on/off getter, global `0x00A04B4C` |
| `0x00699C30` | `jne` rel8 | display on/off getter, global `0x00A04B48` |
| `0x006CFA47` | `jne` rel32 | the input-info panel (pushes `TRI_InputInfo` on the next line) |

Each of the four getters is `if (its own on/off global && !IsGamePaused()) return 1;`. NOPing the
branch drops the `&& !paused` half and leaves the game's own on/off switch deciding, exactly as it
does while the replay plays. `src/Game/ReplayPauseHud.cpp` does that while
`ShowHudWhenReplayPaused` is on and restores the original bytes when it is off. The four getters
are byte-identical apart from their global and their relative call, so one signature finds all
four and the scan keeps going after each hit - a single-match scan would patch one and silently
leave three displays hidden. Globals and string addresses are wildcarded because the image is
relocated at runtime.

Nothing is written to game state. An earlier attempt in this investigation wrote to a field whose
meaning came only from a diff and crashed the game with an access violation; reading game memory
to decide, and patching only code whose meaning is known, is the rule that came out of it.

## Verifying it

The `bbcf-agentctl` harness drives this end to end without a human:

```
bbcfctl run boot main-menu replay-theater first-replay pause-shots
```

Two things make the result trustworthy, and both exist because their absence produced a false
pass:

* **The pause is asserted, not assumed.** The mod logs `playback=` on the `[State]` line, read
  straight from `replayManager+0x64EE0` - 0 playing, 1 paused - and `pause-shots` waits for it.
  As a second check, two screenshots taken a second apart while paused must be byte-identical.
* **The round intro is waited out.** `gameState` is 15 from the moment the match loads, and the
  next several seconds are the character intro and the round banner. The mod reports
  `round=intro` until the match timer first ticks down; `first-replay` waits for `round=live`.

With the setting off, the input display is gone from the paused shot. With it on, it is there and
the playback control shows the play triangle.
