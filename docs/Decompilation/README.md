# Decompiling BBCF, one subsystem at a time

The long-term goal: understand all of BBCF.exe (Steam build 11073075) well enough to override
any part of it, and replace the parts that are broken with C++ in the mod. Method: the re3/reVC
approach. Take one subsystem, document every function in it, rewrite it in C++ inside the mod,
redirect the original functions to the rewrite, verify in-game, move on.

How to work on this: the `bbcf-decompile` skill (`.claude/skills/bbcf-decompile/SKILL.md`).

## Where things are

| What | Where | In git? |
|---|---|---|
| Preserved game build, hashes, manifest | `C:\BBCF_Archive\build_11073075\` | no (private) |
| Analysed Ghidra project with the mod's names | `C:\BBCF_Archive\build_11073075\ghidra\` | no |
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
| Ranked / LP | `docs/Research/RankedProgress.md` | partial L1 | none |
| Effect palettes / renderer palette lookup | `tools/build_effect_palette_map.py` | partial L2 | offline map |
| Spectator input sync | `docs/Research/SpectatorDesyncInvestigation.md` | one function L2 | none |
| Training reset | `docs/Research/ResetLabOverride.md` | one function L2 | hook |

Everything else is L0. Add a row when a subsystem gets its own doc.
