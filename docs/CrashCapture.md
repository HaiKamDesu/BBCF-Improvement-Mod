# Why we only catch ~10% of crashes, and what actually catches the rest

The complaint this document answers: *"CF crashed, no IM message. The log just says normal
IM shutdown."* That is not a bug in the crash handler. It is the crash handler working
exactly as designed and being structurally unable to see most of what kills this game.

Everything below is measured on this machine, not inferred.

## The measurement

288 WER report folders for `BBCF.exe` in `C:\ProgramData\Microsoft\Windows\WER\ReportArchive`
(286 with a readable `Report.wer`), against 28 crash bundles in
`BBCF_IM\CrashReports`. Correlating each WER report against a mod bundle within ±3 minutes:

| exception code | meaning | count | caught by the mod |
|---|---|---:|---:|
| `0xC0000409` | `__fastfail` (`int 29h`) | 130 | **0** |
| `0xC0000005` | access violation | 147 | 6 |
| `0xC00000FD` | stack overflow | 5 | 0 |
| `0x80000001` | guard page violation | 2 | 0 |
| `0xC000041D` | fatal user-callback exception | 1 | 0 |
| `0xE06D7363` | C++ exception | 1 | 0 |
| **total** | | **286** | **6 (2.1%)** |

28 bundles exist but only 6 line up with a WER-recorded crash, so the rest were written for
non-crash reasons. **The real capture rate against actual crashes is 2%, not 10%.**

Grouping by fault offset shows the crashes are not diffuse — five signatures are 80% of
everything:

| count | code | RVA | what is actually there |
|---:|---|---|---|
| 92 | `C0000409` | `0x3A7B59` | `int 29h` after `push 7` → `__fastfail(FAST_FAIL_FATAL_APP_EXIT)`, the CRT `abort()` path |
| 60 | `C0000005` | `0x3854FF` | `call dword ptr [eax+4]` — a virtual call on a dead object |
| 36 | `C0000409` | `0x3A02C1` | `int 29h` after `push 2` → `__fastfail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE)` |
| 29 | `C0000005` | `0x34CE33` | `cmp dword ptr [esi+0Ch],0` — plain bad-pointer read |
| 27 | `C0000005` | `0x59D0` | `mov dword ptr [ecx],84B804h` — storing a vtable into a bad `this` |

(RVA + `0x400000` gives the static address the disassembly uses.)

## Cause 1: `int 29h` never reaches user mode at all — 45% of crashes

`__fastfail` is not an exception that gets dispatched and then goes unhandled. `int 29h`
traps directly into the kernel, which terminates the process immediately and hands WER a
`STATUS_STACK_BUFFER_OVERRUN`. No vectored handler runs. No SEH runs. No unhandled
exception filter runs. `STATUS_STACK_BUFFER_OVERRUN` [does not mean a stack buffer was
overrun](https://devblogs.microsoft.com/oldnewthing/20190108-00/?p=100655) — it is the
generic "terminate with great haste" code, and `FAST_FAIL_FATAL_APP_EXIT` is what
`abort()` uses.

`SetUnhandledExceptionFilter` and `AddVectoredExceptionHandler` cannot be made to see
this. There is no in-process API that can. This is the single largest bucket.

## Cause 2: the CRT deliberately unregisters our filter — most of the access violations

At static `0x007B2533`, BBCF's CRT contains this:

```
007B2533: push ebp
007B2534: mov  ebp,esp
007B2536: push 0
007B2538: call [SetUnhandledExceptionFilter]   ; installs NULL - deletes ours
007B253E: push [ebp+8]
007B2541: call [UnhandledExceptionFilter]      ; hands the record straight to WER
```

This is the UCRT's report-fault helper. It **sets the top-level filter to NULL and then
calls `UnhandledExceptionFilter` directly**, specifically so the application's own filter
cannot interfere with Watson reporting. Every CRT-originated fatal error routes through
here. BBCF.exe imports both `SetUnhandledExceptionFilter` and `UnhandledExceptionFilter`,
which is what makes this visible from the import table alone.

There is a second plain `SetUnhandledExceptionFilter(arg)` wrapper at `0x007B2510`. Since
`dinput8.dll` is a static import of `BBCF.exe`, our `DllMain` runs *before* the game's CRT
startup, so anything the game installs later silently replaces ours. Last writer wins, and
we always write first.

## Cause 3: BBCF catches its own crashes and exits cleanly — and we already hook it

This is the one that produces the exact reported symptom. At static `0x0044E740` there is a
function referenced from an SEH scope table rather than by any `call` — i.e. it is BBCF's
`__except` **filter expression**, receiving `(exceptionCode, PEXCEPTION_POINTERS)`:

```
0044E740: push ebp
0044E741: mov  ebp,esp                    ; [ebp+8] = code, [ebp+0Ch] = EXCEPTION_POINTERS
0044E754: mov  ebx,dword ptr [ebp+0Ch]
   ...   formats a comment string
0044E782: push 898F04h                    ; <-- the mod's SetDumpfileCommentString hook
0044E793: push eax
0044E794: call [SteamAPI_SetMiniDumpComment]
0044E7A1: push [ebp+8]
0044E7A4: call [SteamAPI_WriteMiniDump]
```

The game writes a **Steam** minidump, sets a comment, and then unwinds and exits through a
normal path. Because it exits rather than dying, `DLL_PROCESS_DETACH` runs, so the mod logs
`BBCF_IM_Shutdown` and `BBCF_FIX STOP` — **a game crash is recorded in our log as a clean
shutdown.** That is precisely the "no IM message, log says normal exit" case.

Two things follow, and the second is the important one:

1. `SteamAPI_WriteMiniDump` means Steam has been quietly collecting BBCF crash dumps this
   whole time. Those are not ours and we cannot read them, but their existence explains why
   the game feels like it "just closes".
2. **The mod already hooks this function.** `SetDumpfileCommentString` in `hooks_bbcf.cpp`
   patches `0x0044E782`, which is *inside BBCF's crash filter*, and logs it as
   `[MenuExit] SetDumpfileCommentString: gameMode=... gameState=... sceneStatus=...`. It
   was mislabelled as a menu transition. It is a crash notification, it fires on the class
   of crashes we currently lose entirely, and it fires with the real
   `PEXCEPTION_POINTERS` still sitting at `[ebp+0Ch]`.

Confirmation that it is crash-only, not a scene-change hook: a full normal session's
`DEBUG.txt` (1086 lines, menus, character select, matches) contains **zero** occurrences.
In the one reporter log where it appears, it is the final line before the log stops dead.

## What each layer buys

| layer | mechanism | covers | admin? |
|---|---|---|---|
| 1 | Capture BBCF's own SEH filter at `0x0044E782`, reading `[ebp+0Ch]` | the AV class BBCF swallows | no |
| 2 | Hook the two `int 29h` sites and snapshot before the trap | the 45% fastfail bucket | no |
| 3 | Detour `SetUnhandledExceptionFilter` so nothing can unregister ours | CRT-routed faults, other modules | no |
| 4 | `SetThreadStackGuarantee` so the filter has stack to run on | `0xC00000FD` | no |
| 5 | WER `RuntimeExceptionHelperModules` + `WerRegisterRuntimeExceptionModule` | everything, out of process | HKCU since Win10 20H1 |
| 6 | WER `LocalDumps` registry | everything | **yes, HKLM** |
| 7 | Out-of-process watchdog as a debugger | everything, first-chance | no |

Layers 1-4 are in-process and shipped. They should convert the great majority of the 286
into real bundles.

Layer 5 is the only *fully general* in-box mechanism, and it is how Chromium's Crashpad and
Mozilla both solve this. It needs a small separate DLL exporting
`OutOfProcessExceptionEventCallback`, `OutOfProcessExceptionEventSignatureCallback` and
`OutOfProcessExceptionEventDebuggerLaunchCallback`, its filename listed under
`SOFTWARE\Microsoft\Windows\Windows Error Reporting\RuntimeExceptionHelperModules`, and a
`WerRegisterRuntimeExceptionModule` call at startup. Since Windows 10 20H1 the key may live
in **HKCU**, so no elevation is needed; the only loss versus HKLM is that an HKCU-registered
handler cannot *claim* the crash, which does not matter to us because we only want to write
our own report, not suppress WER's. This is the recommended next step if layers 1-4 leave a
residue.

Layer 6 is trivial to configure and catches literally everything, but writing
`HKLM\...\Windows Error Reporting\LocalDumps\BBCF.exe` needs elevation, so it is at best an
opt-in "advanced" button, not a default.

Layer 7 catches the most (a real debugger sees first-chance exceptions before BBCF's own
SEH gets them, which is the only way to see crashes BBCF successfully swallows *and*
recovers from) but it means shipping a second always-running process, and being attached as
a debugger changes `IsDebuggerPresent` — which BBCF.exe imports. Not worth it yet.

## The dump size problem

`BBCF_IM\CrashReports` on this machine is **14 GB across 28 bundles** — roughly 500 MB each,
and the reporter bundle analysed in the 8.5.1 investigation was a 917 MB dump that had to be
split into three archive parts to be sent at all.

Cause: `GetCrashDumpFlags()` requested `MiniDumpWithFullMemory`, which writes the entire
committed address space. For a game holding hundreds of megabytes of textures and audio
that is almost all payload nobody will ever look at.

Two independent fixes, both shipped:

- **Detail level.** `CrashDumpDetail` selects small / medium / full. Medium is the default:
  thread stacks, the memory those stacks point at, data segments, handles, thread info and
  unloaded modules. That keeps everything the 8.5.1 diagnosis actually used — registers,
  the walked stack, module bases, the objects the faulting frame touched — while dropping
  the bulk texture memory. Full remains available for the cases that genuinely need the
  whole heap; it is a setting, not a removal.

  Measured against a test process holding 300 MB of committed private memory, which is the
  thing that makes BBCF's dumps large:

  | detail | with 0 MB bulk | with 300 MB bulk |
  |---|---:|---:|
  | small | 28.8 MB | 28.8 MB |
  | medium | 29.3 MB | 29.3 MB |
  | full | 47.9 MB | 362.2 MB |

  Only `full` grows with the process's own allocations, which is the entire problem. Medium
  costs 0.5 MB more than small and buys back the referenced-object memory that makes the
  faulting object inspectable, so small is rarely worth choosing.

  The ~29 MB floor is **not** the game's memory — it is the code sections of the ~42 loaded
  system modules (`windows.storage.dll` 5.9 MB, `shell32.dll` 5.7 MB, `combase.dll` 2.3 MB,
  and so on), captured as `module base + 0x1000` ranges. Every documented flag combination
  was measured and none of them move it, so the lever is a `MINIDUMP_CALLBACK_INFORMATION`
  callback that clears `ModuleWriteModule` for modules that are not ours or the game's. That
  is the next thing to do if 29 MB is still too big to ask a reporter to upload; it was left
  out here rather than implemented on an unverified guess.
- **Retention.** `CrashReportsToKeep` prunes oldest-first before each write, so the folder
  cannot grow without bound.

The two together are why 14 GB was allowed to happen: nothing capped the count and every
dump was maximal.

## The crash path must survive a broken process

A crash on 2026-09-03 produced `[Crash] UnhandledExFilter invoked.` and then nothing: no
bundle, and because the filter ends in `ExitProcess`, no WER report and no LocalDump either.

The root cause turned out to be the recursion documented in the next section, not the crash
path itself - the handler was running on a stack that had already been consumed. But the
audit that followed found the crash path was genuinely unsafe anyway, and would have failed
on its own for a heap-corruption crash. `ForceLog` builds a `std::string` and then pushes a
copy into the in-memory ring under the log mutex - two heap operations and a second lock
acquisition, on a path where the heap may be the thing that is broken and where the fault
may have happened inside the logger while it held that very mutex. Three rules came out of
it, all of them still worth having:

- **Nothing on the crash path may allocate or block.** `ForceLogRaw` formats nothing and
  takes the log mutex with `try_lock`, writing anyway if it cannot get it. An interleaved
  line beats no report. Every log call in `crashdump.cpp` goes through `CrashProgress`,
  which formats into a stack buffer; none of them use `ForceLog`.
- **The bundle writer is SEH-guarded and ordered by value.** `WriteCrashBundle` is a thin
  wrapper holding no destructor-bearing object so `__try` is legal; the body is
  `WriteCrashBundleImpl`. Artifacts are written cheapest-and-most-diagnostic first:
  `crash_context.txt`, then the dump, and the log snapshot last behind its own guard,
  because reading the ring is the least valuable and most likely thing to fault.
- **Never swallow a crash we did not capture.** `ExitProcess` was unconditional, which was
  safe only because the CRT kept unregistering our filter. Now that the filter survives,
  exiting suppresses WER and LocalDumps too. `FinishCrashHandling` exits quietly only when a
  dump actually landed; otherwise it returns `EXCEPTION_CONTINUE_SEARCH` and lets Windows
  capture what we could not.

Verified against the shipped code with `build/crashctx_test` (modes 2, 3 and 4):

| scenario | context | logs | dump | outcome |
|---|---|---|---|---|
| normal | 1096 B | 48 B | 29 MB | `dump ok=1`, exit quietly |
| log ring faults (the real failure) | 1073 B | skipped, reported | 29 MB | `dump ok=1`, bundle survives |
| context builder faults | - | - | - | SEH caught, process alive, handed to Windows |

## The mod's own detour recursed until the stack was gone

This is the root cause of both 2026-09-03 crashes, and it was ours.

`hook_SetUnhandledExceptionFilter` (Cause 2's fix) ended by calling
`ReassertUnhandledExceptionFilter()`, a one-line helper that called
`SetUnhandledExceptionFilter(UnhandledExFilter)`. That is **the very API the hook detours**,
so the call re-entered the hook. Release LTCG inlined the helper into the hook, which is why
the mistake is invisible in the source of either function on its own - it only exists once
they are put together.

Two dumps show it identically. A 16-byte frame repeating to the bottom of the stack:

```
074b0e90  68f94ac0  dinput8+0x244ac0   <- UnhandledExFilter, pushed as the argument
074b0e94  68f94ac0  dinput8+0x244ac0
074b0e98  074b0ea8                      <- saved ebp, chaining to the next frame
074b0e9c  68f8f520  dinput8+0x23f520   <- return address, inside the hook
```

**64,437 frames**, and the hook disassembles to exactly that:

```
68f8f4ea  jne  68f8f515          ; every branch...
68f8f4fa  je   68f8f515          ; ...converges...
68f8f503  je   68f8f515          ; ...here
68f8f515  push offset 68f94ac0   ; UnhandledExFilter
68f8f51a  call dword ptr [69089144]   ; kernel32!SetUnhandledExceptionFilterStub - detoured
68f8f520  mov  eax,esi                ; back into this same function
```

The call is unconditional on every path, so **every** entry into the hook recursed.

The trigger was ordinary: the oldest frames on the stack are `crashhandler.dll` on a thread
started by `steamclient.dll`. Steam's own crash handler installs a top-level filter, which is
the exact thing this hook exists to intercept - so this fired during normal play, not in some
exotic state.

It did not always kill the process: a first stack overflow can be recovered if someone
handles it, and Steam's crash-handler thread has SEH (`ntdll!_except_handler4` is on the
stack). Eventually one was not recoverable, and then the layers below made it silent.

The fix is to call the Detours **trampoline** - `orig_SetUnhandledExceptionFilter`, the
unpatched original - and never the API. A thread-local re-entrancy guard was added on top,
because a hook that re-enters itself fails silently by eating a stack rather than reporting
anything, and that is not a failure mode to leave to code review. Verified in the emitted
machine code: the hook now ends in `mov ecx,[orig]; test ecx,ecx; je; push UnhandledExFilter;
call ecx`, and the import's IAT slot is no longer referenced anywhere in the function.

## A stack overflow cannot be reported from the thread that overflowed

That recursion is *why* the stack was gone; this is why nothing could be reported once it
was. The log stopped mid-word with no `[Crash]` line at all - not even "UnhandledExFilter
invoked". WER's LocalDump told the story our own handler could not:

```
Fault Module Name   DINPUT8.dll          <- us, not the game
Exception Code      c0000005
Exception Offset    000a84c7
```

`dinput8+0xa84c7` is `__chkstk`'s probe loop - `sub eax,1000h` / `test dword ptr [eax],eax` -
and `KERNELBASE!UnhandledExceptionFilter` sat in the call stack. The faulting thread had
consumed **1,037,896 bytes**, a whole default 1 MB stack, before our filter was even
entered. The original crash was a stack overflow; our filter then ran on the empty stack and
overflowed it again before it could write one line.

`SetThreadStackGuarantee` does not save this. It is per-thread, and it was only ever called
on the init thread - never on whichever thread eventually overflows.

The earlier crash the same day, at 17:51, is the same fault at the same instruction
(`dinput8+0xa84d7`, one build's worth of drift from `+0xa84c7`) with the same repeating
frame on its stack. Both crashes were the recursion above; the "ForceLog never returned"
reading of the first one was wrong, though the hardening it prompted stands on its own.

The fix is to report from a thread that still has a stack. A reporter thread is created in
`InstallCrashHandlers`, while the process is healthy, and parks on an event; the faulting
thread hands over the reason and `EXCEPTION_POINTERS` and waits. `MiniDumpWriteDump` is
given the faulting thread's id explicitly, because it is no longer the thread calling it.
If the thread or its events could not be created, the bundle is written inline exactly as
before, so the fallback is never worse than the old behaviour.

Verified by overflowing a real stack: `Exception code: 0xC00000FD
(EXCEPTION_STACK_OVERFLOW)` with a complete bundle - context, logs and dump - where the same
test on the previous build produced nothing.

## The last resort: a hang watchdog

Everything above defends against the crash path *faulting*. Nothing defends against it
*hanging*, and SEH cannot: `MiniDumpWriteDump` takes the loader lock, so a crash caused by
heap or loader corruption can wedge the very code trying to report it. Because the handler
has by then already decided not to hand the exception to Windows, the result would be a
process sitting there forever with no report from anyone.

`ArmCrashWatchdog()` starts a thread that force-fails the process if crash handling has not
finished in **60 seconds**. A minute is deliberately generous - a full-memory dump of this
game took 13 seconds on a reporter's machine - so anything still running at 60s is genuinely
stuck, not slow, and the watchdog can never be mistaken for impatience.

It terminates with `__fastfail(FAST_FAIL_FATAL_APP_EXIT)`, not `ExitProcess` or
`TerminateProcess`. Both of those end the process *cleanly* and WER would see nothing, which
is the outcome this exists to prevent. `__fastfail` raises `STATUS_STACK_BUFFER_OVERRUN`,
which no handler can swallow and which WER does capture - and the dump it writes contains
every thread, including the stuck one.

**Where it is armed matters.** Terminal paths arm and never disarm: the unhandled filter, the
vectored handler, and the CRT fastfail hooks (the original `int 29h` runs the moment those
return, so the process is dying anyway). BBCF's own exception filter arms and then **disarms**
around our work only, because the game continues to its Steam minidump afterwards - a
watchdog left running there could fire on a process that was going to shut down on its own,
which would be far worse than the hang it guards against. Arming bumps a generation counter,
so a superseded watchdog retires instead of firing on a later healthy state.

Verified with the shipped constant rather than a shortened one:

| harness mode | behaviour | result |
|---|---|---|
| armed, then hangs | killed at ~60s | exit code `0xC0000409`, reason logged and flushed first |
| armed then disarmed, hangs 70s | survives | exit code 0, no kill |
| bundle written, dialog left open 75s | survives | disarmed before the dialog, no false kill |

## Reading the residue

When a crash still produces no bundle, the OS still recorded it. In order of usefulness:

```bash
# every BBCF crash Windows has seen, with code and faulting offset
ls -d /mnt/c/ProgramData/Microsoft/Windows/WER/ReportArchive/*BBCF*
# Report.wer is UTF-16; Sig[] entries hold code, module and offset
```

An `Exception Offset` plus `0x400000` is a static address you can look straight up in
`tools/bbcf_disasm_ascii.txt`. That is how the table at the top of this document was built,
and it is enough to identify a crash site without any dump at all. `bbcf-crash-triage`
automates the search.

## Sources

- [STATUS_STACK_BUFFER_OVERRUN doesn't mean that there was a stack buffer overrun](https://devblogs.microsoft.com/oldnewthing/20190108-00/?p=100655)
- [Misinterpreting the misleadingly-named STATUS_STACK_BUFFER_OVERRUN](https://devblogs.microsoft.com/oldnewthing/20230731-00/?p=108505)
- [SetUnhandledExceptionFilter](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-setunhandledexceptionfilter)
- [WerRegisterRuntimeExceptionModule](https://learn.microsoft.com/en-us/windows/win32/api/werapi/nf-werapi-werregisterruntimeexceptionmodule)
- [Mozilla bug 1681245 — using WerRegisterRuntimeExceptionModule to capture WER minidumps locally](https://bugzilla.mozilla.org/show_bug.cgi?id=1681245)
- [Crashpad — add WER runtime exception helper module for Windows](https://groups.google.com/a/chromium.org/g/crashpad-dev/c/KfSHqoYHpA4)
- [Mystery of RuntimeExceptionHelperModules](http://shcherbyna.com/?p=1557)
