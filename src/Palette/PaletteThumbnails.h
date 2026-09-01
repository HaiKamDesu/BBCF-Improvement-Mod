#pragma once
#include "imgui.h"

#include <string>

// Sprite thumbnails for the palette grid.
//
// One idle sprite is embedded per character as palette indices (see
// tools/build_palette_thumbnails.py). Every palette of that character draws the same
// sprite; only the 256 colours differ. So a thumbnail is made by running the palette
// over those indices and uploading the result as a texture.
//
// Textures are built on demand and kept in a small LRU cache, because a player with a
// few hundred palettes would otherwise be asking for a few hundred textures at roughly
// 125 KB each - far past what a 32-bit process should be holding for a preview grid.
// Only what the grid actually draws gets built.
struct IDirect3DDevice9;

namespace PaletteThumbnails
{
	// Hands over the D3D9 device textures are created on. Called once the ImGui DX9
	// backend is up, which is what proves the device is usable.
	void Initialize(IDirect3DDevice9* device);

	// True when this build has a sprite for the character.
	bool IsAvailable(int charIndex);

	// Texture for `charIndex` drawn in `paletteData` (IMPL_PALETTE_DATALEN bytes of
	// BGRA, i.e. a palette's file0), sized via outWidth/outHeight. `key` identifies the
	// palette for caching and must be unique per character+palette - the palette's file
	// name is what callers use. Returns nullptr when there is no sprite, no device, or
	// the texture could not be created; callers draw their own placeholder.
	//
	// Only call while drawing: entries touched this frame are the ones kept.
	ImTextureID Get(int charIndex, const std::string& key, const char* paletteData,
		int* outWidth, int* outHeight);

	// Drops a single palette's texture, for when its colours change underneath us.
	void Invalidate(int charIndex, const std::string& key);

	// Releases every texture. Must run before a D3D9 device reset - these live in
	// D3DPOOL_DEFAULT and a reset invalidates them - and again on shutdown.
	void ReleaseAll();

	// ReleaseAll plus forgetting the device, for shutdown.
	void Shutdown();
}
