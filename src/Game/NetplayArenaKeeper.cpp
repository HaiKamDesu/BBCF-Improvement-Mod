#include "NetplayArenaKeeper.h"

#include "Core/HighMemoryReserve.h"
#include "Core/logger.h"
#include "Core/utils.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
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
// v2 shared one range between the two, reserved on the first online match and handed out
// as the buffer itself, and crashed a third way (Crash_20261004_161313): an OFFLINE session
// - a ranked entry set, then training - had never been online, so the range did not exist
// yet, and the same 65,309,392-byte stage buffer threw with a largest free block of
// 62.36 MB. The space was chopped up by 16 MB write-combined GPU driver blocks within
// minutes of boot. Reserving lazily protects nothing that happens before the first match.
//
// v3, this version, never hands our memory to the game at all:
//
//   - At install, while the address space is still clean, reserve the arena's size plus
//     1 MB (see kReserveSize for why the extra MB matters). MEM_RESERVE only:
//     no commit charge, no physical memory, nothing in it.
//   - Both call sites first try the CRT's own malloc (0x007A106D). operator new (0x0079E086)
//     is exactly `loop { p = malloc(n); if (p) return p; if (!_callnewh(n)) throw; }`, so
//     on success this is the very same heap block operator new would have returned.
//   - Only when malloc fails - the moment vanilla is about to throw - release the reserve
//     and call the game's operator new, which now finds the hole we kept. If it still
//     fails, it throws exactly as vanilla would.
//   - A render-loop tick re-reserves once a 100 MB hole exists again, so the next big
//     allocation is protected too.
//
// Every buffer the game gets is an ordinary CRT heap block, so free, realloc, _msize and
// HeapFree on it are all correct no matter which of the game's ~300 direct free() sites or
// the delete thunk eventually releases it. v2 handed out a VirtualAlloc block instead and
// relied on every free going through the delete thunk, which the 278 callers of the slot
// loader could not be proven to do; that thunk is no longer patched.
//
// What holding the reserve costs: 101 MB of address space other allocations cannot use.
// The two hooked sites get it back whenever they would fail. Everything else BBCF
// allocates is much smaller - the only constant-size request of 4 MB or more besides the
// arena is 9.5 MB, and the GPU driver works in 16 MB chunks - and the dump that motivated
// this had plenty of holes that size left. If the reserve itself cannot be made, nothing
// changes: both sites behave exactly as vanilla.
// ---------------------------------------------------------------------------------------
namespace
{
	constexpr uintptr_t kArenaAllocCallRva = 0x003864C8;    // call operator new, in 0x007864C0
	constexpr uintptr_t kFileSlotAllocCallRva = 0x0007C977; // call operator new, in 0x0047C930
	constexpr uintptr_t kOperatorNewRva = 0x00387469;       // push ebp; mov ebp,esp; pop ebp; jmp 0x0079E086
	constexpr uintptr_t kRealOperatorNewRva = 0x0039E086;   // the malloc / _callnewh / throw loop
	constexpr uintptr_t kCrtMallocRva = 0x003A106D;
	constexpr uintptr_t kArenaFreeCallRva = 0x00386A3E;     // call delete, in 0x00786A30 (the arena's only free)
	constexpr uintptr_t kDeleteRva = 0x0039E864;            // jmp 0x0079E0D5
	// The file-slot table's only frees: the loader dropping a slot's old buffer before it
	// loads a new one (0x0047C930), free one slot (0x0047C9A0), free slots 0x82 and up
	// (0x0047C9F0). Each is `mov eax,[table+slot*8]; test; jz; push eax; call delete`.
	constexpr uintptr_t kFileSlotFreeCallRvas[] = { 0x0007C95A, 0x0007C9C2, 0x0007CA13 };
	constexpr BYTE kFileSlotFreeIndexModRm[] = { 0xFB, 0xF7, 0xF3 }; // [ebx+edi*8], [edi+esi*8], [ebx+esi*8]
	constexpr size_t kArenaSize = 0x064A0000;               // the largest taker
	// NOT just the arena's size. A heap block that big is its own VirtualAlloc of the size
	// plus the heap's header, rounded to a page, at a 64 KB-aligned base - so a hole of
	// exactly kArenaSize cannot hold it. That is what Crash_20261004_180532 was: the reserve
	// was released for the arena, the largest free block became exactly 100.625 MB, and the
	// arena still threw. Reproduced in a 32-bit test with the reserve's neighbours occupied:
	// +0 does not fit, +64 KB fits. 1 MB covers any heap's header with room to spare.
	constexpr size_t kReserveSize = kArenaSize + 0x00100000;
	constexpr DWORD kReReserveIntervalMs = 5000;

	// The bytes around each call, minus the rel32. Checked before patching so a different
	// BBCF.exe is left alone instead of corrupted.
	const BYTE kArenaSiteBefore[] = { 0x56, 0x68, 0x00, 0x00, 0x4A, 0x06, 0x8B, 0xF1 };        // push esi; push 64A0000h; mov esi,ecx
	const BYTE kArenaSiteAfter[] = { 0x83, 0xC4, 0x04, 0x89, 0x86, 0xD8, 0x02, 0x00, 0x00 };  // add esp,4; mov [esi+2D8h],eax
	const BYTE kArenaFreeSiteBefore[] = { 0x85, 0xC0, 0x74, 0x13, 0x50 };                      // test eax,eax; jz; push eax
	const BYTE kArenaFreeSiteAfter[] = { 0x83, 0xC4, 0x04, 0xC7, 0x86, 0xD8, 0x02, 0x00, 0x00 }; // add esp,4; mov [esi+2D8h],0
	const BYTE kFileSlotSiteBefore[] = { 0x56 };                                              // push esi (the file size)
	const BYTE kFileSlotSiteAfter[] = { 0x83, 0xC4, 0x04, 0x89, 0x04, 0xFB };                 // add esp,4; mov [ebx+edi*8],eax
	// operator new's first iteration, `push [ebp+8]; call malloc`, proving the malloc we
	// call first is the one it would have called.
	const BYTE kRealOperatorNewHead[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0xEB, 0x0D };   // push ebp; mov ebp,esp; sub esp,10h; jmp +0Dh
	constexpr uintptr_t kRealOperatorNewMallocCallOffset = 0x18;                               // 0x0079E09E: call 0x007A106D
	// The loop's other half: `push [ebp+8]; call _callnewh; pop ecx; test eax,eax; je throw`.
	// A nonzero return makes operator new retry malloc.
	constexpr uintptr_t kRealOperatorNewCallNewhOffset = 0x0B;                                 // 0x0079E091: call 0x007AD412
	constexpr uintptr_t kCallNewhRva = 0x003AD412;
	const BYTE kCallNewhSiteBefore[] = { 0xFF, 0x75, 0x08 };                                  // push [ebp+8]
	const BYTE kCallNewhSiteAfter[] = { 0x59, 0x85, 0xC0, 0x74, 0x0F };                       // pop ecx; test eax,eax; je 0x0079E0AA

	using Alloc_t = void* (__cdecl*)(size_t);
	using Free_t = void(__cdecl*)(void*);

	Alloc_t g_gameOperatorNew = nullptr;
	Free_t g_gameDelete = nullptr;
	Alloc_t g_crtMalloc = nullptr;
	using CallNewh_t = int(__cdecl*)(size_t);
	CallNewh_t g_callNewh = nullptr;

	// Touched from whichever thread allocates and from the render-loop tick.
	std::atomic<void*> g_reserve{ nullptr };
	std::atomic<bool> g_installed{ false };
	// While an arena is alive: the free space right after it, up to kReserveSize from its
	// start. See KeepTail.
	std::atomic<void*> g_arenaTail{ nullptr };
	// A freed arena kept instead of freed. See ArenaFree.
	std::atomic<void*> g_parkedArena{ nullptr };
	// A file buffer that only fit by taking the reserve or the kept arena, and the free space
	// after it, while it is alive. See FileSlotFree.
	std::atomic<void*> g_rescuedFileBuffer{ nullptr };
	std::atomic<void*> g_fileBufferTail{ nullptr };
	DWORD g_lastReReserveAttemptMs = 0;

	void* TryReserve()
	{
		return VirtualAlloc(nullptr, kReserveSize, MEM_RESERVE, PAGE_NOACCESS);
	}

	void ReleaseTail(std::atomic<void*>& holder)
	{
		if (void* tail = holder.exchange(nullptr))
			VirtualFree(tail, 0, MEM_RELEASE);
	}

	void ReleaseArenaTail()
	{
		ReleaseTail(g_arenaTail);
	}

	// Really frees a parked arena so something else can have its address space.
	bool FreeParkedArena(const char* forWhat)
	{
		void* parked = g_parkedArena.exchange(nullptr);
		if (parked == nullptr)
			return false;
		g_gameDelete(parked);
		ReleaseArenaTail();
		LOG(0, "[NetplayArena] %s did not fit; freed the kept arena at 0x%p for it.\n", forWhat, parked);
		return true;
	}

	// *rescued: the block only fit because the kept arena or the reserve was given up for it.
	void* __cdecl RescuingAlloc(size_t size, const char* what, bool* rescued = nullptr)
	{
		if (void* block = g_crtMalloc(size))
		{
			return block;
		}

		// On a large-address-aware exe, the upper 2 GB before anything below it.
		char why[128];
		snprintf(why, sizeof(why), "%s of %u bytes did not fit", what, static_cast<unsigned>(size));
		if (HighMemoryReserve::Release(why))
		{
			if (void* block = g_crtMalloc(size))
			{
				LOG(0, "[NetplayArena] %s placed at 0x%p above 2 GB.\n", what, block);
				return block;
			}
		}

		if (FreeParkedArena(what))
		{
			void* block = g_gameOperatorNew(size); // throws as vanilla would if even that fails
			LOG(0, "[NetplayArena] %s placed at 0x%p after freeing the kept arena.\n", what, block);
			if (rescued != nullptr)
				*rescued = true;
			return block;
		}

		// Vanilla would throw std::bad_alloc right now. Give the address space back and let
		// the game's own operator new have its normal go at it.
		void* reserve = g_reserve.exchange(nullptr);
		if (reserve == nullptr)
		{
			LOG(0, "[NetplayArena] %s of %u bytes did not fit and no reserve is held; "
			       "the game's allocator decides, as in vanilla.\n", what, static_cast<unsigned>(size));
			return g_gameOperatorNew(size);
		}

		VirtualFree(reserve, 0, MEM_RELEASE);
		LOG(0, "[NetplayArena] %s of %u bytes did not fit anywhere; released the address-space reserve "
		       "at 0x%p for it.\n", what, static_cast<unsigned>(size), reserve);
		void* block = g_gameOperatorNew(size); // throws as vanilla would if even that fails
		LOG(0, "[NetplayArena] %s placed at 0x%p after the release.\n", what, block);
		if (rescued != nullptr)
			*rescued = true;
		return block;
	}

	// The heap gives a block this big its own VirtualAlloc of the size plus a random offset
	// of up to 64 KB, at the bottom of the hole. When that hole was our released reserve, the
	// ~1 MB after the arena is left free, and something small takes it during the match.
	// Then the freed arena leaves a hole smaller than kReserveSize, ArenaFree cannot take it
	// back (err=8), the versus screen chops it up, and the next arena throws
	// (Crash_20261010_185545: freed 18:55:30, largest free block 93 MB at 18:55:45).
	// Reserving the exact vacated range is not enough either: the next arena gets a new
	// random offset, and in a 32-bit test it fit the previous one's range 3 times in 40.
	// So hold the tail while the arena lives, and give it back the moment the arena is
	// freed: the hole is then the full kReserveSize again (200 of 200 in the same test).
	void KeepTail(void* block, std::atomic<void*>& holder, const char* what)
	{
		MEMORY_BASIC_INFORMATION info = {};
		if (block == nullptr || VirtualQuery(block, &info, sizeof(info)) != sizeof(info))
			return;
		BYTE* const base = static_cast<BYTE*>(info.AllocationBase);
		BYTE* end = base;
		while (VirtualQuery(end, &info, sizeof(info)) == sizeof(info) && info.AllocationBase == base)
			end += info.RegionSize;

		// Reservations start on 64 KB boundaries; the pages before the next one are unusable
		// to anyone and come back with the arena anyway.
		BYTE* const tail = reinterpret_cast<BYTE*>((reinterpret_cast<uintptr_t>(end) + 0xFFFF) & ~uintptr_t(0xFFFF));
		BYTE* const wantedEnd = base + kReserveSize;
		if (tail >= wantedEnd || VirtualQuery(tail, &info, sizeof(info)) != sizeof(info) || info.State != MEM_FREE)
			return;
		size_t size = static_cast<size_t>(wantedEnd - tail);
		if (info.RegionSize < size)
			size = info.RegionSize;
		void* kept = VirtualAlloc(tail, size, MEM_RESERVE, PAGE_NOACCESS);
		LOG(1, "[NetplayArena] %s at 0x%p; holding the %u bytes after it at 0x%p until it is freed (err=%lu).\n",
			what, block, static_cast<unsigned>(size), kept, kept ? 0ul : GetLastError());
		void* previous = holder.exchange(kept);
		if (previous != nullptr)
			VirtualFree(previous, 0, MEM_RELEASE);
	}

	void* __cdecl ArenaAlloc(size_t size)
	{
		// The arena is always the same constant size, so a kept one is exactly what the game
		// is asking for. The game initialises the whole block itself, as it does a new one.
		if (size == kArenaSize)
		{
			if (void* parked = g_parkedArena.exchange(nullptr))
			{
				LOG(1, "[NetplayArena] Netplay arena reuses the kept block at 0x%p.\n", parked);
				return parked;
			}
		}
		void* arena = RescuingAlloc(size, "Netplay arena");
		KeepTail(arena, g_arenaTail, "Arena");
		return arena;
	}

	void* __cdecl FileSlotAlloc(size_t size)
	{
		bool rescued = false;
		void* block = RescuingAlloc(size, "File buffer", &rescued);
		if (rescued)
		{
			g_rescuedFileBuffer.store(block);
			KeepTail(block, g_fileBufferTail, "File buffer");
		}
		return block;
	}

	// The stage file's hole is only free for a moment too. Dato, 2026-10-17
	// (Crash_20261017_191407, player lobby, every match): entering lobby character select,
	// the 62 MB char_select_stage.pac fit nowhere and took the reserve, as designed. Char
	// select then freed it, the versus screen loaded a 29 MB block and several smaller ones
	// straight into the hole, no 101 MB hole was left for the 5 s tick, and the arena threw
	// 18 s after the release with a largest free block of 99.12 MB. So take the hole back
	// the moment that buffer is freed, exactly as ArenaFree does for the arena. Its tail was
	// held while it lived, so the hole is a full kReserveSize again.
	//
	// Every slot buffer is freed through one of three call sites (kFileSlotFreeCallRvas); all
	// three come here, and only the rescued buffer is anything but a plain delete.
	void __cdecl FileSlotFree(void* block)
	{
		void* expected = block;
		if (block == nullptr || !g_rescuedFileBuffer.compare_exchange_strong(expected, nullptr))
		{
			g_gameDelete(block);
			return;
		}

		MEMORY_BASIC_INFORMATION info = {};
		void* const hole = VirtualQuery(block, &info, sizeof(info)) == sizeof(info) ? info.AllocationBase : nullptr;
		g_gameDelete(block);
		ReleaseTail(g_fileBufferTail);
		if (g_reserve.load() != nullptr)
		{
			LOG(1, "[NetplayArena] Rescued file buffer at 0x%p freed; a reserve is already held.\n", block);
			return;
		}

		// The exact range first: it is the one hole we know is whole right now.
		void* reserve = hole != nullptr ? VirtualAlloc(hole, kReserveSize, MEM_RESERVE, PAGE_NOACCESS) : nullptr;
		if (reserve == nullptr)
			reserve = TryReserve();
		expected = nullptr;
		if (reserve != nullptr && g_reserve.compare_exchange_strong(expected, reserve))
		{
			LOG(1, "[NetplayArena] Rescued file buffer at 0x%p freed; took its address space back as the reserve at 0x%p.\n",
				block, reserve);
		}
		else
		{
			if (reserve != nullptr)
				VirtualFree(reserve, 0, MEM_RELEASE);
			LOG(1, "[NetplayArena] Rescued file buffer at 0x%p freed but no %u-byte hole was left to reserve (err=%lu).\n",
				block, static_cast<unsigned>(kReserveSize), GetLastError());
		}
	}

	// The arena's hole is only free for a moment: a rematch frees the old arena on the result
	// screen and the versus screen loads straight into the hole, so the new arena then has
	// nowhere to go (Crash_20261007_230314: freed 23:03:00, hole taken by a 4 MB and a 28 MB
	// block, arena threw 23:03:14). The 5 s tick was too slow; take the hole back the moment
	// the game frees the arena, while it is still one piece.
	//
	// Even with the tail held, a free with no reserve held lost the hole once
	// (Crash_20261010_210536, a replay: the mod's replay rewind builds an arena per replay).
	// The full-size hole re-formed only for the moment between the free and our reserve, and
	// everything below it was packed solid, so any thread reserving memory in that moment got
	// the bottom of it. So do not free the arena at all: keep the
	// block and hand it to the next arena request. Its address space never becomes free, so
	// nothing can take it. The pages are marked discardable, so a kept arena costs no RAM,
	// only commit charge the game already uses during every match. Anything that then fails
	// to fit (the 62 MB stage file, a texture) frees the kept arena for itself, the way
	// vanilla time-shares that space.
	//
	// Keep it even while a reserve is held. Freeing it then let the versus screen chop the
	// old arena's hole up, and the char-select glyph-atlas texture spent the reserve 17 s
	// later, so the next arena had neither (Crash_20261012_132628, Huues: largest free block
	// 62 MB). Kept, the texture frees the kept arena instead (ReleaseNetplayArenaReserve) and
	// the reserve survives for the next arena. Between matches this holds the same two
	// 100 MB ranges a match does: the arena and the reserve.
	void __cdecl ArenaFree(void* block)
	{
		if (block != nullptr && g_installed.load())
		{
			void* expected = nullptr;
			if (g_parkedArena.compare_exchange_strong(expected, block))
			{
				// Whole pages only, so the heap's block header before it and anything the heap
				// keeps after it are untouched.
				const uintptr_t first = (reinterpret_cast<uintptr_t>(block) + 0xFFF) & ~uintptr_t(0xFFF);
				const uintptr_t last = (reinterpret_cast<uintptr_t>(block) + kArenaSize) & ~uintptr_t(0xFFF);
				if (last > first)
					VirtualAlloc(reinterpret_cast<void*>(first), last - first, MEM_RESET, PAGE_READWRITE);
				LOG(1, "[NetplayArena] Arena freed; kept its block at 0x%p for the next one (reserve %s).\n",
					block, g_reserve.load() != nullptr ? "held" : "not held");
				return;
			}
		}

		g_gameDelete(block);
		ReleaseArenaTail();
		if (!g_installed.load() || g_reserve.load() != nullptr)
		{
			return;
		}
		void* reserve = TryReserve();
		void* expected = nullptr;
		if (reserve != nullptr && g_reserve.compare_exchange_strong(expected, reserve))
		{
			LOG(1, "[NetplayArena] Arena freed; took its address space back as the reserve at 0x%p.\n", reserve);
		}
		else
		{
			if (reserve != nullptr)
				VirtualFree(reserve, 0, MEM_RELEASE);
			LOG(1, "[NetplayArena] Arena freed but no %u-byte hole was left to reserve (err=%lu).\n",
				static_cast<unsigned>(kReserveSize), GetLastError());
		}
	}

	// Every operator new in the game that fails to get its block lands here before it throws
	// std::bad_alloc - which WinMain's catch(...) turns into the game closing. Give it address
	// space and let it retry: the upper 2 GB on a large-address-aware exe, else the kept arena or
	// the reserve. Each release can only happen once, so a request that still fails ends up in the
	// game's own _callnewh and throws, as vanilla.
	int __cdecl OperatorNewOutOfMemory(size_t size)
	{
		char what[64];
		snprintf(what, sizeof(what), "An operator new of %u bytes", static_cast<unsigned>(size));
		if (ReleaseNetplayArenaReserve(what))
			return 1;
		return g_callNewh(size);
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
	const uintptr_t operatorNew = base + kOperatorNewRva;
	const uintptr_t realOperatorNew = base + kRealOperatorNewRva;
	const uintptr_t crtMalloc = base + kCrtMallocRva;
	const uintptr_t arenaFreeCall = base + kArenaFreeCallRva;
	const uintptr_t gameDelete = base + kDeleteRva;

	const bool sitesMatch =
		BytesMatch(arenaCall - sizeof(kArenaSiteBefore), kArenaSiteBefore, sizeof(kArenaSiteBefore)) &&
		BytesMatch(arenaCall + 5, kArenaSiteAfter, sizeof(kArenaSiteAfter)) &&
		IsRel32To(arenaCall, 0xE8, operatorNew) &&
		BytesMatch(fileSlotCall - sizeof(kFileSlotSiteBefore), kFileSlotSiteBefore, sizeof(kFileSlotSiteBefore)) &&
		BytesMatch(fileSlotCall + 5, kFileSlotSiteAfter, sizeof(kFileSlotSiteAfter)) &&
		IsRel32To(fileSlotCall, 0xE8, operatorNew) &&
		IsRel32To(operatorNew + 4, 0xE9, realOperatorNew) &&
		BytesMatch(realOperatorNew, kRealOperatorNewHead, sizeof(kRealOperatorNewHead)) &&
		IsRel32To(realOperatorNew + kRealOperatorNewMallocCallOffset, 0xE8, crtMalloc);
	if (!sitesMatch)
	{
		LOG(0, "[NetplayArena] Call sites did not match the expected BBCF.exe; not patched. "
		       "Long sessions can still end in a bad_alloc on a big load.\n");
		return;
	}

	// Optional: without it the 5 s tick still re-reserves, just later.
	const bool freeSiteMatches =
		BytesMatch(arenaFreeCall - sizeof(kArenaFreeSiteBefore), kArenaFreeSiteBefore, sizeof(kArenaFreeSiteBefore)) &&
		BytesMatch(arenaFreeCall + 5, kArenaFreeSiteAfter, sizeof(kArenaFreeSiteAfter)) &&
		IsRel32To(arenaFreeCall, 0xE8, gameDelete);

	g_gameOperatorNew = reinterpret_cast<Alloc_t>(operatorNew);
	g_gameDelete = reinterpret_cast<Free_t>(gameDelete);
	g_crtMalloc = reinterpret_cast<Alloc_t>(crtMalloc);

	// Reserve before patching: with no reserve the hooks are a plain malloc-then-operator-new,
	// which is still exactly vanilla, but there is no point patching for that.
	void* reserve = TryReserve();
	if (reserve == nullptr)
	{
		LOG(0, "[NetplayArena] Could not reserve %u bytes at boot (err=%lu); not patched.\n",
			static_cast<unsigned>(kReserveSize), GetLastError());
		return;
	}
	g_reserve.store(reserve);

	if (!RetargetRel32(arenaCall, reinterpret_cast<void*>(&ArenaAlloc)) ||
		!RetargetRel32(fileSlotCall, reinterpret_cast<void*>(&FileSlotAlloc)))
	{
		// A site that did get patched is still correct on its own: the hooks only ever
		// return ordinary heap blocks.
		LOG(0, "[NetplayArena] VirtualProtect failed (err=%lu); keeper only partly installed.\n", GetLastError());
	}
	const uintptr_t callNewhSite = realOperatorNew + kRealOperatorNewCallNewhOffset;
	g_callNewh = reinterpret_cast<CallNewh_t>(base + kCallNewhRva);
	const bool callNewhMatches =
		BytesMatch(callNewhSite - sizeof(kCallNewhSiteBefore), kCallNewhSiteBefore, sizeof(kCallNewhSiteBefore)) &&
		BytesMatch(callNewhSite + 5, kCallNewhSiteAfter, sizeof(kCallNewhSiteAfter)) &&
		IsRel32To(callNewhSite, 0xE8, base + kCallNewhRva);
	if (!callNewhMatches || !RetargetRel32(callNewhSite, reinterpret_cast<void*>(&OperatorNewOutOfMemory)))
		LOG(0, "[NetplayArena] operator new's out-of-memory call not patched; only the arena and file buffer sites can use the reserve.\n");
	if (!freeSiteMatches || !RetargetRel32(arenaFreeCall, reinterpret_cast<void*>(&ArenaFree)))
		LOG(0, "[NetplayArena] Arena free site not patched; the reserve comes back on the 5 s tick only.\n");
	// Optional too, but all three or none: a rescued buffer freed through an unpatched site
	// would leave its tail held (the tick notices, see TickNetplayArenaKeeper).
	bool fileFreeSitesMatch = true;
	for (size_t i = 0; i < sizeof(kFileSlotFreeCallRvas) / sizeof(kFileSlotFreeCallRvas[0]); ++i)
	{
		const uintptr_t site = base + kFileSlotFreeCallRvas[i];
		const BYTE before[] = { 0x8B, 0x04, kFileSlotFreeIndexModRm[i], 0x85, 0xC0, 0x74, 0x10, 0x50 };      // mov eax,[..*8]; test eax,eax; jz; push eax
		const BYTE after[] = { 0x83, 0xC4, 0x04, 0xC7, 0x04, kFileSlotFreeIndexModRm[i], 0, 0, 0, 0 }; // add esp,4; mov [..*8],0
		fileFreeSitesMatch = fileFreeSitesMatch &&
			BytesMatch(site - sizeof(before), before, sizeof(before)) &&
			BytesMatch(site + 5, after, sizeof(after)) &&
			IsRel32To(site, 0xE8, gameDelete);
	}
	bool fileFreeSitesPatched = fileFreeSitesMatch;
	for (size_t i = 0; fileFreeSitesPatched && i < sizeof(kFileSlotFreeCallRvas) / sizeof(kFileSlotFreeCallRvas[0]); ++i)
		fileFreeSitesPatched = RetargetRel32(base + kFileSlotFreeCallRvas[i], reinterpret_cast<void*>(&FileSlotFree));
	if (!fileFreeSitesPatched)
		LOG(0, "[NetplayArena] File slot free sites %s; a rescued stage buffer's space comes back on the 5 s tick only.\n",
			fileFreeSitesMatch ? "only partly patched" : "did not match, not patched");
	g_installed.store(true);

	LOG(1, "[NetplayArena] Keeper installed: %u bytes of address space reserved at 0x%p, released "
	       "only if the netplay arena or a file buffer would otherwise fail to fit.\n",
		static_cast<unsigned>(kReserveSize), reserve);
}

bool ReleaseNetplayArenaReserve(const char* forWhat)
{
	char why[128];
	snprintf(why, sizeof(why), "%s did not fit", forWhat);
	if (HighMemoryReserve::Release(why))
		return true;

	// The kept arena goes first, as in RescuingAlloc: the reserve is what the next arena
	// needs, and a texture or a file fits in either.
	if (FreeParkedArena(forWhat))
	{
		g_lastReReserveAttemptMs = GetTickCount();
		return true;
	}
	void* reserve = g_reserve.exchange(nullptr);
	if (reserve == nullptr)
		return false;
	VirtualFree(reserve, 0, MEM_RELEASE);
	// Not straight back on the next tick: whatever needed the room has to get it first.
	g_lastReReserveAttemptMs = GetTickCount();
	LOG(0, "[NetplayArena] %s did not fit; released the address-space reserve at 0x%p for it.\n",
		forWhat, reserve);
	return true;
}

void* ProbeGameOperatorNew(size_t size)
{
	if (!g_installed.load())
		return nullptr;
	void* block = g_gameOperatorNew(size);
	g_gameDelete(block);
	return block;
}

void TickNetplayArenaKeeper()
{
	// A rescued file buffer freed some way other than the three patched sites: stop holding
	// the space after it, or no full-size hole can form there again.
	if (void* buffer = g_rescuedFileBuffer.load())
	{
		MEMORY_BASIC_INFORMATION info = {};
		if (VirtualQuery(buffer, &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE)
		{
			void* expected = buffer;
			if (g_rescuedFileBuffer.compare_exchange_strong(expected, nullptr))
			{
				ReleaseTail(g_fileBufferTail);
				LOG(0, "[NetplayArena] Rescued file buffer at 0x%p was freed outside the slot table; released the space held after it.\n", buffer);
			}
		}
	}

	// A kept arena already holds a 100 MB range.
	if (!g_installed.load() || g_reserve.load() != nullptr || g_parkedArena.load() != nullptr)
	{
		return;
	}
	const DWORD now = GetTickCount();
	if (now - g_lastReReserveAttemptMs < kReReserveIntervalMs)
	{
		return;
	}
	g_lastReReserveAttemptMs = now;

	void* reserve = TryReserve();
	if (reserve == nullptr)
	{
		return; // no 100 MB hole yet; whatever took the last one is still alive
	}
	void* expected = nullptr;
	if (!g_reserve.compare_exchange_strong(expected, reserve))
	{
		VirtualFree(reserve, 0, MEM_RELEASE);
		return;
	}
	LOG(1, "[NetplayArena] Re-reserved %u bytes of address space at 0x%p.\n",
		static_cast<unsigned>(kReserveSize), reserve);
}
