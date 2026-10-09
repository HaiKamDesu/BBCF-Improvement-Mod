#pragma once

// Keeps 100 MB of the 32-bit address space in reserve from boot and gives it up only when
// BBCF's two biggest transient buffers - the 100 MB per-match netplay arena and the 62 MB
// char_select_stage.pac load - would otherwise fail to fit, which is a std::bad_alloc that
// closes the game. See NetplayArenaKeeper.cpp.
void InstallNetplayArenaKeeper();
// Re-reserves after a release once a 100 MB hole exists again. Call once per rendered frame;
// it does nothing most frames.
void TickNetplayArenaKeeper();
// Gives the reserve up for something else that did not fit - a texture the game cannot
// create otherwise. Returns false when no reserve is held. The 5 s tick takes it back once
// a big enough hole exists again.
bool ReleaseNetplayArenaReserve(const char* forWhat);
