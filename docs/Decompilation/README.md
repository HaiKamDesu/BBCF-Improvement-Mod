# Decompiling BBCF, one subsystem at a time

The long-term goal: understand all of BBCF.exe (Steam build 11073075) well enough to override
any part of it, and replace the parts that are broken with C++ in the mod. Method: the re3/reVC
approach. Take one subsystem, document every function in it, rewrite it in C++ inside the mod,
redirect the original functions to the rewrite, verify in-game, move on.

How to work on this: the `bbcf-decompile` skill (`.claude/skills/bbcf-decompile/SKILL.md`).
Joining in from another machine: [LocalSetup.md](LocalSetup.md) builds the same decompile from your
own copy of BBCF.exe (`tools/decomp/build_local_decomp.sh`), and explains why we never upload it.

## Keeping the archive complete

The archive should always hold everything this repo knows. It gets that from two sources:
- the ledger rows in these docs (Name column = the function's name in Ghidra, so use an
  identifier; Level and Notes become its comment)
- the `ADDR_` constants in `src/Game/GhidraDefs.h` (with the `//` lines above them as notes)

`tools/decomp/sync_archive.sh` applies both to the archive's Ghidra project (plate comments between
`[repo]` and `[/repo]`, so notes typed in the GUI survive), re-exports `decomp/` so the C carries
them too, and copies these docs into `knowledge/` with the commit they came from. Run it after any
session that adds rows or names; `--zip <path>` packs the result for handing over (private).

### Types: structs and signatures

The decompiler only prints `defender->invuln_timer` instead of `*(int *)(param_1 + 0x994)` when
it knows the struct and the function's signature. Both come from the repo too:

- **Struct layouts.** Game structs are declared in the mod's headers (`src/Game/CharData.h`,
  `Room/`, `ReplayFiles/`, `GhidraDefs.h`). Offset comments in those headers are hand-written and
  some are wrong, so `tools/decomp/gen_struct_layouts.py` asks MSVC for the real layout and writes
  `types/structs.json`, which goes into Ghidra as the `/BBCF` category (pad_* fields stay
  undefined, so their offsets still show). Name a field by splitting a pad in the header, and add
  a `static_assert(offsetof(...))` for it. Commit the regenerated JSON: Linux collaborators have
  no MSVC and use the committed copy.
- **Typed signatures.** `types/signatures.h` has one line per understood function:
  `/*0x0057EA70*/ int __cdecl Hit_PiercesInvul(CharData* defender, CharData* attacker);`. Add one
  once the convention is checked in the assembly. Functions without a line still get their
  convention and parameter names from a ledger note written as `thiscall(kbd, gameKey), ret 4`,
  when the stack argument count matches `ret N`. `decomp/types_report.txt` lists the notes that
  disagree with their own `ret`; those are doc errors to fix.

The goal is a decompile that reads like source, one subsystem at a time, until enough of it is
understood to run the game from our own code.

## Where things are

| What | Where | In git? |
|---|---|---|
| Preserved game build, hashes, manifest | `C:\BBCF_Archive\build_11073075\` | no (private) |
| Analysed Ghidra project with the repo's names and notes | `C:\BBCF_Archive\build_11073075\ghidra\` | no |
| Every function decompiled to C, function and string indexes | `C:\BBCF_Archive\build_11073075\decomp\` | no: decompiled game code is never committed |
| What each function *means*: this ledger and the per-subsystem docs | `docs/Decompilation/` | yes |
| Names and addresses the mod uses | `src/Game/GhidraDefs.h` | yes |
| Rewritten subsystems | `src/...` (listed below) | yes |

## Levels

Every function in a subsystem doc carries one level:

| Level | Meaning |
|---|---|
| **L0** | Machine-decompiled only (every function is at least this). |
| **L1** | Mapped: its role in the subsystem is known, and its callers/callees are listed. |
| **L2** | Understood: calling convention, arguments, return value and every field it touches are documented; ready to reimplement. |
| **L3** | Rewritten: replaced by C++ in the mod, hooked in, and verified in-game. |

A subsystem is *fully decompiled* when every function inside its boundary is L2, and
*rewritten* when every one is L3, or is deliberately left to the game and called from the
rewrite (record that, with the reason).

## Progress

Run `python3 tools/decomp_progress.py` for the live numbers. At 2026-10-09 (after the lobby
map), 1,988 functions are referenced with meaning in src/ or docs/, 15.4% of the code. The table
below is from 2026-10-07:

| | Functions | Code |
|---|---|---|
| In the binary | 20,257 | 4.21 MB |
| Machine-decompiled (L0) | 20,148 (+109 thunks/externals) | 100% |
| Named in Ghidra | 5,745, of which ~4,645 are library/CRT/RTTI auto-names | |
| Referenced with meaning in src/ or docs/ (≈ L1+) | 693 | 7.1% |
| Rewritten (L3) | 1 | 0.05% |

## Subsystems

| Subsystem | Doc | Status | Rewrite |
|---|---|---|---|
| Network-entry profile sync (D-Code sync task) | [ProfileSync.md](ProfileSync.md) | **rewritten** (task L3, helpers L2) | `src/Network/ProfileSync.cpp` |
| Profile transport (tus/read, tus/write, session, latch) | `docs/Research/DCodeNetworkStallBug.md` | partial L1/L2 | hooks only |
| Controllers and input assignment | [Controllers.md](Controllers.md) | **core L2** (256 fn: L2 135, L1 121, L0 0; 18.2% of its code at L2+). Device layer, controller tasks, wrappers, id getters, named queries and key-config apply are L2. Section 6 maps the game's key-config screens (battle/menu editors, char-select Current Set, capture, glyphs) and the per-device binding design. Their open, capture and page handlers are L2; the editor bodies, DInput internals, id-writer bodies and JVS remain L1 | none yet (rewrite answers in sections 0 and 6) |
| Online lobby system (ranked, player rooms, Steam lobbies, Arc server client, network task, GGPO connection, matchmaking UI) | [Lobby.md](Lobby.md) + parts `Lobby_A`..`Lobby_F` | **mapped** (1,289 fn: L2 333, L1 956, L0 0). Every create/search/join/leave/connect/timeout/failure path is L2; verdict per symptom and confirmed bugs in Lobby.md | none yet (fix order proposed in Lobby.md) |
| Memory and address space (allocators, file slots, D3DX effects, large-address-aware) | [Memory.md](Memory.md) | **mapped** (allocator layer L1, effect wrapper and particle fix-up L2). Cause of the OOM crashes is 2 GB fragmentation; large-address-aware is the fix and 3 of its blockers are fixed, 1 open (battle under stress). **Parked**: not regression-safe, and the bit can only be set by rewriting BBCF.exe | branch `experiment/large-address-aware` (LargeAddressAware.cpp, LaaStress.cpp test harness) |
| Ranked / LP | `docs/Research/RankedProgress.md` | partial L1 | none |
| Character palettes (colour selection, live slot block, online records, battle container load/Bind/teardown, round/rematch lifecycle) | [Palettes.md](Palettes.md) | **mapped** (boundary L1, hooked/callable functions L2). Root causes for every field palette report + the mod's own races | **mod side rewritten** 2026-10-19 (dirty-flag redraw, owner-based slots, per-battle gate, mirror-wrap fix); verified online + offline, see Palettes.md |
| Effect palettes / renderer palette lookup | `tools/build_effect_palette_map.py` | partial L2 | offline map |
| Invulnerability (the three invul switches, attribute bits, burst rule, invul script commands) | [Invulnerability.md](Invulnerability.md) | **mapped** (check and setters L2, hit processing L0/L1). Explains why 10 characters' backdashes showed no invul | frame history + script frame data read all three switches |
| Spectator input sync | `docs/Research/SpectatorDesyncInvestigation.md` | one function L2 | none |
| Training reset | `docs/Research/ResetLabOverride.md` | one function L2 | hook |

Everything else is L0. Add a row when a subsystem gets its own doc.
