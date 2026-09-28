# Platinum's nine palettes — the engine's custom palette slots

**Status:** reverse engineered and implemented (2026-09-27), not yet confirmed in-game.
Implementation: `PaletteManager::UpdatePlatinumItemPalettes` / `SyncOwnedCustomPaletteSlots`,
called once per frame from `MatchState::OnUpdate`. Constants in `src/Game/GhidraDefs.h` under
"Engine custom palette slots".

Background and the earlier dead ends: `PlatinumItemPaletteInvestigation.md`. This document
supersedes its "Fix" section.

Addresses are **static VAs** (image base `0x00400000`). Subtract `0x00400000` for the RVA form.

---

## The report

With the palette editor open in training as Platinum, applying the edited palette and then
playing with it (F1 to close the menu), her palette flickers back to the default as she moves
around. `palettes.ini` palettes never did this.

## What the "9 slots" actually are

Platinum does not copy her palette into nine slots of her own. The engine has a general
mechanism, and her script uses eight instances of it:

| Piece | Where |
|---|---|
| Battle scene singleton | static object at `0x011B6AE0` (`FUN_0055C540` returns it) |
| 40 custom palette **handles** | battle scene `+0x178`, one dword each (`0x178..0x217`) |
| 40 **in-use** flags | battle scene `+0x218`, one dword each (`0x218..0x2B7`) |
| Resource registry | static at `0x00A23674` (`FUN_0040E6B0`): `+0` pointer array, `+8` count |
| Per-player palette container | battle scene `+0x2B8 + player*0x20BDC`; its resource handle at `+0x824`, or one handle per color at `+0x24 + n*4` when `+0x18 == 0` |

A **palette id** (the value in `CharData+0x358` / `+0x35C`) is an index into those 40 slots.
`FUN_0057CAA0(id, &handle, &sub)` resolves it: `handle = battle[0x178 + id*4]`, `sub = 0`.
`FUN_004098F0(registry, &handle)` gives the resource, `FUN_00411F90(resource, sub)` gives the
palette object (`resource+0x4` array, `+0xC` count).

The mod's `CharPaletteHandle::m_pPalBaseAddr` **is** such a resource — the player's palette
container. `[[[base+4] + (color*8+file)*4] + 0x1C]` is exactly "sub-object `color*8+file`, its
color data". So every palette the mod has ever patched is one of these same objects.

### The palette object (`AA_CPalette`)

| Offset | Meaning |
|---|---|
| `+0x0C` | **dirty flag** — 1 means "re-upload to the GPU on next use" |
| `+0x1C` | color data (256 × 32-bit = `0x400` bytes) |
| `+0x81C` | second copy of the same data — the "+0x800 duplicate" the mod has always double-written |
| `+0x101C` | width, 256 |
| `+0x1020` | bits per pixel, 32 |

`FUN_00452C60(dst, src)` copies both blocks from another palette object and sets the flag.
`FUN_00452DE0(index, color)` writes one entry and sets the flag (used by the per-frame rainbow
effect in the renderer's `+0x1258 == 1` branch).

**This flag is why the 2026-07 attempt failed.** That investigation found the eight stale
mirrors at a `0x1108` stride (these objects), overwrote all of them, and saw no change: the
bytes were right but nothing told the engine to re-upload. The mod's own storage writes get
away without the flag only because they are followed by the palette-index toggle
(`UpdatePalette()`), which makes the game rebuild the character's palette from scratch.

### How a PaletteControlObj gets its slot

Platinum's script spawns `PaletteControlObj1..8` (names at `0x00954D50..0x00954DDC`). Each runs
a command whose handler is `FUN_00591BC0(this, enable)`:

| Address | What |
|---|---|
| `0x00591BCC` | already has a slot (`+0x358 != -1`)? then nothing |
| `0x00591BE0`–`0x00591BF5` | scan `battle+0x218..0x2B8` for the first zero flag |
| `0x00591BF7` | none free: `"Palette Reserve Error"` (`obj_Command.cpp` line 3945) |
| `0x00591C16` | `mov [ebx+358h], edi` — store the slot index as this entity's palette id |
| `0x00591C23` | mark the slot in use |
| `0x00591C2E` | `FUN_005787E0(this)` — **fill the slot with a copy of the entity's palette** |
| `0x00591C3A`–`0x00591C59` | `enable == false`: release the slot, `+0x358 = -1` |

`FUN_005787E0(entity)`:

1. `FUN_0057C8B0(entity, colorOverride=+0x350, fileIndex=(short)+0x354)` → the source palette:
   the owning player's container, sub-object `color*8 + fileIndex`, where `color` is the
   player's chosen in-game color (`+0x350 == -1`) or the override.
2. `battle[0x178 + id*4]` → the destination handle.
3. `FUN_004B1620(&dstHandle, 0, &srcHandle, srcSub)` → resolves both and calls
   `FUN_00452C60(dstObj, srcObj)`.

This runs **once**, when the object reserves its slot. Nothing in the game refreshes the copy.

### How the copy is used

`PT_LinkColor` (handler `0x005B2AC3`, see the older doc) looks up `PaletteControlObjN` by item
type and stores *its* `+0x358` into Platinum's `+0x35C` (`linkedPaletteId`).

The render-side chooser `FUN_005A6C80(entity, &handle, &sub, ...)` then picks, in order:

1. the entity's own palette id `+0x358` if `>= 0`;
2. else the linked id `+0x35C` if `>= 0` → `FUN_0057CAA0`;
3. else the player's container (`vtable+0x54` gives the file index; `FUN_0057C8B0`).

So "9 slots" = her own storage (path 3) + eight `PaletteControlObj` slots (path 2). Vanilla fills
all eight with identical copies of her palette; the difference is only visible once something
changes her storage after the copies were taken.

`PT_Light` (`0x005B2B7F`) reads `+0x358` on the entity running it and computes lighting; it
writes no palette entries, so keeping the slots current does not fight it.

## Why it flickered

The 8.7 fix wrote `-1` into `linkedPaletteId` every frame from the render hook. The script
re-runs `PT_LinkColor` on some state changes (movement states, hence "as you move around"), and
on those frames the link was set *after* our clear and before the draw: one frame drawn from the
stale slot — the default colors — then our clear won again. The `palettes.ini` path never
flickered because there the copies were taken from already-custom storage, so both paths drew
the same colors.

## The fix

Own the copies instead of fighting the link:

- `CharPaletteHandle::ReplacePalArrayInMemory` bumps a write generation on every storage write.
- Once per frame, for a Platinum player whose generation changed,
  `PaletteManager::SyncOwnedCustomPaletteSlots` walks `g_gameVals.pEntityList`, and for every
  entity with `ownPaletteId >= 0` whose root owner (`+0x1E8`) is that player, resolves the slot
  as above, and if its data differs from her live storage for that entity's `paletteFileIndex`,
  writes both `0x400` blocks and sets the dirty flag.
- The link (`+0x35C`) is no longer touched, unless a slot could not be resolved — then the old
  clear runs as a fallback and the sync is retried next frame.

Nothing here calls into the game: it is pointer reads with the same bounds checks the engine's
own lookups perform, plus a 256/32 shape check on the object, inside SEH.

This also removes the per-frame write in unmodified play entirely (previously gated only on
`IsCustomPaletteActive`), and it makes "back to Default mid-match" correct, which the clear never
was: the slots held the custom colors, so she showed them while holding an item after reverting.

### What this enables

Any of the 40 slots can now be written independently. A per-item palette feature
(`PlatinumPerItemPaletteFeasibility.md`) no longer needs to swap her whole storage on item
change: write the wanted palette into the slot of the `PaletteControlObj` that item type maps to
(the mapping table is in the older doc) and leave her storage alone. The write is the same
three lines: two `memcpy`s and the flag.

## Verifying in-game

Debug log lines to grep (log level 2):

- `PaletteManager::SyncOwnedCustomPaletteSlots wrote N item palette slot(s), 0 unresolved` —
  expected once per palette change while she is loaded, with `N` = 8 the first time and 0..8 for
  later edits (equal data is skipped).
- `... slot sync incomplete, dropping item palette link` — the fallback fired; the mechanism is
  not doing its job and the older behaviour is covering.

Steps:

1. Training, Platinum on either side, an unmodified `palettes.ini` slot for her (Default).
2. Open the palette editor, change some obvious colors on the Character file, apply.
3. Pick up an item (5D) and hold it, walk around, jump, use the item.
   Expected: the edited colors throughout, no single-frame flashes of the default palette.
4. Switch back to Default in the mod menu while holding an item. Expected: default colors at
   once, also while holding the item (the old fix got this wrong).
5. Optional: a second Platinum on the other side with a `palettes.ini` palette, to check the
   ini path still works and neither player's log shows `unresolved`.
