#include "ReplayPauseProbe.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"

#include <Windows.h>

#include <vector>

namespace
{
	// The whole writable data section, located from the module's own PE headers rather than from a
	// hand-computed address.
	//
	// The first version of this took an RVA worked out by hand and got it wrong - the image base was
	// subtracted twice, so it scanned a region 4 MB below the one its comment described. It happened
	// to land somewhere useful, which is luck, not method. Asking the headers removes the arithmetic
	// and with it that whole class of mistake, and scanning all of .data means no region has to be
	// guessed at in advance.
	struct Region { const unsigned char* base = nullptr; size_t size = 0; };

	Region FindDataSection()
	{
		Region out;
		char* base = GetBbcfBaseAdress();
		if (!base) { return out; }

		const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return out; }

		const IMAGE_NT_HEADERS* nt =
			reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) { return out; }

		const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
		{
			if (memcmp(sec->Name, ".data", 5) != 0) { continue; }
			out.base = reinterpret_cast<const unsigned char*>(base + sec->VirtualAddress);
			out.size = sec->Misc.VirtualSize;
			return out;
		}
		return out;
	}

	// Steady-state settling, in rendered frames, before a snapshot is trusted. A pause transition
	// has visual settling of its own, so sampling immediately would capture the transition rather
	// than the state.
	const int kSettleFrames = 45;

	enum class Stage { NeedFirstUnpaused, NeedSecondUnpaused, NeedPaused, Reported };

	Stage g_stage = Stage::NeedFirstUnpaused;
	Region g_region;
	std::vector<unsigned char> g_unpausedA;
	std::vector<unsigned char> g_unpausedB;
	std::vector<unsigned char> g_paused;
	int g_steadyFrames = 0;
	bool g_lastPaused = false;
	bool g_wasInTheater = false;

	void Capture(std::vector<unsigned char>& into)
	{
		if (!g_region.base) { return; }
		into.assign(g_region.base, g_region.base + g_region.size);
	}

	void Reset(const char* why)
	{
		g_stage = Stage::NeedFirstUnpaused;
		g_steadyFrames = 0;
		g_unpausedA.clear(); g_unpausedA.shrink_to_fit();
		g_unpausedB.clear(); g_unpausedB.shrink_to_fit();
		g_paused.clear();    g_paused.shrink_to_fit();
		g_region = FindDataSection();
		LOG(2, "[PauseProbe] armed (%s). Watching %zu KB of .data at 0x%08X. Let the replay run a "
		       "second, then pause and hold it.\n",
			why, g_region.size / 1024,
			(unsigned)(g_region.base ? (uintptr_t)g_region.base : 0));
	}
}

// The replay pause state, found by the differ: two adjacent dwords that read 0 while a replay runs
// and 1 and 2 while it is paused. They sit 12 and 8 bytes before PREINIT_OFFSET_FROM_BBCF_P1
// (0xDB6B2C), the script pointer ScrStateReader already uses, so they live in the same per-player
// battle struct.
//
// Nothing in the disassembly references them absolutely - but neither does anything reference those
// script pointers, which the mod reads successfully every session, so that says nothing either way.
// This region is reached through base registers only.
const uintptr_t kReplayPausedFlagA = 0x00DB6B20;
const uintptr_t kReplayPausedFlagB = 0x00DB6B24;

bool ReplayPauseProbe::IsReplayPaused()
{
	char* base = GetBbcfBaseAdress();
	if (!base) { return false; }
	return *reinterpret_cast<const int*>(base + kReplayPausedFlagA) != 0;
}

void ReplayPauseProbe::Update(bool inReplayTheater, bool paused)
{
	if (!inReplayTheater)
	{
		if (g_wasInTheater) { Reset("left Replay Theater"); }
		g_wasInTheater = false;
		return;
	}

	if (!g_wasInTheater)
	{
		g_wasInTheater = true;
		Reset("entered Replay Theater");
	}

	if (g_stage == Stage::Reported) { return; }

	g_steadyFrames = (paused == g_lastPaused) ? g_steadyFrames + 1 : 0;
	g_lastPaused = paused;
	if (g_steadyFrames < kSettleFrames) { return; }

	switch (g_stage)
	{
	case Stage::NeedFirstUnpaused:
		if (paused) { return; }
		Capture(g_unpausedA);
		g_stage = Stage::NeedSecondUnpaused;
		g_steadyFrames = 0;
		LOG(2, "[PauseProbe] captured running snapshot 1 of 2.\n");
		return;

	case Stage::NeedSecondUnpaused:
		if (paused)
		{
			// Paused before the churn baseline was complete; the diff would be unreadable
			// without it, so start over rather than report noise.
			Reset("paused too early, needed two running snapshots first");
			return;
		}
		Capture(g_unpausedB);
		g_stage = Stage::NeedPaused;
		g_steadyFrames = 0;
		{
			size_t churn = 0;
			for (size_t i = 0; i < g_region.size; i++)
			{
				if (g_unpausedA[i] != g_unpausedB[i]) { churn++; }
			}
			LOG(2, "[PauseProbe] captured running snapshot 2 of 2; %zu of %zu bytes change on "
			       "their own and will be ignored. Now pause and hold it.\n", churn, g_region.size);
		}
		return;

	case Stage::NeedPaused:
	{
		if (!paused) { return; }
		Capture(g_paused);
		g_stage = Stage::Reported;

		// Contiguous changed bytes are reported as one run. A dword going 0 -> 1 is four adjacent
		// byte differences; listing them individually buries the result in its own detail.
		int runs = 0;
		size_t suppressed = 0;
		const uintptr_t moduleBase = (uintptr_t)GetBbcfBaseAdress();

		LOG(2, "[PauseProbe] === regions that changed when paused, excluding normal churn ===\n");
		for (size_t i = 0; i < g_region.size; )
		{
			const bool changed = g_paused[i] != g_unpausedB[i] && g_unpausedA[i] == g_unpausedB[i];
			if (!changed) { i++; continue; }

			size_t end = i;
			while (end < g_region.size
				&& g_paused[end] != g_unpausedB[end]
				&& g_unpausedA[end] == g_unpausedB[end])
			{
				end++;
			}

			if (runs >= 40) { suppressed++; i = end; continue; }
			runs++;

			// Both the RVA (stable across launches, what the disassembly uses once the image base
			// is added) and the live address, so a hit can be chased either way.
			const uintptr_t live = (uintptr_t)(g_region.base + i);
			char before[40] = {};
			char after[40] = {};
			for (size_t k = i; k < end && k < i + 8; k++)
			{
				sprintf_s(before + strlen(before), sizeof(before) - strlen(before), "%02X ", g_unpausedB[k]);
				sprintf_s(after + strlen(after), sizeof(after) - strlen(after), "%02X ", g_paused[k]);
			}

			LOG(2, "[PauseProbe]   rva 0x%06X (live 0x%08X) %zu byte(s): running[ %s] paused[ %s]\n",
				(unsigned)(live - moduleBase), (unsigned)live, end - i, before, after);

			i = end;
		}

		if (runs == 0)
		{
			LOG(2, "[PauseProbe] nothing outside normal churn changed anywhere in .data.\n");
		}
		else if (suppressed)
		{
			LOG(2, "[PauseProbe] ... and %zu more run(s), not listed.\n", suppressed);
		}

		// The snapshots are tens of megabytes; there is no reason to hold them after reporting.
		g_unpausedA.clear(); g_unpausedA.shrink_to_fit();
		g_unpausedB.clear(); g_unpausedB.shrink_to_fit();
		g_paused.clear();    g_paused.shrink_to_fit();

		LOG(2, "[PauseProbe] === end ===\n");
		return;
	}

	default:
		return;
	}
}
