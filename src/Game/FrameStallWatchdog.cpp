#include "FrameStallWatchdog.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/Settings.h"
#include "Game/gamestates.h"

#include <Windows.h>
#include <mmsystem.h>

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
		const HANDLE hFile = CreateFileW(L"BBCF_IM\\FrameStallIncidents.log", FILE_APPEND_DATA,
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

	bool IsInMatch()
	{
		return g_gameVals.pGameState && *g_gameVals.pGameState == GameState_InMatch;
	}

	// Suspends the render thread just long enough to read its instruction
	// pointer, then resumes it immediately. No allocation, no locks, no
	// loader calls happen between suspend and resume.
	bool SampleInstructionPointer(HANDLE hThread, uintptr_t& outIp)
	{
		if (SuspendThread(hThread) == static_cast<DWORD>(-1))
		{
			return false; // thread may have exited or the handle is stale; skip this sample
		}

		CONTEXT ctx = {};
		ctx.ContextFlags = CONTEXT_CONTROL;
		const BOOL gotContext = GetThreadContext(hThread, &ctx);

		ResumeThread(hThread);

		if (!gotContext)
		{
			return false;
		}

#if defined(_M_IX86)
		outIp = static_cast<uintptr_t>(ctx.Eip);
#elif defined(_M_X64)
		outIp = static_cast<uintptr_t>(ctx.Rip);
#else
		return false;
#endif
		return true;
	}

	// Resolves an address to "module.dll" (just the filename). Only called
	// with the target thread already running again -- safe to allocate here.
	std::string ResolveModuleName(uintptr_t address)
	{
		HMODULE hModule = nullptr;
		if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(address), &hModule) || !hModule)
		{
			return "unknown";
		}

		char path[MAX_PATH] = {};
		if (GetModuleFileNameA(hModule, path, MAX_PATH) == 0)
		{
			return "unknown";
		}

		const char* fileName = strrchr(path, '\\');
		return fileName ? (fileName + 1) : path;
	}

	void FlushStallSummary(const std::vector<uintptr_t>& samples, double durationMs, bool capped)
	{
		if (samples.empty())
		{
			return;
		}

		struct ModuleCount
		{
			std::string name;
			int count = 0;
		};

		std::vector<ModuleCount> counts;
		for (const uintptr_t ip : samples)
		{
			const std::string name = ResolveModuleName(ip);
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

		std::sort(counts.begin(), counts.end(),
			[](const ModuleCount& a, const ModuleCount& b) { return a.count > b.count; });

		std::string breakdown;
		for (const ModuleCount& c : counts)
		{
			char part[160];
			sprintf_s(part, "%s=%d(%.0f%%) ", c.name.c_str(), c.count,
				100.0 * c.count / static_cast<double>(samples.size()));
			breakdown += part;
		}

		char message[1400];
		sprintf_s(message, "[FrameStallWatchdog] stall ~%.1fms, %zu samples%s: %s\n",
			durationMs, samples.size(), capped ? " (capped, still ongoing)" : "", breakdown.c_str());

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
		std::vector<uintptr_t> samples;
		samples.reserve(kMaxSamplesPerStall);

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

			if (!inStall)
			{
				if (seq != lastSeenSeq)
				{
					lastSeenSeq = seq;
					continue;
				}

				// Same frame as last poll: how long have we been stuck on it?
				const double elapsedMs = ToMs(now.QuadPart - lastHeartbeat);
				const ULONGLONG nowTickMs = GetTickCount64();
				if (elapsedMs >= kStallArmThresholdMs && IsInMatch() && nowTickMs >= cooldownUntilTickMs)
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
					samples.clear();

					uintptr_t ip;
					if (SampleInstructionPointer(hThread, ip))
					{
						samples.push_back(ip);
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

					LARGE_INTEGER endQpc;
					QueryPerformanceCounter(&endQpc);
					FlushStallSummary(samples, ToMs(endQpc.QuadPart - stallStartQpc), false);
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

				uintptr_t ip;
				if (SampleInstructionPointer(hThread, ip) && samples.size() < kMaxSamplesPerStall)
				{
					samples.push_back(ip);
				}

				if (samples.size() >= kMaxSamplesPerStall)
				{
					FlushStallSummary(samples, ToMs(now.QuadPart - stallStartQpc), true);
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
