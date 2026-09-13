#include "ReplayPauseHud.h"

#include "Core/logger.h"
#include "Core/utils.h"
#include "Hooks/PatternScan.h"

#include <Windows.h>
#include <cstring>

namespace ReplayPauseHud
{
	namespace
	{
		// The four "is this display switched on" getters. All four are byte-identical apart from
		// the global they read and the relative call, so one signature finds all of them and the
		// scan simply keeps going after each hit:
		//
		//     cmp  dword ptr [<display on/off global>],0
		//     je   return_false
		//     call IsGamePaused                 ; 0xD1FC0
		//     test eax,eax
		//     jne  return_false                 ; <-- NOPed
		//     mov  eax,1
		//     ret
		//   return_false:
		//     xor  eax,eax
		//     ret
		//
		// The global is an absolute address, so it has to be wildcarded: the image is relocated
		// at runtime and the bytes in a hardcoded signature would no longer be there.
		const char* kGetterPattern =
			"\x83\x3D" "\x00\x00\x00\x00" "\x00"   // cmp [global],0
			"\x74\x0F"                             // je  return_false
			"\xE8" "\x00\x00\x00\x00"              // call IsGamePaused
			"\x85\xC0"                             // test eax,eax
			"\x75\x06"                             // jne return_false      <-- patched
			"\xB8\x01\x00\x00\x00"                 // mov eax,1
			"\xC3";                                // ret
		const char* kGetterMask = "xx" "????" "x" "xx" "x????" "xx" "xx" "xxxxx" "x";
		const int kGetterLength = 24;
		const int kGetterBranchOffset = 16;
		const int kGetterBranchLength = 2;   // jne rel8
		const int kGetterCount = 4;

		// The input-info panel. The `push` right after the branch is the resource name
		// "TRI_InputInfo", which is what identifies this site; its operand is an absolute
		// address and is wildcarded for the same reason as above.
		//
		//     call IsGamePaused
		//     test eax,eax
		//     jne  skip_the_panel               ; <-- NOPed
		//     push offset "TRI_InputInfo"
		//     lea  ecx,[ebp-24h]
		const char* kPanelPattern =
			"\xE8" "\x00\x00\x00\x00"              // call IsGamePaused
			"\x85\xC0"                             // test eax,eax
			"\x0F\x85" "\xAE\x00\x00\x00"          // jne skip_the_panel    <-- patched
			"\x68" "\x00\x00\x00\x00"              // push offset "TRI_InputInfo"
			"\x8D\x4D\xDC";                        // lea ecx,[ebp-24h]
		const char* kPanelMask = "x????" "xx" "xxxxxx" "x????" "xxx";
		const int kPanelLength = 21;
		const int kPanelBranchOffset = 7;
		const int kPanelBranchLength = 6;    // jne rel32

		// The replay manager singleton and the playback-state field inside it, as RVAs. The
		// getter the game uses is `mov eax,[ecx+64EE8h-8]; ret` at 0x69D370; this reads the same
		// field directly rather than calling into the game.
		const uintptr_t kReplayManagerRva = 0x115B470;
		const uintptr_t kPlaybackStateOffset = 0x64EE0;

		const int kMaxSites = 8;

		struct Site
		{
			unsigned char* addr = nullptr;
			int length = 0;
			unsigned char original[8] = {};
		};

		Site g_sites[kMaxSites];
		int g_siteCount = 0;
		bool g_enabled = false;

		void AddSite(const unsigned char* branch, int length)
		{
			if (g_siteCount >= kMaxSites) { return; }

			Site& site = g_sites[g_siteCount];
			site.addr = const_cast<unsigned char*>(branch);
			site.length = length;
			memcpy(site.original, branch, length);
			g_siteCount++;
		}

		// Every match in the range, not just the first - the four getters are the same shape and
		// a single-match scan would patch one of them and silently leave three displays hidden.
		int FindAll(const char* pattern, const char* mask, int patternLength, int branchOffset,
			int branchLength, int expected, const char* label)
		{
			ScanRange ranges[PATTERN_SCAN_MAX_RANGES];
			const int rangeCount =
				PatternScan_ExecutableRanges(GetModuleHandleA(NULL), ranges, PATTERN_SCAN_MAX_RANGES);

			int found = 0;
			for (int r = 0; r < rangeCount; r++)
			{
				ScanRange range = ranges[r];
				while (range.size >= (size_t)patternLength)
				{
					const unsigned char* hit = PatternScan_Find(range, pattern, mask);
					if (!hit) { break; }

					AddSite(hit + branchOffset, branchLength);
					found++;

					const size_t consumed = (size_t)(hit - range.base) + 1;
					range.base += consumed;
					range.size -= consumed;
				}
			}

			if (found != expected)
			{
				LOG(2, "[ReplayPauseHud] %s: found %d site(s), expected %d\n", label, found, expected);
			}
			return found;
		}

		bool Write(const Site& site, const unsigned char* bytes)
		{
			DWORD oldProtect = 0;
			if (!VirtualProtect(site.addr, site.length, PAGE_EXECUTE_READWRITE, &oldProtect))
			{
				LOG(2, "[ReplayPauseHud] VirtualProtect failed at 0x%08X (%lu)\n",
					(unsigned)(uintptr_t)site.addr, GetLastError());
				return false;
			}

			memcpy(site.addr, bytes, site.length);

			DWORD ignored = 0;
			VirtualProtect(site.addr, site.length, oldProtect, &ignored);
			FlushInstructionCache(GetCurrentProcess(), site.addr, site.length);
			return true;
		}
	}

	void Locate()
	{
		if (g_siteCount) { return; }

		// The branch to NOP is 2 bytes in the getters (jne rel8) and 6 in the panel (jne rel32),
		// which is why each site carries its own length rather than assuming one.
		FindAll(kGetterPattern, kGetterMask, kGetterLength, kGetterBranchOffset,
			kGetterBranchLength, kGetterCount, "display getters");
		FindAll(kPanelPattern, kPanelMask, kPanelLength, kPanelBranchOffset,
			kPanelBranchLength, 1, "input-info panel");

		if (!g_siteCount)
		{
			LOG(2, "[ReplayPauseHud] found none of the display branches; the option will do "
			       "nothing this session\n");
			return;
		}

		for (int i = 0; i < g_siteCount; i++)
		{
			LOG(2, "[ReplayPauseHud] display branch %d at 0x%08X (%d bytes)\n",
				i, (unsigned)(uintptr_t)g_sites[i].addr, g_sites[i].length);
		}
	}

	void SetEnabled(bool enabled)
	{
		if (!g_siteCount || enabled == g_enabled) { return; }

		const unsigned char nops[8] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

		for (int i = 0; i < g_siteCount; i++)
		{
			Write(g_sites[i], enabled ? nops : g_sites[i].original);
		}

		g_enabled = enabled;
		LOG(2, "[ReplayPauseHud] keeping the input display up while a replay is paused: %s\n",
			enabled ? "ON" : "off");
	}

	int PlaybackState()
	{
		const uintptr_t base = (uintptr_t)GetBbcfBaseAdress();
		if (!base) { return -1; }

		const int state = *(const int*)(base + kReplayManagerRva + kPlaybackStateOffset);

		// The field only ever holds 0..3. Anything else means this is not the replay manager -
		// a different game build, most likely - and reporting it as a pause state would be a lie.
		if (state < 0 || state > 3) { return -1; }
		return state;
	}
}
