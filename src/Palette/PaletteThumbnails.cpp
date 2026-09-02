#include "PaletteThumbnails.h"

#include "impl_format.h"

#include "Core/EmbeddedResources.h"
#include "Core/logger.h"

#include <Windows.h>
#include <d3d9.h>

#include <cstring>
#include <list>
#include <map>
#include <vector>

#include "stb_image.h"

namespace
{
	// Blob layout, written by tools/build_palette_thumbnails.py:
	//   "BBPB", u32 version, u32 count,
	//   count * { u32 width, u32 height, u32 offset, u32 compressedSize }
	//   the deflate streams themselves
	const char kBlobMagic[4] = { 'B', 'B', 'P', 'B' };
	const unsigned int kBlobVersion = 1;
	const size_t kHeaderSize = 4 + 4 * 2;
	const wchar_t* const kResourceName = L"palette_thumbnails";

	// A grid shows a few dozen at once; this leaves room to scroll a screenful either way
	// without rebuilding, and caps the cache at about 12 MB. It is a soft cap: see the
	// eviction loop in Get(), which will not release a texture the current frame is still
	// going to draw.
	const size_t kMaxCachedTextures = 96;

	unsigned int ReadU32(const unsigned char* p)
	{
		return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
			((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
	}

	const std::string* Blob()
	{
		static std::string blob;
		static bool tried = false;
		if (!tried)
		{
			tried = true;
			if (!LoadEmbeddedResource(kResourceName, blob))
				blob.clear();
		}
		return blob.empty() ? NULL : &blob;
	}

	// Decoded indices for one character, kept because every palette of that character
	// reuses them; decoding once per character instead of once per palette is the
	// difference between a grid that scrolls and one that stutters.
	struct Sprite
	{
		int width = 0;
		int height = 0;
		std::vector<unsigned char> indices;
	};

	const Sprite* GetSprite(int charIndex)
	{
		static std::map<int, Sprite> cache;
		static std::map<int, bool> failed;

		std::map<int, Sprite>::const_iterator hit = cache.find(charIndex);
		if (hit != cache.end())
			return &hit->second;
		if (failed.count(charIndex))
			return NULL;

		const std::string* blob = Blob();
		if (!blob || blob->size() < kHeaderSize)
		{
			failed[charIndex] = true;
			return NULL;
		}

		const unsigned char* base = (const unsigned char*)blob->data();
		if (memcmp(base, kBlobMagic, sizeof(kBlobMagic)) != 0 ||
			ReadU32(base + 4) != kBlobVersion)
		{
			failed[charIndex] = true;
			return NULL;
		}

		const unsigned int count = ReadU32(base + 8);
		if (charIndex < 0 || (unsigned int)charIndex >= count ||
			blob->size() < kHeaderSize + (size_t)count * 16)
		{
			failed[charIndex] = true;
			return NULL;
		}

		const unsigned char* entry = base + kHeaderSize + (size_t)charIndex * 16;
		const unsigned int width = ReadU32(entry);
		const unsigned int height = ReadU32(entry + 4);
		const unsigned int offset = ReadU32(entry + 8);
		const unsigned int size = ReadU32(entry + 12);

		if (width == 0 || height == 0 || width > 1024 || height > 1024 ||
			size == 0 || (size_t)offset + size > blob->size())
		{
			failed[charIndex] = true;
			return NULL;
		}

		Sprite sprite;
		sprite.width = (int)width;
		sprite.height = (int)height;
		sprite.indices.resize((size_t)width * height);

		const int written = stbi_zlib_decode_buffer((char*)sprite.indices.data(),
			(int)sprite.indices.size(), (const char*)(base + offset), (int)size);
		if (written != (int)sprite.indices.size())
		{
			LOG(2, "PaletteThumbnails: sprite %d is damaged\n", charIndex);
			failed[charIndex] = true;
			return NULL;
		}

		return &(cache[charIndex] = sprite);
	}

	struct CacheKey
	{
		int charIndex = -1;
		std::string key;

		bool operator<(const CacheKey& other) const
		{
			if (charIndex != other.charIndex)
				return charIndex < other.charIndex;
			return key < other.key;
		}
	};

	struct Entry
	{
		IDirect3DTexture9* texture;
		std::list<CacheKey>::iterator recency; // position in g_recency, most recent at front
		int lastUsedFrame;                     // ImGui frame this was last handed out in
	};

	std::map<CacheKey, Entry> g_cache;
	std::list<CacheKey> g_recency;

	IDirect3DDevice9* g_device = NULL;

	IDirect3DDevice9* Device()
	{
		return g_device;
	}

	// Runs the palette over the sprite's indices. Index 0 is BBCF's transparency slot,
	// so it is written fully transparent whatever colour the palette holds there.
	IDirect3DTexture9* BuildTexture(const Sprite& sprite, const char* paletteData)
	{
		IDirect3DDevice9* device = Device();
		if (!device)
			return NULL;

		IDirect3DTexture9* texture = NULL;
		if (device->CreateTexture(sprite.width, sprite.height, 1, D3DUSAGE_DYNAMIC,
			D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, NULL) != D3D_OK)
		{
			return NULL;
		}

		D3DLOCKED_RECT locked;
		if (texture->LockRect(0, &locked, NULL, 0) != D3D_OK)
		{
			texture->Release();
			return NULL;
		}

		const unsigned char* palette = (const unsigned char*)paletteData;
		for (int y = 0; y < sprite.height; y++)
		{
			const unsigned char* src = &sprite.indices[(size_t)y * sprite.width];
			unsigned char* dst = (unsigned char*)locked.pBits + (size_t)y * locked.Pitch;
			for (int x = 0; x < sprite.width; x++)
			{
				const unsigned char index = src[x];
				if (index == 0)
				{
					*(unsigned int*)dst = 0;
				}
				else
				{
					// Palette entries are BGRA; D3DFMT_A8R8G8B8 wants BGRA in memory
					// too, so this is a straight copy apart from forcing opacity.
					const unsigned char* entry = palette + (size_t)index * 4;
					dst[0] = entry[0];
					dst[1] = entry[1];
					dst[2] = entry[2];
					dst[3] = 0xFF;
				}
				dst += 4;
			}
		}

		texture->UnlockRect(0);
		return texture;
	}

	// --- Full reference sheet, for the detail panel -------------------------------------
	// Same substitution, but over the big multi-pose sheet rather than the idle thumbnail.
	// The sheet blob stores a ready-made PNG image stream, so getting pixels back out of
	// it means inflating and dropping the per-scanline filter byte.
	const char kSheetBlobMagic[4] = { 'B', 'B', 'P', 'T' };
	const unsigned int kSheetBlobVersion = 2;
	const wchar_t* const kSheetResourceName = L"palette_templates";

	IDirect3DTexture9* g_sheetTexture = NULL;
	CacheKey g_sheetKey;
	int g_sheetWidth = 0;
	int g_sheetHeight = 0;

	const std::string* SheetBlob()
	{
		static std::string blob;
		static bool tried = false;
		if (!tried)
		{
			tried = true;
			if (!LoadEmbeddedResource(kSheetResourceName, blob))
				blob.clear();
		}
		return blob.empty() ? NULL : &blob;
	}

	bool DecodeSheet(int charIndex, std::vector<unsigned char>& outIndices,
		int& outWidth, int& outHeight)
	{
		const std::string* blob = SheetBlob();
		if (!blob || blob->size() < 20)
			return false;

		const unsigned char* base = (const unsigned char*)blob->data();
		if (memcmp(base, kSheetBlobMagic, 4) != 0 || ReadU32(base + 4) != kSheetBlobVersion)
			return false;

		const unsigned int count = ReadU32(base + 8);
		const unsigned int width = ReadU32(base + 12);
		const unsigned int height = ReadU32(base + 16);
		if (charIndex < 0 || (unsigned int)charIndex >= count)
			return false;
		if (width == 0 || height == 0 || width > 4096 || height > 4096)
			return false;
		if (blob->size() < 20 + (size_t)count * 8)
			return false;

		const unsigned char* entry = base + 20 + (size_t)charIndex * 8;
		const unsigned int offset = ReadU32(entry);
		const unsigned int size = ReadU32(entry + 4);
		if (size == 0 || (size_t)offset + size > blob->size())
			return false;

		// Inflate to filtered scanlines, then drop the filter byte on each row. Every row
		// uses filter 0 (see the packer), so there is nothing to undo beyond that.
		std::vector<unsigned char> scanlines;
		try
		{
			scanlines.resize((size_t)height * (width + 1));
		}
		catch (const std::bad_alloc&)
		{
			return false;
		}

		if (stbi_zlib_decode_buffer((char*)scanlines.data(), (int)scanlines.size(),
			(const char*)(base + offset), (int)size) != (int)scanlines.size())
		{
			return false;
		}

		outIndices.resize((size_t)width * height);
		for (unsigned int y = 0; y < height; y++)
		{
			memcpy(&outIndices[(size_t)y * width],
				&scanlines[(size_t)y * (width + 1) + 1], width);
		}
		outWidth = (int)width;
		outHeight = (int)height;
		return true;
	}

	void Evict(std::map<CacheKey, Entry>::iterator it)
	{
		if (it->second.texture)
			it->second.texture->Release();
		g_recency.erase(it->second.recency);
		g_cache.erase(it);
	}
}

namespace PaletteThumbnails
{
	void Initialize(IDirect3DDevice9* device)
	{
		g_device = device;
	}

	bool IsAvailable(int charIndex)
	{
		return GetSprite(charIndex) != NULL;
	}

	ImTextureID Get(int charIndex, const std::string& key, const char* paletteData,
		int* outWidth, int* outHeight)
	{
		const Sprite* sprite = GetSprite(charIndex);
		if (!sprite || !paletteData)
			return NULL;

		if (outWidth) *outWidth = sprite->width;
		if (outHeight) *outHeight = sprite->height;

		CacheKey cacheKey;
		cacheKey.charIndex = charIndex;
		cacheKey.key = key;

		std::map<CacheKey, Entry>::iterator hit = g_cache.find(cacheKey);
		if (hit != g_cache.end())
		{
			// Touch: move to the front so the least recently drawn is what gets evicted.
			g_recency.splice(g_recency.begin(), g_recency, hit->second.recency);
			hit->second.recency = g_recency.begin();
			hit->second.lastUsedFrame = ImGui::GetFrameCount();
			return (ImTextureID)(uintptr_t)hit->second.texture;
		}

		IDirect3DTexture9* texture = BuildTexture(*sprite, paletteData);
		if (!texture)
			return NULL;

		// Evicting Release()es the texture, but a thumbnail drawn earlier in this same
		// frame is only recorded in ImGui's draw list so far - it is not bound until the
		// frame is rendered. Releasing it here would leave a dangling pointer in that
		// draw list and fault in the DX9 backend at render time. So the cap only applies
		// to entries from earlier frames: if a frame asks for more than
		// kMaxCachedTextures the cache overshoots for that frame and is trimmed on the
		// next one.
		const int frame = ImGui::GetFrameCount();
		while (g_cache.size() >= kMaxCachedTextures && !g_recency.empty())
		{
			std::map<CacheKey, Entry>::iterator oldest = g_cache.find(g_recency.back());
			if (oldest == g_cache.end())
			{
				g_recency.pop_back();
				continue;
			}
			if (oldest->second.lastUsedFrame == frame)
				break; // everything left is in this frame's draw list
			Evict(oldest);
		}

		g_recency.push_front(cacheKey);
		Entry entry;
		entry.texture = texture;
		entry.recency = g_recency.begin();
		entry.lastUsedFrame = frame;
		g_cache[cacheKey] = entry;

		return (ImTextureID)(uintptr_t)texture;
	}

	ImTextureID GetSheet(int charIndex, const std::string& key, const char* paletteData,
		int* outWidth, int* outHeight)
	{
		if (!paletteData)
			return 0;

		CacheKey wanted;
		wanted.charIndex = charIndex;
		wanted.key = key;

		if (g_sheetTexture && !(g_sheetKey < wanted) && !(wanted < g_sheetKey))
		{
			if (outWidth) *outWidth = g_sheetWidth;
			if (outHeight) *outHeight = g_sheetHeight;
			return (ImTextureID)(uintptr_t)g_sheetTexture;
		}

		std::vector<unsigned char> indices;
		int width = 0, height = 0;
		if (!DecodeSheet(charIndex, indices, width, height))
			return 0;

		Sprite sheet;
		sheet.width = width;
		sheet.height = height;
		sheet.indices.swap(indices);

		IDirect3DTexture9* texture = BuildTexture(sheet, paletteData);
		if (!texture)
			return 0;

		// One at a time: this is several megabytes as RGBA.
		if (g_sheetTexture)
			g_sheetTexture->Release();
		g_sheetTexture = texture;
		g_sheetKey = wanted;
		g_sheetWidth = width;
		g_sheetHeight = height;

		if (outWidth) *outWidth = width;
		if (outHeight) *outHeight = height;
		return (ImTextureID)(uintptr_t)texture;
	}

	void Invalidate(int charIndex, const std::string& key)
	{
		CacheKey cacheKey;
		cacheKey.charIndex = charIndex;
		cacheKey.key = key;

		std::map<CacheKey, Entry>::iterator hit = g_cache.find(cacheKey);
		if (hit != g_cache.end())
			Evict(hit);
	}

	void ReleaseAll()
	{
		for (std::map<CacheKey, Entry>::iterator it = g_cache.begin(); it != g_cache.end(); ++it)
		{
			if (it->second.texture)
				it->second.texture->Release();
		}
		g_cache.clear();
		g_recency.clear();

		if (g_sheetTexture)
		{
			g_sheetTexture->Release();
			g_sheetTexture = NULL;
		}
		g_sheetKey = CacheKey();
	}

	void Shutdown()
	{
		ReleaseAll();
		g_device = NULL;
	}
}
