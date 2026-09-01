#include "CharSelectPalettes.h"

#include "PaletteManager.h"
#include "impl_format.h"

#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Game/characters.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "stb_image.h"

namespace
{
	// Entry for "char_select_dot.pac" in the game's resource filename table, as an RVA
	// off the module base (the exe relocates). Confirmed by walking the PE section table:
	// this sits in .data among its siblings char_select_stage.pac, char_select_obj.pac,
	// char_select_line.pac. Note .data's file offset and RVA differ by 0x2000, so a raw
	// file offset read out of the exe is NOT this address.
	const uintptr_t kFilenameRva = 0x005DBF74;

	// Where our copy goes, relative to the folder the game resolves the name against, and
	// what the patched pointer therefore has to say. A subfolder, so nothing shipped is
	// touched and Steam's file verification has nothing to undo.
	const char* const kOutputDirRel = "data/ETC/BBCFIM_Palettes";
	const char* const kOutputPathRel = "data/ETC/BBCFIM_Palettes/char_select_dot.pac";
	const char* const kPointerValue = "BBCFIM_Palettes/char_select_dot.pac";
	const char* const kOriginalPathRel = "data/ETC/char_select_dot.pac";
	// Records which slot assignments the copy on disk was built from, so a relaunch with
	// unchanged palettes reuses it instead of rewriting 12 MB.
	const char* const kSignaturePathRel = "data/ETC/BBCFIM_Palettes/char_select_dot.sig";

	// An HPAL entry is a 32-byte header then IMPL_PALETTE_DATALEN bytes of BGRA, which is
	// the same shape as a custom palette's file0 - that identity is what makes this work.
	const size_t kHpalHeaderSize = 32;
	const size_t kHpalSize = kHpalHeaderSize + IMPL_PALETTE_DATALEN;

	// The mod maps 24 colour slots; the game ships 26 palettes per character.
	const int kSlotCount = 24;

	// Asset tag per CharIndex, matching src/Palette/PaletteSheet.cpp.
	const char* const kCharTags[] = {
		"rg", "jn", "no", "rc", "tk", "tg", "lc", "ar", "bn", "ca", "ha", "ny",
		"tb", "hz", "mu", "mk", "vh", "pt", "rl", "iz", "am", "bl", "az", "kg",
		"kk", "tm", "ce", "rm", "hb", "ph", "nt", "mi", "su", "es", "ma", "jb",
	};
	const int kCharTagCount = sizeof(kCharTags) / sizeof(kCharTags[0]);

	const unsigned int kMaxPackedSize = 32u * 1024u * 1024u;
	const unsigned int kMaxUnpackedSize = 64u * 1024u * 1024u;

	uintptr_t g_slotAddr = 0;
	const char* g_original = NULL;      // the shipped string, for Restore()
	std::string g_owned;                // backing store for the value we point at
	std::string g_lastError;
	std::string g_lastSignature;        // slot assignments the current copy was built from
	bool g_active = false;

	void LogCsp(const char* fmt, ...)
	{
		char buf[512];
		va_list args;
		va_start(args, fmt);
		vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
		va_end(args);
		LOG(2, "CharSelectPalettes: %s", buf);
	}

	unsigned int ReadU32(const unsigned char* p)
	{
		return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
			((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
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
		DWORD read = 0;
		const BOOL ok = ReadFile(hFile, out.data(), (DWORD)out.size(), &read, NULL);
		CloseHandle(hFile);
		if (!ok || read != out.size())
		{
			out.clear();
			return false;
		}
		return true;
	}

	// Shipped .pac files are usually a DFASFPAC header wrapping a zlib stream, but the
	// game also ships plain ones (data/Sound/sample.pac), which is what lets us write our
	// copy uncompressed - the mod has a zlib decompressor but no compressor.
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
			return false;
		}

		if (stbi_zlib_decode_buffer((char*)out.data(), (int)out.size(),
			(const char*)data.data() + 16, (int)packed) != (int)unpacked)
		{
			return false;
		}

		data.swap(out);
		return true;
	}

	struct PacEntry
	{
		std::string name;
		size_t offset;   // into the containing buffer
		size_t size;
	};

	// FPAC: dataStart @0x04, fileCount @0x0C, name length @0x14, then fixed-stride records
	// of {name, u32 index, u32 offset, u32 size}, offsets relative to dataStart.
	bool ParseFpac(const unsigned char* data, size_t size, std::vector<PacEntry>& out)
	{
		if (size < 0x20 || memcmp(data, "FPAC", 4) != 0)
			return false;

		const unsigned int dataStart = ReadU32(data + 0x04);
		const unsigned int fileCount = ReadU32(data + 0x0C);
		const unsigned int nameLen = ReadU32(data + 0x14);
		if (fileCount == 0 || fileCount > 100000u || nameLen == 0 || nameLen > 256u)
			return false;
		if (dataStart < 0x20 || dataStart > size)
			return false;

		const unsigned int stride = (dataStart - 0x20) / fileCount;
		if (stride < nameLen + 12)
			return false;

		out.clear();
		out.reserve(fileCount);
		for (unsigned int i = 0; i < fileCount; i++)
		{
			const size_t rec = 0x20 + (size_t)i * stride;
			if (rec + nameLen + 12 > size)
				return false;

			const char* rawName = (const char*)data + rec;
			size_t chars = 0;
			while (chars < nameLen && rawName[chars] != '\0')
				chars++;

			const unsigned int offset = ReadU32(data + rec + nameLen + 4);
			const unsigned int entrySize = ReadU32(data + rec + nameLen + 8);
			if ((size_t)dataStart + offset + entrySize > size)
				return false;

			PacEntry entry;
			entry.name.assign(rawName, chars);
			entry.offset = (size_t)dataStart + offset;
			entry.size = entrySize;
			out.push_back(entry);
		}
		return true;
	}

	// "<tag><NN>_00.hpl" for colour slot `slot` (0-based), which is the naming paldata.pac
	// uses and matches char_XX_pal.pac entry for entry.
	std::string HplNameFor(int charIndex, int slot)
	{
		char buf[32];
		sprintf_s(buf, "%s%02d_00.hpl", kCharTags[charIndex], slot);
		return buf;
	}

	// Identifies the current slot assignments, so Refresh can skip rebuilding a 12 MB file
	// when nothing has changed.
	std::string SlotSignature()
	{
		std::string signature;
		if (!g_interfaces.pPaletteManager)
			return signature;

		const std::vector<std::vector<std::string> >& slots =
			g_interfaces.pPaletteManager->GetPaletteSlots();
		for (size_t c = 0; c < slots.size(); c++)
		{
			for (size_t s = 0; s < slots[c].size(); s++)
			{
				if (slots[c][s].empty())
					continue;
				char buf[16];
				sprintf_s(buf, "%u:%u=", (unsigned)c, (unsigned)s);
				signature += buf;
				signature += slots[c][s];
				signature += ';';
			}
		}
		return signature;
	}

	bool WriteAllBytes(const std::string& path, const std::vector<unsigned char>& data)
	{
		HANDLE hFile = CreateFileA(path.c_str(), GENERIC_WRITE, 0, NULL,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE)
			return false;

		size_t written = 0;
		bool ok = true;
		while (written < data.size())
		{
			const DWORD chunk = (DWORD)((data.size() - written > 0x100000)
				? 0x100000 : (data.size() - written));
			DWORD got = 0;
			if (!WriteFile(hFile, data.data() + written, chunk, &got, NULL) || got != chunk)
			{
				ok = false;
				break;
			}
			written += got;
		}
		CloseHandle(hFile);
		return ok;
	}

	std::string ReadSignatureFile()
	{
		std::vector<unsigned char> bytes;
		if (!ReadAllBytes(GamePath(kSignaturePathRel), bytes) || bytes.empty())
			return std::string();
		return std::string((const char*)bytes.data(), bytes.size());
	}

	void WriteSignatureFile(const std::string& signature)
	{
		std::vector<unsigned char> bytes(signature.begin(), signature.end());
		WriteAllBytes(GamePath(kSignaturePathRel), bytes);
	}

	bool CopyLooksUsable()
	{
		const DWORD attributes = GetFileAttributesA(GamePath(kOutputPathRel).c_str());
		return attributes != INVALID_FILE_ATTRIBUTES &&
			!(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool WritePointer(const char* value)
	{
		if (!g_slotAddr)
			return false;

		DWORD oldProtect = 0;
		if (!VirtualProtect((void*)g_slotAddr, sizeof(char*), PAGE_READWRITE, &oldProtect))
		{
			LogCsp("VirtualProtect failed (error %lu)\n", GetLastError());
			return false;
		}
		*(const char**)g_slotAddr = value;
		DWORD ignored = 0;
		VirtualProtect((void*)g_slotAddr, sizeof(char*), oldProtect, &ignored);
		return true;
	}

	bool ResolveSlot()
	{
		if (g_slotAddr)
			return true;

		const uintptr_t moduleBase = (uintptr_t)GetModuleHandle(NULL);
		if (!moduleBase)
			return false;

		const uintptr_t slot = moduleBase + kFilenameRva;
		const char* current = *(const char**)slot;

		// Refuse to touch anything unless the entry really is the name we expect. A wrong
		// address here would be a stray write into the game's data segment.
		if (!current || strcmp(current, "char_select_dot.pac") != 0)
		{
			LogCsp("filename table entry did not hold the expected name; not patching\n");
			return false;
		}

		g_slotAddr = slot;
		g_original = current;
		return true;
	}

	// Rewrites the palettes inside an inflated char_select_dot.pac. Entries are a fixed
	// size, so this is an in-place overwrite of the buffer rather than a repack - which
	// also means the result is still a valid plain FPAC and can be written straight out.
	int SubstitutePalettes(std::vector<unsigned char>& outer)
	{
		std::vector<PacEntry> top;
		if (!ParseFpac(outer.data(), outer.size(), top))
			return -1;

		const PacEntry* paldata = NULL;
		for (size_t i = 0; i < top.size(); i++)
		{
			if (_stricmp(top[i].name.c_str(), "paldata.pac") == 0)
				paldata = &top[i];
		}
		if (!paldata)
			return -1;

		// paldata.pac ships uncompressed inside the outer container, so its entries can be
		// addressed directly. If that ever stops being true this bails rather than writing
		// palettes into compressed bytes.
		unsigned char* palBase = outer.data() + paldata->offset;
		std::vector<PacEntry> palettes;
		if (!ParseFpac(palBase, paldata->size, palettes))
			return -1;

		PaletteManager* manager = g_interfaces.pPaletteManager;
		if (!manager)
			return -1;

		const std::vector<std::vector<std::string> >& slots = manager->GetPaletteSlots();

		int replaced = 0;
		for (int charIndex = 0; charIndex < kCharTagCount; charIndex++)
		{
			if ((size_t)charIndex >= slots.size())
				break;

			for (int slot = 0; slot < kSlotCount && (size_t)slot < slots[charIndex].size(); slot++)
			{
				const std::string& palName = slots[charIndex][slot];
				if (palName.empty())
					continue;

				// "Random" and friends pick at match start, so there is nothing stable to
				// show on the select screen; leave those colours as the game's own.
				const int palIndex = manager->FindCustomPalIndex((CharIndex)charIndex, palName.c_str());
				if (palIndex < 0)
					continue;

				const IMPL_data_t* data = manager->GetCustomPalData((CharIndex)charIndex, palIndex);
				if (!data)
					continue;

				const std::string wanted = HplNameFor(charIndex, slot);
				for (size_t e = 0; e < palettes.size(); e++)
				{
					if (_stricmp(palettes[e].name.c_str(), wanted.c_str()) != 0)
						continue;
					if (palettes[e].size < kHpalSize)
						break;

					memcpy(palBase + palettes[e].offset + kHpalHeaderSize,
						data->file0, IMPL_PALETTE_DATALEN);
					replaced++;
					break;
				}
			}
		}

		return replaced;
	}
}

namespace CharSelectPalettes
{
	void Refresh()
	{
		g_lastError.clear();

		if (!Settings::settingsIni.customPalettesInCharSelect)
		{
			// Turned off: undo anything a previous session left pointing at our copy.
			Restore();
			g_lastSignature.clear();
			return;
		}
		if (!g_interfaces.pPaletteManager)
		{
			g_lastError = "palettes are not loaded yet";
			return;
		}
		if (!ResolveSlot())
		{
			g_lastError = "this build of the game does not have the expected filename table";
			return;
		}

		const std::string signature = SlotSignature();
		if (signature.empty())
		{
			// Nothing assigned: put the shipped file back rather than leaving a stale copy.
			Restore();
			g_lastSignature.clear();
			return;
		}
		if (g_active && signature == g_lastSignature)
			return;

		// A copy built from these same assignments is already on disk (previous launch):
		// point at it and skip the rebuild entirely.
		if (!g_active && CopyLooksUsable() && ReadSignatureFile() == signature)
		{
			WritePointer(g_original);
			g_owned = kPointerValue;
			if (WritePointer(g_owned.c_str()))
			{
				g_active = true;
				g_lastSignature = signature;
				LogCsp("reusing the existing copy; character select reads \"%s\"\n", kPointerValue);
				return;
			}
		}

		std::vector<unsigned char> outer;
		if (!ReadAllBytes(GamePath(kOriginalPathRel), outer))
		{
			g_lastError = "could not read the game's character select data";
			return;
		}
		if (!Inflate(outer))
		{
			g_lastError = "could not unpack the game's character select data";
			return;
		}

		const int replaced = SubstitutePalettes(outer);
		if (replaced < 0)
		{
			g_lastError = "the character select data is not in the format this build expects";
			return;
		}
		if (replaced == 0)
		{
			Restore();
			g_lastSignature = signature;
			return;
		}

		CreateDirectoryA(GamePath(kOutputDirRel).c_str(), NULL);
		const std::string outPath = GamePath(kOutputPathRel);
		if (!WriteAllBytes(outPath, outer))
		{
			g_lastError = "could not write the character select copy";
			return;
		}

		// Point at the shipped name first: reassigning g_owned frees the buffer the entry
		// may currently reference.
		WritePointer(g_original);
		g_owned = kPointerValue;
		if (!WritePointer(g_owned.c_str()))
		{
			g_lastError = "could not write to the game's filename table";
			return;
		}

		g_active = true;
		g_lastSignature = signature;
		WriteSignatureFile(signature);
		LogCsp("%d colour(s) replaced; character select now reads \"%s\"\n",
			replaced, kPointerValue);
	}

	void Restore()
	{
		if (!g_active || !g_slotAddr || !g_original)
			return;
		WritePointer(g_original);
		g_active = false;
		LogCsp("character select filename restored\n");
	}

	const std::string& LastError()
	{
		return g_lastError;
	}

	bool IsActive()
	{
		return g_active;
	}
}
