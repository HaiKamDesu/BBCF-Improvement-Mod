#pragma once

// Keeps 100 MB of the 32-bit address space in reserve from boot and gives it up only when
// something would otherwise fail to fit - BBCF's two biggest transient buffers (the 100 MB
// per-match netplay arena and the 62 MB char_select_stage.pac load), or any other operator new,
// whose failure is a std::bad_alloc that closes the game. See NetplayArenaKeeper.cpp.
void InstallNetplayArenaKeeper();
// Re-reserves after a release once a 100 MB hole exists again. Call once per rendered frame;
// it does nothing most frames.
void TickNetplayArenaKeeper();
// Gives address space up for something else that did not fit - a texture the game cannot
// create otherwise: the kept arena from the last match if there is one, else the reserve.
// Returns false when neither is held. The 5 s tick takes the reserve back once a big enough
// hole exists again.
// On a large-address-aware exe the upper 2 GB (HighMemoryReserve) goes first.
bool ReleaseNetplayArenaReserve(const char* forWhat);
// Test hook for LaaStress: one block from BBCF's own operator new (so its out-of-memory path
// runs) and back through its delete. Returns where it landed; throws as the game's new does.
void* ProbeGameOperatorNew(size_t size);
