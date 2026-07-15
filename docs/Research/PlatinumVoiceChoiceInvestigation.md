# Platinum Voice Choice (Sena/Luna) Investigation (2026-07-15)

**Verdict: flag identified and confirmed by two independent methods. Sync-across-network status is unresolved (no evidence either way) — treat the settings.ini override as a local-only cosmetic force unless/until proven otherwise.**

## Background

Platinum the Trinity randomly picks one of two "personalities" (fan-named Sena/Luna) each
time she's selected, affecting her voice lines (and, per the memory-diff evidence below,
possibly some move data too). Goal: find the flag controlling this and add a
`PlatinumVoiceChoice` settings.ini override.

## Method 1: empirical memory diffing (DEBUG.txt)

Added a temporary hook dumping Platinum's `CharData` block to `DEBUG.txt` on load, then ran
labeled training-mode sessions ("voice 1"/"voice 2", user-identified as Sena/Luna) and
diffed byte-for-byte across sessions.

- No difference in any documented `CharData` field.
- Found a large (~12KB, offsets ~0x2278-0x5170) undocumented padding region containing what
  looks like per-move active-frame debug strings (`pt417`, `a9c7c8`-style pairs). Content
  differed consistently between Sena/Luna sessions (208 offsets, 0 exceptions across 9
  independent loads), including *before* any inputs were thrown — ruling out "just an
  action-dependent cache coincidence." This suggests personality may affect more than just
  voice (frame data?), but it's a downstream effect, not the root flag, so it wasn't
  pursued further.
- Extended the dump to the small per-player "select struct" that the existing
  `GetPaletteIndexPointers` hook already points into (used for palette-index tracking).
  This struct sits at a **fixed, static address** (not heap-allocated), one slot per player,
  0x20 bytes apart. Diffed across 5 labeled sessions (voice 1,1,2,2,2... etc across two
  separate test runs, 14 samples total): a 4-byte field at **struct+0x4** matched the
  reported voice with **zero exceptions**: `1 = Sena, 0 = Luna`. struct+0x0 = character
  index (confirmed 0x11 = 17 = Platinum in every sample).

## Method 2: static disassembly (BBCF.exe v8.0 dumpbin dump, `tools/bbcf_disasm_ascii.txt`)

Note: the disasm file has a stray UTF-16 BOM (`FF FE`) despite being plain ASCII after that
— `file` misdetects it as UTF-16 and breaks naive `grep`. Strip the first 2 bytes
(`tail -c +2`) before grepping, or tools will silently return zero matches.

Located the copy function that commits the per-player select struct into its battle-time
copy (`edx+0x1648..0x1668` → `edx+0x24D8..0x24F8`, 0x20 bytes/player, matches the 0x20
player stride found empirically):

```
0047D910: mov edx,ecx           ; edx = select-struct singleton base (from 0047E860())
  ...
0047D927: lea esi,[edx+1648h]   ; source = "live" struct (what our hook captures)
0047D92D: lea edi,[edx+24D8h]   ; dest = "committed" struct (battle-time copy)
0047D933: mov ecx,8
0047D938: rep movs dword ptr es:[edi],dword ptr [esi]   ; 0x20 bytes = 8 dwords
```

Traced backward to the write site for `+0x164C` (== struct+0x4, our confirmed flag),
inside character-select confirm handling:

```
0047DA02-0480AA2: cmp dword ptr [edi+108h],10h   ; mode/context gate
0047DA02-0480B1A: jne  <skip the randomization block entirely>
  ...
0047DA02-0480A18: call 0040BF00       ; per-stream RNG, used just above for rand()%10 (palette 0-9)
0047DA02-0480B5A: call 0040BF00       ; same RNG, this draw feeds the personality flag
0047DA02-0480B64: and edx,80000001h   ; classic MSVC signed-mod-2 idiom
0047DA02-0480B71: mov esi,[ebp+8]     ; player index (0/1)
0047DA02-0480B74: shl esi,5           ; * 0x20 stride
0047DA02-0480B7E: mov dword ptr [esi+edi+164Ch],edx   ; <-- writes our flag: rand()%2
```

`0x0040BF00` resolves to `GetRandomStream(streamIndex)->Next()`-style wrapper: indexes into
an array of RNG-state objects (`streamIndex*0x9CC + [0xA135C8]`, 0x9CC-byte stride per
stream — consistent with each stream being an independent PRNG instance, e.g. Mersenne
Twister-sized state). This is a **different** RNG entity than the battle-scoped
`AA_CRandomManager` referenced elsewhere in `GhidraDefs.h` (that one lives inside the
in-battle object-manager struct; this one is indexed from a standalone global array used at
character-select time).

Three read sites for the flag were also found (generic engine predicates, likely
script-callable), each checking one specific literal:
```
cmp dword ptr [esi+ecx+164Ch],1  ; sete al   (used elsewhere by 1+ characters, generic reuse)
cmp dword ptr [esi+ecx+164Ch],2  ; sete al
cmp dword ptr [esi+ecx+164Ch],3  ; sete al
```
Empirically we only ever observed 0 or 1 for Platinum; the `==2`/`==3` checks are presumably
for other characters reusing the same generic slot for unrelated purposes (BBCF's engine
reuses generic per-character "variant" fields across the cast — same pattern as the `SLOT_`
fields already documented in `CharData.h`).

## Network sync: unresolved

Grepped for all other references to the "committed" destination offset (`+24D8h`/`+24DCh`)
in the whole binary: exactly one other hit, a mirror-image **restore** function
(`0047FD20`, committed → live, likely used when returning to character-select from a match
to repopulate the editable UI state). No socket/send/recv calls appear anywhere near either
copy function or their callers.

Per `SpectatorDesyncInvestigation.md`, BBCF's netcode is a GGPO rollback port: **no state
crosses the wire during a match** — correctness relies entirely on deterministic replay of
synced inputs from frame 0. That guarantee applies to the *in-match simulation*; the
personality roll happens during the pre-match character-select phase, which is a distinct
(and not necessarily deterministic-lockstep) phase. Found no evidence this flag is
explicitly transmitted in a select/ready packet, but also found no evidence it *isn't* —
this would need either a live two-client packet capture or locating the
character-select-state serialization code (not identified in this pass).

**Working assumption until proven otherwise: treat this as a local-only cosmetic pick.**
Each client may roll and hear a different personality; there is no confirmed guarantee the
opponent hears the same choice you force locally.

## Implementation

- `PlatinumVoiceChoice` setting added (`src/Core/settings.def`): `0` = vanilla RNG (default),
  `1` = force Sena, `2` = force Luna.
- Enforced in `src/Hooks/hooks_palette.cpp`, `ForcePlatinumVoiceChoice()`, called from the
  existing `GetPaletteIndexPointers` hook for both players. Overwrites struct+0x4 right
  before the commit copy (`0047D910`) runs, so both the live and committed copies end up
  forced consistently.
