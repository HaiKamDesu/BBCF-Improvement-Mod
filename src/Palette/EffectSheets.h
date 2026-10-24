#pragma once

#include "PaletteThumbnails.h"

#include <string>
#include <vector>

// Pictures to edit a palette's effect files on.
//
// A palette's files 1-7 colour the character's effects, and none of those appear on the
// reference sheet. Which effect sprite the game draws with which file is decided by the
// character's scripts (setPalette) and per-image overrides in the .jonbin data; that is
// worked out offline by tools/build_effect_palette_map.py and embedded as a small map.
// The sprites themselves come from the player's own game files: char_XX_img.pac and
// char_XX_vri.pac are streamed through the inflater and only the listed images are kept,
// so a 30 MB archive never sits in memory whole.
//
// Loading runs on a worker thread, one character at a time. Each effect file then gets
// its own sheet: every sprite drawn with that file, cropped, shrunk to a common size and
// laid out in rows, as palette indices - so the editor can map a click back to an entry
// exactly the way it does on the character's reference sheet.
namespace EffectSheets
{
	enum Status
	{
		Status_Idle,     // nothing requested for this character
		Status_Loading,
		Status_Ready,
		Status_Failed,
	};

	// How many images the map lists for this character and file (1-7), without loading
	// anything. 0 means the game never draws anything with that file for this character.
	int ImageCount(int charIndex, int file);

	// The entries of file `file` (1-7) the character's scripts read single colours from,
	// ascending: 3D models and particles are tinted with one palette colour instead of
	// being drawn from it (setParticleColorFromPalette and the like). Mai's file 2 is only
	// that - entry 1 is the glow of her charged spear - so a file can matter with no image.
	const std::vector<unsigned char>& ColoursRead(int charIndex, int file);

	// Whether the game uses file `file` (1-7) of this character at all: drawn on a sprite,
	// or read as a colour.
	bool FileIsUsed(int charIndex, int file);

	// Starts loading the character's effect sprites if that has not happened yet, and
	// reports how it is going. Asking for a different character drops the previous one.
	Status Request(int charIndex, std::string* error = nullptr);

	// The sheet for `file` (1-7) of a loaded character, as the palette entry that decides
	// each pixel (0 where nothing is drawn): what a click there should select. For most
	// sprites that is simply the pixel's own index. Sprites drawn with a palette recipe
	// (3040) never show their own entries - the game rebuilds their colours from two or
	// three specific entries - so for those it is the entry that contributes most.
	// Composed on first request and kept until another file or character is asked for.
	bool GetSheet(int charIndex, int file, const unsigned char** indices, int* width, int* height);

	// The same sheet as the sprites' own palette indices, for exporting it as an indexed
	// PNG that recolours in any image editor.
	bool GetRawSheet(int charIndex, int file, const unsigned char** indices, int* width, int* height);

	// The composed sheet as BBCF draws it with `paletteData` (a file's 1024 BGRA bytes):
	// each sprite's recipe and blend mode (additive, subtractive...) applied over
	// `background` (0xRRGGBB). Pixels whose deciding entries are not in `highlightMask`
	// are faded per `fade`; nullptr fades nothing. Returns width*height 0xAARRGGBB pixels,
	// or nullptr before GetSheet() has composed one. `changed` says whether they differ
	// from the last call, so the caller only re-uploads when they do.
	const unsigned int* Render(const char* paletteData, const unsigned char* highlightMask,
		const PaletteThumbnails::SheetFade& fade, unsigned int background, bool* changed);

	// Frees everything that has been loaded, once nothing is using it. Safe to call while
	// a load is still running; its result is simply dropped.
	void Release();
}
