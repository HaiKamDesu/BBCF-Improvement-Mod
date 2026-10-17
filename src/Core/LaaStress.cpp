#include "LaaStress.h"

#include "LargeAddressAware.h"
#include "logger.h"

#include "Game/NetplayArenaKeeper.h"

#include <Windows.h>
#include <cstdint>

namespace
{
	constexpr uintptr_t kTwoGb = 0x80000000u;
	constexpr DWORD kTickIntervalMs = 2000;

	bool g_enabled = false;
	bool g_exeIsLaa = false;
	bool g_requested = false;
	bool g_keepPlugging = true;
	// Mode 3: the low half is plugged once, kShortageDelayMs after boot, with the upper half
	// held by HighMemoryReserve - a session running out, to test the release.
	bool g_shortage = false;
	bool g_shortageDone = false;
	bool g_noWatcher = false;
	constexpr DWORD kShortageDelayMs = 25000;
	uintptr_t g_maxAppAddress = 0;

	// Totals of the first sweep, logged once the log is open.
	unsigned g_firstRegions = 0;
	uint64_t g_firstBytes = 0;
	unsigned g_firstFailures = 0;
	DWORD g_lastTick = 0;
	uint64_t g_totalBytes = 0;

	// Reserves every free range below 2 GB. The reservations are never released: the point is
	// that the game can no longer get memory down there.
	void Sweep(unsigned& regions, uint64_t& bytes, unsigned& failures)
	{
		uintptr_t addr = 0x10000;
		while (addr < kTwoGb)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0)
				break;

			const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
			uintptr_t regionEnd = regionBase + mbi.RegionSize;
			if (regionEnd > kTwoGb)
				regionEnd = kTwoGb;

			if (mbi.State == MEM_FREE)
			{
				// Reservations start on the 64 KB allocation granularity; the slivers below it
				// cannot hold an allocation of their own anyway.
				const uintptr_t start = (regionBase + 0xFFFF) & ~uintptr_t(0xFFFF);
				if (start < regionEnd)
				{
					if (VirtualAlloc(reinterpret_cast<void*>(start), regionEnd - start, MEM_RESERVE, PAGE_NOACCESS))
					{
						++regions;
						bytes += regionEnd - start;
					}
					else
					{
						++failures;
					}
				}
			}
			addr = regionBase + mbi.RegionSize;
			if (addr <= regionBase)
				break;
		}
	}
}

namespace LaaStress
{
	void OnProcessAttach()
	{
		wchar_t value[8] = {};
		const DWORD len = GetEnvironmentVariableW(L"BBCF_IM_LAA_STRESS", value, 8);
		// 1: plug the low 2 GB now and keep plugging what the game frees. 2: plug it once, so
		// memory the game frees later can be reused low, as it would be in a real session.
		// 3: boot normally, then plug the low 2 GB once, kShortageDelayMs in.
		// 4: as 3, with HighMemoryReserve's low-water watcher off, so only a failed allocation
		// can release the upper half.
		g_requested = len > 0 && len < 8 && value[0] >= L'1' && value[0] <= L'4';
		g_keepPlugging = value[0] == L'1';
		g_shortage = value[0] == L'3' || value[0] == L'4';
		g_noWatcher = value[0] == L'4';
		if (!g_requested)
			return;

		SYSTEM_INFO si{};
		GetSystemInfo(&si);
		g_maxAppAddress = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
		g_exeIsLaa = LargeAddressAware::IsProcessLargeAddressAware();

		// Without the flag the process has nothing above 2 GB, and plugging the low half would
		// leave the game no memory at all.
		if (!g_exeIsLaa || g_maxAppAddress <= kTwoGb)
			return;

		g_enabled = true;
		if (g_shortage)
		{
			g_lastTick = GetTickCount();
			return;
		}
		Sweep(g_firstRegions, g_firstBytes, g_firstFailures);
		g_totalBytes = g_firstBytes;
		g_lastTick = GetTickCount();
	}

	void LogSummary()
	{
		if (!g_requested)
			return;

		if (!g_enabled)
		{
			ForceLog("[LAA] Stress requested but not run: exe large-address-aware=%d, highest user address 0x%08IX.\n",
				g_exeIsLaa ? 1 : 0, g_maxAppAddress);
			return;
		}

		if (g_shortage)
		{
			ForceLog("[LAA] Stress mode %d: the low 2 GB gets plugged once, %u s after boot%s.\n", g_noWatcher ? 4 : 3,
				kShortageDelayMs / 1000, g_noWatcher ? "; low-water watcher off" : "");
			return;
		}

		// A real allocation, to show where the game's next ones will land.
		void* probe = HeapAlloc(GetProcessHeap(), 0, 1024 * 1024);
		ForceLog("[LAA] Stress on (%s): reserved %u free ranges below 2 GB (%.1f MB, %u failed) before WinMain. "
			"Highest user address 0x%08IX. A 1 MB heap block now lands at %p.\n",
			g_keepPlugging ? "re-plugging freed ranges" : "one sweep", g_firstRegions, g_firstBytes / (1024.0 * 1024.0), g_firstFailures, g_maxAppAddress, probe);
		if (probe)
			HeapFree(GetProcessHeap(), 0, probe);
	}

	bool DisablesLowWaterWatcher()
	{
		return g_enabled && g_noWatcher;
	}

	bool PlugsLowHalfAtBoot()
	{
		return g_enabled && !g_shortage;
	}

	void Tick()
	{
		if (g_enabled && g_shortage && !g_shortageDone && GetTickCount() - g_lastTick >= kShortageDelayMs)
		{
			g_shortageDone = true;
			unsigned regions = 0, failures = 0;
			uint64_t bytes = 0;
			Sweep(regions, bytes, failures);
			ForceLog("[LAA] Stress: plugged %u free ranges below 2 GB (%.1f MB, %u failed). "
				"The game is now out of room below 2 GB.\n", regions, bytes / (1024.0 * 1024.0), failures);
			if (g_noWatcher)
			{
				// Mode 4: run the game's own operator new into the wall once, so its
				// out-of-memory path (the _callnewh hook) is what releases the upper half.
				try
				{
					void* landed = ProbeGameOperatorNew(32u * 1024 * 1024);
					ForceLog("[LAA] Stress: a 32 MB operator new through the game's own path landed at %p.\n", landed);
				}
				catch (...)
				{
					ForceLog("[LAA] Stress: a 32 MB operator new through the game's own path threw.\n");
				}
			}
			return;
		}
		if (!g_enabled || !g_keepPlugging)
			return;

		const DWORD now = GetTickCount();
		if (now - g_lastTick < kTickIntervalMs)
			return;
		g_lastTick = now;

		unsigned regions = 0, failures = 0;
		uint64_t bytes = 0;
		Sweep(regions, bytes, failures);
		if (regions)
		{
			g_totalBytes += bytes;
			LOG(2, "[LAA] Re-plugged %u low ranges the game freed (%.2f MB).\n", regions, bytes / (1024.0 * 1024.0));
		}
	}
}
