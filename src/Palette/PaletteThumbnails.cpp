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

	// A grid shows roughly two dozen at once; this leaves room to scroll a screenful
	// either way without rebuilding, and caps the cache at about 6 MB.
	const size_t kMaxCachedTextures = 48;

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
		int charIndex;
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
			return (ImTextureID)hit->second.texture;
		}

		IDirect3DTexture9* texture = BuildTexture(*sprite, paletteData);
		if (!texture)
			return NULL;

		while (g_cache.size() >= kMaxCachedTextures && !g_recency.empty())
		{
			std::map<CacheKey, Entry>::iterator oldest = g_cache.find(g_recency.back());
			if (oldest == g_cache.end())
			{
				g_recency.pop_back();
				continue;
			}
			Evict(oldest);
		}

		g_recency.push_front(cacheKey);
		Entry entry;
		entry.texture = texture;
		entry.recency = g_recency.begin();
		g_cache[cacheKey] = entry;

		return (ImTextureID)texture;
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
	}

	void Shutdown()
	{
		ReleaseAll();
		g_device = NULL;
	}
}
