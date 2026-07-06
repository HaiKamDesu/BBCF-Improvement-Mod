---
name: bbcf-re-workflow
description: >
  Reverse engineering workflow for BBCF: grepping disasm dumps, running headless Ghidra
  decompile scripts, verifying signature-hook safety before patching live game code, crash
  analysis with CDB, and adding findings to GhidraDefs.h. Trigger: "reverse engineer",
  "find offset", "analyze function", "what does [address] do", "add to GhidraDefs",
  "crash dump", "cdb", "disasm", "ghidra", "decompile", "hook a native function".
---

# BBCF Reverse Engineering Workflow

## Fast path: grepping the disasm dumps (no Ghidra needed)

| File | Size | Use |
|---|---|---|
| `tools/bbcf_disasm.txt` | 104 MB | Full disasm with symbols |
| `tools/bbcf_disasm_ascii.txt` | 52 MB | ASCII-only variant for grep |

**Gotcha:** both files start with a stray 2-byte BOM (`0xFF 0xFE`) that makes some tools
(`ugrep`, some UTF-8-only greps) misdetect them as UTF-16LE and silently match nothing. Strip it
first:

```bash
tail -c +3 tools/bbcf_disasm_ascii.txt | grep -n "FUN_00a8f9b0\|SomeSymbol"
```

This is enough for: finding a symbol, reading a short instruction run, or a quick per-instruction
byte-length calculation (see "Verifying signature-hook safety" below). Reach for full Ghidra
(next section) when you need decompiled C, call-graph traversal (callers/callers-of-callers), or
cross-referencing a field offset back to every function that touches it.

## Running headless Ghidra (decompile, xrefs, call graphs)

This repo keeps the workflow reproducible but does not track the generated project database —
Ghidra's packed DB files exceed GitHub's 100 MB blob limit, so `docs/Research/BBCF-Ghidra-Project/`
is gitignored and gets rebuilt locally whenever needed.

- Local project folder: `docs/Research/BBCF-Ghidra-Project/` (project name `BBCF`, program `BBCF.exe`)
- Binary imported from: `D:\SteamLibrary\steamapps\common\BlazBlue Centralfiction\BBCF.exe`
- Ghidra install: `D:\Programs\ghidra_11.4.2_PUBLIC` (headless exe at `support\analyzeHeadless.bat`)
- **Image base is `0x00400000`.** A runtime module-relative offset (RVA) used in this codebase's
  own `bbcf_base_adress + 0xXXXXXX` pattern maps directly to Ghidra address `0x00400000 + 0xXXXXXX`.
  Always recompute this with `python3 -c "print(hex(0x400000 + 0xXXXXXX))"` rather than by hand —
  it's easy to slip a digit.

### If the project doesn't exist yet (first run, or `BBCF.exe` changed)

Run the import once — it does full analysis and can take several minutes to tens of minutes.
**Gotcha:** `analyzeHeadless` requires the project's *parent* directory to already exist, or it
aborts with `Directory not found` — `mkdir` `docs/Research/BBCF-Ghidra-Project`'s parent (i.e.
just make sure `docs/Research/` exists, which it does) before the first run.

```
'/mnt/c/Windows/System32/cmd.exe' /C 'C:\Users\Usuario\source\repos\HaiKamDesu\BBCF-Improvement-Mod\docs\Research\run_ghidra_YOURSCRIPT_import.cmd'
```

Template for the `.cmd` (copy an existing one, e.g. `run_ghidra_ranked_lp_import.cmd`, don't hand-write from scratch):

```bat
@echo off
setlocal
set "RESEARCH=%~dp0"
set "PROJECT_DIR=%RESEARCH%BBCF-Ghidra-Project"
set "PROJECT_NAME=BBCF"
set "GHIDRA_HEADLESS=D:\Programs\ghidra_11.4.2_PUBLIC\support\analyzeHeadless.bat"
set "BBCF_EXE=D:\SteamLibrary\steamapps\common\BlazBlue Centralfiction\BBCF.exe"
set "SCRIPT_DIR=%RESEARCH%ghidra_scripts"
set "REPORT=%RESEARCH%YourReport.txt"
"%GHIDRA_HEADLESS%" "%PROJECT_DIR%" "%PROJECT_NAME%" -import "%BBCF_EXE%" -overwrite -analysisTimeoutPerFile 1800 -scriptPath "%SCRIPT_DIR%" -postScript YourScript.py "%REPORT%"
```

Let the `-analysisTimeoutPerFile 1800` full pass run to completion — don't cancel it early.

### Once the project exists: fast follow-up runs

Drop `-import`/`-overwrite`, use `-process` + `-noanalysis` instead — much faster (seconds to a
couple minutes instead of tens of minutes):

```bat
"%GHIDRA_HEADLESS%" "%PROJECT_DIR%" "%PROJECT_NAME%" -process BBCF.exe -noanalysis -scriptPath "%SCRIPT_DIR%" -postScript YourScript.py "%REPORT%"
```

### Writing the Jython script

**Use Jython (`.py`), not Java (`.java`).** A Java post-script hit a Ghidra/Java 25 OSGi
script-load failure in this workspace even though the project imported fine — Jython scripts work
reliably. Existing scripts to copy/adapt (all under `docs/Research/ghidra_scripts/`):
- `DecompileRankedLpCallers.py` — finds and decompiles every caller of a list of target addresses
  (the standard "who calls this function/touches this field" pattern — use this as your template).
- `DecompileRankedLp.py`, `DecompileRankedLpHelpers.py`, `DecompileRankedConfirm.py`,
  `DecompileRankedVictory.py` — straight single/multi-function decompiles.

Minimal caller-finding template:

```python
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

TARGET_ADDRS = [0x0XXXXXXX]  # Ghidra addresses (0x400000 + RVA), verify arithmetic first

def collect_callers(target):
    return [(r.getFromAddress(), r.getReferenceType().toString(), getFunctionContaining(r.getFromAddress()))
            for r in getReferencesTo(target)]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
ifc = DecompInterface()
ifc.openProgram(currentProgram)
out = PrintWriter(report_file, "UTF-8")
try:
    for tv in TARGET_ADDRS:
        target = toAddr(tv)
        out.printf("===== References to %s =====%n", target)
        for from_addr, ref_type, fn in collect_callers(target):
            out.printf("%s %-16s %s%n", from_addr, ref_type, fn.getName() if fn else "<no function>")
        # decompile each unique caller function similarly (see DecompileRankedLpCallers.py for the full loop)
finally:
    out.close()
    ifc.dispose()
```

Output goes to a new report file under `docs/Research/` (e.g. `YourFeatureGhidraReport.txt`) —
follow the existing naming convention (`<Topic>GhidraReport.txt`).

### Notes for agents

- Never commit `docs/Research/BBCF-Ghidra-Project/` — it's gitignored on purpose (packed DB files
  exceed GitHub's blob limit). If Git ever shows files under it staged, `git rm --cached -r
  docs/Research/BBCF-Ghidra-Project`.
- Don't edit the *existing* tracked `run_ghidra_*.cmd` / `ghidra_scripts/*.py` files in place for a
  new investigation — add new ones (`run_ghidra_<topic>_*.cmd`, `Decompile<Topic>.py`) so prior
  investigations stay reproducible.
- This is read-only research. Never route a destructive command through Ghidra scripting or the
  safe-readonly executor (next section) — both are for reading/decompiling/dumping only.

## Verifying signature-hook safety before patching live game code

This codebase hooks native functions via `HookManager::SetHook(label, pattern, mask, len,
newFunc)` (`src/Hooks/HookManager.cpp`) — a byte-signature scan (`FindPattern`) locates the first
match of `pattern`/`mask` anywhere in the loaded module, then `PlaceHook` overwrites `len` bytes
there with a 5-byte `E9` JMP (padding any remainder up to `len` with `0x90` NOP — so `len` can be
5 or more, but must land on a real instruction boundary, never mid-instruction).

**Before adding ANY new signature hook, especially into a hot/shared function (a per-tick update,
input handler, anything called constantly regardless of game mode): verify the pattern is
actually unique in the whole binary.** `FindPattern` returns the *first* match — an
insufficiently-specific pattern will silently hook the wrong location and corrupt some unrelated
function's prologue. This is a real, easy mistake: common MSVC prologues like `push ebp; mov
ebp,esp; sub esp,imm8` recur dozens of times across any nontrivial binary.

Practical uniqueness-check recipe (used to validate the `UnlimitedPlaybackPreTick` hook in
`src/Hooks/hooks_bbcf.cpp`, a hook on the game's core per-logic-tick `Update` at Ghidra
`FUN_0056B1F0`):

```python
# Given a known target address's disasm block, count matches for increasingly long windows
# until exactly one match remains, then use THAT window's bytes as the FindPattern pattern.
data = open('tools/bbcf_disasm_ascii.txt','rb').read()[2:].decode('utf-8', errors='replace')  # [2:] strips the BOM
lines = data.splitlines()
def strip_addr(l):
    idx = l.find(': ')
    return l[idx+1:] if idx != -1 else l
norm = [strip_addr(l) for l in lines]
target_idx = next(i for i, l in enumerate(lines) if '<YOUR_ADDR>: ' in l)
for extra in range(6, 30):
    window = norm[target_idx:target_idx+extra]
    matches = sum(1 for i in range(len(norm)-len(window)) if norm[i:i+len(window)] == window)
    if matches == 1:
        print('unique at', extra, 'instructions')
        break
```

Once you know how many leading instructions make the sequence unique, that becomes your
`FindPattern` **pattern** (as many bytes as needed for uniqueness — this can be much longer than
what you actually intend to overwrite). The **overwrite length** (`len` passed to `SetHook`) is a
separate, smaller number: only as many *whole* leading instructions as you need to redirect
execution, always ending exactly on an instruction boundary (check the disasm — never split an
instruction across the JMP/no-JMP boundary). Reconstruct those overwritten instructions verbatim
inside your naked hook function (as raw `_asm` mnemonics) before jumping to `<pattern start> +
len`, exactly like the existing `GetFrameCounter` hook does.

**Never hardcode an absolute-address byte as a fixed (`x`) pattern byte — this game has ASLR.**
An instruction like `mov eax,[0x00A0E0B8]` (opcode `A1` + a 4-byte absolute VA operand) embeds the
*load address*, which is only `0x00400000 + RVA` inside Ghidra's static analysis (fixed image
base). At runtime the module can load at a different base, so those 4 operand bytes will differ
from what Ghidra shows, and `FindPattern` will find **zero** matches — `HookManager::SetHook` logs
`"<label> signature scanning returned 0"` and silently no-ops (no crash, the hook just never
fires — this is exactly what happened when a new hook's pending-request consumer never got called:
the pattern matched Ghidra's own byte dump exactly, byte-for-byte, and still failed to find
anything live, purely because of this). Wildcard (`?`) any bytes that are part of an absolute
memory operand. Bytes that are safe to keep fixed even though they "look like an address": a
`CALL`/`JMP` **rel32** displacement (`E8`/`E9` + 4 bytes) — that's relative to the *next
instruction*, so it stays identical under uniform module rebasing; only direct/absolute-addressed
operands (`mov reg,[imm32]`, `mov [imm32],reg`, etc.) need wildcarding.

Other things this workflow already relies on, safe to assume:
- Pattern/mask strings may embed literal `\x00` bytes — `FindPattern`/`PlaceHook` size their loop
  by `strlen(mask)`, never `strlen(pattern)`, so a `\x00` inside the pattern (e.g. part of an
  absolute address) does not truncate the match. Mask characters are only `x` (must match) or `?`
  (wildcard) — never put an `x` opposite a pattern byte you actually want as a wildcard.
- Always wrap any C++ call made from inside a naked hook in `__asm pushad` / `__asm popad` — the
  original code you jump back to typically still depends on register state (e.g. `ecx` holding
  `this`) that must survive untouched through your hook.
- New hooks are read-only-safe to *develop* and *build* locally, but treat live-hook additions
  into widely-shared functions (per-tick updates, input pumps) as high blast-radius: build, then
  smoke-test that the game boots and runs normally in an unrelated mode before testing the
  specific feature, since a bad hook there can crash far outside the feature you're changing.

## Running safe read-only tools (cdb, WinDbg analysis)

The operator runs `tools/safe_readonly_exec.ps1`. To request a command, write to
`tools/safe_readonly_request.json`:

```json
{
  "tool": "cdb",
  "args": ["-z", "path/to/dump.dmp", "-c", ".ecxr; k; q"],
  "reason": "analyze ranked crash at 0x00XXXXXX"
}
```

Schema documented in `tools/SAFE_READONLY_EXECUTOR.md`. The script only allows allowlisted
commands (cdb, WinDbg, grep on disasm files). Do NOT attempt to run cdb directly.

CDB pre-built commands for ranked crashes: `tools/cdb_ranked_crash_cmds.txt`

## Adding findings to GhidraDefs.h

File: `src/Game/GhidraDefs.h`

Conventions:
- Use `GAME_` prefix for game structs/functions
- Comment the source: `// Ghidra FUN_00xxxxxx` or `// IDA, confirmed via crash dump`
- Function pointers: `typedef RetType (__cdecl *FuncName_t)(Args...);`
- Offsets: document as `// base + 0xXXXX` when relative to module base
- Structs: match Ghidra field layout exactly; use `__pragma(pack(push,1))` if needed

Example:
```cpp
// Scene controller - base+0xa56e0, param1 = GAMESTEAM_CNetworkServer
typedef void (__cdecl *CScene_Controller_ChangeScene_t)(void* pScene, void* pNetworkServer);
static constexpr uintptr_t ADDR_CScene_Controller_ChangeScene = 0x00A56E0; // base-relative
```

Research scratch notes go in `notes.h` (root), not GhidraDefs.h.

## URT crash analysis

Active incident docs: `docs/replay_takeover/`

The snapshot-train crash in URT (Unlimited Replay Takeover) is tracked there. When analyzing:
1. Check `ReplayTakeoverFeatureFlags.h` for current flags
2. The crash reproducer uses `tools/urt_automation/BBCF-Automatic-Debugger.ahk`
3. Full debug cycle: `tools/urt_automation/run_bbcf_debug_cycle.sh`

## Ranked automation harness

`src/Hooks/RankedAutomationHarness.cpp/.h` — hook that automates ranked match flow for RE.
Run script: `tools/run_ranked_harness_autorun.sh`

## Key constraint

**Never auto-build or auto-deploy.** Prepare code changes and tell the operator to build.
`safe_readonly_exec` and headless Ghidra are for read-only analysis only — never route a
destructive command through either.
