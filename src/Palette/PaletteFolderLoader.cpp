#include "PaletteFolderLoader.h"

#include "impl_format.h"
#include "impl_templates.h"

#include "Core/logger.h"
#include "Core/utils.h"
#include "Game/characters.h"

#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Defined in PaletteManager.cpp - the built-in .cfpl template per character, used to give a
// legacy .hpl file the seven effect files it doesn't carry.
extern const char* implTemplates[];

namespace
{
	// One palette file found under a character's folder, with the metadata the cache
	// fingerprint is built from. Opening the file is the expensive part; enumerating it
	// is not, which is exactly why the fingerprint only uses what enumeration already gives us.
	struct FolderEntry
	{
		std::wstring wFullPath;
		std::string fullPath;   // narrowed the same way the pre-cache loader narrowed it
		std::string fileName;
		unsigned long long size = 0;
		unsigned long long writeTime = 0;
	};

	// The pre-cache loader built its ANSI paths with std::string(wstr.begin(), wstr.end()),
	// which truncates each UTF-16 unit to a byte. Reproduced verbatim on purpose: changing it
	// would change which oddly-named files load, and that is not what this change is about.
	std::string NarrowLikeBefore(const std::wstring& wide)
	{
		return std::string(wide.begin(), wide.end());
	}

	void AppendLogLine(std::string& log, const char* fmt, ...)
	{
		char buffer[1024];

		va_list args;
		va_start(args, fmt);
		const int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
		va_end(args);

		if (written <= 0)
			return;

		log.append(buffer, (size_t)std::min<int>(written, (int)sizeof(buffer) - 1));
	}

	void EnumerateFolder(const std::wstring& wFolderPath, std::vector<FolderEntry>& out)
	{
		WIN32_FIND_DATAW data;
		HANDLE hFind = FindFirstFileW(wFolderPath.c_str(), &data);

		if (hFind == INVALID_HANDLE_VALUE)
			return;

		const std::string folderPath = NarrowLikeBefore(wFolderPath);

		do
		{
			if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
				continue;

			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				std::wstring wSubfolderPath(wFolderPath);
				wSubfolderPath.pop_back(); // Delete "*" at the end
				wSubfolderPath += data.cFileName;
				wSubfolderPath += L"\\*";
				EnumerateFolder(wSubfolderPath, out);
				continue;
			}

			const std::wstring wFileName(data.cFileName);

			FolderEntry entry;
			entry.fileName = NarrowLikeBefore(wFileName);
			entry.fullPath = folderPath;
			entry.fullPath.pop_back(); // Delete "*" at the end
			entry.fullPath += entry.fileName;

			entry.wFullPath = wFolderPath;
			entry.wFullPath.pop_back();
			entry.wFullPath += wFileName;

			entry.size = ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
			entry.writeTime = ((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) |
				data.ftLastWriteTime.dwLowDateTime;

			out.push_back(entry);

		} while (FindNextFileW(hFind, &data));

		FindClose(hFind);
	}

	// Fingerprints the folder from path + size + last-write time of every file in it, over a
	// sorted copy so the hash doesn't move when the filesystem hands entries back in a
	// different order. Any add, delete, rename or edit changes it, which is what forces that
	// character to be re-read from disk.
	unsigned long long HashEntries(const std::vector<FolderEntry>& entries)
	{
		std::vector<const FolderEntry*> sorted;
		sorted.reserve(entries.size());

		for (size_t i = 0; i < entries.size(); i++)
			sorted.push_back(&entries[i]);

		std::sort(sorted.begin(), sorted.end(), [](const FolderEntry* a, const FolderEntry* b) {
			return a->wFullPath < b->wFullPath;
		});

		const unsigned long long count = (unsigned long long)sorted.size();
		unsigned long long hash = PaletteCache_HashBytes(&count, sizeof(count), 14695981039346656037ULL);

		for (size_t i = 0; i < sorted.size(); i++)
		{
			const FolderEntry& entry = *sorted[i];
			hash = PaletteCache_HashBytes(entry.wFullPath.data(),
				entry.wFullPath.size() * sizeof(wchar_t), hash);
			hash = PaletteCache_HashBytes(&entry.size, sizeof(entry.size), hash);
			hash = PaletteCache_HashBytes(&entry.writeTime, sizeof(entry.writeTime), hash);
		}

		return hash;
	}

	int FindPalIndexIn(const PaletteCharSet& charSet, const char* palNameToFind)
	{
		// Same contract as PaletteManager::FindCustomPalIndex, minus the char-index check
		// the caller has already done: -3 for the default/empty name, -1 for not found.
		if (strncmp(palNameToFind, "", IMPL_PALNAME_LENGTH) == 0 ||
			strncmp(palNameToFind, "Default", IMPL_PALNAME_LENGTH) == 0)
			return -3;

		for (size_t i = 0; i < charSet.palettes.size(); i++)
		{
			if (strncmp(palNameToFind, charSet.palettes[i].palInfo.palName, IMPL_PALNAME_LENGTH) == 0)
				return (int)i;
		}

		return -1;
	}

	void OverwritePalNameFromFileName(std::string fileName, IMPL_data_t& palData)
	{
		// Overwrite palname in data section with the filename, so renaming the file has effect on the actual ingame palname
		size_t pos = fileName.find('/');

		if (pos != std::string::npos)
			fileName = fileName.substr(pos + 1);

		const std::string fileNameWithoutExt = fileName.substr(0, fileName.rfind('.'));
		memset(palData.palInfo.palName, 0, IMPL_PALNAME_LENGTH);
		strncpy(palData.palInfo.palName, fileNameWithoutExt.c_str(), IMPL_PALNAME_LENGTH - 1);
	}

	bool PushPaletteInto(CharIndex charIndex, PaletteCharSet& charSet, IMPL_data_t& filledPalData)
	{
		if (charIndex > getCharactersCount())
		{
			AppendLogLine(charSet.log, "[error] Custom palette couldn't be loaded: CharIndex out of bound.\n");
			LOG(2, "ERROR, CharIndex out of bound\n");
			return false;
		}

		if (FindPalIndexIn(charSet, filledPalData.palInfo.palName) > 0)
		{
			AppendLogLine(charSet.log,
				"[error] Custom palette couldn't be loaded: a palette with name '%s' is already loaded.\n",
				filledPalData.palInfo.palName);
			LOG(2, "ERROR, A custom palette with name '%s' is already loaded.\n", filledPalData.palInfo.palName);
			return false;
		}

		charSet.palettes.push_back(filledPalData);

		AppendLogLine(charSet.log, "[system] %s: Loaded '%s%s'\n",
			getCharacterNameByIndexA(charIndex).c_str(),
			filledPalData.palInfo.palName,
			IMPL_FILE_EXTENSION);

		return true;
	}

	void LoadImplFileInto(CharIndex charIndex, PaletteCharSet& charSet, const FolderEntry& entry)
	{
		IMPL_t fileContents;

		if (!utils_ReadFile(entry.fullPath.c_str(), &fileContents, sizeof(fileContents), true))
		{
			LOG(2, "\tCouldn't open %s!\n", strerror(errno));
			AppendLogLine(charSet.log, "[error] Unable to open '%s' : %s\n",
				entry.fileName.c_str(), strerror(errno));
			return;
		}

		if (strncmp(fileContents.header.fileSig, IMPL_FILESIG, sizeof(fileContents.header.fileSig)) != 0)
		{
			LOG(2, "ERROR, unrecognized file format!\n");
			AppendLogLine(charSet.log, "[error] '%s' unrecognized file format!\n", entry.fileName.c_str());
			return;
		}

		if (fileContents.header.dataLen != sizeof(IMPL_data_t))
		{
			LOG(2, "ERROR, data size mismatch!\n");
			AppendLogLine(charSet.log, "[error] '%s' data size mismatch!\n", entry.fileName.c_str());
			return;
		}

		if (isCharacterIndexOutOfBound(fileContents.header.charIndex))
		{
			LOG(2, "ERROR, '%s' has invalid character index in the header\n", entry.fileName.c_str());
			AppendLogLine(charSet.log, "[error] '%s' has invalid character index in the header\n",
				entry.fileName.c_str());
		}
		else if (charIndex != fileContents.header.charIndex)
		{
			LOG(2, "ERROR, '%s' belongs to character %s, but is placed in folder %s\n",
				entry.fileName.c_str(), getCharacterNameByIndexA(fileContents.header.charIndex).c_str(),
				getCharacterNameByIndexA(charIndex).c_str());

			AppendLogLine(charSet.log,
				"[error] '%s' belongs to character '%s', but is placed in folder '%s'\n",
				entry.fileName.c_str(), getCharacterNameByIndexA(fileContents.header.charIndex).c_str(),
				getCharacterNameByIndexA(charIndex).c_str());
		}
		else
		{
			OverwritePalNameFromFileName(entry.fileName, fileContents.palData);
			PushPaletteInto(charIndex, charSet, fileContents.palData);
		}
	}

	void LoadHplFileInto(CharIndex charIndex, PaletteCharSet& charSet, const FolderEntry& entry)
	{
		const std::string& fileName = entry.fileName;

		if (fileName.find("_effectbloom") != std::string::npos)
		{
			const std::string palName = fileName.substr(0, fileName.rfind("_effectbloom"));
			const int palIndex = FindPalIndexIn(charSet, palName.c_str());

			if (palIndex < 0)
			{
				LOG(2, "ERROR, '%s' has no custom character palette to match with!\n", fileName.c_str());
				AppendLogLine(charSet.log,
					"[error] '%s' has no custom character palette to match with! Create a character palette named '%s' to load this bloom file on!\n",
					fileName.c_str(), (palName + ".hpl").c_str());
				return;
			}

			charSet.palettes[palIndex].palInfo.hasBloom = true;

			AppendLogLine(charSet.log, "[system] %s: Loaded '%s'\n",
				getCharacterNameByIndexA(charIndex).c_str(), fileName.c_str());
			return;
		}

		char fileContents[LEGACY_HPL_HEADER_LEN + LEGACY_HPL_DATALEN];

		if (!utils_ReadFile(entry.fullPath.c_str(), &fileContents, sizeof(fileContents), true))
		{
			LOG(2, "\tCouldn't open %s!\n", strerror(errno));
			AppendLogLine(charSet.log, "[error] Unable to open '%s' : %s\n", fileName.c_str(), strerror(errno));
			return;
		}

		// Effect file:
		if (fileName.find("_effect0") != std::string::npos)
		{
			const std::string palName = fileName.substr(0, fileName.rfind("_effect0"));
			const int palIndex = FindPalIndexIn(charSet, palName.c_str());

			if (palIndex < 0)
			{
				LOG(2, "ERROR, '%s' has no custom character palette to match with!\n", fileName.c_str());
				AppendLogLine(charSet.log,
					"[error] '%s' has no custom character palette to match with! Create a character palette named '%s' to load this effect file on!\n",
					fileName.c_str(), (palName + ".hpl").c_str());
				return;
			}

			const std::string effectIndex = fileName.substr(fileName.find("_effect0") + 7, 2);
			int fileIndex = 0;

			// stoi throws on a name like "X_effect0z.hpl"; the pre-cache loader let that
			// propagate, which on a worker thread would take the process down with it.
			try
			{
				fileIndex = std::stoi(effectIndex);
			}
			catch (const std::exception&)
			{
				fileIndex = 0;
			}

			if (fileIndex <= 0 || fileIndex > 7)
			{
				LOG(2, "ERROR, '%s'has wrong index of effect file!\n", fileName.c_str());
				AppendLogLine(charSet.log, "[error] '%s' has wrong index!\n", fileName.c_str());
				return;
			}

			IMPL_data_t& implData = charSet.palettes[palIndex];
			char* pImplEffectFile = (char*)&implData.file0 + fileIndex * IMPL_PALETTE_DATALEN;

			memcpy_s(pImplEffectFile, IMPL_PALETTE_DATALEN, (char*)&fileContents + LEGACY_HPL_HEADER_LEN, LEGACY_HPL_DATALEN);

			AppendLogLine(charSet.log, "[system] %s: Loaded '%s'\n",
				getCharacterNameByIndexA(charIndex).c_str(), fileName.c_str());
		}
		else // Palette file
		{
			IMPL_t implTemplate;

			// Make a copy of template
			memcpy_s(&implTemplate, sizeof(IMPL_t), implTemplates[charIndex], sizeof(IMPL_t));

			// Copy .hpl data into cfpl template
			memcpy_s(&implTemplate.palData.file0, IMPL_PALETTE_DATALEN, (char*)&fileContents + LEGACY_HPL_HEADER_LEN, LEGACY_HPL_DATALEN);

			OverwritePalNameFromFileName(fileName, implTemplate.palData);
			PushPaletteInto(charIndex, charSet, implTemplate.palData);
		}
	}
}

// Faithful port of what PaletteManager::LoadPaletteSettingsFile did inline, minus ATL: same
// keys, same defaults, same quote stripping and the same trailing-extension trim, so a slot
// value resolves to exactly the palette it always did.
void PaletteFolderLoader_ReadSlots(PaletteSlotsResult& out)
{
	const int charCount = getCharactersCount();

	out.slots.clear();
	out.slots.resize(charCount);
	for (int i = 0; i < charCount; i++)
	{
		out.slots[i].assign(PALETTE_SLOT_COUNT, std::string());
	}
	out.loadOnlinePalettes = true;
	out.read = false;

	wchar_t exePath[MAX_PATH] = {};
	if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0)
	{
		return;
	}

	std::wstring iniPath(exePath);
	const std::wstring::size_type slash = iniPath.find_last_of(L'\\');
	if (slash == std::wstring::npos)
	{
		return;
	}
	iniPath = iniPath.substr(0, slash) + L"\\palettes.ini";

	if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		LOG(2, "\t'palettes.ini' file was not found!\n");
		return;
	}

	out.read = true;

	wchar_t buffer[MAX_PATH] = {};
	GetPrivateProfileStringW(L"General", L"OnlinePalettes", L"1", buffer, MAX_PATH, iniPath.c_str());
	out.loadOnlinePalettes = _wtoi(buffer) != 0;

	for (int i = 0; i < charCount; i++)
	{
		const std::wstring section = getCharacterNameByIndexW(i);

		for (int slot = 1; slot <= PALETTE_SLOT_COUNT; slot++)
		{
			const std::wstring key = std::to_wstring(slot);
			GetPrivateProfileStringW(section.c_str(), key.c_str(), L"", buffer, MAX_PATH, iniPath.c_str());

			std::wstring value(buffer);

			// Quotes anywhere, as the CString version did with Remove('"').
			value.erase(std::remove(value.begin(), value.end(), L'"'), value.end());

			// Everything from the extension onwards, so "Foo.cfpl" becomes "Foo".
			const std::wstring::size_type ext = value.find(IMPL_FILE_EXTENSION_W);
			if (ext != std::wstring::npos)
			{
				value.erase(ext);
			}

			out.slots[i][slot - 1] = NarrowLikeBefore(value);
		}
	}

	LOG(2, "[PaletteLoad] read palettes.ini (OnlinePalettes=%d)\n", out.loadOnlinePalettes ? 1 : 0);
}

void PaletteFolderLoader_Run(PaletteLoadOutcome& outcome, const std::atomic<bool>* cancel,
	bool forceFullReread)
{
	const int charCount = getCharactersCount();

	outcome.set.clear();
	outcome.set.resize(charCount);

	// The cache is advisory: a miss on any character only means that character is read the
	// slow way, and a completely unreadable cache only means every character is.
	PaletteSet cached;
	const bool haveCache = !forceFullReread &&
		PaletteCache_Read(cached) && (int)cached.size() == charCount;

	if (forceFullReread)
	{
		LOG(1, "[PaletteLoad] full re-read requested; ignoring the cache\n");
	}

	bool anyCharRead = false;

	for (int i = 0; i < charCount; i++)
	{
		if (cancel != nullptr && cancel->load())
		{
			outcome.cancelled = true;
			return;
		}

		PaletteCharSet& charSet = outcome.set[i];

		const std::wstring wPath = std::wstring(L"BBCF_IM\\Palettes\\") + getCharacterNameByIndexW(i) + L"\\*";

		std::vector<FolderEntry> entries;
		EnumerateFolder(wPath, entries);

		charSet.keyHash = HashEntries(entries);

		if (haveCache && cached[i].keyHash == charSet.keyHash && !cached[i].palettes.empty() &&
			strncmp(cached[i].palettes[0].palInfo.palName, "Default", IMPL_PALNAME_LENGTH) == 0)
		{
			charSet.palettes.swap(cached[i].palettes);
			charSet.log.swap(cached[i].log);
			outcome.charsFromCache++;
			outcome.totalPalettes += (int)charSet.palettes.size() - 1;
			continue;
		}

		// Element 0 is the "Default" placeholder that sets a character back to its native
		// colour. It is synthesized, never read from a file, and every custom index the rest
		// of the mod uses is relative to it sitting here.
		const IMPL_data_t defaultPal { "Default" };
		charSet.palettes.push_back(defaultPal);

		LOG(2, "LoadPalettesIntoContainer %s\n", NarrowLikeBefore(wPath).c_str());

		for (size_t e = 0; e < entries.size(); e++)
		{
			if (cancel != nullptr && cancel->load())
			{
				outcome.cancelled = true;
				return;
			}

			const FolderEntry& entry = entries[e];

			LOG(2, "\tFILE: %s", entry.fileName.c_str());
			LOG(2, "\t\tFull path: %s\n", entry.fullPath.c_str());

			if (entry.fileName.find(IMPL_FILE_EXTENSION) != std::string::npos)
			{
				LoadImplFileInto((CharIndex)i, charSet, entry);
				outcome.filesRead++;
			}
			else if (entry.fileName.find(LEGACY_HPL_FILE_EXTENSION) != std::string::npos)
			{
				LoadHplFileInto((CharIndex)i, charSet, entry);
				outcome.filesRead++;
			}
			else
			{
				LOG(2, "Unrecognized file format for '%s'\n", entry.fileName.c_str());
				AppendLogLine(charSet.log, "[error] Unable to open '%s' : not an %s file\n",
					entry.fileName.c_str(), IMPL_FILE_EXTENSION);
			}
		}

		outcome.charsFromDisk++;
		outcome.totalPalettes += (int)charSet.palettes.size() - 1;
		anyCharRead = true;
	}

	// A forced re-read still refreshes the cache, so the next launch is fast again.
	if (outcome.totalPalettes < PALETTE_CACHE_MIN_PALETTES)
	{
		// Small collection: no cache, so the overwhelming majority of installs never grow a
		// PaletteCache.bin at all. Drop a stale one if the collection shrank past the line.
		if (haveCache)
		{
			LOG(1, "[PaletteLoad] only %d palettes; dropping the cache\n", outcome.totalPalettes);
			PaletteCache_Delete();
		}
	}
	else if (anyCharRead || !haveCache)
	{
		outcome.cacheWritten = PaletteCache_Write(outcome.set);
	}

	// Read here rather than on the game thread: 1080 GetPrivateProfileString calls measured
	// 134ms on a reporter's machine, which is eight dropped frames if it lands mid-match.
	if (cancel == nullptr || !cancel->load())
	{
		PaletteFolderLoader_ReadSlots(outcome.slots);
	}

	LOG(1, "[PaletteLoad] %d palettes: %d characters from cache, %d read from disk (%d files), cacheWritten=%d\n",
		outcome.totalPalettes, outcome.charsFromCache, outcome.charsFromDisk, outcome.filesRead,
		outcome.cacheWritten ? 1 : 0);
}
