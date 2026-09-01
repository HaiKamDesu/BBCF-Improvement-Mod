#pragma once

// Puts custom palettes on the character select screen's colour preview.
//
// The preview is not the big artwork - that is true-colour and can never be recoloured.
// It is indexed "dot art", and the palettes it draws with come from the ordinary gameplay
// archives char_<tag>_pal.pac, the same data a custom palette's file0 replaces. (It does
// NOT come from paldata.pac inside char_select_dot.pac, which character select never
// reads at all - verified by disassembly, and the reason an earlier attempt at this by
// substituting that file did nothing.)
//
// Character select bakes every character's 24 colours once, on entry, copying each
// palette out of the archive into its own buffers. So the substitution happens on that
// read: the hook below hands the baker our bytes instead of the game's.
namespace CharSelectPalettes
{
	// Called from the CSS palette-bake hook for each (character, colour) the game reads.
	// `sourceEntry` points at the game's HPAL entry - a 32-byte header then 1024 bytes of
	// palette. Returns `sourceEntry` unchanged when that colour has no custom palette, or
	// a scratch buffer holding the game's own header with our palette bytes.
	//
	// The returned buffer only has to survive until the baker copies out of it, which is
	// the next few instructions, so one static scratch is enough.
	const char* SubstituteEntry(int charIndex, int colourIndex, const char* sourceEntry);

	// Forgets the buffers captured during the last bake. Called when leaving character
	// select, since they belong to that screen's objects.
	void OnLeaveCharacterSelect();
}
