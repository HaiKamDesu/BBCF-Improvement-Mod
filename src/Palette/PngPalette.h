#pragma once
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
	// Reads the first PLTE chunk of an indexed PNG into IMPL_PALETTE_DATALEN bytes of
	// BGRA palette data. Entries 1..255 come from the file; entry 0 is left fully
	// transparent. Alpha comes from tRNS when present, otherwise it is opaque.
	// Returns false and fills outError with a user-facing message on failure.
	bool ReadPaletteFile(const std::string& path, char* outPaletteData, std::string& outError);

	// Writes a 16x16 8-bit indexed PNG (one pixel per color, row-major, index 0
	// top-left) whose PLTE/tRNS chunks carry the 256 palette entries.
	bool WritePaletteFile(const std::string& path, const char* paletteData, std::string& outError);
}
