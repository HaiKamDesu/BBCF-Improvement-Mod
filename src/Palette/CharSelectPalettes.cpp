#include "CharSelectPalettes.h"

#include "PaletteManager.h"
#include "impl_format.h"

#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Game/characters.h"

#include <cstring>
#include <string>

namespace
{
	// An HPAL entry is a 32-byte header then IMPL_PALETTE_DATALEN bytes of BGRA, which is
	// exactly the shape of a custom palette's file0. That identity is the whole trick.
	const size_t kHpalHeaderSize = 32;

	// The mod maps 24 colour slots, which is also how many the bake loops over.
	const int kSlotCount = 24;

	// Handed back to the game in place of its own entry. The baker copies out of this
	// within a few instructions, so a single buffer serves every substitution and never
	// has to outlive the call.
	char g_scratch[kHpalHeaderSize + IMPL_PALETTE_DATALEN];

	int g_substituted = 0;

	// Resolves the custom palette assigned to (character, colour), or null.
	const IMPL_data_t* AssignedPalette(int charIndex, int colourIndex)
	{
		PaletteManager* manager = g_interfaces.pPaletteManager;
		if (!manager)
			return NULL;

		const std::vector<std::vector<std::string> >& slots = manager->GetPaletteSlots();
		if (charIndex < 0 || (size_t)charIndex >= slots.size())
			return NULL;
		if (colourIndex < 0 || (size_t)colourIndex >= slots[charIndex].size())
			return NULL;

		const std::string& name = slots[charIndex][colourIndex];
		if (name.empty())
			return NULL;

		// "Random" and friends are not resolved until a match starts, so there is nothing
		// stable to show here; leave those colours as the game's own.
		const int palIndex = manager->FindCustomPalIndex((CharIndex)charIndex, name.c_str());
		if (palIndex < 0)
			return NULL;

		return manager->GetCustomPalData((CharIndex)charIndex, palIndex);
	}
}

namespace CharSelectPalettes
{
	const char* SubstituteEntry(int charIndex, int colourIndex, const char* sourceEntry)
	{
		if (!sourceEntry)
			return sourceEntry;
		if (!Settings::settingsIni.customPalettesInCharSelect)
			return sourceEntry;
		if (isCharacterIndexOutOfBound(charIndex) || colourIndex < 0 || colourIndex >= kSlotCount)
			return sourceEntry;

		const IMPL_data_t* palette = AssignedPalette(charIndex, colourIndex);
		if (!palette)
			return sourceEntry;

		// Keep the game's own header - it carries the entry's type and length, and the
		// baker validates it - and swap only the colours.
		memcpy(g_scratch, sourceEntry, kHpalHeaderSize);
		memcpy(g_scratch + kHpalHeaderSize, palette->file0, IMPL_PALETTE_DATALEN);

		g_substituted++;
		return g_scratch;
	}

	void OnLeaveCharacterSelect()
	{
		if (g_substituted > 0)
			LOG(2, "CharSelectPalettes: %d colour(s) substituted on that visit\n", g_substituted);
		g_substituted = 0;
	}
}
