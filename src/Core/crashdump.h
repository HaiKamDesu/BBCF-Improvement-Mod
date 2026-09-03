#pragma once

// Avoid Windows.h min/max macros clobbering std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

// Custom exception handler to generate memory dump upon crash
LONG WINAPI UnhandledExFilter(PEXCEPTION_POINTERS ExPtr);
void InstallCrashHandlers();
void WriteCrashBundle(const char* reason, PEXCEPTION_POINTERS ExPtr, bool showDialog = true);

// For fatal conditions that never raise a dispatchable exception, so there is no
// EXCEPTION_POINTERS to hand over: `int 29h` / __fastfail traps straight into the kernel,
// which means no vectored handler, no SEH and no unhandled filter ever run. Captures the
// caller's own register state instead, so the stack walk starts at the abort site and
// shows who called it. See docs/CrashCapture.md.
void WriteCrashBundleForCurrentContext(const char* reason, DWORD pseudoExceptionCode, bool showDialog = false);

// Reinstalls our top-level filter. BBCF's CRT calls SetUnhandledExceptionFilter(NULL)
// before handing a fault to WER, specifically to stop the application's filter running,
// and dinput8 is a static import so our DllMain always registers first and is always the
// one displaced.
void ReassertUnhandledExceptionFilter();

// Last resort for a crash handler that HANGS rather than faults.
//
// SEH cannot catch a deadlock, and the realistic candidate is real: MiniDumpWriteDump takes
// the loader lock, so a crash caused by heap or loader corruption can wedge the very code
// trying to report it. The process then sits there forever having already suppressed
// Windows' own reporting, which is the one outcome worse than crashing.
//
// Arm before entering a crash path that is known to end in termination. Disarm only where
// the process is expected to carry on afterwards (BBCF's own exception filter continues to
// its Steam minidump), because a watchdog that fires on a process that was going to survive
// would be far worse than the hang it is guarding against.
void ArmCrashWatchdog(const char* reason);
void DisarmCrashWatchdog();