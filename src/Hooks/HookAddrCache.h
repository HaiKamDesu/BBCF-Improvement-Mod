#pragma once
#include <Windows.h>

// Remembers where each hook's signature was found last launch, as an RVA, so a launch does
// not have to sweep BBCF.exe once per hook to find out.
//
// The safety property that makes this worth doing: a cached address is NEVER trusted on its
// own. HookManager re-checks that the signature actually matches at that address before using
// it, and falls back to a full scan when it does not. Combined with a cache key that covers
// the exe's identity, a verified hit is provably the same address a fresh scan would have
// returned - the image is byte-identical to when the address was recorded, and the scan that
// recorded it returned the first match. A stale, corrupt or hand-edited cache can therefore
// only cost time, never send a JMP somewhere wrong.

// Loads BBCF_IM\HookAddrCache.bin. 'expectedKey' is the caller's exe fingerprint; entries are
// dropped wholesale if the stored key differs. Safe to call more than once.
void HookAddrCache_Load(unsigned long long expectedKey);

// RVA remembered for 'label', or 0 if there is none. Still has to be verified by the caller.
DWORD HookAddrCache_Get(const char* label);

// Records where a scan actually found 'label'. Nothing is written to disk until Save().
void HookAddrCache_Put(const char* label, DWORD rva);

// Writes the cache out if anything changed since it was loaded. Temp file + atomic replace.
void HookAddrCache_Save(unsigned long long key);

// Fingerprint of the running BBCF.exe: on-disk size and last-write time, plus the mapped
// image's SizeOfImage, PE timestamp, checksum and entry point. Any game update moves it.
unsigned long long HookAddrCache_ModuleKey(HMODULE module);
