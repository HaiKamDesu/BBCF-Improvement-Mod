---
name: bbcf-decompile
description: >
  The long-running, subsystem-by-subsystem decompilation and C++ rewrite of BBCF.exe. Use when
  asked to decompile, fully understand, map, rewrite or "fully override" a game system
  (controllers, network, ranked, replays...), to report decompilation progress, or to continue
  "the decompile". Covers the private archive, the L0-L3 ledger in docs/Decompilation/, the
  mapping method, the rewrite pattern and in-game verification. Builds on bbcf-re-workflow.
---

# Decompiling BBCF subsystem by subsystem

Goal: understand all of BBCF.exe (Steam build 11073075) and replace broken parts with C++ in the
mod, one subsystem at a time (the re3/reVC approach). The ledger of what is done is
`docs/Decompilation/README.md`; read it first, and update it last.

## Ground rules

- **Decompiled game code never goes in the repo.** It lives in `C:\BBCF_Archive\build_11073075\`
  (WSL `/mnt/c/BBCF_Archive/build_11073075/`). The repo gets *meaning*: per-subsystem docs,
  `GhidraDefs.h` names, and the C++ we write ourselves.
- **Never trust Ghidra's C for conventions.** It hides register arguments (`FUN_X(); FUN_Y();` is
  often eax->ecx). Before reimplementing a function, read its assembly
  (`DecompList.py <out> <addrs> asm`) and record: convention, `ret N`, which registers carry what,
  and every callee's `ret N`.
- **One subsystem per effort**, and finish it: every function inside the boundary reaches L2
  before any rewrite starts.

## The tools

| Need | Use |
|---|---|
| What does FUN_X do, who calls it | `grep -A60 "^//==== 00XXXXXX" /mnt/c/BBCF_Archive/build_11073075/decomp/src/00XX0000.c` (bucket = top 16 bits) |
| Call graph | `decomp/functions.tsv` (callers and callees counts) or each function's `// callers:` / `// callees:` header |
| Who uses a string | `decomp/strings.tsv` (string -> referencing functions) |
| Decompile + asm for a list | headless Ghidra `DecompList.py` (see bbcf-re-workflow / reference-headless-ghidra-from-wsl memory) on the repo project, `-readOnly` |
| Raw bytes, jump tables | parse `BBCF.exe` sections in Python (see how the profile-sync jump table at 0x4B1128 was read) |
| Branch-into-patch check | scan `tools/bbcf_disasm_ascii.txt` for jumps/calls landing inside a planned patch |
| Push names back into Ghidra | `ApplyRepoNames.py` on the archive project (reads `ADDR_` constants from GhidraDefs.h) |
| Regenerate the export | `ExportFullDecompile.py` (archive README.txt has the command) |
| Progress numbers | `python3 tools/decomp_progress.py` |

## Method for one subsystem

1. **Find the boundary.** Seed from what is already known: imports (`DirectInput8Create`,
   `XInputGetState`...), strings (`strings.tsv`), RTTI class names, vtables, and functions the
   mod already hooks (`GhidraDefs.h`, `src/Hooks/`). Walk callers/callees until the edges reach
   code that clearly belongs to something else; write down where you stopped and why.
2. **Create `docs/Decompilation/<Subsystem>.md`** with a table
   `| VA | Name | Size | Level | Notes |` (VA as `0x00XXXXXX`; `tools/decomp_progress.py` parses
   exactly that shape). List every function inside the boundary at L0, plus the shared data
   (globals, struct layouts, vtables) in prose below the table.
3. **Raise every function to L1, then L2.** L1 = role + callers/callees. L2 = convention,
   arguments, return, every field touched, every global read/written. Name functions in
   `GhidraDefs.h` (`ADDR_` constants) as they become L2; run `ApplyRepoNames.py` so later
   exports carry the names.
4. **Design the rewrite** in the subsystem doc: which functions get replaced, which stay as game
   calls (and why), what changes behaviour vs. vanilla (mark those, like ProfileSync's `[mod]`).
5. **Rewrite** in `src/...`: replace whole functions at entry where possible (a 5-byte JMP onto a
   C++ function with the identical convention, so the game's own `call` returns normally).
   Byte-check every patch site before patching, and follow `docs/HookEpilogueContract.md`.
   Add a setting that turns the rewrite off (pure vanilla when off).
6. **Verify in-game yourself**: `DebugDeploy|Win32` (auto-launches; Debug builds keep the
   runtime ESP check, which catches a wrong `ret N` immediately) and drive it with bbcf-agentctl.
   If the tool lacks a capability, extend the tool first. Mark functions L3 only after this.
7. **Update the ledger** (`docs/Decompilation/README.md` subsystem row + progress table from
   `tools/decomp_progress.py`) and the project memory.

## Reference implementation

`src/Network/ProfileSync.cpp` + `docs/Decompilation/ProfileSync.md`: a full state-machine
rewrite of FUN_004B0970 from its disassembly, game helpers called through typed function
pointers with verified conventions, byte-checked install, a vanilla-off switch, and in-game
verification against the live server.
