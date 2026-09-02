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