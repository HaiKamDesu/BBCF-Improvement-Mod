#pragma once

// Gives BBCF's two biggest transient buffers - the 100 MB per-match netplay arena and the
// 62 MB char_select_stage.pac load - one shared address range for the whole session, so a
// long online session cannot fragment the 2 GB address space until one of them throws
// std::bad_alloc. See NetplayArenaKeeper.cpp.
void InstallNetplayArenaKeeper();
