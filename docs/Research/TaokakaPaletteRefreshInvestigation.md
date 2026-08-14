# Palette redraw / native-color-index investigation

**Read this whole document before changing `CharPaletteHandle::UpdatePalette`,
`ReplacePalData`, `ReplaceSinglePalFile`, `OnMatchInit`, or anything native-refresh-related.**
Four different approaches have been tried here and each failed in a *different* way. This doc
exists so nobody (human or agent) re-derives and re-tries an approach we already know is broken.

**STATUS AS OF 2026-08-14: RESOLVED — two separate bugs, both caused by the mod, both fixed. Read
"Resolution" and "Bug 2" at the bottom of this document before anything else here.** Everything
between this line and those sections is the record of a four-attempt investigation that chased a
phantom. The conclusions in Attempts 1–4, including the ones labelled "conclusive," did not
survive. They are kept verbatim, wrong parts included, so nobody repeats them.

All addresses below are **static VAs** (image base `0x00400000` unless noted). Subtract the
appropriate base for the `bbcf_base_adress + RVA` form used by `GhidraDefs.h` and the hook code.

---

## The two bugs this has to fix *simultaneously*

1. **Okammunist's bug** (root-caused and fixed 2026-07-06, commit `c8c9b19`): `UpdatePalette()`
   hot-swaps the native color-index byte to force a redraw, but that flip permanently mutates the
   byte in game memory. A *later* `OnMatchInit` (e.g. changing matchup in Training without
   reselecting a color) blindly trusted that mutated byte as "the player's real native color
   choice" and looked up the wrong `palettes.ini` slot — silently applying the wrong custom
   palette (or none).
2. **Scumplank's bug** (reported 2026-08-05): custom palettes bound via `palettes.ini` fail to
   *visually* apply at round start for Taokaka specifically (other characters tested — Makoto,
   Hakumen, Ragna — always worked), independent of which engine player-slot she's in, and gets
   fixed mid-match by unrelated events (freeze-frame pause, landing a specific move). The
   underlying palette *data* is confirmed correct the whole time (the mod's own `hasBloom` cache,
   read straight from memory, always updates); only the on-screen color visibly lags — i.e. the
   render never picks up the new data.

These two bugs pull in opposite directions: fixing #1 by correcting the toggle artifact back into
live memory (as the original fix did) turns out to *cause* #2 (see Attempt 1). Any fix has to solve
both at once, not trade one for the other.

---

## Timeline of attempts

### Attempt 0 — upstream/librebranch behavior (no fix, baseline)

`UpdatePalette()` toggles the native index and never touches it again. Reliable redraw (this is
what a year of upstream play ran on with no reports of Scumplank's class of bug), but exhibits
Okammunist's bug: the mutated byte leaks into the next match's `OnMatchInit`.

### Attempt 1 — commits `c8c9b19` (2026-07-06) + `b657331` (2026-07-21), shipped for weeks

Fix for Okammunist's bug: `OnMatchInit` (and, after `b657331`, every frame via
`CharPaletteHandle::CorrectToggleArtifact`) detects "does the raw native index match the last
toggle pair we created, and differ from the remembered logical slot" and if so **writes the
remembered logical value back into live game memory**.

**Broke Scumplank's case.** Verified live (2026-08-05, two 6-match sessions with per-match
tester-reported outcomes) that Taokaka fails to redraw in ~4 of 6 matches, always fixed by an
unrelated event. Tuning the correction's timing (a 5-frame grace period so the correction couldn't
race ahead of the engine's own reaction) was tried and **confirmed via debug logs to produce a
rock-solid constant 6-frame delay every single match** — yet Taokaka's failure rate was unchanged.
This rules out timing as the variable: something about whether the engine's redraw fires at all is
the deciding factor, not how long the mod waits before correcting.

### Attempt 2 — commit `608d8e3` (2026-08-07), shipped briefly

Stopped *correcting* the live byte (to stop racing the engine's redraw) but kept detecting the
artifact **for `palettes.ini` lookup purposes only** — i.e. use the remembered logical index for
`m_origPalIndex`/slot lookup/backup, but never write it back to `*m_pCurPalIndex`.

**Fixed most of Scumplank's redraw case, but reintroduced a worse version of Okammunist's bug.**
Since the artifact is never corrected, it persists in memory indefinitely, and **a genuine new
palette pick is bit-for-bit indistinguishable from the artifact** whenever it happens to land on
the previous match's toggle pair (adjacent native color index — e.g. picking Palette2 right after
a match where Palette1 was selected). Confirmed live (6-match session, same tester): this silently
applied the *wrong* custom palette (not the default), because the false-positive artifact
detection substituted the wrong remembered slot for the lookup. **This is unfixable by tuning the
heuristic further** — a genuine pick and an artifact can be the literal same byte value with the
same history, so no comparison against remembered state can disambiguate them. Any approach that
keeps "compare the raw byte to remembered toggle state" as its disambiguation strategy will hit
this same wall.

### Attempt 3 — uncommitted, implemented and reverted 2026-08-07

Replaced the index-toggle trick entirely: capture the "owner" object pointer (via `ecx` at the
`GetPalBaseAddresses` hook site, previously discarded — see "Key RE facts" below) and, from
**every** `UpdatePalette()` call site, set the owner's dirty flag and call the engine's own
`FUN_005B6940(owner)` refresh routine directly instead of toggling the index at all.

**Executed without crashing, but produced a new, worse failure live.** Tester (Kam) bound Kokonoe
color 6 to a custom palette (showed default at round start — same redraw-not-happening symptom as
Taokaka) and had Platinum on native color 24 with no custom palette bound. Hovering the palette
editor's palette list repeatedly (which calls `ReplaceSinglePalFile` → `UpdatePalette` on every
hover-target change) fired the native refresh **25 times in under 20 seconds**, alternating between
Kokonoe's and Platinum's owner pointers. Platinum ended up permanently stuck on the wrong colors,
unable to revert to her real native default 24 even after the hover ended — worse than either prior
regression, since it never self-corrected.

**Why, per the disasm (see "Key RE facts"): `FUN_005B6940` has exactly ONE call site in the entire
game binary**, inside the character-load routine, called once per character per match load. The
game itself *never* calls this function more than once per match for any character. Calling it
repeatedly (as the palette editor's hover-preview does, by design, many times per second) is
unsupported/untested behavior from the engine's own perspective — the Lock-like vtable call
(`vtable[2]`) takes three parameters read from the owner object (`owner+4`, `owner+8`, `owner+0Ch`)
that the mod never initializes or advances; if these represent something like a generation counter
or lock-state that the engine itself only ever advances once, hammering the same call repeatedly
with stale values could easily corrupt whatever state it's tracking. This is inference, not a
confirmed root cause — but it's consistent with "works once, breaks on repeat," and with there
being no other caller in the whole binary to learn a safe repeated-call pattern from.

**Reverted before commit** — nothing from this attempt shipped; `608d8e3` (Attempt 2) is still the
last committed state.

### Attempt 4 — implemented 2026-08-07, first live test result: still broken, and reveals the real axis

Implemented as described below. First live test (Kam, same session): P1 Kokonoe (color 6, bound to
`0-KokoLoremaster` in `palettes.ini`) vs P2 Platinum (native color 24, no binding).

- **Kokonoe: 100% failure, every mechanism.** She never showed anything but her default
  appearance — not at round start (native refresh, single call, exactly the engine's own usage
  pattern), and not through *any* amount of palette-editor hovering afterward (index-toggle
  fallback, the same mechanism that's supposedly reliable and matches upstream). Log confirms both
  paths actually ran without error: `ForcePaletteTextureRefresh invoked native refresh
  (owner=016C6D98)` fired once at match init exactly as designed, and `UpdatePalette toggled native
  index (slot=6/5, ...)` fired repeatedly afterward from editor hovering — no "not ready"/"skipped"
  warnings anywhere, i.e. the palette *data* writes all succeeded (same as the earlier
  bloom-flag-updates-but-color-doesn't-render evidence for Taokaka). She simply never visually
  redrew, under either mechanism, ever, this whole session.
- **Platinum: worked correctly**, but only tested through the index-toggle path (editor hover to a
  custom palette, then back to default) — she never had a `palettes.ini` binding this session, so
  the native-refresh path was never exercised for her at all.

**This reframes the whole investigation.** Kokonoe failed identically under BOTH the native
refresh call (a single, correctly-scoped, engine-pattern-matching invocation) AND the index-toggle
fallback (the same mechanism that worked fine for Platinum, and worked for Taokaka often enough to
look intermittent rather than constant). Since the *mechanism* was held constant between Kokonoe's
toggle attempts and Platinum's successful one, and both mechanisms failed identically for Kokonoe,
**the redraw-forcing mechanism (toggle vs. native call) is very likely not the actual axis of
failure at all** — something about specific characters' render setup doesn't pick up palette
changes regardless of how we ask for a refresh. Taokaka's original *intermittent* failure (fixed by
landing a hit or pressing freeze-frame) is still unexplained by this — it's not yet clear if she
and Kokonoe share one root cause (with Kokonoe just hitting it 100% of the time instead of ~65%)
or if these are two different bugs that happen to look similar. Next step: RE comparison of
Kokonoe's vs. Platinum's character-load/render path specifically, to find what's actually different
between a character where forcing an index change (however we force it) works and one where it
doesn't.

### Attempt 4 design (superseded understanding above, keeping for the implementation detail)

Given Attempt 3's finding that `FUN_005B6940` is only ever called once per match by the engine
itself, restrict the native-refresh call to the **one call site that matches that exact usage
pattern**: `PaletteManager::ApplyDefaultCustomPalette`, called once per player per match from
`PaletteManager::OnMatchInit`. Every other call site that can fire repeatedly/interactively —
`PaletteEditorWindow`'s hover-preview and click-to-select UI, its randomizer button,
`OnlinePaletteManager` syncing an opponent's palette mid-match, `RestoreOrigPal` (used e.g. for
Platinum's item-holding fix) — keeps using the index-toggle-and-leave-mutated approach, which is
the behavior a year of upstream play validated as reliable for that kind of repeated, in-session
use.

This narrows Okammunist's-bug exposure (rather than eliminating it outright): the toggle mutation
can still happen, but now *only* as a side effect of mid-match palette editing/testing, not on
every single match. The existing artifact-detection-for-lookup-only heuristic (Attempt 2, kept as
a safety net, never writing back to live memory) still covers this narrower residual case. The
common path — a player just has a palette bound in `palettes.ini` and plays normally, no mid-match
editing — should be fully deterministic: the native call always fires exactly once per match per
player, matching the one and only pattern the engine itself uses, so it should carry the same
safety guarantee as the engine's own call.

**Implementation plan:**
- Thread a `bool useNativeRefresh` parameter through `CharPaletteHandle::UpdatePalette` and
  `ReplacePalData`, and `PaletteManager::SwitchPalette`.
- `ApplyDefaultCustomPalette` passes `true`. Every other caller (hover/click/randomize UI, online
  sync, `RestoreOrigPal`, `ReplaceSinglePalFile`) passes `false` / doesn't opt in.
- `UpdatePalette(true)` calls `ForcePaletteTextureRefresh()` if the owner pointer is available,
  falling back to the toggle if not. `UpdatePalette(false)` always uses the toggle.
- **Not yet live-tested.** Confirm before declaring this fixed: (a) the common "just bind a palette
  and play" case redraws reliably for Taokaka and Kokonoe across many matches, (b) mid-match
  palette editing (hover, randomize) still works and cleanly reverts without leaving Okammunist's
  bug in a state that misapplies the *next* match, (c) no repeat of Attempt 3's stuck/corrupted
  state.

---

## Key RE facts (established, don't re-derive)

- The mod's `GetPalBaseAddresses` hook fires at `0x005B6372` (`mov [ecx+830h],eax`), inside
  `FUN_005B6310` (`ADDR_PaletteContainerLoad`). At that instruction `ecx` is an outer "character
  render resource" object (the **owner**) that owns two palette-container slots:

  | Owner offset | Loaded by | Refreshed by | Notes |
  |---|---|---|---|
  | `+0x82C` | `FUN_005B6390` | `FUN_005B6980` (gated on `owner+0x10`) | pushes `0x400`/`0x400` — looks like a fixed-size texture/atlas, not a raw palette. Not used by the mod. |
  | `+0x830` | `FUN_005B6310` (the hooked one) | `FUN_005B6940` (gated on `owner+0x18`) | pushes `0x300` = 768 = exactly a 256-color RGB palette. This is the one the mod writes into (`CharPaletteHandle::m_pPalBaseAddr`). |

- `FUN_005B6940(this=owner)`:
  ```
  cmp   [owner+18h], 0
  je    skip
  mov   ecx, [owner+830h]      ; ecx = the container itself (== CharPaletteHandle::m_pPalBaseAddr)
  push  1
  push  [owner+0Ch]
  mov   eax, [ecx]             ; container vtable
  push  [owner+8]
  push  [owner+4]
  call  [eax+8]                ; vtable slot 2 — Lock-like: (owner+4, owner+8, owner+0Ch, 1)
  mov   ecx, [owner+830h]
  push  300h
  mov   eax, [ecx]
  push  0
  call  [eax+0Ch]              ; vtable slot 3 — Unlock-like: (0, 0x300)
  skip:
  ret
  ```
  Reads like a D3D9-style Lock/Unlock pair pushing the palette bytes at `owner+0x830` into
  whatever the renderer actually samples (almost certainly a GPU texture). Note it never clears
  `owner+0x18` back to 0 — nothing in this function resets the flag it checks.

- **`FUN_005B6940` (`0x005B6940`) has exactly ONE call site in the entire disassembled binary**
  (checked via `grep -n "call.*005B6940" tools/bbcf_disasm_ascii.txt`): at `0x005680BE`, inside the
  same character-load routine that calls `FUN_005B6310`/`FUN_005B6390` (`~0x00567E00`–`0x005680C0`).
  The engine never calls it more than once per character per match load. Established 2026-08-07
  after Attempt 3's regression, to explain why repeated invocation broke things.

- `GetPalBaseAddresses` hook previously only captured `eax` (the container) via
  `CharPaletteHandle::SetPointerBasePal`. It now also captures `ecx` (the owner) via the new
  `CharPaletteHandle::SetPointerPalOwner` (added for Attempt 3, kept for Attempt 4). Both are
  cleared in `PaletteManager::OnMatchRematch`/`OnMatchEnd`.

- Call-site inventory for anything that ends up calling `CharPaletteHandle::UpdatePalette`
  (checked 2026-08-07, relevant if this list needs updating):
  - **Once-per-match (matches the engine's own usage pattern):** `PaletteManager::ApplyDefaultCustomPalette` → `SwitchPalette` → `CharPaletteHandle::ReplacePalData`, called from `PaletteManager::OnMatchInit`.
  - **Repeated/interactive (do NOT use the native call for these):**
    `PaletteEditorWindow` hover-preview (`HandleHoveredPaletteSelection`) and click/select buttons →
    `PaletteManager::SwitchPalette`/`ReplacePaletteFile`; `PaletteEditorWindow`'s randomizer button
    (`ShowPaletteRandomizerButton`) → `SwitchPalette`; `OnlinePaletteManager` (`RecvPaletteDataPacket`
    and the mid-match sync path) → `ReplacePaletteFile`; `CharPaletteHandle::RestoreOrigPal` →
    `ReplacePalData` (used e.g. by Platinum's item-holding fix).

## Reconsidering the whole "native refresh" premise (2026-08-07, after Attempt 4's live result)

Ruled out concretely (checked, not just theorized):
- `0-KokoLoremaster.cfpl`'s data is fine — read the raw file, the `Character` (file0) block is 472
  of 1024 bytes non-zero, nowhere near the all-zero `NULLBLOCK` skip condition. Not a data problem.
- Not an extra-container-per-character issue — `GetPalBaseAddresses` still fires exactly 3 times
  this match (same as every prior session), no 4th call specific to Kokonoe.
- Not a P1/P2 container-swap issue — Platinum's own edits visibly applied to Platinum, not
  Kokonoe; if the two players' containers were swapped, editing Platinum's palette would have
  shown up on Kokonoe instead.

**New working hypothesis: the native refresh call (`FUN_005B6940`) may never have been the actual
mechanism that matters — an index CHANGE might be the real trigger, independent of Lock/Unlock.**
Reasoning: Attempt 4's native-only call path (match-init) never changes the live native index at
all (that's the whole point of it) — it only performs the Lock/Unlock-style call while the index
sits still. If Kokonoe's render pipeline only refreshes on an observed index *change* (matching
upstream/Attempt-0's toggle-and-leave working reliably for a year, and matching why forcibly
correcting the index in Attempt 1 raced whatever periodically checks for a change), then a
same-index Lock/Unlock call with no accompanying change might do nothing meaningful — consistent
with Kokonoe's 100% failure under the "correct," single, engine-pattern-matching native call.
Under this reframing, Taokaka's *intermittent* success even with the plain toggle (Attempt 0/2)
would come from the same index-change signal, just checked at a cadence/timing that doesn't
always line up for her specifically — which is also consistent with everything logged so far
(6-frame-constant correction timing yet still-intermittent failure in Attempt 1). This has NOT
been confirmed, only reasoned from existing data — no new live test targets it directly yet.

**A second, independent lead worth checking:** `OnlinePaletteManager::RecvPaletteDataPacket`
(`src/Network/OnlinePaletteManager.cpp:40`) applies an *opponent's* synced palette data via
`ReplacePaletteFile` → the toggle-fallback path (`useNativeRefresh` effectively false), gated on
`g_modVals.enableForeignPalettes`. This fires for whichever `matchPlayerIndex` the incoming packet
names, which *should* always be the remote opponent's slot, never the local player's own — but
this hasn't been specifically verified to never mis-target the local slot, and both of Kam's
recent repro sessions were online (ranked) matches where this code path is actively running
(`RecvPaletteDataPacket` appears repeatedly in both debug logs). If `matchPlayerIndex` resolution
ever points at the wrong slot, it would silently re-toggle/overwrite the local player's just-applied
palette immediately after match init — worth confirming this can't happen before ruling it out.

**Recommended next step: live dynamic debugging instead of more static disasm guessing.** Static
analysis has now produced four plausible-sounding theories that didn't pan out once tested live.
The RE workflow already supports operator-run `cdb` via `tools/safe_readonly_exec.ps1` — setting a
breakpoint/watch on whatever actually reads the palette container into the renderer (or on
`FUN_005B6940` and the native index write, to see in real time whether Kokonoe's redraw path is
even reached, and with what register/memory state) while reproducing live would settle this in one
session, instead of more rounds of "implement a theory, wait for a live test, find it's wrong."

## Diagnostic tool added (2026-08-07): one-click snapshot, no debugger needed

Kam's cdb/live-debugging setup has reportedly never worked and he doesn't want to operate a
debugger manually, so instead of live breakpoints: `CharPaletteHandle::LogDiagnosticSnapshot`
(new) dumps everything relevant — native index, toggle-pair bookkeeping, owner/container pointers,
the owner's `+0x18`/`+0x10` dirty flags, and the first 16 bytes of the "Character" palette file at
the live index, both switch-pair slots, and the `Default` backup — straight to `DEBUG.txt` via
`LOG(0, ...)` (always emitted when logging is on). Wired to a button ("Dump palette diagnostics
(P1 + P2) to DEBUG.txt") in `DebugWindow`, under the "Character data" tree node. One click, at the
moment a repro is visibly showing wrong colors, and the log will have everything needed —
including whether the dirty flag is set/cleared, whether the container pointer still matches, and
whether the palette bytes at the *live* index actually differ from the ones at the backup/switch
slots (which tells us directly whether this is a data problem or a redraw problem, without having
to infer it from indirect evidence like the bloom flag).

## Conclusive result from the diagnostic snapshot (2026-08-07)

Four snapshot pairs captured live (P1 Kokonoe stuck on default, P2 Platinum, mid-session switch to
a working custom palette for Platinum). This settles the data-vs-redraw question completely:

**Kokonoe (P1), across all 4 snapshots, `liveIndex` stayed 5 the whole time (per the diagnostic
button) even though the player-visible `P1PalIndex` slider in the debug window was independently
observed flickering 5→6→5 while hovering** — i.e. the toggle-and-leave fallback genuinely *is*
changing the native index live, just not at the exact instants captured. More importantly: **the
`Character` palette bytes at the live index changed between snapshots to match whichever palette
was actually hovered** (snapshot 2: `00 00 00 00 B9 E4 FF FF...`, `selectedCustomPalIndex=3`;
snapshots 1/3/4: `00 FF 00 FF E9 E2 E8 FF...`, `selectedCustomPalIndex=1` — genuinely different
palettes, correctly written each time), and in every snapshot these bytes clearly differ from
`origPalBackup` (the real default: `00 FF 00 FF DC EC FF FF...`). **The data write is unambiguously
correct and responsive to every hover — she was never actually showing default in memory, only on
screen.** Kam confirmed visually she never once changed color despite this.

**Platinum (P2)**, identical mechanism, same code path: snapshots 1–3 show her still on default
(`liveIndex` bytes == `origPalBackup`, as expected before any edit). Snapshot 4, after Kam switched
her to a custom palette, shows `liveIndex` bytes now differing from `origPalBackup`
(`00 FF 00 FF CA DF FF FF...` vs. `C3 A6 81 FF 77 A5 D7 FF...`) — and Kam confirmed **this one
visually worked**.

**This rules out data problems, the toggle-vs-native-call question, and index-change timing
entirely.** Same write path, same toggle mechanism, same data correctness, same owner/container
plumbing — Kokonoe never redraws and Platinum always does. The `owner+0x18`/`+0x10` values are
also static for the whole match per character (Kokonoe: `1`/`1137` unchanged across all 4
snapshots — the `1` is literally our own `ForcePaletteTextureRefresh` write from match init, never
touched again since the toggle fallback path doesn't touch it; Platinum: `208`/`1097`, likely
whatever the engine's own one-time load call left there) — nothing here is being actively toggled
per-frame by anything, mod or engine, ruling out a timing race on these specific fields too.

**Conclusion: this is not fixable by anything the mod does with the index, the toggle, or the
native refresh call.** Something specific to how Kokonoe's (and presumably Taokaka's) character
model is rendered doesn't consume the palette container the same way Platinum's does, regardless
of how correctly the mod writes to it or signals a change. The next productive angle is almost
certainly a per-character property/config difference in the game's own data (e.g. a
"supports dynamic recolor" flag, a different shader/technique selection, or a separate texture the
renderer actually samples for certain characters) — not anything reachable by continuing to poke
at `CharPaletteHandle`. Comparing what Kokonoe and Taokaka have in common that Platinum/Makoto/
Hakumen/Ragna don't (both are likely to share *something* structurally, e.g. model complexity,
extra visual elements, or being on some internal "special case" list) is the next concrete research
question, via a fresh disasm search for a per-`CharIndex` branch or table near whatever function
actually issues the character's draw call — not the palette-loading code investigated so far.

## Open questions for whoever picks this up next

- Is Attempt 4 actually sufficient, or does the residual Okammunist exposure (mid-match editing
  leaving an artifact for the next match) still show up often enough to matter in practice? No
  live data yet either way.
- What do `owner+4`, `owner+8`, `owner+0Ch` (the Lock-like call's dynamic parameters) actually mean?
  Unknown — the mod has never written to them, only read whatever the engine's own load-time call
  left there. If Attempt 4 still misbehaves even at a single once-per-match call, this is the next
  thing to dig into.
- Is there a *different*, actually-repeatable engine routine for "redraw this character's palette
  texture" that the mid-match editing paths could use instead of the index-toggle trick? Not
  searched for yet — worth a disasm pass specifically for something called from a per-frame update
  loop (unlike `FUN_005B6940`) if Attempt 4's residual exposure turns out to matter.

---

# Resolution (2026-08-13): the mod caused the bug, reverted to v8.1 palette behavior

## What the bug actually was

`b657331` ("Palette rng bug issue reported by Okammunist", 2026-07-21) moved the toggle-artifact
correction from `OnMatchInit` only to **every frame**, via `CharPaletteHandle::CorrectToggleArtifact`
called from `PaletteManager::OnUpdate`. `UpdatePalette()` forces a redraw by toggling the native
color index; the per-frame correction undid that toggle one frame later. The engine needs to
*observe* the changed index to refresh the character's palette texture, so the correction
frequently cancelled the redraw before it happened.

`b657331` is contained in **v8.2 and no earlier release**. v7.3/v8.0/v8.1 have the `OnMatchInit`-only
correction from `c8c9b19` and are unaffected.

## How that was established

Purely from field evidence — no disassembly, no new RE:

- **Version bisect with the reporter (Scumplank).** He originally reported on stock v8.2. On stock
  v8.1, with his *identical* `palettes.ini` and `.cfpl` files: 10/10 matches correct, then a
  second session of 12/12 correct in rooms. ~22 clean matches against ~4-in-6 failures on v8.2.
- **His `DEBUG.txt` from the clean v8.1 session** (`2026-08-10`, 12 matches, Makoto on palette 2):
  every match logs `OnMatchInit raw native color slot = 1` → `ApplyDefaultCustomPalette
  char=Makoto slot=1 -> ini entry='RaccoonGirl'`, correct every time, with **zero** `detected
  toggle artifact` lines and a stable `pCurPalIndex`.
- **The v8.1 release predates the whole investigation**, so this is not a fix that needs testing —
  it is a return to a state with a year of play behind it.

## Why the earlier conclusions were wrong

- **"The engine's native refresh call is the right mechanism."** It is not. `FUN_005B6940` has one
  call site in the entire binary and is not safe to call repeatedly. See `GhidraDefs.h`.
- **"Certain characters never visually redraw; this needs per-character render RE."** This came from
  the Attempt-4 diagnostic snapshots and is **confounded**. In that session the mod called
  `ForcePaletteTextureRefresh` (writing `[owner+0x18] = 1`) only for characters with a
  `palettes.ini` binding. Kokonoe had a binding, got the write, and never redrew. Platinum had no
  binding, never got the write, and always redrew. "Which character" and "did the mod write to the
  owner object" were perfectly correlated — one observation each, two variables. The most likely
  reading is that the modified build broke Kokonoe itself. Kam's Kokonoe repro was never once
  reproduced on a clean build.
- **"Scumplank's reports describe one bug."** They describe two. See below.

## The two bugs, separated

1. **Palettes randomly do not apply at round start** — caused by `b657331`, v8.2 only, **fixed by
   this revert**.
2. **Picking a new color gives you the previous one** — a *different*, pre-existing bug, present
   since v7.3 and **not** fixed by the revert. Root-caused and fixed separately; see
   "Bug 2" below. Conflating it with bug 1 is most of why this investigation went wrong: a redraw
   failure shows the *default* colors, whereas loading a different custom palette means wrong
   *data* was selected. They cannot share a root cause.

## Okammunist's bug is not regressed by this

`c8c9b19`'s `OnMatchInit` correction is retained in full — that is v8.1 behavior and it is what
fixed his report. Only the per-frame extension from `b657331` is removed. There is no tradeoff
here: v8.1 has both his fix and correct redraw.

Residual known weakness, unchanged from v8.1 and *not* introduced by this revert: the correction
compares the raw byte against remembered toggle state, so a genuine new color pick that lands on
the previous match's toggle pair is indistinguishable from an artifact. If bug 2 above turns out to
be this, the fix is **not** another heuristic — it is to stop leaving the artifact in memory at all
by restoring the pre-toggle index at match teardown (`OnMatchEnd`, which currently only nulls
pointers; `OnMatchRematch` already restores `m_origPalIndex`), and then delete the heuristic
entirely.

## What was reverted

- `608d8e3` (Attempt 2 — artifact detection for lookup only, never writing back).
- `b657331`'s per-frame correction (`CorrectToggleArtifact`), already absent from `608d8e3`.
- All uncommitted Attempt 3/4 work: owner-pointer capture (`SetPointerPalOwner`), the native
  refresh call (`ForcePaletteTextureRefresh`), the `useNativeRefresh` plumbing through
  `SwitchPalette`/`ReplacePalData`, `LogDiagnosticSnapshot`, and its `DebugWindow` button.

Deliberately **kept** (unrelated to this bug, wanted):
- `7ad5416` Platinum item-palette fix — `ClearPlatinumItemPaletteLink` and `IsCustomPaletteActive`.
- The `Random_Exclude_Default` guard against `dist(1, 0)` UB when a character has no custom palettes.

## Reporter config issues found along the way (not the bug, but worth fixing)

Verified against his files and confirmed in his `DEBUG.txt` (42 `[error]` lines at load):
- `[Hakumen] 2="UndyneHkn"` and `[Izanami] 3="Remilia Scarlet"` — no such files; both slots
  silently fall back to default.
- `Palettes/Lambda/Void_Neon_Nu.cfpl` has charIndex 11 (Nu) in its header and is rejected at load;
  it belongs in the `Nu` folder.
- ~30 stray `.png` files inside character folders, each logging a load error. Harmless, but noisy.

His Taokaka set itself is clean: `ConceptTao` and `RTL` both load, correct headers, no duplicate
palette names anywhere (which matters, since `FindCustomPalIndex` returns the first name match).

---

# Bug 2 (2026-08-14): picking a new color gives you the previous one — fixed

## Symptom

Pick native color `N` for a match, return to character select, pick `N+1` — you play as `N`, with
`N`'s `palettes.ini` binding applied. Reported independently by three people, each describing it
differently, which is why it took so long to see it was one bug:

- **Okammunist:** "I had color 7 set for custom color, gave me color 6" — and it showed on his
  opponent's stream too, because the wrong index is written into live game memory, so the game's
  own netcode syncs it.
- **Cuack:** "entré a training, color 2, volví a char select, puse color 3, sigue color 2", plus
  "cuando escojo un color y después cambio a otro, queda en el primer color" and "usar el color
  vanilla lo resetea".
- **Scumplank:** "instead of 2 it loaded 1", roughly once in 22+ matches — rare for him only
  because he rarely changed color between matches.

Present in **v7.3 through v8.2 and the v8.1 revert**, i.e. every release containing `c8c9b19`.

## Root cause

`c8c9b19`'s artifact heuristic in `CharPaletteHandle::OnMatchInit`. `OnMatchInit` recorded a toggle
pair of `(rawIndex, rawIndex + 1)` and the "logical" slot, then on the *next* match treated any raw
value inside that pair that differed from the remembered logical slot as its own leftover toggle
and overwrote it.

A genuine pick of `N+1` after a match on `N` satisfies that test exactly. The heuristic cannot
distinguish the two cases, because they are the same byte value with the same history — which the
Attempt 2 section above had already concluded, but only about Attempt 2's variant, missing that
`c8c9b19` had the identical flaw.

Confirmed on Kam's machine with the repro above:

```
01:16:03  CharPaletteHandle::OnMatchInit raw native color slot = 5      <- pair becomes (5,6)
01:16:33  CharPaletteHandle::OnMatchInit detected toggle artifact (raw=6), restoring logical slot 5
01:16:33  ApplyDefaultCustomPalette char=Kokonoe slot=5 -> ini entry='0-KokoLoremaster'
```

## Fix

Stop leaving the artifact in memory, and delete the heuristic entirely.

`CharPaletteHandle::RestoreNativePalIndex()` puts `m_origPalIndex` back into `*m_pCurPalIndex`, and
`m_lastLogicalPalIndex` / `m_lastTogglePairA` / `m_lastTogglePairB` are gone. `OnMatchInit` simply
trusts the byte it reads.

**Where that restore runs is the entire problem, and the first attempt got it wrong.** It was
initially placed in `PaletteManager::OnMatchEnd`, which sounds right and is not: `OnMatchEnd` is
hooked on `GetGameStateVersusScreen`, and in BBCF the versus screen comes *after* character select.
Live test showed the restore firing at 01:27:44 — 13 seconds after the player entered character
select and picked their new color — overwriting that pick in the same millisecond the game
committed it:

```
01:27:31  GetGameStateCharacterSelect                                   <- player picks color 07
01:27:44  GetGameStateVersusScreen
01:27:44  RestoreNativePalIndex (OnMatchEnd) native color slot 6 -> 5   <- destroys the pick
01:27:44  GetPaletteIndexPointers                                       <- game commits pending copy
01:27:49  OnMatchInit raw native color slot = 5
```

The disassembly explains why the timing is so tight. The mod's `GetPaletteIndexPointers` hook sits
at `0x0047D92D`, inside a routine (`0x0047D910`) that copies the pending character-select config
over the committed in-match copy:

```
[edx+0x0118] -> [edx+0x16B8]   0x1C4 dwords   P1 character config
[edx+0x1648] -> [edx+0x24D8]   8 dwords       P1 palette/color block   <- hook is here
[edx+0x0828] -> [edx+0x1DC8]   0x1C4 dwords   P2 character config
[edx+0x1668] -> [edx+0x24F8]   8 dwords       P2 palette/color block
```

The color index is at `+8` inside each 8-dword block, so `m_pCurPalIndex` is `edx+0x1650` (P1) —
the **pending** copy, the same words character select writes into. Anything the mod writes there
after character select is destroyed-by-us data.

So the correct and only moment is **entry to character select**: after the match, before any new
pick. `PaletteManager::OnCharacterSelect` is called from the existing `GetGameStateCharacterSelect`
hook; `OnMatchRematch` keeps its own restore for the rematch path, which skips character select.

Ordering that has to hold:

```
match ends -> CHARACTER SELECT (restore here) -> player picks -> versus screen (commit) -> match init
```

- **Per frame** (v8.2 / `b657331`) cancels the index change before the engine observes it — bug 1.
- **On the versus screen** overwrites the player's pick — the failed first attempt above.
- **At the next `OnMatchInit`** is later still, and needs the heuristic that caused bug 2.

`m_origPalIndex` is initialized to `-1` and the restore no-ops on `-1` or a null index pointer,
since the member was previously uninitialized and character select can be reached without a
preceding match init.

## How to verify

Training → pick color `N` → character select → pick `N+1` → you should be `N+1`. `DEBUG.txt` should
show `RestoreNativePalIndex (CharacterSelect) native color slot X -> N` on entering character
select, then `OnMatchInit raw native color slot = N+1`. Also re-test Okammunist's original case:
training, change matchup *without* reselecting a color, and confirm the palette does not drift.
