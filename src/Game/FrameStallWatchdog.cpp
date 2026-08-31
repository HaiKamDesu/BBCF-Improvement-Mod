#include "FrameStallWatchdog.h"
#include "Core/utils.h"

#include "Core/BuildInfo.autogen.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/Settings.h"
#include "Game/gamestates.h"

#include <Windows.h>
#include <mmsystem.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace
{
	// Not stalled: how often to check whether the frame has advanced.
	constexpr DWORD kPollIntervalMs = 5;
	// A frame is considered a candidate stall once it has held the render
	// thread for this long without reaching the next Heartbeat(). Below the
	// FrameStallDiagnostics report threshold (33ms) on purpose, so sampling
	// has a head start and even borderline hitches get some coverage.
	constexpr double kStallArmThresholdMs = 20.0;
	// While actively sampling a stall, how often to capture the instruction
	// pointer.
	constexpr DWORD kSampleIntervalMs = 3;
	// Safety cap so a truly hung/crashed process (not just a slow frame)
	// can't grow this unbounded. At ~3ms/sample this is ~12 seconds of
	// coverage before we force a flush.
	constexpr size_t kMaxSamplesPerStall = 4000;
	// After a forced flush (cap hit), wait this long before arming again, so
	// a hard freeze doesn't spam the log every 12 seconds indefinitely.
	constexpr DWORD kCooldownAfterForcedFlushMs = 2000;
	// How often the log states that the watchdog is still running, so a gap in
	// the file can be read as "quiet" rather than "game was closed".
	constexpr ULONGLONG kAliveLineIntervalMs = 60 * 1000;

	std::atomic<bool> g_running{ false };
	std::atomic<bool> g_stopRequested{ false };
	HANDLE g_watchdogThread = nullptr;
	HANDLE g_wakeEvent = nullptr;
	bool g_timePeriodRaised = false;

	std::mutex g_handleMutex;
	DWORD g_heartbeatThreadId = 0;
	HANDLE g_heartbeatThreadHandle = nullptr;

	LARGE_INTEGER g_qpcFrequency{};
	bool g_haveFrequency = false;

	std::atomic<LONGLONG> g_lastHeartbeatQpc{ 0 };
	std::atomic<uint64_t> g_frameSeq{ 0 };

	bool IsEnabled()
	{
		return Settings::settingsIni.logFrameStalls;
	}

	double ToMs(LONGLONG ticks)
	{
		return g_haveFrequency
			? (1000.0 * static_cast<double>(ticks) / static_cast<double>(g_qpcFrequency.QuadPart))
			: 0.0;
	}

	void AppendToIncidentFile(const char* message)
	{
		const HANDLE hFile = CreateFileW(GamePathW(L"BBCF_IM\\FrameStallIncidents.log").c_str(), FILE_APPEND_DATA,
			FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			return;
		}

		SYSTEMTIME st;
		GetLocalTime(&st);
		char line[1200];
		const int len = sprintf_s(line, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, message);
		if (len > 0)
		{
			DWORD written = 0;
			WriteFile(hFile, line, static_cast<DWORD>(len), &written, nullptr);
		}
		CloseHandle(hFile);
	}

	int CurrentGameState()
	{
		return g_gameVals.pGameState ? *g_gameVals.pGameState : -1;
	}

	// --- Module table -------------------------------------------------------
	// Sampling resolves raw addresses to modules thousands of times per stall.
	// Doing that with GetModuleHandleExA per address would be far too slow, so
	// the loaded-module ranges are snapshotted up front and resolution becomes
	// a range compare. Refreshed when a stall arms (before any suspend), which
	// is often enough - modules rarely load mid-match.
	struct ModuleRange
	{
		uintptr_t base = 0;
		uintptr_t end = 0;
		char name[64] = {};
	};

	std::vector<ModuleRange> g_modules;
	uintptr_t g_selfBase = 0;
	uintptr_t g_selfEnd = 0;

	void RefreshModuleTable()
	{
		g_modules.clear();

		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
		if (snapshot == INVALID_HANDLE_VALUE)
		{
			return;
		}

		MODULEENTRY32W entry = {};
		entry.dwSize = sizeof(entry);
		if (Module32FirstW(snapshot, &entry))
		{
			do
			{
				ModuleRange range;
				range.base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
				range.end = range.base + entry.modBaseSize;
				WideCharToMultiByte(CP_UTF8, 0, entry.szModule, -1, range.name, sizeof(range.name) - 1,
					nullptr, nullptr);
				g_modules.push_back(range);
			} while (Module32NextW(snapshot, &entry));
		}
		CloseHandle(snapshot);
	}

	void InitSelfRange()
	{
		HMODULE hSelf = nullptr;
		if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&InitSelfRange), &hSelf) || !hSelf)
		{
			return;
		}

		const IMAGE_DOS_HEADER* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hSelf);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		{
			return;
		}
		const IMAGE_NT_HEADERS* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
			reinterpret_cast<const BYTE*>(hSelf) + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
		{
			return;
		}

		g_selfBase = reinterpret_cast<uintptr_t>(hSelf);
		g_selfEnd = g_selfBase + nt->OptionalHeader.SizeOfImage;
	}

	const ModuleRange* FindModule(uintptr_t address)
	{
		for (const ModuleRange& m : g_modules)
		{
			if (address >= m.base && address < m.end)
			{
				return &m;
			}
		}
		return nullptr;
	}

	// Modules that only ever appear as the *callee* of a blocking call. When
	// working out who is responsible for a kernel wait, these are skipped so
	// the first real caller underneath them is reported instead.
	bool IsSystemGlue(const char* name)
	{
		static const char* const kGlue[] = {
			"ntdll.dll", "KERNELBASE.dll", "KERNEL32.DLL", "kernel32.dll",
			"win32u.dll", "user32.dll", "sechost.dll", "msvcrt.dll",
			"ucrtbase.dll", "gdi32.dll", "gdi32full.dll", "combase.dll",
			"RPCRT4.dll", "rpcrt4.dll", "ws2_32.dll", "WS2_32.dll",
			"kernel.appcore.dll", "bcryptprimitives.dll"
		};
		for (const char* glue : kGlue)
		{
			if (_stricmp(name, glue) == 0)
			{
				return true;
			}
		}
		return false;
	}

	// --- Stack sampling -----------------------------------------------------
	// How much of the render thread's stack to copy per sample. Scanned
	// upward from ESP (toward addresses already used), never downward, so the
	// guard page at the low end of the stack is never touched -- reading it
	// would sabotage the thread's own stack growth.
	// Deep enough to reach our own frames underneath a long OS call chain. 2KB
	// was not: a DirectInput device enumeration buries the caller under
	// dinput8 -> SETUPAPI -> cfgmgr32 -> RPC -> ntdll, so the scan stopped short
	// and reported "BBCF-IM in call path: 0%" for a stall that was entirely our
	// own EnumDevices call. Under-reporting our own involvement is the single
	// worst failure mode this tool can have, so err heavily toward more depth.
	constexpr size_t kStackScanBytes = 16384;
	constexpr size_t kStackCopyChunk = 256;

	unsigned char g_stackBuffer[kStackScanBytes];

	// Kept free of C++ objects on purpose: SEH (__try) cannot coexist with
	// unwindable locals in the same function under MSVC.
	size_t CopyStackTop(uintptr_t stackPointer, unsigned char* out, size_t maxBytes)
	{
		size_t copied = 0;
		while (copied + kStackCopyChunk <= maxBytes)
		{
			__try
			{
				memcpy(out + copied, reinterpret_cast<const void*>(stackPointer + copied), kStackCopyChunk);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				break; // ran off the end of the committed stack; keep what we have
			}
			copied += kStackCopyChunk;
		}
		return copied;
	}

	struct StallSample
	{
		uintptr_t ip = 0;
		bool modInCallPath = false;
		// First non-glue module found walking up the stack: the best available
		// answer to "who made the call that is currently blocked".
		const ModuleRange* blamedCaller = nullptr;
	};

	// Suspends the render thread just long enough to read its instruction
	// pointer and copy the top of its stack, then resumes it immediately. No
	// allocation, no locks, no loader calls happen between suspend and resume;
	// all resolution work below happens with the thread already running again.
	bool SampleThread(HANDLE hThread, StallSample& out)
	{
		if (SuspendThread(hThread) == static_cast<DWORD>(-1))
		{
			return false; // thread may have exited or the handle is stale; skip this sample
		}

		CONTEXT ctx = {};
		ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		const BOOL gotContext = GetThreadContext(hThread, &ctx);

		size_t stackBytes = 0;
		if (gotContext)
		{
#if defined(_M_IX86)
			stackBytes = CopyStackTop(static_cast<uintptr_t>(ctx.Esp), g_stackBuffer, kStackScanBytes);
#elif defined(_M_X64)
			stackBytes = CopyStackTop(static_cast<uintptr_t>(ctx.Rsp), g_stackBuffer, kStackScanBytes);
#endif
		}

		ResumeThread(hThread);

		if (!gotContext)
		{
			return false;
		}

#if defined(_M_IX86)
		out.ip = static_cast<uintptr_t>(ctx.Eip);
#elif defined(_M_X64)
		out.ip = static_cast<uintptr_t>(ctx.Rip);
#else
		return false;
#endif

		// The sampled IP sits in ntdll for every kernel wait, whether the wait
		// was started by the game or by us -- which is exactly why report 2
		// could not attribute its stalls. Scanning the stack for return
		// addresses recovers the call path. This is a raw scan rather than a
		// frame walk: x86 release builds omit frame pointers, so an EBP chain
		// is not reliable. Stale leftovers can produce false positives, hence
		// the per-sample counting in the summary rather than a single verdict.
		out.modInCallPath = false;
		out.blamedCaller = nullptr;

		const size_t slots = stackBytes / sizeof(uintptr_t);
		const uintptr_t* const words = reinterpret_cast<const uintptr_t*>(g_stackBuffer);
		for (size_t i = 0; i < slots; ++i)
		{
			const uintptr_t candidate = words[i];
			if (candidate < 0x10000)
			{
				continue;
			}

			if (g_selfBase && candidate >= g_selfBase && candidate < g_selfEnd)
			{
				out.modInCallPath = true;
			}

			if (out.blamedCaller == nullptr)
			{
				const ModuleRange* const owner = FindModule(candidate);
				if (owner != nullptr && !IsSystemGlue(owner->name))
				{
					out.blamedCaller = owner;
				}
			}
		}

		return true;
	}

	struct ModuleCount
	{
		std::string name;
		int count = 0;
	};

	void Tally(std::vector<ModuleCount>& counts, const char* name)
	{
		const auto it = std::find_if(counts.begin(), counts.end(),
			[&](const ModuleCount& m) { return m.name == name; });
		if (it != counts.end())
		{
			++it->count;
		}
		else
		{
			counts.push_back({ name, 1 });
		}
	}

	std::string FormatCounts(const std::vector<ModuleCount>& counts, size_t total)
	{
		std::string out;
		for (const ModuleCount& c : counts)
		{
			char part[160];
			sprintf_s(part, "%s=%d(%.0f%%) ", c.name.c_str(), c.count,
				100.0 * c.count / static_cast<double>(total));
			out += part;
		}
		return out;
	}

	void FlushStallSummary(const std::vector<StallSample>& samples, double durationMs, bool capped,
		int gameState)
	{
		if (samples.empty())
		{
			return;
		}

		std::vector<ModuleCount> ipCounts;
		std::vector<ModuleCount> callerCounts;
		int modInCallPath = 0;

		for (const StallSample& sample : samples)
		{
			const ModuleRange* const ipModule = FindModule(sample.ip);
			Tally(ipCounts, ipModule ? ipModule->name : "unknown");
			Tally(callerCounts, sample.blamedCaller ? sample.blamedCaller->name : "unresolved");
			if (sample.modInCallPath)
			{
				++modInCallPath;
			}
		}

		const auto byCount = [](const ModuleCount& a, const ModuleCount& b) { return a.count > b.count; };
		std::sort(ipCounts.begin(), ipCounts.end(), byCount);
		std::sort(callerCounts.begin(), callerCounts.end(), byCount);

		const std::string ipBreakdown = FormatCounts(ipCounts, samples.size());
		const std::string callerBreakdown = FormatCounts(callerCounts, samples.size());

		char message[2400];
		sprintf_s(message,
			"[FrameStallWatchdog] stall ~%.1fms state=%d, %zu samples%s\n"
			"    blocked in: %s\n"
			"    called by:  %s\n"
			"    BBCF-IM in call path: %d/%zu (%.0f%%)\n",
			durationMs, gameState, samples.size(), capped ? " (capped, still ongoing)" : "",
			ipBreakdown.c_str(), callerBreakdown.c_str(),
			modInCallPath, samples.size(),
			100.0 * modInCallPath / static_cast<double>(samples.size()));

		LOG(1, "%s", message);
		AppendToIncidentFile(message);
	}

	DWORD WINAPI WatchdogThreadProc(LPVOID)
	{
		// Wait until the first Heartbeat() has established which thread to sample.
		while (!g_stopRequested.load(std::memory_order_relaxed))
		{
			{
				std::lock_guard<std::mutex> lock(g_handleMutex);
				if (g_heartbeatThreadHandle != nullptr)
				{
					break;
				}
			}
			WaitForSingleObject(g_wakeEvent, 50);
		}

		uint64_t lastSeenSeq = 0;
		bool inStall = false;
		LONGLONG stallStartQpc = 0;
		ULONGLONG cooldownUntilTickMs = 0;
		int stallGameState = -1;
		std::vector<StallSample> samples;
		samples.reserve(kMaxSamplesPerStall);

		// Report 2 (Ramiro) ended at 23:39 while the incident he reported was
		// at 23:58, and there was no way to tell whether the game had been
		// closed, the watchdog had died, or the window was simply quiet. A
		// periodic liveness line makes the log state its own coverage window.
		unsigned long long stallsSeen = 0;
		ULONGLONG nextAliveLineTickMs = GetTickCount64() + kAliveLineIntervalMs;

		while (!g_stopRequested.load(std::memory_order_relaxed))
		{
			const DWORD waitMs = inStall ? kSampleIntervalMs : kPollIntervalMs;
			WaitForSingleObject(g_wakeEvent, waitMs);
			if (g_stopRequested.load(std::memory_order_relaxed))
			{
				break;
			}

			const uint64_t seq = g_frameSeq.load(std::memory_order_acquire);
			const LONGLONG lastHeartbeat = g_lastHeartbeatQpc.load(std::memory_order_acquire);

			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);

			{
				const ULONGLONG nowTickMs = GetTickCount64();
				if (nowTickMs >= nextAliveLineTickMs)
				{
					nextAliveLineTickMs = nowTickMs + kAliveLineIntervalMs;
					char alive[256];
					sprintf_s(alive, "[FrameStallWatchdog] alive, state=%d, %llu stalls captured so far\n",
						CurrentGameState(), stallsSeen);
					AppendToIncidentFile(alive);
				}
			}

			if (!inStall)
			{
				if (seq != lastSeenSeq)
				{
					lastSeenSeq = seq;
					continue;
				}

				// Same frame as last poll: how long have we been stuck on it?
				//
				// No longer gated on being in a match. Report 2 was captured in
				// training mode and the gate meant menu, lobby and loading
				// stalls were invisible; the state is recorded per incident
				// instead, so the filtering can happen while reading the log.
				const double elapsedMs = ToMs(now.QuadPart - lastHeartbeat);
				const ULONGLONG nowTickMs = GetTickCount64();
				if (elapsedMs >= kStallArmThresholdMs && nowTickMs >= cooldownUntilTickMs)
				{
					HANDLE hThread;
					{
						std::lock_guard<std::mutex> lock(g_handleMutex);
						hThread = g_heartbeatThreadHandle;
					}
					if (hThread == nullptr)
					{
						continue;
					}

					inStall = true;
					stallStartQpc = lastHeartbeat;
					stallGameState = CurrentGameState();
					samples.clear();

					// Done before the first suspend: allocates and takes a
					// snapshot handle, neither of which is safe to do while
					// the render thread is held.
					RefreshModuleTable();

					StallSample sample;
					if (SampleThread(hThread, sample))
					{
						samples.push_back(sample);
					}
				}
			}
			else
			{
				if (seq != lastSeenSeq)
				{
					// Frame advanced: the stall is over. Flush what we captured.
					lastSeenSeq = seq;
					inStall = false;
					++stallsSeen;

					LARGE_INTEGER endQpc;
					QueryPerformanceCounter(&endQpc);
					FlushStallSummary(samples, ToMs(endQpc.QuadPart - stallStartQpc), false, stallGameState);
					continue;
				}

				HANDLE hThread;
				{
					std::lock_guard<std::mutex> lock(g_handleMutex);
					hThread = g_heartbeatThreadHandle;
				}
				if (hThread == nullptr)
				{
					inStall = false;
					continue;
				}

				StallSample sample;
				if (SampleThread(hThread, sample) && samples.size() < kMaxSamplesPerStall)
				{
					samples.push_back(sample);
				}

				if (samples.size() >= kMaxSamplesPerStall)
				{
					++stallsSeen;
					FlushStallSummary(samples, ToMs(now.QuadPart - stallStartQpc), true, stallGameState);
					inStall = false;
					cooldownUntilTickMs = GetTickCount64() + kCooldownAfterForcedFlushMs;
				}
			}
		}

		return 0;
	}
}

void FrameStallWatchdog::Start()
{
	if (!IsEnabled() || g_running.load(std::memory_order_relaxed))
	{
		return;
	}

	if (!g_haveFrequency)
	{
		QueryPerformanceFrequency(&g_qpcFrequency);
		g_haveFrequency = g_qpcFrequency.QuadPart != 0;
	}

	if (timeBeginPeriod(1) == TIMERR_NOERROR)
	{
		g_timePeriodRaised = true;
	}

	// Must happen before the sampling thread exists: SampleThread() reads this
	// range to decide whether the mod is in the blocked call path.
	InitSelfRange();

	g_stopRequested.store(false, std::memory_order_relaxed);
	g_wakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!g_wakeEvent)
	{
		LOG(1, "[FrameStallWatchdog] CreateEventW failed, watchdog disabled this session\n");
		return;
	}

	g_watchdogThread = CreateThread(nullptr, 0, WatchdogThreadProc, nullptr, 0, nullptr);
	if (!g_watchdogThread)
	{
		LOG(1, "[FrameStallWatchdog] CreateThread failed, watchdog disabled this session\n");
		CloseHandle(g_wakeEvent);
		g_wakeEvent = nullptr;
		return;
	}

	g_running.store(true, std::memory_order_relaxed);

	// Session banner: makes each run of the game a distinct, self-describing
	// block in an append-only file that spans sessions.
	char banner[512];
	sprintf_s(banner,
		"===== session start: build %s%s (%s), threshold %dms =====\n",
		BUILD_GIT_HASH, BUILD_GIT_DIRTY ? "-dirty" : "", BUILD_TIMESTAMP,
		Settings::settingsIni.frameStallThresholdMs);
	AppendToIncidentFile(banner);

	LOG(1, "[FrameStallWatchdog] started\n");
}

void FrameStallWatchdog::Stop()
{
	if (!g_running.load(std::memory_order_relaxed))
	{
		return;
	}

	g_stopRequested.store(true, std::memory_order_relaxed);
	if (g_wakeEvent)
	{
		SetEvent(g_wakeEvent);
	}
	if (g_watchdogThread)
	{
		WaitForSingleObject(g_watchdogThread, 2000);
		CloseHandle(g_watchdogThread);
		g_watchdogThread = nullptr;
	}
	if (g_wakeEvent)
	{
		CloseHandle(g_wakeEvent);
		g_wakeEvent = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(g_handleMutex);
		if (g_heartbeatThreadHandle)
		{
			CloseHandle(g_heartbeatThreadHandle);
			g_heartbeatThreadHandle = nullptr;
		}
		g_heartbeatThreadId = 0;
	}

	if (g_timePeriodRaised)
	{
		timeEndPeriod(1);
		g_timePeriodRaised = false;
	}

	AppendToIncidentFile("===== session end (clean shutdown) =====\n");

	g_running.store(false, std::memory_order_relaxed);
}

void FrameStallWatchdog::Heartbeat()
{
	if (!g_running.load(std::memory_order_relaxed))
	{
		return;
	}

	const DWORD currentThreadId = GetCurrentThreadId();
	if (currentThreadId != g_heartbeatThreadId)
	{
		std::lock_guard<std::mutex> lock(g_handleMutex);
		if (currentThreadId != g_heartbeatThreadId)
		{
			if (g_heartbeatThreadHandle)
			{
				CloseHandle(g_heartbeatThreadHandle);
				g_heartbeatThreadHandle = nullptr;
			}

			HANDLE hDup = nullptr;
			if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &hDup,
				THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
			{
				g_heartbeatThreadHandle = hDup;
				g_heartbeatThreadId = currentThreadId;
			}
		}
	}

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	g_lastHeartbeatQpc.store(now.QuadPart, std::memory_order_release);
	g_frameSeq.fetch_add(1, std::memory_order_acq_rel);
}
