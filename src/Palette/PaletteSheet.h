#pragma once
#include "impl_format.h"

#include <string>

// Exports a palette as a reference sheet: a picture of the character, drawn in the
// palette being exported, that you recolour in an image editor and import back.
//
// The sheets are HIKARI's, from their long-standing CFPL-PNG / PNG-CFPL converters,
// embedded in the DLL as pixel indices (see tools/build_palette_templates.py). They are
// laid out so every colour a character actually uses appears on a sprite, with a swatch
// block for the entries that are not visible on one - which is the part that makes a
// palette editable by eye, and the reason these are worth carrying rather than
// assembling something automatically.
//
// The mod substitutes the palette; only the indices are stored, so the same sheet serves
// every palette for that character.
namespace PaletteSheet
{
	// True when a sheet for this character is embedded in this build.
	bool IsAvailable(int charIndex);

	// Writes the sheet for `charIndex` to `outPath`, coloured with the palette's character
	// colours. The whole palette is taken, not just those colours, so the effect files and
	// metadata can be embedded too - that is what lets the PNG be re-imported without
	// losing anything. Returns false and fills outError with a user-facing message.
	bool Write(int charIndex, const IMPL_data_t& palette, const std::string& outPath,
		std::string& outError);
}
