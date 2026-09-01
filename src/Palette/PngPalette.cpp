#include "PngPalette.h"

#include "impl_format.h"

#include "Core/utils.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
	const unsigned char kPngSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

	// PLTE holds RGB triples only, so a full palette is 256 * 3 bytes.
	const int kPaletteEntryCount = IMPL_PALETTE_DATALEN / 4;
	const int kPlteLength = kPaletteEntryCount * 3;

	// Exported sheets are one pixel per color, laid out row-major.
	const int kSheetWidth = 16;
	const int kSheetHeight = 16;

	unsigned int ReadBE32(const unsigned char* p)
	{
		return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
			((unsigned int)p[2] << 8) | (unsigned int)p[3];
	}

	void AppendBE32(std::vector<unsigned char>& out, unsigned int value)
	{
		out.push_back((unsigned char)(value >> 24));
		out.push_back((unsigned char)(value >> 16));
		out.push_back((unsigned char)(value >> 8));
		out.push_back((unsigned char)value);
	}

	unsigned int Crc32(const unsigned char* data, size_t length)
	{
		static unsigned int table[256];
		static bool tableBuilt = false;

		if (!tableBuilt)
		{
			for (unsigned int n = 0; n < 256; n++)
			{
				unsigned int c = n;
				for (int k = 0; k < 8; k++)
					c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
				table[n] = c;
			}
			tableBuilt = true;
		}

		unsigned int c = 0xFFFFFFFFu;
		for (size_t i = 0; i < length; i++)
			c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
		return c ^ 0xFFFFFFFFu;
	}

	unsigned int Adler32(const unsigned char* data, size_t length)
	{
		unsigned int a = 1;
		unsigned int b = 0;
		for (size_t i = 0; i < length; i++)
		{
			a = (a + data[i]) % 65521;
			b = (b + a) % 65521;
		}
		return (b << 16) | a;
	}

	void AppendChunk(std::vector<unsigned char>& out, const char type[4],
		const unsigned char* data, size_t length)
	{
		AppendBE32(out, (unsigned int)length);

		const size_t crcStart = out.size();
		out.insert(out.end(), type, type + 4);
		if (length > 0)
			out.insert(out.end(), data, data + length);

		AppendBE32(out, Crc32(out.data() + crcStart, out.size() - crcStart));
	}

	bool ReadAllBytes(const std::string& path, std::vector<unsigned char>& out)
	{
		HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE)
			return false;

		const DWORD size = GetFileSize(hFile, NULL);
		if (size == INVALID_FILE_SIZE)
		{
			CloseHandle(hFile);
			return false;
		}

		out.resize(size);
		DWORD bytesRead = 0;
		const bool ok = size == 0 ||
			(ReadFile(hFile, out.data(), size, &bytesRead, NULL) && bytesRead == size);
		CloseHandle(hFile);
		return ok;
	}

	// Wraps raw bytes in a zlib stream made of stored (uncompressed) deflate blocks.
	// A 16x16 sheet is a few hundred bytes, so paying for a real compressor -- and a
	// dependency the project does not have -- would buy nothing.
	void ZlibStore(const std::vector<unsigned char>& raw, std::vector<unsigned char>& out)
	{
		out.push_back(0x78); // CMF: deflate, 32K window
		out.push_back(0x01); // FLG: no dictionary, fastest

		size_t offset = 0;
		do
		{
			const size_t blockLen = (std::min)(raw.size() - offset, (size_t)0xFFFF);
			const bool isLast = (offset + blockLen) >= raw.size();

			out.push_back(isLast ? 0x01 : 0x00);
			out.push_back((unsigned char)(blockLen & 0xFF));
			out.push_back((unsigned char)(blockLen >> 8));
			out.push_back((unsigned char)(~blockLen & 0xFF));
			out.push_back((unsigned char)((~blockLen >> 8) & 0xFF));
			out.insert(out.end(), raw.begin() + offset, raw.begin() + offset + blockLen);

			offset += blockLen;
		} while (offset < raw.size());

		AppendBE32(out, Adler32(raw.data(), raw.size()));
	}
}

namespace PngPalette
{
	bool ReadPaletteFile(const std::string& path, char* outPaletteData, std::string& outError)
	{
		std::vector<unsigned char> bytes;
		if (!ReadAllBytes(path, bytes))
		{
			outError = "unable to open the file";
			return false;
		}

		if (bytes.size() < 8 + 12 || memcmp(bytes.data(), kPngSignature, sizeof(kPngSignature)) != 0)
		{
			outError = "not a PNG file";
			return false;
		}

		const unsigned char* plte = nullptr;
		size_t plteLen = 0;
		const unsigned char* trns = nullptr;
		size_t trnsLen = 0;
		int colorType = -1;

		// Walk the chunk list by hand. The palette lives in PLTE as plaintext RGB
		// triples, so there is nothing here that needs an image decoder.
		size_t offset = 8;
		while (offset + 12 <= bytes.size())
		{
			const unsigned int length = ReadBE32(bytes.data() + offset);
			if (length > bytes.size() || offset + 12 + length > bytes.size())
			{
				outError = "the PNG is truncated or corrupt";
				return false;
			}

			const char* type = (const char*)bytes.data() + offset + 4;
			const unsigned char* data = bytes.data() + offset + 8;

			if (memcmp(type, "IHDR", 4) == 0 && length >= 13)
			{
				colorType = data[9];
			}
			else if (memcmp(type, "PLTE", 4) == 0 && plte == nullptr)
			{
				plte = data;
				plteLen = length;
			}
			else if (memcmp(type, "tRNS", 4) == 0 && trns == nullptr)
			{
				trns = data;
				trnsLen = length;
			}
			else if (memcmp(type, "IEND", 4) == 0)
			{
				break;
			}

			offset += 12 + length;
		}

		if (colorType != 3 || plte == nullptr)
		{
			outError = "not an indexed PNG; re-export it as indexed (8-bit palette) color";
			return false;
		}

		if (plteLen == 0 || plteLen % 3 != 0)
		{
			outError = "the PNG has a malformed PLTE chunk";
			return false;
		}

		const int entryCount = (std::min)((int)(plteLen / 3), kPaletteEntryCount);

		memset(outPaletteData, 0, IMPL_PALETTE_DATALEN);

		// Index 0 is the transparency slot; leave it cleared, the same way UNI2's
		// importer skips it.
		for (int i = 1; i < entryCount; i++)
		{
			unsigned char* dst = (unsigned char*)outPaletteData + i * 4;
			dst[0] = plte[i * 3 + 2]; // B
			dst[1] = plte[i * 3 + 1]; // G
			dst[2] = plte[i * 3 + 0]; // R
			dst[3] = ((size_t)i < trnsLen) ? trns[i] : 0xFF;
		}

		return true;
	}

	bool WriteIndexedPng(const std::string& path, int width, int height,
		const char* paletteData, const unsigned char* pixels, std::string& outError)
	{
		if (width <= 0 || height <= 0 || !pixels)
		{
			outError = "nothing to write";
			return false;
		}

		unsigned char plte[kPlteLength];
		unsigned char trns[kPaletteEntryCount];

		for (int i = 0; i < kPaletteEntryCount; i++)
		{
			const unsigned char* src = (const unsigned char*)paletteData + i * 4;
			plte[i * 3 + 0] = src[2]; // R
			plte[i * 3 + 1] = src[1]; // G
			plte[i * 3 + 2] = src[0]; // B
			trns[i] = src[3];
		}
		// Index 0 is BBCF's transparency slot -- in a sprite it is the background. Forcing
		// it clear here is what makes an exported sheet come out on transparency instead of
		// on the palette's stand-in colour, and it matches the importer leaving 0 alone.
		trns[0] = 0;

		unsigned char ihdr[13] = {};
		ihdr[0] = (unsigned char)((width >> 24) & 0xFF);
		ihdr[1] = (unsigned char)((width >> 16) & 0xFF);
		ihdr[2] = (unsigned char)((width >> 8) & 0xFF);
		ihdr[3] = (unsigned char)(width & 0xFF);
		ihdr[4] = (unsigned char)((height >> 24) & 0xFF);
		ihdr[5] = (unsigned char)((height >> 16) & 0xFF);
		ihdr[6] = (unsigned char)((height >> 8) & 0xFF);
		ihdr[7] = (unsigned char)(height & 0xFF);
		ihdr[8] = 8; // bit depth
		ihdr[9] = 3; // color type: indexed
		// compression / filter / interlace all stay 0

		// One filter byte per scanline, then one palette index per pixel.
		std::vector<unsigned char> raw;
		raw.reserve((size_t)height * (1 + (size_t)width));
		for (int y = 0; y < height; y++)
		{
			raw.push_back(0); // filter: none
			const unsigned char* row = pixels + (size_t)y * (size_t)width;
			raw.insert(raw.end(), row, row + width);
		}

		std::vector<unsigned char> idat;
		ZlibStore(raw, idat);

		std::vector<unsigned char> png;
		png.insert(png.end(), kPngSignature, kPngSignature + sizeof(kPngSignature));
		AppendChunk(png, "IHDR", ihdr, sizeof(ihdr));
		AppendChunk(png, "PLTE", plte, sizeof(plte));
		AppendChunk(png, "tRNS", trns, sizeof(trns));
		AppendChunk(png, "IDAT", idat.data(), idat.size());
		AppendChunk(png, "IEND", nullptr, 0);

		if (!utils_WriteFile(path.c_str(), png.data(), (unsigned long)png.size(), true))
		{
			outError = "unable to write the file";
			return false;
		}

		return true;
	}

	bool WritePaletteFile(const std::string& path, const char* paletteData, std::string& outError)
	{
		// The plain swatch grid: one pixel per colour, index 0 top-left. This is the
		// fallback for characters whose sprite archive we cannot read; the nicer export
		// is HipReference, which paints the palette onto the character's own sprite.
		unsigned char pixels[kSheetWidth * kSheetHeight];
		for (int i = 0; i < kSheetWidth * kSheetHeight; i++)
			pixels[i] = (unsigned char)i;

		return WriteIndexedPng(path, kSheetWidth, kSheetHeight, paletteData, pixels, outError);
	}
}
