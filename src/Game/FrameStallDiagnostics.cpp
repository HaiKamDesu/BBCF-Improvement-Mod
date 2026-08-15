#include "FrameStallDiagnostics.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/Settings.h"
#include "Game/FrameStallWatchdog.h"
#include "Game/gamestates.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>

namespace
{
	LARGE_INTEGER g_qpcFrequency{};
	bool g_haveFrequency = false;

	double ToMs(LONGLONG ticks)
	{
		return g_haveFrequency
			? (1000.0 * static_cast<double>(ticks) / static_cast<double>(g_qpcFrequency.QuadPart))
			: 0.0;
	}

	LARGE_INTEGER g_frameBegin{};
	LARGE_INTEGER g_prevFrameBegin{};
	bool g_havePrevFrameBegin = false;

	// Accumulated per frame, reset in OnFrameBegin. Written from the render
	// thread; the Steam pump detour is expected to run on that same thread,
	// but it is reached through a Detours trampoline rather than our own call
	// stack, so these are atomics rather than plain longs.
	std::atomic<LONGLONG> g_sectionTicks[FrameStallDiagnostics::Section_Count];

	const char* const kSectionNames[FrameStallDiagnostics::Section_Count] = {
		"match", "endscene", "overlay", "steamhook", "steamnative", "steamapps", "present"
	};

	// Sections that contribute to the mod total. Present and Steam's native
	// callback dispatch are measured but reported separately, so a slow driver
	// or a slow Steam client is never counted as mod cost. SteamAppsPoll is
	// mod cost, but it runs *inside* MatchState::OnUpdate and is already
	// included there - it is broken out for visibility only, so adding it
	// again would double-count it.
	bool IsModSection(int section)
	{
		return section != FrameStallDiagnostics::Section_Present
			&& section != FrameStallDiagnostics::Section_SteamNative
			&& section != FrameStallDiagnostics::Section_SteamAppsPoll;
	}

	// Throttles disk writes so a sustained slowdown logs periodically instead
	// of once per frame.
	ULONGLONG g_lastIncidentLogMs = 0;
	constexpr ULONGLONG kIncidentCooldownMs = 500;

	// Baseline sanity check. The incident lines above only ever record frames
	// that BLEW the threshold, so on their own they cannot prove the
	// measurement is sound -- if this hook ran twice per frame, or the game
	// never reached 60fps at all, the incident log would look exactly the
	// same. This periodic summary states the plain frame rate between
	// incidents, so the numbers can be checked against reality: it must read
	// ~60fps with a ~16.7ms median on a healthy machine.
	constexpr ULONGLONG kRateSummaryIntervalMs = 30 * 1000;
	ULONGLONG g_rateWindowStartMs = 0;
	unsigned long long g_rateFrameCount = 0;
	double g_rateTotalMs = 0.0;
	double g_rateWorstMs = 0.0;
	double g_rateBestMs = 0.0;
	unsigned long long g_rateOver33 = 0;
	// Which A/B arm the frames accumulated so far belong to.
	bool g_rateArmOn = true;

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
		char line[512];
		const int len = sprintf_s(line, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, message);
		if (len > 0)
		{
			DWORD written = 0;
			WriteFile(hFile, line, static_cast<DWORD>(len), &written, nullptr);
		}
		CloseHandle(hFile);
	}
}

bool FrameStallDiagnostics::IsEnabled()
{
	return Settings::settingsIni.logFrameStalls;
}

namespace
{
	// Which A/B arm the current wall-clock instant falls in. Derived from the
	// clock rather than a frame counter so both arms get equal *time* even if
	// one of them runs at a lower framerate - which is the whole thing being
	// measured.
	bool OverlayAbPhaseOn()
	{
		const int periodSec = Settings::settingsIni.overlayAbTestSeconds;
		if (periodSec <= 0)
		{
			return true;
		}
		const ULONGLONG periodMs = static_cast<ULONGLONG>(periodSec) * 1000ull;
		return ((GetTickCount64() / periodMs) % 2ull) == 0ull;
	}
}

bool FrameStallDiagnostics::OverlayAbSkipActive()
{
	if (Settings::settingsIni.overlayAbTestSeconds <= 0)
	{
		return false;
	}
	return !OverlayAbPhaseOn();
}

void FrameStallDiagnostics::AddSectionTicks(Section section, long long ticks)
{
	if (section < 0 || section >= Section_Count || ticks <= 0)
	{
		return;
	}
	g_sectionTicks[section].fetch_add(ticks, std::memory_order_relaxed);
}

FrameStallDiagnostics::ScopedOperation::ScopedOperation(const char* name, double warnMs)
	: m_name(name)
	, m_warnMs(warnMs)
	, m_startTicks(0)
	, m_active(false)
{
	if (!g_haveFrequency)
	{
		QueryPerformanceFrequency(&g_qpcFrequency);
		g_haveFrequency = g_qpcFrequency.QuadPart != 0;
	}
	if (!g_haveFrequency || name == nullptr)
	{
		return;
	}
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	m_startTicks = now.QuadPart;
	m_active = true;
}

FrameStallDiagnostics::ScopedOperation::~ScopedOperation()
{
	if (!m_active)
	{
		return;
	}

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	const double elapsedMs = ToMs(now.QuadPart - m_startTicks);
	if (elapsedMs < m_warnMs)
	{
		return;
	}

	// Global rate limit: a permanently slow operation should be obvious, not
	// drown the log (and its own writes must not become the next stall).
	static ULONGLONG s_lastWriteMs = 0;
	const ULONGLONG nowTickMs = GetTickCount64();
	if (s_lastWriteMs != 0 && (nowTickMs - s_lastWriteMs) < 2000)
	{
		return;
	}
	s_lastWriteMs = nowTickMs;

	char message[320];
	sprintf_s(message, "[SlowModOperation] %s took %.1fms (budget %.1fms)\n",
		m_name, elapsedMs, m_warnMs);

	LOG(1, "%s", message);
	AppendToIncidentFile(message);
}

FrameStallDiagnostics::ScopedSection::ScopedSection(Section section)
	: m_section(section)
	, m_startTicks(0)
	, m_active(false)
{
	if (!IsEnabled())
	{
		return;
	}
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	m_startTicks = now.QuadPart;
	m_active = true;
}

FrameStallDiagnostics::ScopedSection::~ScopedSection()
{
	if (!m_active)
	{
		return;
	}
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	AddSectionTicks(m_section, now.QuadPart - m_startTicks);
}

namespace
{
	void ReportCompletedFrame(double frameGapMs, const LONGLONG* sectionTicks);
}

void FrameStallDiagnostics::OnFrameBegin()
{
	if (!IsEnabled())
	{
		g_havePrevFrameBegin = false;
		return;
	}

	if (!g_haveFrequency)
	{
		QueryPerformanceFrequency(&g_qpcFrequency);
		g_haveFrequency = g_qpcFrequency.QuadPart != 0;
	}

	QueryPerformanceCounter(&g_frameBegin);

	// Everything the counters hold right now was recorded since the previous
	// OnFrameBegin: that frame's EndScene work AND its Present, which runs after
	// EndScene returns. So this harvest and the gap below describe the same
	// frame - the one that just finished.
	LONGLONG sectionTicks[Section_Count];
	for (int i = 0; i < Section_Count; ++i)
	{
		sectionTicks[i] = g_sectionTicks[i].exchange(0, std::memory_order_relaxed);
	}

	if (g_haveFrequency && g_havePrevFrameBegin)
	{
		ReportCompletedFrame(ToMs(g_frameBegin.QuadPart - g_prevFrameBegin.QuadPart), sectionTicks);
	}

	g_prevFrameBegin = g_frameBegin;
	g_havePrevFrameBegin = g_haveFrequency;

	FrameStallWatchdog::Heartbeat();
}

namespace
{
	// Reports one COMPLETED frame: `frameGapMs` is how long it took, and
	// `sectionTicks` is the work recorded during it.
	//
	// Getting that pairing right is the whole point of calling this from
	// OnFrameBegin rather than OnFrameEnd. The original version harvested the
	// section counters at the end of EndScene while measuring the gap back to
	// the *previous* frame's start, so an expensive call inside EndScene was
	// reported against the previous (fast) frame, and the slow frame it
	// actually caused was reported one frame later with the next frame's
	// (small) section values. A 100ms stall inside WindowManager::Render came
	// out as "frame 103ms, overlay 0.1ms, unattributed 102.8ms" - which read as
	// "not the mod" when it was entirely the mod. Do not move this back.
	void ReportCompletedFrame(double frameGapMs, const LONGLONG* sectionTicks)
	{
		using namespace FrameStallDiagnostics;

		// --- Baseline sanity: plain frame rate, every frame, not just bad ones ---
		{
			const ULONGLONG nowTickMs = GetTickCount64();
			if (g_rateWindowStartMs == 0)
			{
				g_rateWindowStartMs = nowTickMs;
			}

			++g_rateFrameCount;
			g_rateTotalMs += frameGapMs;
			if (frameGapMs > g_rateWorstMs) g_rateWorstMs = frameGapMs;
			if (g_rateBestMs == 0.0 || frameGapMs < g_rateBestMs) g_rateBestMs = frameGapMs;
			if (frameGapMs >= 33.0) ++g_rateOver33;

			// With the overlay A/B running, close each summary exactly when the
			// arm flips, so every line covers one pure arm and the two are
			// directly comparable. Otherwise fall back to a fixed window.
			const bool abActive = Settings::settingsIni.overlayAbTestSeconds > 0;
			const bool armOn = !OverlayAbSkipActive();
			const bool armFlipped = abActive && (armOn != g_rateArmOn);
			const bool windowElapsed = (nowTickMs - g_rateWindowStartMs) >= kRateSummaryIntervalMs;

			if ((abActive && armFlipped && g_rateFrameCount > 0) || (!abActive && windowElapsed))
			{
				const double windowSec = (nowTickMs - g_rateWindowStartMs) / 1000.0;
				const double avgMs = g_rateFrameCount ? (g_rateTotalMs / g_rateFrameCount) : 0.0;
				const int gameState = g_gameVals.pGameState ? *g_gameVals.pGameState : -1;

				char rateLine[400];
				sprintf_s(rateLine,
					"[FrameRate]%s %llu frames in %.1fs = %.1f fps (avg %.1fms, best %.1fms, worst %.1fms, %llu frames over 33ms) state=%d\n",
					abActive ? (g_rateArmOn ? " [overlay ON ]" : " [overlay OFF]") : "",
					g_rateFrameCount, windowSec,
					windowSec > 0.0 ? g_rateFrameCount / windowSec : 0.0,
					avgMs, g_rateBestMs, g_rateWorstMs, g_rateOver33, gameState);

				LOG(1, "%s", rateLine);
				AppendToIncidentFile(rateLine);

				g_rateWindowStartMs = nowTickMs;
				g_rateFrameCount = 0;
				g_rateTotalMs = 0.0;
				g_rateWorstMs = 0.0;
				g_rateBestMs = 0.0;
				g_rateOver33 = 0;
				g_rateArmOn = armOn;
			}
		}

		if (frameGapMs >= static_cast<double>(Settings::settingsIni.frameStallThresholdMs))
		{
			const ULONGLONG now = GetTickCount64();
			if (g_lastIncidentLogMs == 0 || now - g_lastIncidentLogMs >= kIncidentCooldownMs)
			{
				g_lastIncidentLogMs = now;

				double sectionMs[Section_Count];
				double modMs = 0.0;
				for (int i = 0; i < Section_Count; ++i)
				{
					sectionMs[i] = ToMs(sectionTicks[i]);
					if (IsModSection(i))
					{
						modMs += sectionMs[i];
					}
				}

				char sections[320];
				int written = 0;
				for (int i = 0; i < Section_Count; ++i)
				{
					written += sprintf_s(sections + written, sizeof(sections) - written,
						"%s%s %.1f", i == 0 ? "" : " + ", kSectionNames[i], sectionMs[i]);
				}

				const int gameState = g_gameVals.pGameState ? *g_gameVals.pGameState : -1;
				const double unattributedMs =
					frameGapMs - modMs - sectionMs[Section_Present] - sectionMs[Section_SteamNative];

				char message[640];
				sprintf_s(message,
					"[FrameStall] frame %.1fms state=%d | mod %.1fms | Present %.1fms | Steam %.1fms | unattributed %.1fms | breakdown: %s\n",
					frameGapMs, gameState, modMs, sectionMs[Section_Present], sectionMs[Section_SteamNative],
					unattributedMs, sections);

				LOG(1, "%s", message);
				AppendToIncidentFile(message);
			}
		}
	}
}

void FrameStallDiagnostics::OnFrameEnd()
{
	// Deliberately empty. Frame accounting happens in OnFrameBegin(), where the
	// elapsed frame and the work recorded during it line up. Kept as a call site
	// so EndScene still reads as a matched begin/end pair.
}
