# Platinum item-state custom palette bug — investigation

**Status:** root-caused and fixed (2026-07-26). Fix lives in
`PaletteManager::ClearPlatinumItemPaletteLink`, called once per frame from `MatchState::OnUpdate`.

All addresses below are **static VAs** (image base `0x00400000`). Subtract `0x00400000` for the
`bbcf_base_adress + RVA` form used by `GhidraDefs.h` and the hook code.

---

## The report

Selecting a custom palette for Platinum **mid-match** from the mod menu worked normally until she
grabbed a drive item; while holding the item she reverted to her native color slot, and the custom
palette came back the moment the item was used up. Assigning the very same palette through
`palettes.ini` instead had no such problem — item states stayed custom.

Documented for years as known issue #2 in `USER_README.txt` ("Platinum keeps swapping between her
default and the selected custom palette whenever she has her drive active") with the same
workaround, but never root-caused.

## Root cause

Platinum's script runs a character-specific BBScript command, **`PT_LinkColor`**, while she holds
an item. Its handler:

| Address | What it does |
|---|---|
| `0x005B2AC3` | `PT_LinkColor` command-name compare (string `"PT_LinkColor"` @ `0x00954D40`) |
| `0x005B2AE1` | `mov [ebx+35Ch], -1` — item type is 0, i.e. **"no link, draw from my own palette"** |
| `0x005B2AFE`–`0x005B2B0D` | `add eax,-3` / `cmp eax,0Dh` / jump table on item type (`[ebx+1A0h]`) |
| `0x005B2B14`–`0x005B2B45` | per-case load of a `PaletteControlObjN` name string |
| `0x005B2B54` | `FUN_0055C540` — find scene object by name |
| `0x005B2B5B` | `FUN_0055DF60` — resolve it |
| `0x005B2B66` | `mov [ebx+35Ch], eax` — **store that object's palette id (`+0x358`) as the character's linked palette** |

`ebx` is the player's `CharData`:

- `CharData+0x1A0` — item type. Already named `SLOT_59 //Plat item type` in `src/Game/CharData.h`.
- `CharData+0x35C` — the linked palette id, `-1` = none. Now named `linkedPaletteId`.

While the link is set, the renderer draws her from the `PaletteControlObj`'s palette **instead of
her own palette storage** — the storage the mod patches.

**Those `PaletteControlObj` palettes are built once, when the round loads, and nothing refreshes
them afterwards.** That is the entire bug:

- `palettes.ini` works because `PaletteManager::OnMatchInit` patches the palette storage *before*
  the round's palettes are built, so the copy they capture is already the custom one.
- A mid-match switch writes the storage correctly but cannot reach the already-built copy.
- A round reset (training backspace) fixes it, because the rebuild re-captures current storage.

### Item type → palette object mapping

Item types `3..16` decode through the byte table at `0x005B52D0` and the jump table at
`0x005B52B4`, pairing up two-per-object:

| Item type | Object |
|---|---|
| 3, 4 | `PaletteControlObj2` |
| 5, 6 | `PaletteControlObj3` |
| 7, 8 | `PaletteControlObj4` |
| 9, 10 | `PaletteControlObj5` |
| 11, 12 | `PaletteControlObj6` |
| 13, 14 | `PaletteControlObj7` |
| 15, 16 | `PaletteControlObj8` |
| anything else in range | `PaletteControlObj1` (default case at `0x005B2B45`) |
| 0 (no item) | no link — `-1` written at `0x005B2AE1` |

Name strings live at `0x00954D50`, `0x00954D64`, `0x00954D78`, `0x00954D8C`, `0x00954DA0`,
`0x00954DB4`, `0x00954DC8`, `0x00954DDC`. The only code reference to them is this handler; the
objects themselves are created from data, not from code.

In vanilla all eight hold what is effectively her ordinary palette, so the redirection is
invisible — which is exactly why the bug looks like "the custom palette stopped applying" rather
than "she turned a different color".

## Fix

`PaletteManager::ClearPlatinumItemPaletteLink` runs once per frame and, **only while a custom
palette is actually active**, writes `-1` to `CharData::linkedPaletteId` for a Platinum player.

Two properties make this safe rather than a hack:

- `-1` is not an invented state — it is the value the *same game handler* writes at `0x005B2AE1`
  when she has no item, meaning "draw from my own palette storage".
- The gate is `CharPaletteHandle::IsCustomPaletteActive()`, which compares the live palette bytes
  against the match-start backup rather than trusting a flag. It is therefore true for
  `palettes.ini`, the palette editor, and an opponent's synced palette alike, and false in
  unmodified play — so vanilla behaviour is untouched.

Per-frame (rather than hooking the store at `0x005B2B66`) because the command only runs when the
item is obtained: if the player picks a palette *after* grabbing, a hook at the store site would
never fire and the stale link would persist.

`CharData+0x35C` is read in the render path only, so this is expected to be desync-safe online,
but that has not been verified against a real online match.

## How it was narrowed down (what to skip next time)

Ruled out by runtime instrumentation, in order — each of these is a dead end:

1. **Palette index switching.** `palIdx` stayed on the patched slot for every item-state frame.
2. **Data being wiped.** The patched slot kept the custom bytes the whole time; nothing restored
   them.
3. **A third palette container.** `GetPalBaseAddresses` fires three times per match load and the
   mod keeps only two (P1/P2), which looked damning — but the third resolves only 2 slots, is not
   a 24-slot character container, and belongs to a different resource. (Writing to it as if it
   were one corrupts the heap and crashes on the next item grab.) The owning objects carry a
   `vr<charcode>` tag at `owner+0x834` matching `char_%s_vri.pac` (`0x008A61D0`); Platinum's is
   `vrpt`.
4. **Extra copies of the palette data.** Only the primary and the known `+0x800` duplicate exist;
   a `0x800`-stride walk out to `+0x8000` found nothing else.
5. **Stale mirrors anywhere in memory.** A full committed-memory scan found 18 byte-exact copies
   of her original palette at match start. Overwriting *all* of them with the custom data (15
   remained after our own writes) still left her rendering default — proving the palette the item
   state uses is no longer in system memory in that form at all.
6. **Forcing a re-upload via the palette index.** Neither the mod's existing toggle trick nor
   moving her onto an index unused that match refreshed it — the snapshot is per-object and
   index-independent.

The decisive clue was the `PaletteControlObj1..8` strings, which matched the eight stale mirrors
found at a `0x1108` stride in step 5.

## Ghidra artifacts

Generated with the runners in `docs/Research/` (see `GhidraHeadless.md`):

- `run_ghidra_palette_symbols.cmd` → `PaletteSymbolsGhidraReport.txt` — every palette-related
  symbol, RTTI class (`AA_CPalette_Custom`, `AA_CPalette_HIP`, `AA_CPaletteFactory_*`) and string.
  This is what surfaced `PaletteControlObj1..8`.
- `run_ghidra_palette_control_objs.cmd` → `PaletteControlObjsGhidraReport.txt` — everything
  referencing those names and the palette vtables. Contains the `PT_LinkColor` handler's caller.
- `run_ghidra_palette_link.cmd` → `PaletteLinkGhidraReport.txt` — the `+0x358` / `+0x35C`
  producers and consumers.
- `run_ghidra_platinum_item_palette.cmd` → `PlatinumItemPaletteGhidraReport.txt` — the palette
  container loader `FUN_005B6310` (stores the container at `owner+0x830`; the mod's
  `GetPalBaseAddresses` hook site is the store at `0x005B6372`) and its consumers.

## Follow-up: per-item palettes

The engine already has a **seven-way per-item palette mechanism** here that vanilla fills with
identical copies. See `docs/Research/PlatinumPerItemPaletteFeasibility.md`.
