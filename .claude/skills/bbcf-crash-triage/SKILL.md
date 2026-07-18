---
name: bbcf-crash-triage
description: >
  Full triage workflow for "the game crashed" reports on BBCF Improvement Mod. Checks the mod's
  own DEBUG.txt/CrashReports first, then Windows Application/System event logs, then the raw WER
  (Windows Error Reporting) report folders on disk directly — because WER can silently fail to
  log to the event log while still writing report files. Establishes whether a crash actually
  happened, when, and hands off to bbcf-re-workflow for dump/disasm analysis once found. Trigger:
  "the game crashed", "it just crashed", "find out why it crashed", "game closed unexpectedly",
  "BBCF froze/hung".
---

# BBCF Crash Triage

Goal: go from "the game crashed" to either (a) a confirmed crash artifact with exception
code/address ready for deep analysis, or (b) a confident, evidence-based statement that no
OS-level crash occurred in the relevant window — don't stop at "I didn't find anything" without
having checked all three layers below, since layer 2 alone is known to give false negatives.

## Layer 1: the mod's own crash handling (cheapest, check first)

Use the `bbcf-debug-log` skill's path conventions for this machine. Read, in order:

1. **Live `DEBUG.txt`** (`BBCF_IM/DEBUG.txt`) — tail it. A clean session end looks like:
   ```
   [...] BBCF_IM_Shutdown
   [...] WindowManager::Shutdown
   [...] CleanupInterfaces
   BBCF_FIX STOP - <timestamp>
   ```
   If the file just stops mid-session with no `BBCF_FIX STOP` line, that session was killed
   abruptly (crash, hard hang + Task Manager kill, or power loss) — note the last logged
   timestamp, that's your window.
2. **`BBCF_IM/DebugHistory/`** — rotated prior sessions (`DEBUG_<timestamp>.txt`, newest by
   filename timestamp = latest rotation). Check the same clean-vs-abrupt ending for the session
   that covers the time the user reports the crash.
3. **`BBCF_IM/CrashReports/Crash_<timestamp>/`** — the mod installs its own vectored exception
   handler (`[Crash] Unhandled exception filter installed` at boot) and writes `crash.dmp` +
   `crash_context.txt` + `logs.txt` here when it catches something. `crash_context.txt` has the
   exception code/address directly, no dump analysis needed for a first read. **Check the
   directory timestamp against "now" carefully** — an old crash folder sitting there is not
   evidence of anything happening today.
4. **`BBCF_IM/DCodeIncidents.log`** + `DCodeBlobFail_*.bin` — the known D-code state-6 auto-recovery
   path (see the project's D-code bug memory). This is **not** a crash — the game keeps running,
   auto-recovers, and eventually exhausts its retry budget and "leaves state as-is." Don't mistake
   a burst of these for the crash the user is describing; they're a separate, already-tracked bug.

If layer 1 shows nothing (all sessions end clean, no new `CrashReports` folder), that means BBCF's
*own* handler never caught anything — proceed to layer 2. This does NOT by itself mean no crash
happened; it means whatever happened wasn't an exception the mod's vectored handler intercepted
(e.g. it crashed somewhere the handler doesn't cover, or it was killed rather than faulted).

## Layer 2: Windows Application/System event logs

From WSL, use `powershell.exe -NoProfile -Command "..."`. Get local time first — this machine's
clock is Argentina Standard Time (UTC-3); WSL mount mtimes shown via `ls -la --time-style=full-iso`
are UTC, so don't eyeball-compare a `DEBUG.txt` local timestamp against an `ls` mtime without
converting. `powershell.exe -Command "Get-Date"` gives you local "now" to anchor a window.

```powershell
$start = (Get-Date).AddHours(-N)   # N = however far back the reported crash could be
Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=$start; Level=1,2,3} -ErrorAction SilentlyContinue |
  Select-Object TimeCreated, Id, ProviderName, LevelDisplayName, Message | Format-List
```

Repeat with `-LogName System` for OS-level faults (unexpected shutdown, `Kernel-Power`,
`BugCheck`, GPU driver resets — provider names like `nvlddmkm`/`amdkmdap`/`Display`, event ID 4101
is the classic TDR "stopped responding and has recovered").

**Known false negative:** `Get-WinEvent` against `Application`/`System` can come back completely
empty even when `BBCF.exe` genuinely crashed — Windows Error Reporting can fail to log an
Application Error / WER event to the event log (reporting policy, throttling, etc.) while still
writing the actual report bundle to disk. Do not conclude "no crash" from an empty event log
alone — always also run layer 3.

## Layer 3: raw WER report folders (ground truth — check even if layer 2 is empty)

This is the layer that actually surfaces a crash when the event log doesn't:

```powershell
$paths = @(
  'C:\ProgramData\Microsoft\Windows\WER\ReportQueue',
  'C:\ProgramData\Microsoft\Windows\WER\ReportArchive'
)
foreach ($p in $paths) {
  Get-ChildItem $p -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'BBCF' } |
    Sort-Object LastWriteTime -Descending |
    Select-Object FullName, LastWriteTime
}
```

- `ReportQueue` = pending/not-yet-archived (check this too, a very fresh crash can sit here
  briefly before Windows files it into `ReportArchive`).
- Compare the newest `AppCrash_BBCF.exe_*` folder's `LastWriteTime` against both "now" and the
  newest folder already known in the mod's own `BBCF_IM/CrashReports/` — if the WER folder is
  *newer* than anything the mod caught itself, that's your incident, and the mod's own vectored
  handler either didn't fire or fired after the OS had already begun crash reporting.
- Inside a matching folder: `Report.wer` (text — has `AppPath`, `Exception Code`, fault module/
  offset directly), plus `Memory.hdmp`/`Minidump.dmp` if present. Read `Report.wer` first, it's
  cheap and often enough to identify the fault module/exception code before touching the dump.

If all three layers are empty/stale for the reported time window, say so explicitly and consider:
- Is `BBCF.exe` even running right now? `Get-Process BBCF -ErrorAction SilentlyContinue`.
- A hard freeze that the user killed via Task Manager produces **no** WER report and **no**
  Application Error event (`TerminateProcess` isn't a fault) — ask the user directly whether they
  saw a "has stopped working" dialog / the window vanish instantly, vs. the screen freezing and
  them ending the task manually. This changes where to look next (e.g. GPU driver TDR in the
  System log for a freeze, vs. nothing at all for a manual kill).
- Confirm the crash window itself — if the user says "just now" but no session has ended (mod
  still running, or last session ended cleanly minutes before the report), the crash may not have
  reproduced yet; ask them to reproduce it now and re-run this triage once it has.

## Handing off to deep analysis

Once you have a confirmed exception code + faulting address (from `crash_context.txt` or
`Report.wer`), switch to the **bbcf-re-workflow** skill:
- Resolve the faulting module offset against `tools/bbcf_disasm_ascii.txt` / headless Ghidra to
  identify the function.
- For a full register/stack dump, use `tools/safe_readonly_exec.ps1` (never run `cdb` directly)
  per that skill's "Running safe read-only tools" section.
- Cross-reference against `src/Game/GhidraDefs.h` for any already-documented structs/functions at
  that address, and add new findings there once confirmed.

## Key constraint

This is read-only triage. Never build, deploy, or modify game/mod files as part of this workflow —
only after the root cause is understood and the user has agreed on a fix should code changes
happen, and even then, never auto-build/auto-deploy (see `bbcf-re-workflow`'s key constraint).
