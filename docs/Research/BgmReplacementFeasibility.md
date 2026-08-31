# BGM Replacement Feasibility — reverse engineering report

**Date:** 2026-08-31
**Question:** can the mod replace any in-game track with a user-supplied song, reversibly,
without modifying any shipped file (so Steam file verification has nothing to restore),
covering Astral Heat music, and without desyncing online?

**Verdict: yes — confirmed in-game 2026-08-31 (see §9).** The mechanism is a single data
write; no code hooks required. Details below.

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

## 5. Converter work — DONE (2026-08-31)

`BuildSoundBank(cueName)` used to ignore its parameter and return a fixed blob with
`000_btl_rg` baked in. It now honours the name, and the surrounding container geometry was
corrected to match the shipped files exactly.

**Sound bank layout** (confirmed against four shipped banks of differing name length, which
are byte-identical apart from these fields and a build GUID at `+0x08`):

- fixed 0x120-byte prefix, then the name + NUL as a variable-length tail
- the name also sits in two 64-byte zero-padded fields at `+0x4A` and `+0x8A`
- u16 at `+0x1E` = name length + 1
- total size = `0x120 + len + 1`

Generating a bank for a shipped track's own name reproduces that track's real `.xsb`
byte-for-byte (bar the build GUID) for every battle-family track tested, including the
longest native name, `088_btl_bangthem2_short` at 23 chars.

**Two container bugs found and fixed** while verifying against all 186 shipped `.pac` files:

| field | was | correct |
|---|---|---|
| FPAC `stride` | `nameField + 16` | `align16(nameField + 16)` |
| FPAC `nameField` | `align4(len + 1)` | `align4(len + 2)` |

The stride error produced a 4/8-byte-misaligned file table no shipped file has. The
`nameField` error was subtler — it only shows on the 21 files whose name length is already
4-aligned, which is exactly why an exhaustive check was worth running. With both fixed,
the generated header and file table match **all 186 shipped files with zero mismatches.**

**Replacements do not generate a sound bank at all.** Shipped banks carry per-track
authoring values beyond the name — a pair of `0`/`0xFFFF` reference fields at `+0xFA` and
near the tail differ between tracks — so `ConvertMp3ToReplacementPac` reads the original
`.pac`, reuses its `.xsb` **byte for byte**, and swaps only the wave bank (named to match,
since the reused bank refers to it by name). The replacement then behaves exactly like the
track it stands in for, with no guesswork. `BuildSoundBank` is still needed for the
Jukebox's own custom tracks, which have no original to borrow from.

Cached `.pac` files from an older converter carry a stale cue name, so a `CONVERTER_VERSION`
stamp in the custom folder forces a rebuild when the layout changes.

New API in `CustomMusicConverter.h`:

```cpp
bool ConvertMp3ToPac(mp3Path, outPacPath, cueName, errorOut);              // new custom track
bool ConvertMp3ToReplacementPac(mp3Path, originalPacPath, outPacPath, errorOut); // stand-in
```

`MusicManager`'s play path no longer special-cases custom tracks: every `.pac`'s cue is
named after its own base filename, so the requested cue is always just the bgm name.

### Playback confirmed end-to-end (2026-08-31)

`01 Centralfiction.mp3` from the game's own Digital Extras was converted as a stand-in for
`000_btl_rg` and the table entry redirected at it. In Training with that song selected, the
MP3 played instead of Ragna's theme, and looped cleanly.

The heard track was not immediately recognisable, so it was verified offline rather than by
ear. Extracting the generated wave bank, wrapping it as xWMA and decoding with ffmpeg:

| | generated | source MP3 |
|---|---|---|
| decoded duration | 104.2576 s | 104.2548 s |
| mean level | 8340.8 | 8274.3 |

Loudness-envelope correlation over 1042 100 ms frames: **0.994**. Same audio. (The track is
an instrumental by 石渡太輔 / Daisuke Ishiwatari, which is why it sounded like an Astral
theme rather than anything obviously "custom".)

This exercises the whole chain: MP3 → WMA transcode, wave bank build, byte-for-byte reuse
of the original sound bank, FPAC assembly with the corrected geometry, subdirectory
redirect via the table pointer, cue resolution by name, and clean looping.

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

## 9. In-game verification — both questions answered (2026-08-31)

Tested with `BgmTableProbe` (`src/Audio/BgmTableProbe.*`, temporary instrumentation exposed
in the Debug window). Log evidence in `BBCF_IM/DEBUG.txt`; table resolved to `0x00A4C650`,
i.e. an ASLR base of `0x470000` — the RVA-relative approach handles relocation correctly.

**Subdirectories work — CONFIRMED.** Entry [0] was pointed at `imtest/000_btl_rg.pac` (a
copy of the original placed in a new subfolder). Ragna's theme played normally in Training.
So the reader resolves `data/sound/BGM/<subdir>/<file>.pac`, and replacements can live in a
mod-owned subfolder without touching a single shipped file.

**The table is re-read on every load — CONFIRMED.** Entry [0] was then pointed at a
non-existent file. Music kept playing for the rest of that match (nothing reloads mid-match),
and on re-entering Training the game reached `OnMatchInit` and then died. It could only have
died there by acting on the newly-written pointer, so the entry is re-read per load and is
not cached across matches. A replacement therefore takes effect on the next load of that
track, with no restart needed.

**NEW — a missing target file wedges the game.** This was not a graceful fallback to silence:
the game froze and had to be killed, producing no crash report, with `DEBUG.txt` ending
mid-match-init. That signature matches the hang aikuxa documented for the match-summary black
screen — the main thread waiting forever on an audio clock that never starts because the bank
never loaded.

**This is a hard requirement on the feature:** never write a pointer without first confirming
the target file exists, and re-validate every assignment at startup (the user can delete or
move a converted file between sessions). Treat a missing target as "assignment disabled,
restore the original pointer, tell the user" — never as something to pass to the game.

On the upside, the crash confirmed reversibility for free: the patch is process memory only,
so the relaunched game came up with a pristine table and all 186 shipped files untouched.

## 9a. Remaining minor unknown: which track is playing

None of the three "what is playing" sources agreed during the test (`audioMgr+0x1690` = 0,
`musicSelect_X` = 2, `MusicManager` = 0). `audioMgr+0x1690` is already documented as
unreliable during matches (`MusicManager.cpp:971`), and `musicSelect_X` is paired with a
`musicSelect_Y`, so it is likely a 2D menu-cursor coordinate rather than a track id —
which would make `MusicManager::Initialize`'s use of it as a startup track id a latent bug
worth a second look.

This does **not** block the replacement feature, which is driven by an explicitly chosen
table index and never needs to know what is currently playing. It only matters for the
Jukebox's own display, which has its own working detection path.

## 10. Reproducing the table dump

`grep` returns nothing on `tools/bbcf_disasm_ascii.txt` — use `awk` instead.

```python
# table VA 0x9DC650; walk 4-byte pointers, resolve each through the PE section map
# entries 0..159 are BGM .pac names, 160..174 are voice format strings
```

---

## 11. Astral Heat music — where each of the twelve is actually used (2026-08-31)

Prompted by a report that replacing all twelve Astral tracks changed nothing in game.

**It was not a bug.** The log showed the last match loaded at 23:35:04 and the Astral
pointers were patched between 23:37:23 and 23:38:16 — all *after* that match had started.
Astral music is loaded up front by `FUN_00555A20` along with the rest of the match's audio,
so a swap made mid-match cannot be heard until the next match starts. The UI now says this
in colour rather than grey small print.

**Only 9 of the 12 entries are ever read.** Each of indices 97–105 is referenced exactly
once, all inside `FUN_00555A20`. Indices 106–108 (`609/610/611_cs_astral_*`) are referenced
**nowhere in the executable**, and are byte-identical duplicates of `600/601/602`
(537696 / 295808 / 205600 bytes). They are dead weight; the browser now refuses to replace
them and says so.

**Which set loads is a user setting.** The loader reads a saved option — global
`0x16BB0B0`, written by an options-apply routine at `0x6B4701`, menu id `MOGO_AstralHeatBGM`
with items labelled `MOGO_AstralHeatBGMV%d` — into `[+0xBBB0]`, and switches on it three
times, once per variant slot:

| option value | set loaded | durations |
|---|---|---|
| 0 | `603/604/605_cs_astral_*` | 26.4 / 20.2 / 20.2 s |
| 1 | `600/601/602_astral_*` | 47.8 / 25.9 / 17.7 s |
| 2 | `606/607/608_v2_astral_*` | 33.2 / 24.0 / 24.0 s |

Only the selected set is loaded, so replacing the other two sets has no effect until the
player changes that option.

**All three variants of the chosen set are resident at once.** They are loaded into tags
`0x41`/`0x42`/`0x43` and registered together into sound-registry slot 5 at `0x56BACB`,
alongside `088_btl_bangthem2_short` (tag `0x40`) — Bang's theme, the other in-match special
cue. Playback picks one of them by index at Astral time.

**Not determined:** which of A/B/C plays in which situation. The play path does not read the
filename table (each Astral entry has exactly one xref, in the loader), so the choice is an
index into registry slot 5 made by game logic that was not traced. Practically this does not
matter — replacing all three variants of a set covers every case, which is what the browser
now tells the user to do.

## 12. Working-directory hazard (2026-08-31)

A user reported the mod creating a `BBCF_IM` folder inside the folder they had picked an MP3
from. Reproduced: `Downloads/Kam te juego/BBCF_IM/` contained both `bgm_replacements.ini`
**and** `FrameStallIncidents.log`, so this is not specific to one feature.

`NativeFileDialog` is not at fault — it passes `OFN_NOCHANGEDIR` and restores the working
directory before publishing its result. The gap is that the shell moves the working
directory *while the dialog is on screen*, and anything that writes a relative path during
that window lands in the browsed folder. Queueing several conversions while opening more
pickers is exactly that situation.

Fix: `GetGameDirectory()` / `GamePath()` in `Core/utils`, resolved once from the executable's
own module path. The music code now anchors every filesystem path through it.
`UnlimitedPlaybackManager`, `UnlimitedReplayTakeoverManager` and the frame-stall logger still
use relative paths and have the same latent bug.

