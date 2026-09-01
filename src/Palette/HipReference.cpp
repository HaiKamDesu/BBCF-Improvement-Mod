#include "HipReference.h"

#include "PngPalette.h"
#include "impl_format.h"

#include "Core/logger.h"
#include "Core/utils.h"
#include "Game/characters.h"

#include <Windows.h>

#include <cstring>
#include <string>
#include <vector>

// stb_image is compiled exactly once, in Overlay/Branding.cpp. Including the header here
// only pulls in declarations - we want its zlib inflater, which comes along with PNG
// support and saves the project a compression dependency it otherwise does not have.
#include "stb_image.h"

namespace
{
	// Asset tag for each CharIndex, i.e. the XX in data/Char/char_XX_img.pac.
	//
	// Derived, not guessed: every entry was confirmed by byte-matching the mod's own
	// per-character default palette template (implTemplates[] in PaletteManager.cpp)
	// against the first palette inside each shipped char_XX_pal.pac. All 36 matched
	// exactly. Worth knowing because several tags are not what the character's name
	// suggests - Izanami is "mi", Nine is "ph", Nu is "ny", and Lambda is "rm" whose
	// sprites are in turn named ny* because she shares Nu's Murakumo body. The game
	// also ships a "ta" archive that is not a playable character at all.
	const char* const kCharTags[] = {
		"rg", "jn", "no", "rc", "tk", "tg", // Ragna .. Tager
		"lc", "ar", "bn", "ca", "ha", "ny", // Litchi .. Nu
		"tb", "hz", "mu", "mk", "vh", "pt", // Tsubaki .. Platinum
		"rl", "iz", "am", "bl", "az", "kg", // Relius .. Kagura
		"kk", "tm", "ce", "rm", "hb", "ph", // Kokonoe .. Nine
		"nt", "mi", "su", "es", "ma", "jb", // Naoto .. Jubei
	};
	const int kCharTagCount = sizeof(kCharTags) / sizeof(kCharTags[0]);

	// The archives run 5-12 MB packed and 17-40 MB unpacked. These caps are generous
	// enough for every shipped file and still refuse anything absurd, which matters in a
	// 32-bit process where a bad length field would otherwise mean a huge allocation.
	const unsigned int kMaxPackedSize = 64u * 1024u * 1024u;
	const unsigned int kMaxUnpackedSize = 128u * 1024u * 1024u;

	unsigned int ReadU32(const unsigned char* p)
	{
		return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
			((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
	}

	std::string ArchivePathFor(int charIndex)
	{
		if (charIndex < 0 || charIndex >= kCharTagCount)
			return std::string();
		return GamePath(std::string("data/Char/char_") + kCharTags[charIndex] + "_img.pac");
	}

	bool ReadAllBytes(const std::string& path, std::vector<unsigned char>& out)
	{
		HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE)
			return false;

		LARGE_INTEGER size;
		if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 ||
			size.QuadPart > (LONGLONG)kMaxPackedSize)
		{
			CloseHandle(hFile);
			return false;
		}

		out.resize((size_t)size.QuadPart);
		DWORD bytesRead = 0;
		const BOOL ok = ReadFile(hFile, out.data(), (DWORD)out.size(), &bytesRead, NULL);
		CloseHandle(hFile);
		if (!ok || bytesRead != out.size())
		{
			out.clear();
			return false;
		}
		return true;
	}

	// Shipped .pac files are a "DFASFPAC" header followed by a raw zlib stream wrapping
	// the real FPAC archive. A few are stored plain, so an absent wrapper is not an error.
	bool Inflate(std::vector<unsigned char>& data)
	{
		if (data.size() < 16 || memcmp(data.data(), "DFASFPAC", 8) != 0)
			return true;

		const unsigned int unpacked = ReadU32(data.data() + 8);
		const unsigned int packed = ReadU32(data.data() + 12);
		if (unpacked == 0 || unpacked > kMaxUnpackedSize ||
			packed == 0 || (size_t)packed + 16 > data.size())
			return false;

		std::vector<unsigned char> out;
		try
		{
			out.resize(unpacked);
		}
		catch (const std::bad_alloc&)
		{
			return false; // 32-bit address space said no
		}

		const int written = stbi_zlib_decode_buffer((char*)out.data(), (int)out.size(),
			(const char*)data.data() + 16, (int)packed);
		if (written != (int)unpacked)
			return false;

		data.swap(out);
		return true;
	}

	struct PacEntry
	{
		std::string name;
		const unsigned char* data;
		unsigned int size;
	};

	// FPAC: dataStart @0x04, fileCount @0x0C, name field length @0x14, then fileCount
	// records of {name[nameLen], u32 index, u32 offset, u32 size} laid out at a fixed
	// stride. Offsets are relative to dataStart.
	bool ParseFpac(const std::vector<unsigned char>& data, std::vector<PacEntry>& out)
	{
		if (data.size() < 0x20 || memcmp(data.data(), "FPAC", 4) != 0)
			return false;

		const unsigned int dataStart = ReadU32(data.data() + 0x04);
		const unsigned int fileCount = ReadU32(data.data() + 0x0C);
		const unsigned int nameLen = ReadU32(data.data() + 0x14);
		if (fileCount == 0 || fileCount > 100000u || nameLen == 0 || nameLen > 256u)
			return false;
		if (dataStart < 0x20 || dataStart > data.size())
			return false;

		const unsigned int stride = (dataStart - 0x20) / fileCount;
		if (stride < nameLen + 12)
			return false;

		out.clear();
		out.reserve(fileCount);
		for (unsigned int i = 0; i < fileCount; i++)
		{
			const size_t rec = 0x20 + (size_t)i * stride;
			if (rec + nameLen + 12 > data.size())
				return false;

			const char* rawName = (const char*)data.data() + rec;
			size_t nameChars = 0;
			while (nameChars < nameLen && rawName[nameChars] != '\0')
				nameChars++;

			const unsigned int offset = ReadU32(data.data() + rec + nameLen + 4);
			const unsigned int size = ReadU32(data.data() + rec + nameLen + 8);
			if ((size_t)dataStart + offset + size > data.size())
				return false;

			PacEntry entry;
			entry.name.assign(rawName, nameChars);
			entry.data = data.data() + dataStart + offset;
			entry.size = size;
			out.push_back(entry);
		}
		return true;
	}

	struct HipInfo
	{
		unsigned int width;
		unsigned int height;
		const unsigned char* palette;   // 256 BGRA entries
		const unsigned char* runs;      // (index, runLength) pairs
		size_t runBytes;
	};

	// HIP: "HIP\0", version, file size, palette entry count @0x0C, texture w/h @0x10,
	// encoding tag @0x18, extra header size @0x1C. When the extra header is present it
	// carries the real image dimensions, which are what the pixel data is sized to.
	// Encoding 0x01 is the palette-indexed form; the others (0x10 ARGB, 0x04 luma) carry
	// no palette and so cannot be recoloured, which is exactly what we want to skip.
	bool ParseHip(const unsigned char* data, size_t size, HipInfo& out)
	{
		if (size < 0x20 || memcmp(data, "HIP\0", 4) != 0)
			return false;
		if (data[0x18] != 0x01)
			return false;

		const unsigned int paletteCount = ReadU32(data + 0x0C);
		if (paletteCount != (unsigned int)(IMPL_PALETTE_DATALEN / 4))
			return false;

		const unsigned int extraSize = ReadU32(data + 0x1C);
		unsigned int width = ReadU32(data + 0x10);
		unsigned int height = ReadU32(data + 0x14);
		size_t offset = 0x20;

		if (extraSize >= 0x10)
		{
			if (size < 0x30)
				return false;
			width = ReadU32(data + 0x20);
			height = ReadU32(data + 0x24);
			offset = 0x20 + extraSize;
		}

		if (width == 0 || height == 0 || width > 8192 || height > 8192)
			return false;
		if (offset + IMPL_PALETTE_DATALEN > size)
			return false;

		out.width = width;
		out.height = height;
		out.palette = data + offset;
		out.runs = data + offset + IMPL_PALETTE_DATALEN;
		out.runBytes = size - offset - IMPL_PALETTE_DATALEN;
		return true;
	}

	// Walks the run pairs without materialising pixels, to judge a sprite cheaply.
	// `outDistinct` is how many palette entries the sprite actually uses and is the
	// score that matters: the point of the sheet is to show the user as much of their
	// palette as possible. Returns false if the runs do not exactly fill the image,
	// which is the cheapest way to reject anything malformed.
	bool ScoreSprite(const HipInfo& hip, int& outDistinct, unsigned int& outSolid)
	{
		const size_t total = (size_t)hip.width * hip.height;
		bool seen[256] = {};
		size_t filled = 0;
		unsigned int solid = 0;

		for (size_t i = 0; i + 1 < hip.runBytes && filled < total; i += 2)
		{
			const unsigned char index = hip.runs[i];
			const unsigned char length = hip.runs[i + 1];
			if (length == 0)
				continue; // zero-length runs really do occur in shipped sprites

			filled += length;
			seen[index] = true;
			if (index != 0)
				solid += length;
		}

		if (filled != total)
			return false;

		outDistinct = 0;
		for (int i = 0; i < 256; i++)
			outDistinct += seen[i] ? 1 : 0;
		outSolid = solid;
		return true;
	}

	bool DecodeSprite(const HipInfo& hip, std::vector<unsigned char>& outPixels)
	{
		const size_t total = (size_t)hip.width * hip.height;
		outPixels.clear();
		outPixels.reserve(total);

		for (size_t i = 0; i + 1 < hip.runBytes && outPixels.size() < total; i += 2)
		{
			const unsigned char index = hip.runs[i];
			size_t length = hip.runs[i + 1];
			if (length == 0)
				continue;
			if (outPixels.size() + length > total)
				length = total - outPixels.size();
			outPixels.insert(outPixels.end(), length, index);
		}

		return outPixels.size() == total;
	}

	// Sprites in an archive are named "<prefix><digits>_<frame>", and the archive also
	// holds a scattering of other characters' sprites (from shared interactions) plus
	// effect sprites, which index into the effect palettes rather than the character
	// one. Taking the most common prefix picks out the character's own sprites without
	// a hardcoded exception - it is what makes Lambda work, whose tag is "rm" but whose
	// sprites are named ny* because she shares Nu's body.
	std::string DominantPrefix(const std::vector<PacEntry>& entries)
	{
		std::vector<std::pair<std::string, int> > counts;

		for (size_t i = 0; i < entries.size(); i++)
		{
			const std::string& name = entries[i].name;
			size_t letters = 0;
			while (letters < name.size() && name[letters] >= 'a' && name[letters] <= 'z')
				letters++;
			// Require a digit right after the letters, so "rgef611" (an effect) does not
			// masquerade as the prefix "rgef" and outvote the real sprites.
			if (letters == 0 || letters >= name.size() || name[letters] < '0' || name[letters] > '9')
				continue;

			const std::string prefix = name.substr(0, letters);
			size_t j = 0;
			for (; j < counts.size(); j++)
			{
				if (counts[j].first == prefix) { counts[j].second++; break; }
			}
			if (j == counts.size())
				counts.push_back(std::make_pair(prefix, 1));
		}

		std::string best;
		int bestCount = 0;
		for (size_t i = 0; i < counts.size(); i++)
		{
			if (counts[i].second > bestCount)
			{
				bestCount = counts[i].second;
				best = counts[i].first;
			}
		}
		return best;
	}

	bool HasPrefix(const std::string& name, const std::string& prefix)
	{
		if (name.size() <= prefix.size())
			return false;
		if (name.compare(0, prefix.size(), prefix) != 0)
			return false;
		const char next = name[prefix.size()];
		return next >= '0' && next <= '9';
	}
}

namespace HipReference
{
	bool IsAvailable(int charIndex)
	{
		const std::string path = ArchivePathFor(charIndex);
		if (path.empty())
			return false;
		const DWORD attributes = GetFileAttributesA(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES &&
			!(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool WriteReferenceSheet(int charIndex, const char* paletteData,
		const std::string& outPath, std::string& outError)
	{
		if (isCharacterIndexOutOfBound(charIndex) || charIndex >= kCharTagCount)
		{
			outError = "unknown character";
			return false;
		}
		if (!paletteData)
		{
			outError = "no palette to apply";
			return false;
		}

		const std::string archivePath = ArchivePathFor(charIndex);
		std::vector<unsigned char> archive;
		if (!ReadAllBytes(archivePath, archive))
		{
			outError = "could not read '" + archivePath + "'";
			return false;
		}
		if (!Inflate(archive))
		{
			outError = "could not unpack '" + archivePath + "'";
			return false;
		}

		std::vector<PacEntry> entries;
		if (!ParseFpac(archive, entries))
		{
			outError = "'" + archivePath + "' is not in the format this build expects";
			return false;
		}

		const std::string prefix = DominantPrefix(entries);
		if (prefix.empty())
		{
			outError = "no usable sprites in '" + archivePath + "'";
			return false;
		}

		// Pick the sprite that shows the most of the palette, breaking ties on how much
		// of it is actually drawn, so the sheet is a big recognisable pose rather than a
		// stray hand or a puff of smoke.
		const PacEntry* bestEntry = NULL;
		HipInfo bestHip = {};
		int bestDistinct = 0;
		unsigned int bestSolid = 0;

		for (size_t i = 0; i < entries.size(); i++)
		{
			if (!HasPrefix(entries[i].name, prefix))
				continue;

			HipInfo hip;
			if (!ParseHip(entries[i].data, entries[i].size, hip))
				continue;

			int distinct = 0;
			unsigned int solid = 0;
			if (!ScoreSprite(hip, distinct, solid))
				continue;

			if (distinct > bestDistinct || (distinct == bestDistinct && solid > bestSolid))
			{
				bestEntry = &entries[i];
				bestHip = hip;
				bestDistinct = distinct;
				bestSolid = solid;
			}
		}

		if (!bestEntry)
		{
			outError = "no usable sprite found for this character";
			return false;
		}

		std::vector<unsigned char> pixels;
		if (!DecodeSprite(bestHip, pixels))
		{
			outError = "that sprite could not be decoded";
			return false;
		}

		LOG(2, "HipReference: %s -> %s (%ux%u, %d colors used)\n",
			getCharacterNameByIndexA(charIndex).c_str(), bestEntry->name.c_str(),
			bestHip.width, bestHip.height, bestDistinct);

		// Note the palette written is the caller's, NOT the one embedded in the sprite:
		// the embedded copy is only what that frame happened to ship with, while the
		// indices mean the same thing across all of a character's sprites.
		return PngPalette::WriteIndexedPng(outPath, (int)bestHip.width, (int)bestHip.height,
			paletteData, pixels.data(), outError);
	}
}
