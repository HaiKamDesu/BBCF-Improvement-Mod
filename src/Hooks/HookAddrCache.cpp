#include "HookAddrCache.h"

#include "Core/logger.h"

#include <Psapi.h>

#include <map>
#include <string>
#include <vector>

namespace
{
	const char HOOKCACHE_MAGIC[8] = { 'B','B','C','F','I','M','H','A' };
	const unsigned int HOOKCACHE_VERSION = 1;

	// A label is a compile-time string in the hook table; anything longer or more numerous
	// than this means the file is not ours.
	const unsigned int HOOKCACHE_MAX_ENTRIES = 4096;
	const unsigned int HOOKCACHE_MAX_LABEL = 128;

	const wchar_t* HOOKCACHE_PATH = L"BBCF_IM\\HookAddrCache.bin";
	const wchar_t* HOOKCACHE_TEMP_PATH = L"BBCF_IM\\HookAddrCache.bin.tmp";

	struct HookCacheHeader
	{
		char magic[8];
		unsigned int version;
		unsigned int entryCount;
		unsigned long long key;
	};

	std::map<std::string, DWORD> g_entries;
	bool g_loaded = false;
	bool g_dirty = false;

	unsigned long long HashBytes(const void* data, size_t len, unsigned long long seed)
	{
		const unsigned char* p = (const unsigned char*)data;
		unsigned long long hash = seed;

		for (size_t i = 0; i < len; i++)
		{
			hash ^= (unsigned long long)p[i];
			hash *= 1099511628211ULL;
		}

		return hash;
	}
}

unsigned long long HookAddrCache_ModuleKey(HMODULE module)
{
	unsigned long long key = 14695981039346656037ULL;

	// On-disk identity first: this is what actually changes when the game updates.
	wchar_t exePath[MAX_PATH + 1] = {};
	if (GetModuleFileNameW(NULL, exePath, MAX_PATH))
	{
		WIN32_FILE_ATTRIBUTE_DATA attr = {};
		if (GetFileAttributesExW(exePath, GetFileExInfoStandard, &attr))
		{
			key = HashBytes(&attr.nFileSizeLow, sizeof(attr.nFileSizeLow), key);
			key = HashBytes(&attr.nFileSizeHigh, sizeof(attr.nFileSizeHigh), key);
			key = HashBytes(&attr.ftLastWriteTime, sizeof(attr.ftLastWriteTime), key);
		}
	}

	MODULEINFO info = { 0 };
	if (module != nullptr &&
		GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
	{
		key = HashBytes(&info.SizeOfImage, sizeof(info.SizeOfImage), key);

		const unsigned char* base = (const unsigned char*)info.lpBaseOfDll;
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;

		if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
			(size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) <= (size_t)info.SizeOfImage)
		{
			const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
			if (nt->Signature == IMAGE_NT_SIGNATURE)
			{
				key = HashBytes(&nt->FileHeader.TimeDateStamp, sizeof(nt->FileHeader.TimeDateStamp), key);
				key = HashBytes(&nt->OptionalHeader.CheckSum, sizeof(nt->OptionalHeader.CheckSum), key);
				key = HashBytes(&nt->OptionalHeader.AddressOfEntryPoint,
					sizeof(nt->OptionalHeader.AddressOfEntryPoint), key);
			}
		}
	}

	// Bake the layout version in, so a format change invalidates every existing file even if
	// the header check were ever relaxed.
	key = HashBytes(&HOOKCACHE_VERSION, sizeof(HOOKCACHE_VERSION), key);
	return key;
}

void HookAddrCache_Load(unsigned long long expectedKey)
{
	if (g_loaded)
		return;

	g_loaded = true;

	HANDLE hFile = CreateFileW(HOOKCACHE_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);

	if (hFile == INVALID_HANDLE_VALUE)
	{
		LOG(1, "[HookScan] no address cache yet; every signature will be scanned\n");
		return;
	}

	LARGE_INTEGER size;
	if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024)
	{
		LOG(1, "[HookScan] address cache has an implausible size; ignoring it\n");
		CloseHandle(hFile);
		return;
	}

	std::vector<unsigned char> raw((size_t)size.QuadPart);
	size_t total = 0;

	while (total < raw.size())
	{
		DWORD got = 0;
		if (!ReadFile(hFile, raw.data() + total, (DWORD)(raw.size() - total), &got, NULL) || got == 0)
		{
			LOG(1, "[HookScan] address cache read failed; ignoring it\n");
			CloseHandle(hFile);
			return;
		}
		total += got;
	}

	CloseHandle(hFile);

	size_t pos = 0;
	HookCacheHeader header = {};

	if (raw.size() < sizeof(header))
	{
		LOG(1, "[HookScan] address cache is too small for its header; ignoring it\n");
		return;
	}

	memcpy(&header, raw.data(), sizeof(header));
	pos += sizeof(header);

	if (memcmp(header.magic, HOOKCACHE_MAGIC, sizeof(header.magic)) != 0 ||
		header.version != HOOKCACHE_VERSION)
	{
		LOG(1, "[HookScan] address cache magic/version mismatch; ignoring it\n");
		return;
	}

	if (header.key != expectedKey)
	{
		// Normal after a game update - the addresses in it describe a different exe.
		LOG(1, "[HookScan] address cache was built for a different BBCF.exe; rescanning\n");
		g_dirty = true;
		return;
	}

	if (header.entryCount > HOOKCACHE_MAX_ENTRIES)
	{
		LOG(1, "[HookScan] address cache claims %u entries; ignoring it\n", header.entryCount);
		return;
	}

	for (unsigned int i = 0; i < header.entryCount; i++)
	{
		unsigned int labelLength = 0;
		DWORD rva = 0;

		if (pos + sizeof(labelLength) + sizeof(rva) > raw.size())
		{
			LOG(1, "[HookScan] address cache truncated at entry %u; keeping what parsed\n", i);
			return;
		}

		memcpy(&labelLength, raw.data() + pos, sizeof(labelLength));
		pos += sizeof(labelLength);
		memcpy(&rva, raw.data() + pos, sizeof(rva));
		pos += sizeof(rva);

		if (labelLength == 0 || labelLength > HOOKCACHE_MAX_LABEL || pos + labelLength > raw.size())
		{
			LOG(1, "[HookScan] address cache entry %u has a bad label length; keeping what parsed\n", i);
			return;
		}

		std::string label((const char*)raw.data() + pos, labelLength);
		pos += labelLength;

		g_entries[label] = rva;
	}

	LOG(1, "[HookScan] address cache loaded with %u entries\n", (unsigned int)g_entries.size());
}

DWORD HookAddrCache_Get(const char* label)
{
	if (label == nullptr)
		return 0;

	const std::map<std::string, DWORD>::const_iterator it = g_entries.find(label);
	return it == g_entries.end() ? 0 : it->second;
}

void HookAddrCache_Put(const char* label, DWORD rva)
{
	if (label == nullptr || rva == 0)
		return;

	DWORD& stored = g_entries[label];
	if (stored != rva)
	{
		stored = rva;
		g_dirty = true;
	}
}

void HookAddrCache_Save(unsigned long long key)
{
	if (!g_dirty || g_entries.empty())
		return;

	HANDLE hFile = CreateFileW(HOOKCACHE_TEMP_PATH, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);

	if (hFile == INVALID_HANDLE_VALUE)
	{
		LOG(1, "[HookScan] couldn't create the temp address cache (err %lu)\n", GetLastError());
		return;
	}

	std::vector<unsigned char> out;
	HookCacheHeader header = {};
	memcpy(header.magic, HOOKCACHE_MAGIC, sizeof(header.magic));
	header.version = HOOKCACHE_VERSION;
	header.entryCount = (unsigned int)g_entries.size();
	header.key = key;

	out.insert(out.end(), (const unsigned char*)&header, (const unsigned char*)&header + sizeof(header));

	for (std::map<std::string, DWORD>::const_iterator it = g_entries.begin(); it != g_entries.end(); ++it)
	{
		const unsigned int labelLength = (unsigned int)it->first.size();
		const DWORD rva = it->second;

		if (labelLength == 0 || labelLength > HOOKCACHE_MAX_LABEL)
			continue;

		out.insert(out.end(), (const unsigned char*)&labelLength,
			(const unsigned char*)&labelLength + sizeof(labelLength));
		out.insert(out.end(), (const unsigned char*)&rva, (const unsigned char*)&rva + sizeof(rva));
		out.insert(out.end(), it->first.begin(), it->first.end());
	}

	DWORD written = 0;
	const bool ok = WriteFile(hFile, out.data(), (DWORD)out.size(), &written, NULL) &&
		written == out.size();
	CloseHandle(hFile);

	if (!ok)
	{
		LOG(1, "[HookScan] failed writing the temp address cache (err %lu)\n", GetLastError());
		DeleteFileW(HOOKCACHE_TEMP_PATH);
		return;
	}

	if (!MoveFileExW(HOOKCACHE_TEMP_PATH, HOOKCACHE_PATH, MOVEFILE_REPLACE_EXISTING))
	{
		LOG(1, "[HookScan] couldn't replace the address cache (err %lu)\n", GetLastError());
		DeleteFileW(HOOKCACHE_TEMP_PATH);
		return;
	}

	g_dirty = false;
	LOG(1, "[HookScan] address cache written with %u entries\n", header.entryCount);
}
