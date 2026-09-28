#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdint>
#include <sstream>

// Records how the process is ending, so a crash that happens *while the game is closing*
// can be told apart from a crash that *made* it close.
//
// Why this exists (Randomtrr2, 8.8, 2026-09-28): the report was "crashes when starting an
// online match". The crash bundle showed an access violation on a BBCF worker thread, but
// the dump's main thread was already inside exit(): WinMain had returned, a static
// destructor was tearing down BBCF's thread pool, its 3 s wait timed out and BBCF showed
// "Thread Delete Time Out.", and the worker then touched a D-Code job the shutdown had
// already freed. The crash was a symptom. Nothing we logged could say why WinMain returned,
// and crash_context.txt did not even show the main thread. See docs/CrashCapture.md.
//
// Everything recorded here goes to DEBUG.txt as it happens and into a fixed ring that the
// crash bundle prints, so the next report of this shape carries its own explanation.
namespace ExitDiagnostics
{
	// DllMain PROCESS_ATTACH. dinput8 is a static import of BBCF.exe, so that is the thread
	// that goes on to run WinMain. A handle is opened now, while the process is healthy,
	// because a crash report must not depend on OpenThread succeeding later.
	void RecordMainThread();
	DWORD MainThreadId();
	HANDLE MainThreadHandle();

	// Detours on the APIs a process ends through, plus BBCF's own exit(). Idempotent.
	void InstallApiHooks();

	// Thread-local window hooks on the main thread for the lifecycle messages (WM_CLOSE,
	// SC_CLOSE, WM_DESTROY, WM_QUIT, WM_ENDSESSION...). Windows hooks, not code patches, so
	// they cannot break the game's own message handling. Only acts when called ON the main
	// thread (a hook installed from another thread never fires); a no-op elsewhere and once
	// installed, so it is safe to call from hot main-thread paths.
	void InstallWindowHooks();

	// D3D device results. Only a change of result is recorded: a device-lost HRESULT
	// repeats every frame and would flood the ring.
	void OnPresentResult(const char* call, long hr);
	void OnDeviceCall(const char* call, long hr);
	// Resets are rare and always worth a line, whatever they return.
	void OnDeviceReset(const char* call, long hr);

	// A free-form lifecycle event: goes to DEBUG.txt and the ring. Allocation-free.
	void Note(const char* format, ...);

	// Same, plus the caller's stack (captured here, so it names whoever called the hook
	// that calls this).
	void NoteWithStack(const char* format, ...);

	// True once exit()/ExitProcess has been entered. The crash bundle uses it to say
	// "this happened during shutdown" at the top of the report.
	bool IsExiting();

	// Appended to crash_context.txt: process phase, the time since the last Present, and
	// every recorded lifecycle event.
	void AppendReport(std::ostringstream& oss);
}
