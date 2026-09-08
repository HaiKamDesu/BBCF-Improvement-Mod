#include "HookManager.h"

#include "HookAddrCache.h"
#include "PatternScan.h"

#include "Core/logger.h"

#include <Psapi.h>

std::vector<functionhook_t> HookManager::hooks;

JMPBACKADDR HookManager::SetHook(const char* label, const char* pattern, const char* mask,
	const int len, void* newFunc, bool activate)
{
	/*Hooks to an adress found using a pattern*/
	if (len > MAX_LENGTH)
	{
		LOG(2, "Overwritten bytes more than %d (%d)! \n", MAX_LENGTH, len);
		return 0;
	}

	//check if there is already a hook registered with same label
	int index = GetHookStructIndex(label);
	if (index != -1)
	{
		LOG(2, "%s hook already present!\n", label);
		return hooks[index].jmpBackAddr;
	}

	hooks.push_back(functionhook_t{});
	index = hooks.size() - 1;
	hooks[index].label = label;
	hooks[index].pattern = pattern;
	hooks[index].mask = mask;
	hooks[index].length = len;
	hooks[index].newFunc = newFunc;

	DWORD startAddress = FindPattern(label, pattern, mask);
	hooks[index].startAddress = startAddress;

	if (!startAddress)
	{
		LOG(2, "%s signature scanning returned 0\n", label);
		return 0;
	}

	LOG(2, "%s found at: 0x%p\n", label, startAddress);

	if (!SaveOriginalBytes(index, (void*)startAddress, len))
	{
		LOG(2, "Saving original bytes failed.\n");
		return 0;
	}

	DWORD jmpBackAddr = startAddress + len;
	hooks[index].jmpBackAddr = jmpBackAddr;

	if (activate)
	{
		if (!PlaceHook((void*)startAddress, newFunc, len))
		{
			LOG(2, "%s hook failed.\n", label);
			return 0;
		}
		hooks[index].activated = true;
		LOG(2, "Hook set on %s\n", label);
	}

	return jmpBackAddr;
}

JMPBACKADDR HookManager::SetHook(const char* label, DWORD startAddress, const int len, void* newFunc, bool activate)
{
	/*Hooks to a direct adress*/
	if (len > MAX_LENGTH)
	{
		LOG(2, "Overwritten bytes more than %d (%d)! \n", MAX_LENGTH, len);
		return 0;
	}

	//check if there is already a hook registered with same label
	int index = GetHookStructIndex(label);
	if (index != -1)
	{
		LOG(2, "%s hook already present!\n", label);
		return hooks[index].jmpBackAddr;
	}

	hooks.push_back(functionhook_t{});
	index = hooks.size() - 1;
	hooks[index].label = label;
	hooks[index].pattern = "";
	hooks[index].mask = "";
	hooks[index].length = len;
	hooks[index].newFunc = newFunc;
	hooks[index].startAddress = startAddress;

	if (!startAddress)
	{
		LOG(2, "%s invalid start address provided: 0x%p\n", label, startAddress);
		return 0;
	}

	LOG(2, "%s found at: 0x%p\n", label, startAddress);

	if (!SaveOriginalBytes(index, (void*)startAddress, len))
	{
		LOG(2, "Saving original bytes failed.\n");
		return 0;
	}

	DWORD jmpBackAddr = startAddress + len;
	hooks[index].jmpBackAddr = jmpBackAddr;

	if (activate)
	{
		if (!PlaceHook((void*)startAddress, newFunc, len))
		{
			LOG(2, "%s hook failed.\n", label);
			return 0;
		}
		hooks[index].activated = true;
		LOG(2, "Hook set on %s\n", label);
	}

	return jmpBackAddr;
}
//sets new hooked address to an existing hook struct
bool HookManager::SetHook(const char* label, void* newFunc, bool activate)
{
	int index = GetHookStructIndex(label);
	if (index == -1)
	{
		LOG(2, "%s hook already present!\n", label);
		return false;
	}

	hooks.push_back(functionhook_t());
	hooks[index].newFunc = newFunc;

	if (activate)
	{
		if (!PlaceHook((void*)hooks[index].startAddress, newFunc, hooks[index].length))
		{
			LOG(2, "%s hook failed.\n", label);
			return false;
		}
		hooks[index].activated = true;
		LOG(2, "Hook set on %s\n", label);
	}

	return true;
}

bool HookManager::SaveOriginalBytes(int index, void* startAddress, int len)
{
	DWORD curProtection;
	if (!VirtualProtect(startAddress, len, PAGE_EXECUTE_READWRITE, &curProtection))
		return false;

	for (int i = 0; i < len; i++)
	{
		hooks[index].originalBytes[i] = *(BYTE*)((DWORD)startAddress + i);
	}

	DWORD temp;
	if (!VirtualProtect(startAddress, len, curProtection, &temp))
		return false;

	return true;
}

bool HookManager::IsHookActivated(const char* label)
{
	int index = GetHookStructIndex(label);
	if (index == -1)
	{
		return 0;
	}

	return hooks[index].activated;
}

bool HookManager::ActivateHook(const char* label)
{
	LOG(2, "Activating %s hook.\n", label);
	int index = GetHookStructIndex(label);
	if (index == -1)
	{
		LOG(2, "%s hook not found!\n", label);
		return false;
	}

	//if its already activated then dont do anything
	if (hooks[index].activated)
		return true;

	if (!PlaceHook((void*)hooks[index].startAddress, hooks[index].newFunc, hooks[index].length))
	{
		LOG(2, "%s hook failed.\n", label);
		return false;
	}

	hooks[index].activated = true;
	return true;
}

bool HookManager::DeactivateHook(const char* label)
{
	LOG(2, "Deactivating %s hook.\n", label);
	int index = GetHookStructIndex(label);
	if (index == -1)
	{
		LOG(2, "%s hook not found!\n", label);
		return false;
	}

	if (!hooks[index].activated) // already deactivated
		return true;

	int ret = RestoreOriginalBytes(index);
	if (!ret)
		LOG(2, "RestoreOriginalBytes failed.\n");

	hooks[index].activated = false;
	return ret;
}

JMPBACKADDR HookManager::GetJmpBackAddr(const char* label)
{
	int index = GetHookStructIndex(label);
	if (index == -1)
	{
		LOG(2, "%s hook not found!\n", label);
		return 0;
	}
	return hooks[index].jmpBackAddr;
}

bool HookManager::SetJmpBackAddr(const char* label, DWORD newJmpBackAddr)
{
	int index = GetHookStructIndex(label);
	if (index == -1)
	{
		LOG(2, "SetJmpBackAddr: %s hook not found!\n", label);
		return false;
	}

	hooks[index].jmpBackAddr = newJmpBackAddr;
	hooks[index].startAddress = newJmpBackAddr - hooks[index].length;

	int startAddress = hooks[index].startAddress;
	int len = hooks[index].length;

	if (!SaveOriginalBytes(index, (void*)startAddress, len))
	{
		LOG(2, "Saving original bytes failed.\n");
		return false;
	}

	return true;
}

DWORD HookManager::GetStartAddress(const char* label)
{
	int index = GetHookStructIndex(label);
	if (index == -1)
	{
		LOG(2, "SetJmpBackAddr: %s hook not found!\n", label);
		return 0;
	}

	return hooks[index].startAddress;
}

//registering a new hook struct without hooking
JMPBACKADDR HookManager::RegisterHook(const char* label, const char* pattern, const char* mask, const int len)
{
	int index = GetHookStructIndex(label);
	if (index != -1)
	{
		LOG(2, "%s hook already present!\n", label);
		return hooks[index].jmpBackAddr;
	}

	hooks.push_back(functionhook_t());
	index = hooks.size() - 1;
	hooks[index].label = label;
	hooks[index].pattern = pattern;
	hooks[index].mask = mask;
	hooks[index].length = len;
	hooks[index].newFunc = 0;

	DWORD startAddress = FindPattern(label, pattern, mask);
	hooks[index].startAddress = startAddress;

	if (!startAddress)
	{
		LOG(2, "%s signature scanning returned 0\n", label);
		return 0;
	}

	LOG(2, "%s found at: 0x%p\n", label, startAddress);

	return startAddress;
}

//startIndexOfWildCard = set to the wildcard's starting index
//bytesToReturn = 1/2/4
int HookManager::GetOriginalBytes(const char* label, int startIndex, int bytesToReturn)
{
	int index = GetHookStructIndex(label);
	if (index == -1)
	{
		LOG(2, "%s hook not found!\n", label);
		return 0;
	}

	void* needle = (char*)hooks[index].originalBytes + (char)startIndex;

	switch (bytesToReturn)
	{
	case 1:
		return *(unsigned char*)needle;
	case 2:
		return *(unsigned short*)needle;
	case 4:
		return *(unsigned int*)needle;
	default:
		return 0;
	}
}

//startIndexOfWildCard = set to the wildcard's starting index
//bytesToReturn = 1/2/4
int HookManager::GetBytesFromAddr(const char* label, int startIndex, int bytesToReturn)
{
	int index = GetHookStructIndex(label);
	if (index == -1)
	{
		LOG(2, "%s hook not found!\n", label);
		return 0;
	}

	void* needle = (char*)hooks[index].startAddress + (char)startIndex;

	switch (bytesToReturn)
	{
	case 1:
		return *(unsigned char*)needle;
	case 2:
		return *(unsigned short*)needle;
	case 4:
		return *(unsigned int*)needle;
	default:
		return 0;
	}
}

void HookManager::Cleanup()
{
	//
}

int HookManager::GetHookStructIndex(const char* label)
{
	for (unsigned int i = 0; i < hooks.size(); i++)
	{
		if (strcmp(hooks[i].label.c_str(), label) == 0)
			return i;
	}
	return -1;
}

bool HookManager::RestoreOriginalBytes(int index)
{
	DWORD curProtection;
	if (!VirtualProtect((void*)hooks[index].startAddress, hooks[index].length, PAGE_EXECUTE_READWRITE, &curProtection))
		return false;

	for (int i = 0; i < hooks[index].length; i++)
	{
		*((BYTE*)(hooks[index].startAddress + i)) = hooks[index].originalBytes[i];
	}

	DWORD temp;
	if (!VirtualProtect((void*)hooks[index].startAddress, hooks[index].length, curProtection, &temp))
		return false;

	return true;
}

bool HookManager::PlaceHook(void* toHook, void* ourFunc, int len)
{
	if (len < 5 || len > MAX_LENGTH)
	{
		return false;
	}

	DWORD curProtection;
	if (!VirtualProtect(toHook, len, PAGE_EXECUTE_READWRITE, &curProtection))
		return false;

	memset(toHook, 0x90, len);

	DWORD relativeAddress = ((DWORD)ourFunc - (DWORD)toHook) - 5;

	*(BYTE*)toHook = 0xE9;
	*(DWORD*)((DWORD)toHook + 1) = relativeAddress;

	DWORD temp;
	if (!VirtualProtect(toHook, len, curProtection, &temp))
		return false;

	return true;
}

int HookManager::OverWriteBytes(void* startAddress, void* endAddress, const char* pattern,
	const char* mask, const char* newBytes)
{
	int overwrittenCount = 0;

	DWORD base = (DWORD)startAddress;
	DWORD size = (DWORD)endAddress - (DWORD)startAddress;

	//Get length for our mask, this will allow us to loop through our array
	DWORD patternLength = (DWORD)strlen(mask);

	for (DWORD i = 0; i < size - patternLength; i++)
	{
		bool found = true;
		for (DWORD j = 0; j < patternLength; j++)
		{
			//if we have a ? in our mask then we have true by default, 
			//or if the bytes match then we keep searching until finding it or not
			found &= mask[j] == '?' || pattern[j] == *(char*)(base + i + j);
		}
		//found = true, our entire pattern was found
		//return the memory addy so we can write to it
		if (found)
		{
			char* toOverwrite = (char*)base + i;

			DWORD curProtection;
			if (!VirtualProtect(toOverwrite, patternLength, PAGE_EXECUTE_READWRITE, &curProtection))
				return overwrittenCount;

			for (int k = 0; k < patternLength; k++)
			{
				*((BYTE*)(toOverwrite + k)) = newBytes[k];
			}

			overwrittenCount++;

			DWORD temp;
			if (!VirtualProtect(toOverwrite, patternLength, curProtection, &temp))
				return overwrittenCount;
		}
	}
	return overwrittenCount;
}

namespace
{
	// Hook placement runs on the game's main thread inside the D3D device wrapper's
	// constructor, in front of the game's first frame, and it cannot be moved: SteamDRM has to
	// have unpacked the exe, and the hooks have to be in place before the game runs. So the
	// only thing that can be done about the ~1.55s it used to cost is to make it cheap.
	//
	// Three things do that, in descending order of effect:
	//  - a verified address cache, so a repeat launch does a pattern-length compare per hook
	//    instead of sweeping a 23MB image per hook. This is the one that matters on a cold
	//    boot: it touches the handful of pages holding the hook sites instead of paging in the
	//    whole exe once per scan.
	//  - scanning the executable sections instead of the whole mapped image.
	//  - breaking out of the byte compare on the first mismatch, and using memchr to find
	//    candidate offsets rather than testing every one.
	struct HookScanStats
	{
		int scans = 0;          // FindPattern calls
		int cacheHits = 0;      // resolved by a verified cached address
		int execHits = 0;       // found by scanning the executable sections
		int fullImageHits = 0;  // only found once the scan widened to the whole image
		int misses = 0;         // not found at all
		double totalMs = 0.0;
		double worstMs = 0.0;
		std::string worstLabel;
		unsigned long long bytesSwept = 0;
	};

	HookScanStats g_scanStats;
	bool g_cacheInitialised = false;
	unsigned long long g_moduleKey = 0;

	double QpcToMs(LONGLONG ticks)
	{
		static LARGE_INTEGER frequency = {};
		if (frequency.QuadPart == 0)
		{
			QueryPerformanceFrequency(&frequency);
		}
		return frequency.QuadPart == 0 ? 0.0 : (double)ticks * 1000.0 / (double)frequency.QuadPart;
	}

	HMODULE GameModule()
	{
		TCHAR szFileName[MAX_PATH + 1];
		if (GetModuleFileName(NULL, szFileName, MAX_PATH + 1) == 0)
			return nullptr;

		return GetModuleHandle(szFileName);
	}
}

DWORD HookManager::FindPattern(const char* label, const char* pattern, const char* mask)
{
	const HMODULE hModule = GameModule();
	if (hModule == nullptr)
		return 0;

	LARGE_INTEGER start;
	QueryPerformanceCounter(&start);

	if (!g_cacheInitialised)
	{
		g_cacheInitialised = true;
		g_moduleKey = HookAddrCache_ModuleKey(hModule);
		HookAddrCache_Load(g_moduleKey);
	}

	const unsigned char* const base = (const unsigned char*)hModule;
	const ScanRange whole = PatternScan_WholeImage(hModule);
	if (whole.base == nullptr || whole.size == 0)
		return 0;

	g_scanStats.scans++;

	const unsigned char* found = nullptr;
	bool fromCache = false;
	bool fromFullImage = false;

	// A cached address is only ever a hint. It is used solely when the signature still matches
	// at it, which is the same predicate a fresh scan satisfies - so a stale or tampered cache
	// costs a scan, it can never point a JMP at the wrong instruction.
	const DWORD cachedRva = HookAddrCache_Get(label);
	if (cachedRva != 0 && (size_t)cachedRva < whole.size &&
		PatternScan_MatchesAt(base + cachedRva, pattern, mask))
	{
		found = base + cachedRva;
		fromCache = true;
	}

	if (found == nullptr)
	{
		ScanRange ranges[PATTERN_SCAN_MAX_RANGES];
		const int rangeCount = PatternScan_ExecutableRanges(hModule, ranges, PATTERN_SCAN_MAX_RANGES);

		for (int i = 0; i < rangeCount && found == nullptr; i++)
		{
			g_scanStats.bytesSwept += ranges[i].size;
			found = PatternScan_Find(ranges[i], pattern, mask);
		}

		// BBCF.exe puts its code first, so a hit in the executable sections is the same first
		// hit a whole-image sweep would have returned. The widened pass exists for the case
		// that assumption does not hold - a packer leaving code in a section that is not
		// marked executable, say - and the summary line reports whenever it was needed.
		if (found == nullptr)
		{
			g_scanStats.bytesSwept += whole.size;
			found = PatternScan_Find(whole, pattern, mask);
			fromFullImage = found != nullptr;
		}
	}

	LARGE_INTEGER end;
	QueryPerformanceCounter(&end);
	const double elapsedMs = QpcToMs(end.QuadPart - start.QuadPart);

	g_scanStats.totalMs += elapsedMs;
	if (elapsedMs > g_scanStats.worstMs)
	{
		g_scanStats.worstMs = elapsedMs;
		g_scanStats.worstLabel = label != nullptr ? label : "?";
	}

	if (found == nullptr)
	{
		g_scanStats.misses++;
		LOG(2, "[HookScan] %s NOT FOUND after %.1fms\n", label, elapsedMs);
		return 0;
	}

	if (fromCache)
		g_scanStats.cacheHits++;
	else if (fromFullImage)
		g_scanStats.fullImageHits++;
	else
		g_scanStats.execHits++;

	const DWORD rva = (DWORD)(found - base);
	HookAddrCache_Put(label, rva);

	LOG(2, "[HookScan] %s rva=0x%06X in %.2fms (%s)\n", label, rva, elapsedMs,
		fromCache ? "cached" : (fromFullImage ? "full-image scan" : "code scan"));

	// A single scan that runs long is worth seeing at the default log level - it is the shape
	// of the cold-boot complaint this instrumentation exists to answer.
	if (elapsedMs >= 10.0)
	{
		LOG(1, "[HookScan] slow scan: %s took %.1fms (%s)\n", label, elapsedMs,
			fromFullImage ? "full-image scan" : "code scan");
	}

	return (DWORD)(DWORD_PTR)found;
}

void HookManager::LogScanSummary(const char* phase)
{
	LOG(1, "[HookScan] %s: %d signatures in %.1fms (cached %d, code scan %d, full-image %d, missing %d), "
		"swept %.1fMB, worst %.1fms on '%s'\n",
		phase != nullptr ? phase : "hook placement",
		g_scanStats.scans, g_scanStats.totalMs,
		g_scanStats.cacheHits, g_scanStats.execHits, g_scanStats.fullImageHits, g_scanStats.misses,
		(double)g_scanStats.bytesSwept / (1024.0 * 1024.0),
		g_scanStats.worstMs, g_scanStats.worstLabel.empty() ? "-" : g_scanStats.worstLabel.c_str());

	if (g_scanStats.fullImageHits > 0)
	{
		LOG(1, "[HookScan] %d signature(s) were only found outside the executable sections; "
			"the code-section fast path did not cover them\n", g_scanStats.fullImageHits);
	}

	HookAddrCache_Save(g_moduleKey);
}
