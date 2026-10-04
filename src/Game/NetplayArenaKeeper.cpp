#include "NetplayArenaKeeper.h"

#include "Core/logger.h"
#include "Core/utils.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------------------
// The online match-load crash.
//
// Every online match load builds a fresh netplay arena (static addresses, image base
// 0x400000):
//   0x004E5C15  new arena (0x2E0 bytes), ctor 0x00786020
//   0x007855F0  arena init -> 0x007864C0:
//                 push 64A0000h ; call 0x00787469 (operator new)  -> [arena+2D8h]
//               then carves the 105,512,960-byte (100.6 MB) block into sub-pools.
// and every match teardown frees it again (0x004E5FDD -> 0x007859E0 -> 0x00786A30).
//
// BBCF is a 32-bit process without large-address-awareness: 2 GB of address space. A long
// online session frees and re-requests that 100 MB contiguous block every match while
// everything else fragments around it, until no 100 MB hole is left. operator new then
// throws std::bad_alloc on the main thread, WinMain's catch(...) swallows it, and the game
// closes on the versus -> match load as if it had been quit. Captured 2026-10-02
// (Crash_20261002_230400): bad_alloc from 0x007864C8, largest free block 90.07 MB.
//
// The first fix pinned that range to the arena alone, and crashed the very next session
// (Crash_20261003_210657) one step earlier: entering online character select loads
// data/ETC/char_select_stage.pac through the file-slot loader 0x0047C930 (slot 0xF9) into
// one buffer of its UNPACKED size, 65,309,392 bytes (62.3 MB), and the largest free block
// was 50.27 MB. Vanilla time-shares the address space: the arena's hole is free between
// matches and that is where the stage buffer goes. Pinning the hole to the arena starved it.
//
// So the range is shared instead. One 100 MB range is reserved on the first online match
// and handed to whichever big buffer asks while it is free: the arena, or a >= 32 MB file
// buffer from the slot loader. A request that finds it busy goes to the game's allocator,
// which is exactly what vanilla would have done. Freeing it DECOMMITS rather than releases,
// so physical memory and commit go back but the hole stays ours; the next taker recommits
// and gets demand-zero pages, the same thing a fresh allocation this size returns (the CRT
// heap serves it straight from VirtualAlloc).
//
// Frees are caught at the operator delete thunk 0x0079E0D5 (`jmp 0x007A1035`, the CRT free)
// rather than at individual free sites: the slot table frees from at least three places
// and the arena from another, all through 0x0079E864 -> 0x0079E0D5, and missing one would
// hand our VirtualAlloc block to HeapFree. Retargeting the thunk rewrites only its rel32,
// which sits inside one cache line, so threads running through it never see a torn jump.
// Detouring free's own prologue would rewrite 5 bytes of the hottest function in the
// process while other threads are executing it.
// Offline sessions never build the arena, so the range is never reserved and nothing changes.
// ---------------------------------------------------------------------------------------
namespace
{
	constexpr uintptr_t kArenaAllocCallRva = 0x003864C8;    // call operator new, in 0x007864C0
	constexpr uintptr_t kFileSlotAllocCallRva = 0x0007C977; // call operator new, in 0x0047C930
	constexpr uintptr_t kOperatorNewRva = 0x00387469;
	constexpr uintptr_t kDeleteThunkRva = 0x0039E0D5;       // jmp CRT free
	constexpr uintptr_t kCrtFreeRva = 0x003A1035;
	constexpr size_t kRegionSize = 0x064A0000;              // the arena, the largest taker
	constexpr size_t kMinFileBufferSize = 32u * 1024 * 1024;

	// The bytes around each call, minus the rel32. Checked before patching so a different
	// BBCF.exe is left alone instead of corrupted.
	const BYTE kArenaSiteBefore[] = { 0x56, 0x68, 0x00, 0x00, 0x4A, 0x06, 0x8B, 0xF1 };        // push esi; push 64A0000h; mov esi,ecx
	const BYTE kArenaSiteAfter[] = { 0x83, 0xC4, 0x04, 0x89, 0x86, 0xD8, 0x02, 0x00, 0x00 };  // add esp,4; mov [esi+2D8h],eax
	const BYTE kFileSlotSiteBefore[] = { 0x56 };                                              // push esi (the file size)
	const BYTE kFileSlotSiteAfter[] = { 0x83, 0xC4, 0x04, 0x89, 0x04, 0xFB };                 // add esp,4; mov [ebx+edi*8],eax

	using OperatorNew_t = void* (__cdecl*)(size_t);
	using Free_t = void(__cdecl*)(void*);

	OperatorNew_t g_gameOperatorNew = nullptr;
	Free_t g_crtFree = nullptr;

	enum class Taker : int { None = 0, Arena = 1, FileBuffer = 2 };

	// HookedFree runs on every thread, so ownership is claimed with a CAS and handed
	// back only after the decommit has finished.
	std::atomic<void*> g_region{ nullptr };
	std::atomic<int> g_regionTaker{ static_cast<int>(Taker::None) };
	std::atomic<size_t> g_regionTakerSize{ 0 };

	const char* TakerName(Taker taker)
	{
		switch (taker)
		{
		case Taker::Arena: return "netplay arena";
		case Taker::FileBuffer: return "file buffer";
		default: return "nothing";
		}
	}

	// Returns the region committed and owned by `taker`, or nullptr if it is not available.
	void* TryTakeRegion(Taker taker, size_t size)
	{
		void* region = g_region.load();
		if (region == nullptr)
		{
			return nullptr;
		}

		int expected = static_cast<int>(Taker::None);
		if (!g_regionTaker.compare_exchange_strong(expected, static_cast<int>(taker)))
		{
			LOG(1, "[NetplayArena] %s (%u bytes) found the shared range held by the %s (%u bytes); "
			       "using the game's allocator, as vanilla would.\n",
				TakerName(taker), static_cast<unsigned>(size),
				TakerName(static_cast<Taker>(expected)), static_cast<unsigned>(g_regionTakerSize.load()));
			return nullptr;
		}

		// Commit only what this taker asked for; the release decommits the whole range.
		if (VirtualAlloc(region, size, MEM_COMMIT, PAGE_READWRITE) == nullptr)
		{
			// Out of commit, not address space; the game's allocator would fail the same way.
			LOG(0, "[NetplayArena] Recommitting the shared range for the %s failed (err=%lu).\n",
				TakerName(taker), GetLastError());
			g_regionTaker.store(static_cast<int>(Taker::None));
			return nullptr;
		}

		g_regionTakerSize.store(size);
		LOG(2, "[NetplayArena] Shared range 0x%p -> %s (%u bytes).\n", region, TakerName(taker), static_cast<unsigned>(size));
		return region;
	}

	void* __cdecl ArenaAlloc(size_t size)
	{
		if (size != kRegionSize)
		{
			return g_gameOperatorNew(size);
		}

		if (g_region.load() == nullptr)
		{
			// First online match: reserve while the address space is still in good shape.
			// Only this thread builds the arena, so there is no reservation race.
			void* region = VirtualAlloc(nullptr, kRegionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if (region == nullptr)
			{
				LOG(0, "[NetplayArena] Could not reserve the %u-byte shared range (err=%lu); "
				       "using the game's allocator.\n", static_cast<unsigned>(kRegionSize), GetLastError());
				return g_gameOperatorNew(size);
			}
			g_regionTakerSize.store(size);
			g_regionTaker.store(static_cast<int>(Taker::Arena));
			g_region.store(region);
			LOG(1, "[NetplayArena] Reserved the shared 100 MB range at 0x%p for the rest of the session.\n", region);
			return region;
		}

		void* region = TryTakeRegion(Taker::Arena, size);
		return region != nullptr ? region : g_gameOperatorNew(size);
	}

	void* __cdecl FileSlotAlloc(size_t size)
	{
		if (size >= kMinFileBufferSize && size <= kRegionSize)
		{
			if (void* region = TryTakeRegion(Taker::FileBuffer, size))
			{
				return region;
			}
		}
		return g_gameOperatorNew(size);
	}

	void __cdecl HookedFree(void* block)
	{
		if (block == nullptr || block != g_region.load())
		{
			g_crtFree(block);
			return;
		}

		// Decommit, never release: the pages and the commit charge go back, the hole stays.
		const Taker taker = static_cast<Taker>(g_regionTaker.load());
		VirtualFree(block, kRegionSize, MEM_DECOMMIT);
		g_regionTakerSize.store(0);
		g_regionTaker.store(static_cast<int>(Taker::None));
		LOG(2, "[NetplayArena] Shared range released by the %s.\n", TakerName(taker));
	}

	bool BytesMatch(uintptr_t address, const BYTE* expected, size_t length)
	{
		return memcmp(reinterpret_cast<const void*>(address), expected, length) == 0;
	}

	// `call rel32` (E8) or `jmp rel32` (E9) at `site` going to `target`.
	bool IsRel32To(uintptr_t site, BYTE opcode, uintptr_t target)
	{
		return *reinterpret_cast<const BYTE*>(site) == opcode &&
			site + 5 + *reinterpret_cast<const int32_t*>(site + 1) == target;
	}

	// Rewrites only the rel32 with one 32-bit store; the opcode byte is never touched.
	bool RetargetRel32(uintptr_t site, void* newTarget)
	{
		DWORD oldProtect = 0;
		if (!VirtualProtect(reinterpret_cast<void*>(site), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			return false;
		}
		*reinterpret_cast<volatile int32_t*>(site + 1) =
			static_cast<int32_t>(reinterpret_cast<uintptr_t>(newTarget) - (site + 5));
		VirtualProtect(reinterpret_cast<void*>(site), 5, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);
		return true;
	}
}

void InstallNetplayArenaKeeper()
{
	const uintptr_t base = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	const uintptr_t arenaCall = base + kArenaAllocCallRva;
	const uintptr_t fileSlotCall = base + kFileSlotAllocCallRva;
	const uintptr_t deleteThunk = base + kDeleteThunkRva;
	const uintptr_t crtFree = base + kCrtFreeRva;
	const uintptr_t operatorNew = base + kOperatorNewRva;

	const bool sitesMatch =
		BytesMatch(arenaCall - sizeof(kArenaSiteBefore), kArenaSiteBefore, sizeof(kArenaSiteBefore)) &&
		BytesMatch(arenaCall + 5, kArenaSiteAfter, sizeof(kArenaSiteAfter)) &&
		IsRel32To(arenaCall, 0xE8, operatorNew) &&
		BytesMatch(fileSlotCall - sizeof(kFileSlotSiteBefore), kFileSlotSiteBefore, sizeof(kFileSlotSiteBefore)) &&
		BytesMatch(fileSlotCall + 5, kFileSlotSiteAfter, sizeof(kFileSlotSiteAfter)) &&
		IsRel32To(fileSlotCall, 0xE8, operatorNew) &&
		IsRel32To(deleteThunk, 0xE9, crtFree) &&
		((deleteThunk + 1) >> 6) == ((deleteThunk + 4) >> 6); // rel32 inside one cache line
	if (!sitesMatch)
	{
		LOG(0, "[NetplayArena] Call sites did not match the expected BBCF.exe; not patched. "
		       "Long online sessions can still end in a bad_alloc on match load.\n");
		return;
	}

	g_gameOperatorNew = reinterpret_cast<OperatorNew_t>(operatorNew);
	g_crtFree = reinterpret_cast<Free_t>(crtFree);

	// The free side first: until an alloc site is redirected nothing can hand out the
	// range, so on its own it only adds a pointer compare to every delete. If it fails,
	// nothing else is patched.
	if (!RetargetRel32(deleteThunk, reinterpret_cast<void*>(&HookedFree)))
	{
		LOG(0, "[NetplayArena] VirtualProtect failed on the delete thunk (err=%lu); not patched.\n", GetLastError());
		return;
	}

	if (!RetargetRel32(arenaCall, reinterpret_cast<void*>(&ArenaAlloc)) ||
		!RetargetRel32(fileSlotCall, reinterpret_cast<void*>(&FileSlotAlloc)))
	{
		// A half-installed state is still consistent: every pointer the range hands out
		// is caught by the delete thunk.
		LOG(0, "[NetplayArena] VirtualProtect failed (err=%lu); keeper only partly installed.\n", GetLastError());
		return;
	}

	LOG(1, "[NetplayArena] Keeper installed: the netplay arena and big file buffers share one "
	       "100 MB range from the first online match on.\n");
}
