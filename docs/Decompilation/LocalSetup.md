# Working on the decompile with us

You do not download Kam's decompile. You build the same thing from your own copy of the game, and
we share everything *about* it (what functions mean, names, offsets, rewrites) through this repo.

## Why it works this way

Not legal advice, just what we found when SIY asked (2026-11-08) whether we could host it.

- A Ghidra decompile of BBCF.exe is derived from Arc System Works' code. Putting it on GitHub,
  a file host, or sending it to one person is distributing it. Doing it privately or briefly
  ("upload it, you grab it, I delete it") lowers the chance anyone notices. It doesn't change
  what it is.
- The closest precedent is GTA's re3/reVC. Take-Two had them taken down on GitHub. The developers
  filed counter-notices, arguing it was reverse-engineered code, not a straight decompile. Take-Two
  sued, and it ended in a settlement with undisclosed terms. A cleaner argument than ours did not
  keep them out of court.
- The big public decomp projects (Mario 64, Zelda) exist, but nobody has tested them in court, so
  "they're still up" doesn't make them legal.
- We found no record of Arc System Works sending takedowns over BBCF mods. That shows they haven't
  bothered so far, not that it would be fine.
- What *is* ours is our own work: the explanations in `docs/Decompilation/`, the addresses and
  names in `src/Game/GhidraDefs.h`, and the C++ rewrites in `src/`. The mod has always lived on
  that layer. The rule in [README.md](README.md) stays: decompiled game code is never committed.

So the decompile itself never leaves anyone's machine. Since everyone runs the same Ghidra
version, scripts and names on the same build, everyone gets the same function names and C output.
Pointing at `FUN_00573E50` or `0x998` means the same thing on every machine.

If we ever want to go further (a public decomp, sharing a Ghidra project), ask an IP lawyer or the
EFF first. A Ghidra project folder contains the game binary itself, so never share one either.

## Setup

You need:

- BBCF from Steam, **build 11073075**, the only build there is. The script checks the exe's
  SHA-256 (`9a94ac40...1f25`).
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra/releases) 11.x (we use 11.4.2) and a
  JDK 21. Ghidra is Java, so on Linux, a Steam Deck or anything running BBCF through WINE/Proton,
  install the native Linux Ghidra. You don't need WINE for this part.
- This repo checked out. The naming and export scripts live in `docs/Research/ghidra_scripts/`.

Then run:

```bash
tools/decomp/build_local_decomp.sh "<steam>/steamapps/common/BlazBlue Centralfiction/BBCF.exe" ~/bbcf-decomp ~/ghidra_11.4.2_PUBLIC
```

It imports and fully analyses the exe, applies every name and note the repo has (the `ADDR_`
constants in `src/Game/GhidraDefs.h` and every ledger row in `docs/Decompilation/`), and exports
every function as C, each with our notes in a `/* [repo] ... */` comment above it. Expect around an hour. You end up with:

| Path | What |
|---|---|
| `~/bbcf-decomp/ghidra/BBCF.gpr` | the analysed project, open it in the Ghidra GUI to browse |
| `~/bbcf-decomp/decomp/src/XXXX0000.c` | every function as C, in 64 KB address buckets |
| `~/bbcf-decomp/decomp/functions.tsv` | entry, name, size, file, callers, callees |
| `~/bbcf-decomp/decomp/strings.tsv` | every string and which functions use it |

Grep, don't scroll: `grep -n '^//==== 00573E50' ~/bbcf-decomp/decomp/src/*.c` finds a function,
and `grep -n '0x998)' ~/bbcf-decomp/decomp/src/*.c` finds everything that touches a field.

## Reading the game's scripts

Character move scripts aren't in the exe. They're in `data/Char/char_XX_scr.pac` in your
install. `tools/bbcf_scr_scan.py` walks them exactly the way the game's interpreter does. It uses
the interpreter's own command-size table, read out of your BBCF.exe, so it never loses its place:

```bash
python3 tools/bbcf_scr_scan.py "<install>" --chars ny --states CmnActBDash --all-ops
python3 tools/bbcf_scr_scan.py "<install>" --ops 22008        # every invul-timer use
```

## Sharing what you find

- Write what a function *means* into the subsystem doc in `docs/Decompilation/`. The format and the
  L0-L3 levels are in [README.md](README.md). Quoting a few lines of decompiled code to explain a
  point is fine. Pasting whole functions isn't.
- Name a function by giving it a ledger row with an identifier in the Name column
  (`| 0x00573E50 | Hit_InvulGuardCheck | 2099 | L1 | notes |`), or, for an address the mod's
  code uses, a `static constexpr uintptr_t ADDR_<Name> = 0x<RVA>;` in `src/Game/GhidraDefs.h`.
- After pulling, refresh your project with everyone's latest names and notes (about 4 minutes):
  `BBCF_ARCHIVE=~/bbcf-decomp GHIDRA_HOME=~/ghidra_11.4.2_PUBLIC BBCF_GHIDRA_USER=$USER tools/decomp/sync_archive.sh`
- PRs to this repo as usual.
