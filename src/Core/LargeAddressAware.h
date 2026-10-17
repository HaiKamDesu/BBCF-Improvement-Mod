#pragma once

// Running BBCF with the large-address-aware bit (4 GB of address space under WOW64 instead of 2).
//
// The game's own code was checked for it (docs/Decompilation/Memory.md). The one thing that
// breaks is D3DX9: unless an effect is created with D3DXFX_LARGEADDRESSAWARE, D3DX uses the top
// bit of an address to tell handles from string names, so once its memory lands above 2 GB every
// D3DXCreateEffect fails with E_OUTOFMEMORY, the game keeps the null effect and the next draw
// crashes (BBCF.exe+0x4972C). The flag's one condition, that no string is passed where a handle is
// expected, holds: the game only ever passes handles it got back from GetParameterBySemantic and
// GetTechniqueByName (FUN_004497B0, FUN_00449AE0).

namespace LargeAddressAware
{
	// Whether the running exe carries IMAGE_FILE_LARGE_ADDRESS_AWARE.
	bool IsProcessLargeAddressAware();

	// From DllMain, before WinMain: on a large-address-aware exe, points BBCF.exe's import of
	// D3DXCreateEffect at a shim that adds D3DXFX_LARGEADDRESSAWARE. Does nothing otherwise, so a
	// stock exe creates its effects exactly as vanilla.
	void InstallD3DXEffectFlag();

	// Once the log is open: what InstallD3DXEffectFlag did.
	void LogStatus();
}
