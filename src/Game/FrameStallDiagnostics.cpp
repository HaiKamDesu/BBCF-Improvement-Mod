#include "FrameStallDiagnostics.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/Settings.h"
#include "Game/FrameStallWatchdog.h"
#include "Game/gamestates.h"

#include <Windows.h>
#include <cstdio>

namespace
{
	bool IsEnabled()
	{
		return Settings::settingsIni.logFrameStalls;
	}

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

	LARGE_INTEGER g_matchStateDoneAt{};

	// Throttles disk writes so a sustained slowdown logs periodically instead
	// of once per frame.
	ULONGLONG g_lastIncidentLogMs = 0;
	constexpr ULONGLONG kIncidentCooldownMs = 500;

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

void FrameStallDiagnostics::OnFrameBegin()
{
	if (!IsEnabled())
	{
		return;
	}

	if (!g_haveFrequency)
	{
		QueryPerformanceFrequency(&g_qpcFrequency);
		g_haveFrequency = g_qpcFrequency.QuadPart != 0;
	}

	QueryPerformanceCounter(&g_frameBegin);
	FrameStallWatchdog::Heartbeat();
}

void FrameStallDiagnostics::OnAfterMatchStateUpdate()
{
	if (!IsEnabled())
	{
		return;
	}

	QueryPerformanceCounter(&g_matchStateDoneAt);
}

void FrameStallDiagnostics::OnFrameEnd()
{
	if (!IsEnabled() || !g_haveFrequency)
	{
		g_havePrevFrameBegin = false;
		return;
	}

	LARGE_INTEGER renderDoneAt{};
	QueryPerformanceCounter(&renderDoneAt);

	// Frame-to-frame gap: real elapsed time since our hook was last reached,
	// i.e. what a player's FPS counter reflects. It includes the base game's
	// own simulation/rendering, not just our hook code.
	if (g_havePrevFrameBegin)
	{
		const double frameGapMs = ToMs(g_frameBegin.QuadPart - g_prevFrameBegin.QuadPart);
		const bool inMatch = g_gameVals.pGameState && *g_gameVals.pGameState == GameState_InMatch;

		if (inMatch && frameGapMs >= static_cast<double>(Settings::settingsIni.frameStallThresholdMs))
		{
			const ULONGLONG now = GetTickCount64();
			if (g_lastIncidentLogMs == 0 || now - g_lastIncidentLogMs >= kIncidentCooldownMs)
			{
				g_lastIncidentLogMs = now;

				const double matchStateMs = ToMs(g_matchStateDoneAt.QuadPart - g_frameBegin.QuadPart);
				const double renderMs = ToMs(renderDoneAt.QuadPart - g_matchStateDoneAt.QuadPart);
				const double modMs = matchStateMs + renderMs;

				char message[256];
				sprintf_s(message,
					"[FrameStall] frame took %.1fms (mod hook code: %.1fms = MatchState::OnUpdate %.1fms + WindowManager::Render %.1fms; rest of the frame: %.1fms)\n",
					frameGapMs, modMs, matchStateMs, renderMs, frameGapMs - modMs);

				LOG(1, "%s", message);
				AppendToIncidentFile(message);
			}
		}
	}

	g_prevFrameBegin = g_frameBegin;
	g_havePrevFrameBegin = true;
}
