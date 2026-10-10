#include "ExitDiagnostics.h"

#include "Core/crashdump.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Game/FrameStallWatchdog.h"

#include <detours.h>
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

// Everything in this file can run on a process that is dying - ExitProcess is called by our
// own crash handler, and a hook on it must not be the thing that deadlocks the report. So no
// std::string, no LOG() (it allocates and waits on the log mutex): lines are formatted into
// stack buffers and handed to ForceLogRaw, and events live in a fixed POD ring.
namespace
{
	constexpr int kEventCount = 64;
	constexpr int kEventTextSize = 600;

	struct Event
	{
		ULONGLONG tick;
		SYSTEMTIME localTime;
		DWORD threadId;
		char text[kEventTextSize];
	};

	Event g_events[kEventCount];
	std::atomic<LONG> g_eventSequence{ 0 };

	DWORD g_mainThreadId = 0;
	HANDLE g_mainThreadHandle = nullptr;

	std::atomic<bool> g_exiting{ false };
	std::atomic<ULONGLONG> g_exitTick{ 0 };
	std::atomic<DWORD> g_exitThreadId{ 0 };

	std::atomic<ULONGLONG> g_lastPresentTick{ 0 };
	std::atomic<ULONGLONG> g_presentCount{ 0 };

	HMODULE SelfModule()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&SelfModule), &self);
		return self;
	}

	// "BBCF.exe+0x3a5684" into a caller-owned buffer. No allocation.
	void FormatAddress(uintptr_t address, char* out, size_t outSize)
	{
		HMODULE module = nullptr;
		if (address != 0 &&
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(address), &module) && module != nullptr)
		{
			wchar_t path[MAX_PATH] = {};
			const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
			const wchar_t* name = path;
			for (DWORD i = 0; i < length; ++i)
			{
				if (path[i] == L'\\' || path[i] == L'/')
				{
					name = path + i + 1;
				}
			}
			char narrow[MAX_PATH] = {};
			WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
			_snprintf_s(out, outSize, _TRUNCATE, "%s+0x%x", narrow,
				static_cast<unsigned>(address - reinterpret_cast<uintptr_t>(module)));
			return;
		}
		_snprintf_s(out, outSize, _TRUNCATE, "0x%08x <unmapped>", static_cast<unsigned>(address));
	}

	void WriteLine(const char* text)
	{
		SYSTEMTIME now;
		GetLocalTime(&now);
		char line[kEventTextSize + 96];
		_snprintf_s(line, sizeof(line), _TRUNCATE, "[%04u-%02u-%02u %02u:%02u:%02u.%03u][T%lu][L0] [Exit] %s\n",
			now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
			GetCurrentThreadId(), text);
		ForceLogRaw(line);
	}

	void Record(const char* text)
	{
		const LONG sequence = g_eventSequence.fetch_add(1);
		Event& slot = g_events[static_cast<unsigned>(sequence) % kEventCount];
		slot.tick = GetTickCount64();
		GetLocalTime(&slot.localTime);
		slot.threadId = GetCurrentThreadId();
		strncpy_s(slot.text, text, _TRUNCATE);

		WriteLine(text);
	}

	// Appends " | stack: a <- b <- c" for the calling thread, skipping 'skip' frames.
	void AppendStack(char* buffer, size_t bufferSize, ULONG skip)
	{
		void* captured[24] = {};
		const USHORT capturedCount = RtlCaptureStackBackTrace(skip, _countof(captured), captured, nullptr);

		// Drop our own leading frames (this file, the hook, the Detours trampoline hop) so the
		// first frame printed is whoever called the hooked API. Inlining decides how many of
		// those there are, so they are recognised by module rather than counted.
		const HMODULE self = SelfModule();
		const uintptr_t selfBegin = reinterpret_cast<uintptr_t>(self);
		const PIMAGE_NT_HEADERS nt = self
			? reinterpret_cast<PIMAGE_NT_HEADERS>(selfBegin + reinterpret_cast<PIMAGE_DOS_HEADER>(self)->e_lfanew)
			: nullptr;
		const uintptr_t selfEnd = nt ? selfBegin + nt->OptionalHeader.SizeOfImage : selfBegin;
		USHORT first = 0;
		while (first < capturedCount &&
			reinterpret_cast<uintptr_t>(captured[first]) >= selfBegin &&
			reinterpret_cast<uintptr_t>(captured[first]) < selfEnd)
		{
			++first;
		}
		if (first == capturedCount)
		{
			first = 0; // called from the mod itself - keep those frames, they are the caller
		}
		void* const* const frames = captured + first;
		const USHORT count = static_cast<USHORT>((capturedCount - first) < 16 ? (capturedCount - first) : 16);
		size_t used = strlen(buffer);
		if (used + 1 >= bufferSize)
		{
			return;
		}
		_snprintf_s(buffer + used, bufferSize - used, _TRUNCATE, " | stack:");
		for (USHORT i = 0; i < count; ++i)
		{
			used = strlen(buffer);
			if (used + 2 >= bufferSize)
			{
				break;
			}
			char frame[128];
			FormatAddress(reinterpret_cast<uintptr_t>(frames[i]), frame, sizeof(frame));
			_snprintf_s(buffer + used, bufferSize - used, _TRUNCATE, "%s %s", i == 0 ? "" : " <-", frame);
		}
	}

	void MarkExiting()
	{
		if (!g_exiting.exchange(true))
		{
			g_exitTick.store(GetTickCount64());
			g_exitThreadId.store(GetCurrentThreadId());
		}
	}

	const char* ThreadLabel()
	{
		return GetCurrentThreadId() == g_mainThreadId ? "the main thread" : "a non-main thread";
	}

	const char* DescribeHresult(long hr)
	{
		switch (static_cast<unsigned long>(hr))
		{
		case 0x00000000: return "D3D_OK";
		// MAKE_D3DSTATUS(2168) / (2167). 0x08760878 is what PresentEx returns while the
		// game window is being destroyed on a normal quit, so it is expected at exit.
		case 0x08760878: return "S_PRESENT_OCCLUDED (window hidden, minimised or closing)";
		case 0x08760877: return "S_PRESENT_MODE_CHANGED";
		case 0x8876021C: return "D3DERR_WASSTILLDRAWING";
		case 0x88760868: return "D3DERR_DEVICELOST";
		case 0x88760869: return "D3DERR_DEVICENOTRESET";
		case 0x88760870: return "D3DERR_DEVICEREMOVED (GPU/driver reset)";
		case 0x88760874: return "D3DERR_DEVICEHUNG";
		case 0x88760827: return "D3DERR_DRIVERINTERNALERROR";
		case 0x8876017C: return "D3DERR_OUTOFVIDEOMEMORY";
		case 0x8876086C: return "D3DERR_INVALIDCALL";
		case 0x8007000E: return "E_OUTOFMEMORY";
		default: return "unrecognised";
		}
	}

	// ------------------------------------------------------------------------------
	// API detours
	// ------------------------------------------------------------------------------

	using PostQuitMessage_t = VOID(WINAPI*)(int);
	using DestroyWindow_t = BOOL(WINAPI*)(HWND);
	using MessageBoxA_t = int(WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
	using MessageBoxW_t = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
	using ExitProcess_t = VOID(WINAPI*)(UINT);
	using TerminateProcess_t = BOOL(WINAPI*)(HANDLE, UINT);
	using CrtExit_t = void(__cdecl*)(int);

	PostQuitMessage_t orig_PostQuitMessage = nullptr;
	DestroyWindow_t orig_DestroyWindow = nullptr;
	MessageBoxA_t orig_MessageBoxA = nullptr;
	MessageBoxW_t orig_MessageBoxW = nullptr;
	ExitProcess_t orig_ExitProcess = nullptr;
	TerminateProcess_t orig_TerminateProcess = nullptr;
	CrtExit_t orig_BbcfExit = nullptr;

	// BBCF's statically linked CRT exit(int), static 0x007A54B4. It is what runs the
	// game's static destructors - the phase the Randomtrr2 crash happened in - and the
	// call at static 0x007A567F, right after WinMain (0x0044EB10) returns, is how a
	// normal quit gets here.
	constexpr uintptr_t kBbcfExitRva = 0x003A54B4;
	constexpr uintptr_t kAfterWinMainExitReturnRva = 0x003A5684;
	const BYTE kBbcfExitPrologue[] = { 0x55, 0x8B, 0xEC, 0x6A, 0x00, 0x6A, 0x00, 0xFF, 0x75, 0x08, 0xE8 };

	VOID WINAPI hook_PostQuitMessage(int exitCode)
	{
		// Only the main thread's loop is the game's. Other threads end their own loops this
		// way all the time - the intro movie's LAV decoder threads do it on every boot.
		ExitDiagnostics::NoteWithStack(GetCurrentThreadId() == g_mainThreadId
				? "PostQuitMessage(%d) called on %s - the GAME's main loop will end"
				: "PostQuitMessage(%d) called on %s - ends only that thread's own loop, not the game",
			exitCode, ThreadLabel());
		orig_PostQuitMessage(exitCode);
	}

	BOOL WINAPI hook_DestroyWindow(HWND hwnd)
	{
		if (hwnd != nullptr && hwnd == g_gameProc.hWndGameWindow)
		{
			ExitDiagnostics::NoteWithStack("DestroyWindow on the GAME window 0x%p (%s)", hwnd, ThreadLabel());
		}
		return orig_DestroyWindow(hwnd);
	}

	int WINAPI hook_MessageBoxA(HWND owner, LPCSTR text, LPCSTR caption, UINT type)
	{
		ExitDiagnostics::NoteWithStack("MessageBoxA shown on %s: caption='%.80s' text='%.200s' type=0x%X",
			ThreadLabel(), caption ? caption : "", text ? text : "", type);
		// BBCF's own thread-pool teardown (static 0x0041AD90) waits 3 s for its worker and
		// shows this when the worker is still busy - during exit, usually because a
		// profile-server request is still in flight. The worker then typically crashes on
		// a job the shutdown already freed. Named here so a log reader does not have to know.
		if (text != nullptr && strcmp(text, "Thread Delete Time Out.") == 0)
		{
			ExitDiagnostics::Note("^ BBCF's thread pool could not stop its worker within 3 s%s. A crash on a BBCF "
				"worker thread right after this is a shutdown side effect, not the cause.",
				g_exiting.load() ? " while the game was exiting" : "");
		}
		const int result = orig_MessageBoxA(owner, text, caption, type);
		ExitDiagnostics::Note("MessageBoxA '%.60s' closed with %d", text ? text : "", result);
		return result;
	}

	int WINAPI hook_MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type)
	{
		char narrowText[256] = {};
		char narrowCaption[96] = {};
		if (text) WideCharToMultiByte(CP_UTF8, 0, text, -1, narrowText, sizeof(narrowText) - 1, nullptr, nullptr);
		if (caption) WideCharToMultiByte(CP_UTF8, 0, caption, -1, narrowCaption, sizeof(narrowCaption) - 1, nullptr, nullptr);
		ExitDiagnostics::NoteWithStack("MessageBoxW shown on %s: caption='%s' text='%.200s' type=0x%X",
			ThreadLabel(), narrowCaption, narrowText, type);
		const int result = orig_MessageBoxW(owner, text, caption, type);
		ExitDiagnostics::Note("MessageBoxW '%.60s' closed with %d", narrowText, result);
		return result;
	}

	VOID WINAPI hook_ExitProcess(UINT exitCode)
	{
		MarkExiting();
		ExitDiagnostics::NoteWithStack("ExitProcess(%u) on %s", exitCode, ThreadLabel());
		// ExitProcess kills every other thread while this one keeps going. If the stall
		// watchdog is between SuspendThread and ResumeThread on this thread at that moment,
		// nobody resumes it: the game never finishes closing and stays "Suspended" forever.
		FrameStallWatchdog::Stop();
		orig_ExitProcess(exitCode);
	}

	BOOL WINAPI hook_TerminateProcess(HANDLE process, UINT exitCode)
	{
		if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
		{
			MarkExiting();
			ExitDiagnostics::NoteWithStack("TerminateProcess(self, %u) on %s", exitCode, ThreadLabel());
		}
		return orig_TerminateProcess(process, exitCode);
	}

	// The game's background file reader (AAWin_CFileReader_Thread, a lazy static at RVA
	// 0x65C140 with its guard bit at 0x65C198) keeps loading after the main loop ends. A
	// compressed file is inflated straight into the requester's buffer (0x0044FB70 ->
	// 0x00814AA0), and exit's static destructors free requesters before the reader's own
	// destructor stops the thread, so quitting mid-load wrote into freed memory
	// (Crash_20261008_211441: inflate at BBCF+0x4182b4, main thread in the reader's
	// destructor 0x0044FA30). Let it finish its queue first: +0x2C is the queue count, which
	// the thread sets back to 0 once it has nothing left; +0x0C is the thread handle.
	void WaitForGameFileReader()
	{
		constexpr uintptr_t kReaderRva = 0x0065C140;
		constexpr uintptr_t kReaderGuardRva = 0x0065C198;
		constexpr DWORD kLimitMs = 3000;
		const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		const uint32_t* const guard = reinterpret_cast<const uint32_t*>(base + kReaderGuardRva);
		const uint8_t* const reader = reinterpret_cast<const uint8_t*>(base + kReaderRva);
		if (IsBadReadPtr(guard, 4) || !(*guard & 1) || IsBadReadPtr(reader, 0x58))
			return;
		const HANDLE thread = *reinterpret_cast<const HANDLE*>(reader + 0x0C);
		const volatile int32_t* const queued = reinterpret_cast<const volatile int32_t*>(reader + 0x2C);
		DWORD code = 0;
		if (!thread || !GetExitCodeThread(thread, &code) || code != STILL_ACTIVE)
			return;
		if (*queued == 0)
		{
			ExitDiagnostics::Note("The game's file reader was idle at exit");
			return;
		}
		const ULONGLONG start = GetTickCount64();
		const int32_t atStart = *queued;
		while (*queued != 0 && GetTickCount64() - start < kLimitMs &&
			GetExitCodeThread(thread, &code) && code == STILL_ACTIVE)
			Sleep(5);
		ExitDiagnostics::Note("Waited %llu ms for the game's file reader to finish %d queued load(s) before exit%s",
			GetTickCount64() - start, atStart, *queued != 0 ? " - gave up, it is still busy" : "");
	}

	void __cdecl hook_BbcfExit(int exitCode)
	{
		MarkExiting();
		WaitForGameFileReader();
		const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
		const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		if (caller == base + kAfterWinMainExitReturnRva)
		{
			if (exitCode == -1)
			{
				// WinMain only returns -1 from its catch(...) (see WinMainCatchAllHook).
				ExitDiagnostics::Note("BBCF exit(-1): WinMain RETURNED through its catch(...) - a C++ "
					"exception escaped the game's main loop and the game swallowed it. This is a crash, "
					"not a quit; the [Crash] WinMain catch(...) report just before this names the throw.");
			}
			else
			{
				ExitDiagnostics::Note("BBCF exit(%d): WinMain RETURNED - the game's main loop ended and the game is "
					"closing normally. Static destructors run next; anything that crashes from here on is a "
					"shutdown side effect. What ended the loop is in the events just before this one.", exitCode);
			}
		}
		else
		{
			ExitDiagnostics::NoteWithStack("BBCF exit(%d) called directly (not the normal after-WinMain path) on %s",
				exitCode, ThreadLabel());
		}
		orig_BbcfExit(exitCode);
	}

	template <typename T>
	void Detour(T& original, PBYTE target, PBYTE hook, const char* name)
	{
		if (original != nullptr)
		{
			return;
		}
		if (target == nullptr)
		{
			LOG(0, "[Exit] %s: no target, not hooked\n", name);
			return;
		}
		original = reinterpret_cast<T>(DetourFunction(target, hook));
		LOG(0, "[Exit] %s hook %s (target=0x%p)\n", name, original ? "installed" : "FAILED", target);
	}

	// ------------------------------------------------------------------------------
	// Window hooks
	// ------------------------------------------------------------------------------

	HHOOK g_callWndProcHook = nullptr;
	HHOOK g_getMessageHook = nullptr;

	const char* WindowRole(HWND hwnd)
	{
		return (hwnd != nullptr && hwnd == g_gameProc.hWndGameWindow) ? "game window" : "other window";
	}

	bool AltHeld()
	{
		return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
	}

	LRESULT CALLBACK CallWndProcHook(int code, WPARAM wParam, LPARAM lParam)
	{
		if (code == HC_ACTION && lParam != 0)
		{
			const CWPSTRUCT* const message = reinterpret_cast<const CWPSTRUCT*>(lParam);
			const HWND hwnd = message->hwnd;
			switch (message->message)
			{
			case WM_CLOSE:
				ExitDiagnostics::Note("WM_CLOSE sent to the %s 0x%p (Alt held=%d, F4 held=%d)",
					WindowRole(hwnd), hwnd, AltHeld() ? 1 : 0, (GetAsyncKeyState(VK_F4) & 0x8000) ? 1 : 0);
				break;
			case WM_SYSCOMMAND:
				if ((message->wParam & 0xFFF0) == SC_CLOSE)
				{
					ExitDiagnostics::Note("WM_SYSCOMMAND SC_CLOSE to the %s 0x%p (lParam=0x%08X, Alt held=%d) - "
						"Alt+F4, the title bar X, or the taskbar 'Close window'",
						WindowRole(hwnd), hwnd, static_cast<unsigned>(message->lParam), AltHeld() ? 1 : 0);
				}
				break;
			case WM_DESTROY:
				if (hwnd == g_gameProc.hWndGameWindow)
				{
					ExitDiagnostics::Note("WM_DESTROY for the game window 0x%p", hwnd);
				}
				break;
			case WM_QUERYENDSESSION:
				ExitDiagnostics::Note("WM_QUERYENDSESSION (Windows is logging off / shutting down / updating) lParam=0x%08X",
					static_cast<unsigned>(message->lParam));
				break;
			case WM_ENDSESSION:
				ExitDiagnostics::Note("WM_ENDSESSION ending=%d lParam=0x%08X",
					static_cast<int>(message->wParam), static_cast<unsigned>(message->lParam));
				break;
			case WM_ACTIVATEAPP:
				if (hwnd == g_gameProc.hWndGameWindow)
				{
					ExitDiagnostics::Note("WM_ACTIVATEAPP active=%d (focus %s the game)",
						static_cast<int>(message->wParam), message->wParam ? "returned to" : "left");
				}
				break;
			case WM_DISPLAYCHANGE:
				ExitDiagnostics::Note("WM_DISPLAYCHANGE %ux%u %ubpp", LOWORD(message->lParam), HIWORD(message->lParam),
					static_cast<unsigned>(message->wParam));
				break;
			default:
				break;
			}
		}
		return CallNextHookEx(g_callWndProcHook, code, wParam, lParam);
	}

	// The game closing without ever finishing. 2026-10-01, dev machine: closed from the
	// taskbar while alt-tabbed, the window was destroyed and WM_QUIT reached the main loop -
	// and then nothing. No exit(), no message box, no WER report: the game sat in its own
	// teardown (inside 0x00482C00, which WinMain calls and which runs the loop) with no window
	// for three hours, Steam still showing it as running, until it was killed by hand.
	//
	// So once WM_QUIT is in, a thread waits for exit(). BBCF's teardown takes a few seconds at
	// most - its longest known wait, the thread pool's 3 s, is inside exit() and so already
	// past the line this checks. Past the deadline the main thread is stuck: a crash bundle
	// records where (the bundle walks the suspended main thread's stack, and the dump holds
	// every thread), and the process is ended, since nothing is left to show or save.
	constexpr DWORD kQuitToExitDeadlineMs = 15000;
	constexpr DWORD kQuitWatchStepMs = 250;
	constexpr DWORD kExitHangPseudoCode = 0xE0484E47; // "HNG"
	// The bundle itself can hang: it runs while the main thread is stuck, possibly holding a
	// lock the bundle needs (2026-10-09: COM's, and the process stayed alive for hours). A
	// second thread ends the process regardless once the bundle has had this long. It does
	// nothing but wait and terminate, so it needs no lock at all.
	constexpr DWORD kQuitBundleBudgetMs = 20000;
	std::atomic<bool> g_quitWatchStarted{ false };

	DWORD WINAPI QuitBackstopThread(LPVOID)
	{
		for (DWORD waited = 0; waited < kQuitToExitDeadlineMs + kQuitBundleBudgetMs; waited += kQuitWatchStepMs)
		{
			if (g_exiting.load())
			{
				return 0;
			}
			Sleep(kQuitWatchStepMs);
		}
		if (!g_exiting.load())
		{
			TerminateProcess(GetCurrentProcess(), 0);
		}
		return 0;
	}

	DWORD WINAPI QuitWatchdogThread(LPVOID)
	{
		for (DWORD waited = 0; waited < kQuitToExitDeadlineMs; waited += kQuitWatchStepMs)
		{
			if (g_exiting.load())
			{
				return 0;
			}
			Sleep(kQuitWatchStepMs);
		}
		if (g_exiting.load())
		{
			return 0;
		}
		ExitDiagnostics::Note("CLOSE HUNG: WM_QUIT was %lu s ago and the game never reached exit(). Writing a "
			"crash bundle (main thread stack + dump), then ending the process so it does not linger windowless.",
			kQuitToExitDeadlineMs / 1000);
		WriteCrashBundleForCurrentContext("The game hung while closing (WM_QUIT, never reached exit())",
			kExitHangPseudoCode, false);
		ExitDiagnostics::Note("CLOSE HUNG: bundle written, terminating");
		TerminateProcess(GetCurrentProcess(), 0);
		return 0;
	}

	void StartQuitWatchdog()
	{
		if (g_quitWatchStarted.exchange(true))
		{
			return;
		}
		HANDLE thread = CreateThread(nullptr, 0, QuitWatchdogThread, nullptr, 0, nullptr);
		if (thread)
		{
			CloseHandle(thread);
		}
		else
		{
			g_quitWatchStarted.store(false);
		}
		// Created now, while nothing holds the loader lock: a thread started once the main
		// thread is stuck in a DLL unload would itself wait on that lock before running.
		HANDLE backstop = CreateThread(nullptr, 0, QuitBackstopThread, nullptr, 0, nullptr);
		if (backstop)
		{
			CloseHandle(backstop);
		}
	}

	LRESULT CALLBACK GetMessageHook(int code, WPARAM wParam, LPARAM lParam)
	{
		if (code == HC_ACTION && wParam == PM_REMOVE && lParam != 0)
		{
			const MSG* const message = reinterpret_cast<const MSG*>(lParam);
			if (message->message == WM_QUIT)
			{
				ExitDiagnostics::Note("WM_QUIT retrieved by the main loop (exit code %d) - the game is about to close",
					static_cast<int>(message->wParam));
				// The stall sampler suspends the main thread; during teardown the main thread
				// unloads DLLs, and a suspension that lands in the middle of one is the worst
				// case for anything the sampler does. Nothing is rendered past here anyway.
				FrameStallWatchdog::Stop();
				StartQuitWatchdog();
			}
			else if (message->message == WM_CLOSE)
			{
				ExitDiagnostics::Note("WM_CLOSE posted to the %s 0x%p", WindowRole(message->hwnd), message->hwnd);
			}
		}
		return CallNextHookEx(g_getMessageHook, code, wParam, lParam);
	}

	struct DeviceCallState
	{
		const char* name;
		long lastResult;
		bool seen;
	};

	DeviceCallState g_deviceCalls[8] = {};

	DeviceCallState* DeviceCallSlot(const char* call)
	{
		for (DeviceCallState& state : g_deviceCalls)
		{
			if (state.name == call || (state.name != nullptr && strcmp(state.name, call) == 0))
			{
				return &state;
			}
			if (state.name == nullptr)
			{
				state.name = call;
				return &state;
			}
		}
		return nullptr;
	}

	void NoteResultChange(const char* call, long hr)
	{
		DeviceCallState* const state = DeviceCallSlot(call);
		if (state == nullptr)
		{
			return;
		}
		if (state->seen && state->lastResult == hr)
		{
			return;
		}
		const bool first = !state->seen;
		state->seen = true;
		state->lastResult = hr;
		// The first success is the normal state and not worth a line; everything else is.
		if (first && hr == 0)
		{
			return;
		}
		ExitDiagnostics::Note("D3D %s now returns 0x%08lX %s", call, static_cast<unsigned long>(hr), DescribeHresult(hr));
	}
}

namespace ExitDiagnostics
{
	void RecordMainThread()
	{
		if (g_mainThreadId != 0)
		{
			return;
		}
		g_mainThreadId = GetCurrentThreadId();
		g_mainThreadHandle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
			FALSE, g_mainThreadId);
	}

	DWORD MainThreadId()
	{
		return g_mainThreadId;
	}

	HANDLE MainThreadHandle()
	{
		return g_mainThreadHandle;
	}

	void InstallApiHooks()
	{
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");

		Detour(orig_PostQuitMessage, user32 ? (PBYTE)GetProcAddress(user32, "PostQuitMessage") : nullptr,
			(PBYTE)hook_PostQuitMessage, "PostQuitMessage");
		Detour(orig_DestroyWindow, user32 ? (PBYTE)GetProcAddress(user32, "DestroyWindow") : nullptr,
			(PBYTE)hook_DestroyWindow, "DestroyWindow");
		Detour(orig_MessageBoxA, user32 ? (PBYTE)GetProcAddress(user32, "MessageBoxA") : nullptr,
			(PBYTE)hook_MessageBoxA, "MessageBoxA");
		Detour(orig_MessageBoxW, user32 ? (PBYTE)GetProcAddress(user32, "MessageBoxW") : nullptr,
			(PBYTE)hook_MessageBoxW, "MessageBoxW");
		Detour(orig_ExitProcess, kernel32 ? (PBYTE)GetProcAddress(kernel32, "ExitProcess") : nullptr,
			(PBYTE)hook_ExitProcess, "ExitProcess");
		Detour(orig_TerminateProcess, kernel32 ? (PBYTE)GetProcAddress(kernel32, "TerminateProcess") : nullptr,
			(PBYTE)hook_TerminateProcess, "TerminateProcess");

		// Byte-checked: a different BBCF build must fail closed, not patch the wrong code.
		PBYTE bbcfExit = reinterpret_cast<PBYTE>(GetModuleHandleW(nullptr)) + kBbcfExitRva;
		if (!IsBadReadPtr(bbcfExit, sizeof(kBbcfExitPrologue)) &&
			memcmp(bbcfExit, kBbcfExitPrologue, sizeof(kBbcfExitPrologue)) == 0)
		{
			Detour(orig_BbcfExit, bbcfExit, (PBYTE)hook_BbcfExit, "BBCF exit()");
		}
		else
		{
			LOG(0, "[Exit] BBCF exit() prologue did not match at RVA 0x%X; not hooked\n",
				static_cast<unsigned>(kBbcfExitRva));
		}
	}

	void InstallWindowHooks()
	{
		// Must run ON the main thread. A thread-specific hook installed from another thread
		// of the same process returns a valid HHOOK and then never fires - measured with
		// build/exitdiag_test/hookprobe.cpp: 0 hits installed from a helper thread, with or
		// without an hMod, versus every WM_CLOSE/WM_QUIT when the target installs its own.
		// Installing before the thread owns a window works, so the first main-thread call
		// site to get here wins; the others are then a load and a compare.
		if (g_mainThreadId == 0 || GetCurrentThreadId() != g_mainThreadId ||
			(g_callWndProcHook != nullptr && g_getMessageHook != nullptr))
		{
			return;
		}
		const HMODULE self = SelfModule();
		if (g_callWndProcHook == nullptr)
		{
			g_callWndProcHook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcHook, self, g_mainThreadId);
		}
		const DWORD callError = g_callWndProcHook ? 0 : GetLastError();
		if (g_getMessageHook == nullptr)
		{
			g_getMessageHook = SetWindowsHookExW(WH_GETMESSAGE, GetMessageHook, self, g_mainThreadId);
		}
		const DWORD getError = g_getMessageHook ? 0 : GetLastError();
		LOG(0, "[Exit] window lifecycle hooks on main thread %lu: callwndproc=%s (err %lu) getmessage=%s (err %lu)\n",
			g_mainThreadId, g_callWndProcHook ? "ok" : "FAILED", callError,
			g_getMessageHook ? "ok" : "FAILED", getError);
	}

	void OnPresentResult(const char* call, long hr)
	{
		InstallWindowHooks();
		g_lastPresentTick.store(GetTickCount64());
		g_presentCount.fetch_add(1);
		NoteResultChange(call, hr);
	}

	void OnDeviceCall(const char* call, long hr)
	{
		NoteResultChange(call, hr);
	}

	void OnDeviceReset(const char* call, long hr)
	{
		Note("D3D %s returned 0x%08lX %s", call, static_cast<unsigned long>(hr), DescribeHresult(hr));
	}

	void Note(const char* format, ...)
	{
		char text[kEventTextSize];
		va_list args;
		va_start(args, format);
		_vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
		va_end(args);
		Record(text);
	}

	void NoteWithStack(const char* format, ...)
	{
		char text[kEventTextSize];
		va_list args;
		va_start(args, format);
		_vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
		va_end(args);
		// Skip this function and the hook that called it, so frame 0 is the hook's caller.
		AppendStack(text, sizeof(text), 2);
		Record(text);
	}

	bool IsExiting()
	{
		return g_exiting.load();
	}

	void AppendReport(std::ostringstream& oss)
	{
		const ULONGLONG now = GetTickCount64();

		oss << "\nProcess phase: ";
		if (g_exiting.load())
		{
			oss << "EXITING - the shutdown path was entered " << (now - g_exitTick.load())
				<< " ms before this report, on thread " << g_exitThreadId.load()
				<< (g_exitThreadId.load() == g_mainThreadId ? " (main thread)" : "") << ".\n"
				<< "  This crash happened while the game was already closing, so it is most likely a\n"
				<< "  side effect of the shutdown. The real question is what started the shutdown: see\n"
				<< "  the lifecycle events below.\n";
		}
		else
		{
			oss << "running (no exit path entered)\n";
		}

		const ULONGLONG lastPresent = g_lastPresentTick.load();
		if (lastPresent != 0)
		{
			oss << "Last frame presented: " << (now - lastPresent) << " ms before this report ("
				<< g_presentCount.load() << " frames this session).\n";
		}
		else
		{
			oss << "Last frame presented: never\n";
		}

		const LONG total = g_eventSequence.load();
		oss << "\nLifecycle events (oldest first, " << (total < kEventCount ? total : kEventCount)
			<< " of " << total << " kept):\n";
		if (total == 0)
		{
			oss << "  <none>\n";
			return;
		}
		const LONG first = total > kEventCount ? total - kEventCount : 0;
		for (LONG i = first; i < total; ++i)
		{
			const Event& event = g_events[static_cast<unsigned>(i) % kEventCount];
			char line[kEventTextSize + 96];
			_snprintf_s(line, sizeof(line), _TRUNCATE, "  %02u:%02u:%02u.%03u (-%llu ms) [T%lu%s] %s\n",
				event.localTime.wHour, event.localTime.wMinute, event.localTime.wSecond,
				event.localTime.wMilliseconds, now - event.tick, event.threadId,
				event.threadId == g_mainThreadId ? " main" : "", event.text);
			oss << line;
		}
	}
}
