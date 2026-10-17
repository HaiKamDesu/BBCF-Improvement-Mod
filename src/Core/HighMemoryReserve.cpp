#include "HighMemoryReserve.h"

#include "LaaStress.h"
#include "LargeAddressAware.h"
#include "logger.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace
{
	constexpr uintptr_t kTwoGb = 0x80000000u;
	// Below this, the next big request (a 16 MB texture, a 29 MB versus-screen block) is likely
	// to fail somewhere the mod cannot catch, such as a malloc whose null the game dereferences.
	constexpr size_t kLowWaterBytes = 32u * 1024 * 1024;
	constexpr DWORD kWatchIntervalMs = 2000;
	constexpr size_t kMaxRanges = 64;

	// Written only in DllMain, then read-only until Release, which is the one writer after that.
	void* g_ranges[kMaxRanges] = {};
	size_t g_rangeCount = 0;
	uint64_t g_reservedBytes = 0;
	unsigned g_failures = 0;
	uintptr_t g_maxAppAddress = 0;
	const char* g_skipReason = nullptr;

	std::atomic<bool> g_held{ false };

	// The largest free block below 2 GB. The netplay arena keeper's reserve does not count: the
	// upper half is always given up before it, and a plain malloc caller can get neither.
	size_t LargestLowBlock()
	{
		size_t largest = 0;
		uintptr_t addr = 0x10000;
		while (addr < kTwoGb)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0)
				break;
			const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
			const uintptr_t end = base + mbi.RegionSize;
			if (mbi.State == MEM_FREE)
			{
				const uintptr_t freeEnd = (end > kTwoGb) ? kTwoGb : end;
				if (freeEnd - base > largest)
					largest = freeEnd - base;
			}
			if (end <= addr)
				break;
			addr = end;
		}
		return largest;
	}

	DWORD WINAPI WatchLowHalf(LPVOID)
	{
		size_t lowest = SIZE_MAX;
		while (g_held.load())
		{
			const size_t largest = LargestLowBlock();
			if (largest < lowest)
			{
				// Each new low in whole 16 MB steps, so a long session shows how close it got.
				if (lowest == SIZE_MAX || largest / (16u << 20) < lowest / (16u << 20))
					LOG(1, "[LAA] Largest free block below 2 GB: %.1f MB.\n", largest / (1024.0 * 1024.0));
				lowest = largest;
			}
			if (largest < kLowWaterBytes)
			{
				char why[96];
				snprintf(why, sizeof(why), "the largest free block below 2 GB fell to %.1f MB", largest / (1024.0 * 1024.0));
				HighMemoryReserve::Release(why);
				break;
			}
			Sleep(kWatchIntervalMs);
		}
		return 0;
	}
}

namespace HighMemoryReserve
{
	void OnProcessAttach()
	{
		if (!LargeAddressAware::IsProcessLargeAddressAware())
		{
			g_skipReason = "BBCF.exe is not large-address-aware";
			return;
		}
		if (LaaStress::PlugsLowHalfAtBoot())
		{
			g_skipReason = "LAA stress test is putting everything above 2 GB";
			return;
		}
		SYSTEM_INFO si{};
		GetSystemInfo(&si);
		g_maxAppAddress = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
		if (g_maxAppAddress <= kTwoGb)
		{
			g_skipReason = "no address space above 2 GB (32-bit Windows?)";
			return;
		}

		uintptr_t addr = kTwoGb;
		while (addr < g_maxAppAddress && g_rangeCount < kMaxRanges)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0)
				break;
			const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
			const uintptr_t end = base + mbi.RegionSize;
			if (mbi.State == MEM_FREE)
			{
				// Reservations start on the 64 KB allocation granularity.
				const uintptr_t start = (base + 0xFFFF) & ~uintptr_t(0xFFFF);
				if (start < end)
				{
					if (void* range = VirtualAlloc(reinterpret_cast<void*>(start), end - start, MEM_RESERVE, PAGE_NOACCESS))
					{
						g_ranges[g_rangeCount++] = range;
						g_reservedBytes += end - start;
					}
					else
					{
						++g_failures;
					}
				}
			}
			if (end <= addr)
				break;
			addr = end;
		}
		g_held.store(g_rangeCount > 0);
	}

	void LogSummary()
	{
		if (g_skipReason)
		{
			ForceLog("[LAA] Upper 2 GB not held: %s.\n", g_skipReason);
			return;
		}
		ForceLog("[LAA] Holding the upper 2 GB until the game runs out below it: %u range(s), %.1f MB reserved "
			"(%u failed), highest user address 0x%08IX.\n",
			static_cast<unsigned>(g_rangeCount), g_reservedBytes / (1024.0 * 1024.0), g_failures, g_maxAppAddress);
		if (g_held.load() && !LaaStress::DisablesLowWaterWatcher())
		{
			if (HANDLE thread = CreateThread(nullptr, 0, &WatchLowHalf, nullptr, 0, nullptr))
				CloseHandle(thread);
		}
	}

	bool Release(const char* why)
	{
		bool expected = true;
		if (!g_held.compare_exchange_strong(expected, false))
			return false;
		for (size_t i = 0; i < g_rangeCount; ++i)
			VirtualFree(g_ranges[i], 0, MEM_RELEASE);
		LOG(0, "[LAA] Released the upper 2 GB (%.1f MB) to the game: %s.\n",
			g_reservedBytes / (1024.0 * 1024.0), why);
		return true;
	}

	bool IsHeld()
	{
		return g_held.load();
	}
}
