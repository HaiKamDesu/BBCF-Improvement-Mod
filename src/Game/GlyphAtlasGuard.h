#pragma once

// Guards the game's text glyph atlas against the staging texture it failed to create.
//
// The atlas (vtable 0x898C2C) keeps a 2048x2048 D3DPOOL_SYSTEMMEM staging texture at +0x3C
// and uploads glyphs through it (0x44C880). When the 32-bit address space has no 16 MB hole
// left, creating that texture fails, the game stores the null, and the next glyph upload
// reads address 0 - the crash on entering character select after a long session
// (BBCF.exe+0x4c90f).
//
// The patch makes a null staging texture first get recreated (giving up the netplay arena
// reserve if that is what it takes), and if that fails too, makes the upload fail the way a
// failed LockRect already does, so the text is missing for a moment instead of the game
// closing.
void InstallGlyphAtlasGuard();
