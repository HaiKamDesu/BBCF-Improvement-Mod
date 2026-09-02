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
