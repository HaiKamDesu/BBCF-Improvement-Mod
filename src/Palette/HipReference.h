#pragma once
#include <string>

// Builds a palette reference sheet out of the player's own game install.
//
// Exporting a palette as a bare 16x16 grid of swatches is correct but miserable to
// edit: you are recolouring numbered squares with no idea which one is the coat and
// which one is the hair. This reads one of the character's actual sprites out of
// data/Char/char_<tag>_img.pac, paints it in the palette being exported, and writes
// that as an indexed PNG. Recolour it in an indexed-mode editor and import it back.
//
// Nothing is shipped or embedded to make this work - the art comes from the player's
// own installed game, so the DLL stays small and no game asset is redistributed.
// Everything here is read-only; no game file is ever written.
namespace HipReference
{
	// True when this character's sprite archive is present and looks readable. Cheap:
	// it only stats the file, so it is safe to call while drawing a frame.
	bool IsAvailable(int charIndex);

	// Writes the reference sheet for `charIndex` to `outPath`, coloured with
	// `paletteData` (IMPL_PALETTE_DATALEN bytes of BGRA, i.e. a palette's file0).
	//
	// Synchronous and not cheap: it reads a 5-12 MB archive, inflates it to 20-40 MB,
	// and scans every sprite in it. Measured at 300-400 ms per character, so it is fine
	// as a one-shot on an export the user just asked for - it lands right after a file
	// dialog, where a brief pause reads as saving - but it must not be called per frame.
	// Returns false and fills outError with a user-facing message; callers should fall
	// back to PngPalette::WritePaletteFile so an export always produces something.
	bool WriteReferenceSheet(int charIndex, const char* paletteData,
		const std::string& outPath, std::string& outError);
}
