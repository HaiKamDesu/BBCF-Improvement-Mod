#pragma once
#include "impl_format.h"

#include <string>

// PNG palette interchange, matching the convention the UNI2 Improvement Mod uses so
// palettes travel between the two mods (and through unPAC's palette.png) unchanged:
// the colors live in the PNG's PLTE chunk, not in the pixel grid, and index 0 is the
// transparency slot and is left alone on import.
//
// BBCF palettes are BGRA (a little-endian uint32 reads 0xAARRGGBB) and the editor
// exposes alpha, so unlike UNI2 we do not throw alpha away: PLTE is RGB-only, so the
// alpha channel rides along in the standard tRNS chunk and is read back from it.
namespace PngPalette
{
	// Everything an imported PNG turned out to carry.
	//
	// A PNG's PLTE only holds the 1024 bytes of character colours. The seven effect
	// palettes and the creator/description/bloom metadata are carried separately, in a
	// private PNG chunk the export writes (see kExtrasChunk in the .cpp). That chunk is
	// marked ancillary and safe-to-copy, so a well-behaved editor preserves it across a
	// palette edit - but plenty of editors rewrite a file from scratch and drop chunks
	// they do not know, so `hasExtras` is false often enough that callers must handle it.
	struct Imported
	{
		char characterFile[IMPL_PALETTE_DATALEN];   // always filled
		int  charIndex = -1;                        // -1 when the PNG does not say
		bool hasExtras = false;                     // the rest of this struct is valid
		IMPL_info_t info;
		char effects[IMPL_PALETTE_FILES_COUNT - 1][IMPL_PALETTE_DATALEN]; // files 1..7
	};

	// Reads everything a PNG carries. Returns false and fills outError on failure.
	bool ReadPaletteFileEx(const std::string& path, Imported& out, std::string& outError);

	// Reads the first PLTE chunk of an indexed PNG into IMPL_PALETTE_DATALEN bytes of
	// BGRA palette data. Entries 1..255 come from the file; entry 0 is left fully
	// transparent. Alpha comes from tRNS when present, otherwise it is opaque.
	// Returns false and fills outError with a user-facing message on failure.
	bool ReadPaletteFile(const std::string& path, char* outPaletteData, std::string& outError);

	// As above, and additionally reports the character the PNG was exported for when it
	// carries our tEXt marker (see WriteIndexedPng). Sets *outCharIndex to -1 when the
	// marker is absent - a PNG from any other tool, or one an editor stripped it from -
	// in which case the caller has to ask the user which character it is for.
	bool ReadPaletteFileWithCharacter(const std::string& path, char* outPaletteData,
		int* outCharIndex, std::string& outError);

	// Writes a 16x16 8-bit indexed PNG (one pixel per color, row-major, index 0
	// top-left) whose PLTE/tRNS chunks carry the 256 palette entries. This is the
	// fallback export; HipReference paints the palette onto the character's own sprite.
	bool WritePaletteFile(const std::string& path, const char* paletteData, std::string& outError,
		int charIndex = -1, const IMPL_data_t* extras = nullptr);

	// Writes an arbitrary 8-bit indexed PNG: `pixels` is width*height palette indices,
	// `paletteData` is IMPL_PALETTE_DATALEN bytes of BGRA. Index 0 is always written
	// fully transparent, since that is BBCF's transparency slot.
	// `charIndex` >= 0 stamps a tEXt chunk naming the character, so re-importing the file
	// needs no prompt. Pass -1 to omit it. `extras`, when given, is the full palette data
	// whose effect files and metadata get embedded so an import can be lossless.
	bool WriteIndexedPng(const std::string& path, int width, int height,
		const char* paletteData, const unsigned char* pixels, std::string& outError,
		int charIndex = -1, const IMPL_data_t* extras = nullptr);

	// As above, but `imageStream` is an already-deflated PNG image stream (filtered
	// scanlines) that is copied straight into IDAT. The mod has a zlib decompressor but
	// no compressor, so anything it deflates itself has to use stored blocks; letting a
	// caller supply a stream prepared ahead of time is what keeps exported sheets a
	// sensible size. See tools/build_palette_templates.py.
	bool WriteIndexedPngPrecompressed(const std::string& path, int width, int height,
		const char* paletteData, const unsigned char* imageStream, size_t imageStreamLength,
		std::string& outError, int charIndex = -1, const IMPL_data_t* extras = nullptr);
}
