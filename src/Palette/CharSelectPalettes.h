#pragma once
#include <string>

// Makes the character select screen's colour preview show custom palettes.
//
// The preview on CSS is not the big artwork - that is true-colour and can never be
// recoloured. It is a separate indexed "dot art" sprite from data/ETC/char_select_dot.pac,
// and the palettes inside that file (paldata.pac, 26 per character) are BYTE-IDENTICAL to
// the gameplay palettes in char_XX_pal.pac, i.e. exactly the data a custom palette's
// file0 replaces. Verified across all 903 comparable entries.
//
// So rather than hunting for a runtime buffer, this writes a modified copy of that .pac
// and points the game's own resource filename table at it - the same technique BGM
// replacement uses, with no code hooks at all. Nothing the game shipped is modified: the
// copy lives in a mod-owned subfolder and the pointer is restored on shutdown.
namespace CharSelectPalettes
{
	// Rebuilds the copy from the current palettes.ini slot assignments and points the
	// game at it. Safe to call repeatedly; it rewrites only when the assignments have
	// actually changed. Does nothing if no slot has a custom palette.
	//
	// The game reads this resource when it loads the character select screen, so a
	// rebuild lands on the next visit to CSS rather than the current one.
	void Refresh();

	// Puts the shipped filename back. Called on shutdown.
	void Restore();

	// Why the last Refresh did nothing, for the UI. Empty when all is well.
	const std::string& LastError();

	// True while the game is pointed at our copy.
	bool IsActive();
}
