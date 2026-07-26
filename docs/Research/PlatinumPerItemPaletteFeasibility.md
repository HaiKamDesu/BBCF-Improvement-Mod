# Feasibility: a different custom palette per Platinum item state

**Status:** assessment only, no code written (2026-07-26). Background:
`docs/Research/PlatinumItemPaletteInvestigation.md`.

**Verdict: easy.** The reverse engineering is already done, the runtime signal we need is a single
`int` the mod can already read, and the palette-swapping machinery exists and is proven. The work
is almost entirely config + UI plumbing, not engine work. Rough estimate: **half a day to a day**,
of which the actual palette logic is ~50 lines.

---

## What the game gives us for free

`CharData+0x1A0` (`SLOT_59`, already named `//Plat item type` in `src/Game/CharData.h`) holds
Platinum's current item type: `0` for none, `3..16` while holding one. It pairs up two-per-object
into seven distinct item groups — see the mapping table in the investigation doc. So the natural
granularity is **8 palettes: "no item" + 7 item groups**, and the pairing is the game's own, not
something we impose.

Whether each pair is two items or one item in two states (fresh/expiring) is unconfirmed; her
action names expose the identities (`NmlAtk5D_Bomb`, `NmlAtk5D_Harisen`, `NmlAtk5D_Missaile`, …)
and one training session with a logger on `+0x1A0` would settle the naming for a UI.

## The approach that works

Do **not** try to make the engine's own seven `PaletteControlObj` palettes hold different data.
That is the mechanism the bug lives in: those palettes are built at round load and are past the
reach of memory writes afterwards (established conclusively — a full-memory patch of every copy
did nothing). Driving them would mean finding and re-running the engine's palette upload, which is
real RE work for no user-visible benefit.

Instead, keep the shipped fix (the item link stays cleared, so she always renders from her own
palette storage) and swap **her own storage contents** when the item type changes:

```
each frame, for a Platinum player:
    itemGroup = groupOf(charData->SLOT_59)          // 0 = none, 1..7 = item groups
    if (itemGroup != lastAppliedGroup)
        SwitchPalette(charIndex, palHandle, palettesForThisPlayer[itemGroup])
        lastAppliedGroup = itemGroup
```

`PaletteManager::SwitchPalette` already does everything needed: writes all 8 palette files into
both toggle slots and both `+0x800` copies, sets the palette info (so per-palette bloom follows
along), and triggers the redraw toggle that makes the game re-upload. It is the exact call the
mod menu makes today, so per-item switching is the same operation the user already performs by
hand — just driven by item type instead of a click.

Cost per switch is ~32 KB of memcpy plus one index toggle, at human timescales (a few times per
round). Negligible.

## What actually costs time

1. **Config surface.** `palettes.ini` is keyed by ingame color slot 1–24 (`LoadPaletteSettingsFile`
   loops slots per character section). Per-item palettes need a new key space — e.g. a
   `[Platinum_Items]` section with `none=`, `item1=`…`item7=` — plus the matching writer in
   `SavePaletteSettingsFile` and storage alongside `m_paletteSlots`.
2. **UI.** `PalettesConfigWindow` renders a uniform 24-slot grid per character. A Platinum-only
   extra panel is a special case in a window that currently has none — this is the single biggest
   chunk of the work.
3. **Localization.** New strings through `resource/localization/Localization.csv` and the
   codegen step.
4. **Palette editor interaction.** The editor edits "the current palette". If the item type
   changes while the editor is open, the palette underneath it changes. Simplest resolution:
   suspend per-item switching while the editor window is open.

## Risks and open questions

- **Online.** The mod syncs one palette per player at match init (`OnlinePaletteManager`). Per-item
  switching would be **local-only** unless the packet is extended — your opponent would keep seeing
  your base palette while you see your per-item ones. Extending the protocol means a version-compat
  story with older clients. Recommendation: ship local-only first, and consider it a local
  cosmetic feature.
- **Desync:** none expected. This only writes palette storage the mod already writes, plus the
  index toggle it already performs.
- **The "no item" entry doubles as the base palette,** so it must stay consistent with whatever
  the user picked in the mod menu, or the two features will fight over the same storage. Cleanest
  rule: the menu/ini selection *is* the "no item" palette, and per-item entries are overrides
  layered on top.
- **Other characters.** The mechanism generalizes to any character with a state variable worth
  keying on (Bang's seals, Izayoi's Gain Art, overdrive), since nothing here is Platinum-specific
  except which field is read. Worth keeping in mind when designing the config format — a
  general "palette by state" table would age better than a Platinum-shaped one.
