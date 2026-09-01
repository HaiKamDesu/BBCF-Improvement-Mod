#include "PaletteSheet.h"

#include "PngPalette.h"
#include "impl_format.h"

#include "Core/EmbeddedResources.h"
#include "Core/logger.h"
#include "Game/characters.h"

#include <cstring>
#include <string>

namespace
{
	// Blob layout, written by tools/build_palette_templates.py:
	//   "BBPT", u32 version, u32 count, u32 width, u32 height,
	//   count * { u32 offset, u32 compressedSize },   offsets from the start of the blob
	//   the deflate streams themselves
	const char kBlobMagic[4] = { 'B', 'B', 'P', 'T' };
	const unsigned int kBlobVersion = 2;
	const size_t kHeaderSize = 4 + 4 * 4;
	const wchar_t* const kResourceName = L"palette_templates";

	unsigned int ReadU32(const unsigned char* p)
	{
		return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
			((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
	}

	// The blob is a couple of megabytes and every export needs it, so it is read out of
	// the resource once and kept. Resources are mapped by the loader, but LoadEmbeddedResource
	// hands back a std::string copy, and copying 2.4 MB per export would be silly.
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

	// Locates one sheet's compressed stream and the dimensions shared by all of them.
	bool FindSheet(int charIndex, const unsigned char*& outData, unsigned int& outSize,
		unsigned int& outWidth, unsigned int& outHeight)
	{
		const std::string* blob = Blob();
		if (!blob || blob->size() < kHeaderSize)
			return false;

		const unsigned char* base = (const unsigned char*)blob->data();
		if (memcmp(base, kBlobMagic, sizeof(kBlobMagic)) != 0)
			return false;
		if (ReadU32(base + 4) != kBlobVersion)
			return false;

		const unsigned int count = ReadU32(base + 8);
		outWidth = ReadU32(base + 12);
		outHeight = ReadU32(base + 16);
		if (charIndex < 0 || (unsigned int)charIndex >= count)
			return false;
		if (outWidth == 0 || outHeight == 0 || outWidth > 8192 || outHeight > 8192)
			return false;
		if (blob->size() < kHeaderSize + (size_t)count * 8)
			return false;

		const unsigned char* entry = base + kHeaderSize + (size_t)charIndex * 8;
		const unsigned int offset = ReadU32(entry);
		outSize = ReadU32(entry + 4);
		if (outSize == 0 || (size_t)offset + outSize > blob->size())
			return false;

		outData = base + offset;
		return true;
	}
}

namespace PaletteSheet
{
	bool IsAvailable(int charIndex)
	{
		const unsigned char* data = NULL;
		unsigned int size = 0, width = 0, height = 0;
		return FindSheet(charIndex, data, size, width, height);
	}

	bool Write(int charIndex, const IMPL_data_t& palette, const std::string& outPath,
		std::string& outError)
	{
		const char* paletteData = palette.file0;

		if (isCharacterIndexOutOfBound(charIndex))
		{
			outError = "unknown character";
			return false;
		}
		const unsigned char* compressed = NULL;
		unsigned int compressedSize = 0, width = 0, height = 0;
		if (!FindSheet(charIndex, compressed, compressedSize, width, height))
		{
			outError = "this build has no reference sheet for that character";
			return false;
		}

		// The stored stream IS the PNG's image data - the pixels never change, only the
		// palette does - so it goes straight into IDAT. Nothing is decompressed here.
		LOG(2, "PaletteSheet: %s -> %ux%u sheet\n",
			getCharacterNameByIndexA(charIndex).c_str(), width, height);

		return PngPalette::WriteIndexedPngPrecompressed(outPath, (int)width, (int)height,
			paletteData, compressed, compressedSize, outError, charIndex, &palette);
	}
}
