# BGM Replacement Feasibility — reverse engineering report

**Date:** 2026-08-31
**Question:** can the mod replace any in-game track with a user-supplied song, reversibly,
without modifying any shipped file (so Steam file verification has nothing to restore),
covering Astral Heat music, and without desyncing online?

**Verdict: yes.** The mechanism is a single data write — no code hooks required. Details below.

---

## 1. The filename pointer table at `0x9DC650`

`.rdata` holds an array of 175 `char*` pointers. Everything the game's audio loader opens
by name goes through it.

| index range | contents |
|---|---|
| 0–159 | BGM `.pac` filenames (`000_btl_rg.pac` … `bgm_end_vh.pac`) |
| 160–174 | voice-bank format strings (`vbtl_%s_%d.pac`, `subvoice/vbtl_%s_%s.pac`, …) |

Index 0 is `000_btl_rg.pac`; the numeric prefix of each filename is the game's track id, but
**the table index is not the track id** — the table is packed in file order. Index 8 is
`008_btl_bn.pac`, index 97 is `600_astral_a.pac`. The full index→name map is dumped by the
script in §6.

The voice-bank entries at 160+ are the same table the Platinum voice investigation called
"table 0x9DC8D0" (`0x9DC650 + 160*4 = 0x9DC8D0`). Same table, already partly documented.

## 2. How a load assembles a path

Verified at `0x74B8AC` (the lobby-BGM randomizer, which picks among indices 78–83):

```
0074B8AC: push 8A4150h            ; "data/"
0074B8B1: lea  ecx,[buffer]
0074B8BB: call 00408970           ; string assign   -> "data/"
0074B8C0: push 8A4238h            ; "sound/BGM/"
0074B8C5: lea  ecx,[buffer]
0074B8CB: call 0045F8D0           ; append          -> "data/sound/BGM/"
0074B8D0: push dword ptr [esi*4+009DC650h]   ; <-- THE TABLE
0074B8D7: lea  ecx,[buffer]
0074B8DD: call 0045F8D0           ; append          -> "data/sound/BGM/212_lobby6.pac"
0074B8EB: call 0054FEE0           ; load (MusicManager's LOADER_RVA)
0074B8FB: call 0047C930           ; register/allocate (MusicManager's REGISTER_RVA)
```

So: **path = `data/` + `sound/BGM/` + table[idx]**, appended verbatim. The string is
dereferenced fresh on every load (byte-copied into a stack buffer, see `0x685B37`), never
cached — so a patched pointer takes effect on the next load of that track.

Because the filename is appended verbatim, a **subdirectory in the table string works**.
The native voice entries prove the pattern (`subvoice/vbtl_%s_%s.pac`).

Table read sites (indexed): `0x5566E4`, `0x66E401`, `0x685B14`, `0x6E9B20`, `0x74B8D0`,
`0x76C41C`. Plus many absolute reads of individual entries (e.g. `[0x9DC7A4]` = index 85).
All read the same memory, so patching an entry covers both forms.

## 3. Astral music goes through the same table

Indices 97–108 are the twelve Astral tracks. They are read by absolute address inside
**`FUN_00555A20`** — the per-match audio asset loader already identified by the Platinum
voice work — switching on a value at `[+0xBBB0]` to pick which of the three variant sets to
mount:

```
0055678F: push [009DC7D4]   ; idx 97  -> 600_astral_a.pac
00556797: push [009DC7E0]   ; idx 100 -> 603_cs_astral_a.pac
00556787: push [009DC7EC]   ; idx 103 -> 606_v2_astral_a.pac
...same pattern for _b (98/101/104) and _c (99/102/105)
```

Two consequences:

- **Astral replacement is free** — same table, same patch.
- **Astral BGM is loaded at match load, not when the Astral is triggered.** So a larger
  replacement file cannot stall the game mid-fight. `FUN_00555A20` is also async (worker
  thread, `CreateThread 0x4506AD`; the game thread only enqueues), so it cannot stall the
  game thread at all.

## 4. Cue names — the constraint that shapes the design

Every `.pac` contains an `.xsb` sound bank whose single cue name is **exactly the base
filename**, at any length. Verified by unpacking real files:

| file | xsb cue name | len |
|---|---|---|
| `000_btl_rg.pac` | `000_btl_rg` | 10 |
| `600_astral_a.pac` | `600_astral_a` | 12 |
| `950_btl_rgvsjn_old.pac` | `950_btl_rgvsjn_old` | 18 |
| `084_btl_bangthem_short.pac` | `084_btl_bangthem_short` | 22 |

`CustomMusicConverter`'s `MAX_CUE_NAME_LEN = 14` is a self-imposed limit, not a game limit.

This is corroborated from the other direction: `MusicManager::PlayTrackPhysically` plays
native tracks by requesting cue name = lowercase base filename, and that works in-game.

**Design consequence:** keep the original base filename and change only the directory.
Replace `008_btl_bn.pac` by pointing table[8] at `custom/008_btl_bn.pac`. The base name is
unchanged, so whatever the game derives the cue request from, it matches.

## 5. Converter work required

`BuildSoundBank(cueName)` currently **ignores its parameter** and returns a fixed 299-byte
blob with `000_btl_rg` baked in (which is why the play path hardcodes that cue for custom
tracks). To emit a replacement for an arbitrary track it must write the target's name.

The name appears at three offsets in the template:

| offset | following zero bytes | room |
|---|---|---|
| `0x04A` | 54 | 64-byte fixed field |
| `0x08A` | 54 | 64-byte fixed field |
| `0x120` | 1 (end of file) | variable-length tail |

The two 64-byte fields take any name up to 63 chars in place. The tail string at `0x120`
changes the file length, so the SDBK header size fields need fixing up — real native banks
vary in size with name length (299 / 307 / 311 bytes for 10 / 18 / 22-char names), which
confirms the tail is the variable part. Bounded work, not research.

## 6. Desync analysis

**This is materially safer than the Platinum voice attempt, and the reason is specific.**

The voice desync was never about audio. The personality flag at `+0x164C` is read by
script-VM opcodes (`0x57D3F3/416/439`) and is part of checksummed battle state; forcing it
to a value differing from the opponent's diverged the sim. The *resolved, shipped* approach
was exactly the pattern proposed here — never touch synced state, only bias which audio file
gets mounted client-side, because "the loaded handle is a per-client heap ptr, NOT in the
GGPO checksum."

Here we go one step further from the sim: **no game state changes at all.** Both clients
keep identical track ids, identical RNG, identical everything. The only divergence is which
bytes come off the local disk. There is no channel by which that reaches the checksum.

Corroborating: aikuxa's Jukebox already swaps *playing tracks* mid-match online — stopping
banks, re-initialising cue arrays over live XACT state — and reportedly does not desync. A
filename swap is far less invasive than that.

Remaining (small) risk: load time. Mitigated by the loader being async on a worker thread,
and by keeping converted files near native bitrate (the converter already targets ~96 kbps
CBR, matching native 44.1 kHz tracks).

## 7. Coverage

159 distinct entries are replaceable via this table:

- all 36 character battle themes, all 14 versus themes, all 14 boss themes
- **all 12 Astral tracks**
- all 30 "old" (previous-game) themes
- menus, title, character select, continue, game over, 6 lobby tracks
- 18 `bgm_end_*` ending themes (these live under `data/sound/bgm_End/`, a separate prefix
  string, but are in the same table)

**Not covered:** ~52 story-mode tracks (the 300/400-series `nichijo` / `dorama` / `horror` /
`memory` etc.). These do not appear in this table and must be loaded through a different
path — not investigated, and not needed for the stated goal.

Note: lobby entries differ in case between the table (`207_lobby.pac`) and disk
(`207_Lobby.pac`). Irrelevant on Windows, but do not "fix" it.

## 8. Proposed implementation

1. At startup, snapshot all 175 original pointers.
2. User imports an MP3 and picks a track to replace. Converter writes
   `data/Sound/BGM/<modfolder>/<original_base_name>.pac` with the cue named
   `<original_base_name>`.
3. `VirtualProtect` the table page writable, write a pointer to a mod-owned string
   `"<modfolder>/<original_base_name>.pac"`, restore protection.
4. Unassign = write the snapshotted original pointer back. Reset all = restore the snapshot.

No shipped file is ever modified, so **Steam file verification finds everything intact** and
has nothing to restore. Our files are new files in a new subfolder, which verification
leaves alone.

This is a pure data patch. No JMP hooks, no naked functions, no branch-target hazards —
strictly safer than anything the mod already does to the game.

Preview in the overlay is nearly free: `PlayTrackPhysically` already loads an arbitrary
`.pac` by path and plays a named cue, which is exactly what previewing a replacement needs.

## 9. Open items to confirm in-game

- A subdirectory in the table string is accepted by the BGM reader. Strong static evidence
  (native voice entries use one), not yet empirically confirmed on the BGM path.
- Whether a loaded bank is cached across matches, i.e. whether changing a replacement takes
  effect immediately or only on the next load of that track.

## 10. Reproducing the table dump

`grep` returns nothing on `tools/bbcf_disasm_ascii.txt` — use `awk` instead.

```python
# table VA 0x9DC650; walk 4-byte pointers, resolve each through the PE section map
# entries 0..159 are BGM .pac names, 160..174 are voice format strings
```
