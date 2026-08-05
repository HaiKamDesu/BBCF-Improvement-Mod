# Taokaka round-start palette investigation

**Status:** root cause hypothesized from disasm (2026-08-05), not yet verified live or implemented.
No code changes have been made based on this doc yet.

All addresses below are **static VAs** (image base `0x00780000` for this region, matching
`GetPalBaseAddresses found at: 0x00936372` = base + `0x1B6372`). Subtract the appropriate base for
the `bbcf_base_adress + RVA` form used by `GhidraDefs.h` and the hook code.

---

## The report

Custom palettes bound via `palettes.ini` fail to apply at round start for Taokaka specifically,
while every other character tested (Makoto, Hakumen, Ragna) applies correctly every time. This is
independent of which engine player-slot (P1/P2) she occupies. Confirmed across two live 6-match
debug sessions with per-match outcomes reported by the tester (2026-08-05): Taokaka failed to load
in 4 of 6 matches (P1 side matches 1 & 3, P2 side matches 5 & 6), always fixed mid-match by an
unrelated event — pressing the mod's freeze-frame hotkey, or landing a specific move
(`j.236B`). The underlying palette *data* is confirmed correct the whole time (the mod's own
`hasBloom` cache, read straight from memory with no render dependency, always updates immediately);
only the on-screen color visibly lags.

An earlier fix (`CharPaletteHandle::CorrectToggleArtifact`, deferred by a 5-frame grace period so
the mod's index-toggle-to-force-a-redraw trick isn't undone before the engine can react to it) was
verified via debug logs to behave with rock-solid, constant timing (always exactly 6 frames between
flip and correction, every match). Since that's now deterministic yet Taokaka's failure rate didn't
change, toggle *timing* is ruled out as the variable — something about whether the engine's redraw
actually fires is the remaining unknown, not how long the mod waits.

## What the disasm shows

The mod's `GetPalBaseAddresses` hook fires at `0x005B6372` (`mov [ecx+830h],eax`), inside
`FUN_005B6310` (already documented as `ADDR_PaletteContainerLoad`). At that instruction, `ecx` is
an outer "character render resource" object that owns **two** palette-container slots:

| Owner offset | Loaded by | Refreshed by | Notes |
|---|---|---|---|
| `+0x82C` | `FUN_005B6390` | `FUN_005B6980` (gated on `owner+0x10`) | pushes `0x400`/`0x400` — looks like a fixed-size texture/atlas, not a raw palette |
| `+0x830` | `FUN_005B6310` (the hooked one) | `FUN_005B6940` (gated on `owner+0x18`) | pushes `0x300` = 768 = exactly a 256-color RGB palette |

`FUN_005B6940(this=owner)`:
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

This reads exactly like a D3D9-style Lock/Unlock pair that pushes the palette bytes at
`owner+0x830` into whatever the renderer actually samples (almost certainly a GPU texture — the
mod's writes land in system memory the renderer doesn't read directly). **The mod currently has no
way to invoke this.** `CharPaletteHandle::m_pPalBaseAddr` is only the *container* (the `eax`
captured at the hook site); the *owner* object holding the `+0x18` dirty flag and the refresh
function has never been captured — the hook only reads `eax`, never `ecx`, at that instruction.

## Working theory

The mod's index-toggle trick (`CharPaletteHandle::UpdatePalette`) works by hoping the *engine*
notices the native color index changed and sets `owner+0x18 = 1` on its own, so that the next time
something calls `FUN_005B6940` (likely once per frame, or on specific state transitions), the
refresh actually happens. This is a race against however/whenever the engine decides to set that
flag — which is apparently reliable for most characters' idle/entrance state but not consistently
for Taokaka's, and gets forced by unrelated events (freeze-frame, landing a move) that happen to
also flip it.

## Proposed fix (not yet implemented)

1. In `GetPalBaseAddresses` (`src/Hooks/hooks_palette.cpp`), also capture `ecx` (the owner object)
   alongside the existing `eax` (container) capture, and store it on `CharPaletteHandle` (e.g.
   `SetPointerPalOwner`).
2. After `CharPaletteHandle::ReplacePalData` writes new palette bytes, set `[owner+0x18] = 1` and
   call `FUN_005B6940(owner)` directly (`__thiscall`, `this = owner`) — reusing the engine's own
   proven refresh path deterministically instead of gambling on the index-toggle trick noticing.
3. Do NOT remove the toggle/`CorrectToggleArtifact` mechanism until the direct-call approach is
   confirmed working live — no way to test the theory outside an actual game session, and the
   toggle trick is what's shipping today.

Open question before implementing: confirm `owner` is stable across the whole match (captured once
at load, per `ADDR_PaletteContainerLoad`'s "fires three times per match, mod keeps first two as
P1/P2" note) and that calling `FUN_005B6940` off the game thread/hook context is safe (it makes
virtual calls through the container's own vtable, so as long as `owner` and its `+0x830` container
are still valid — which they should be for the whole match, per existing `PaletteManager::OnMatchEnd`
lifetime handling — this should be safe, but has not been tested).
