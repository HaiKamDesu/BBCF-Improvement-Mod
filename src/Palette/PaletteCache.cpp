#include "PaletteCache.h"

#include "Core/logger.h"

#include <windows.h>

// Why this file exists: users with large palette collections (one report had 1388 files)
// paid ~10ms per file on a cold boot - every .cfpl is a separate open/read/close, and on a
// spinning disk that is a seek each. 1388 of them froze the game for 14.5 seconds at the
// title screen. Reading one packed blob instead turns that into a single sequential read.
//
// The pack stores each character's *resolved* palette list rather than raw file bytes,
// because .hpl loading is order-dependent (an "X_effect01.hpl" mutates the entry a previous
// "X.hpl" pushed). Caching the end result sidesteps having to replay that ordering.

namespace
{
	const char PALCACHE_MAGIC[8] = { 'B','B','C','F','I','M','P','C' };

	// Bump when the on-disk layout changes in a way older/newer builds would misread.
	const unsigned int PALCACHE_VERSION = 1;

	// Refuse anything absurd rather than trusting a corrupt length and allocating on it.
	const unsigned int PALCACHE_MAX_CHARS = 256;
	const unsigned int PALCACHE_MAX_PALS_PER_CHAR = 100000;
	const unsigned int PALCACHE_MAX_LOG_BYTES = 16u * 1024u * 1024u;

	const wchar_t* PALCACHE_PATH = L"BBCF_IM\\PaletteCache.bin";
	const wchar_t* PALCACHE_TEMP_PATH = L"BBCF_IM\\PaletteCache.bin.tmp";

	struct PalCacheHeader
	{
		char magic[8];
		unsigned int version;
		unsigned int implDataSize; // sizeof(IMPL_data_t) at write time
		unsigned int charCount;
		unsigned int reserved;
	};

	// A cursor over the whole file read into memory. Every accessor bounds-checks, so a
	// truncated or garbage file fails the read instead of walking off the buffer.
	class Reader
	{
	public:
		Reader(const unsigned char* data, size_t size) : m_data(data), m_size(size), m_pos(0) {}

		bool Take(void* out, size_t len)
		{
			if (len > m_size - m_pos)
				return false;

			memcpy(out, m_data + m_pos, len);
			m_pos += len;
			return true;
		}

		bool TakeString(std::string& out, size_t len)
		{
			if (len > m_size - m_pos)
				return false;

			out.assign((const char*)(m_data + m_pos), len);
			m_pos += len;
			return true;
		}

		bool AtEnd() const { return m_pos == m_size; }

	private:
		const unsigned char* m_data;
		size_t m_size;
		size_t m_pos;
	};

	bool ReadWholeFile(const wchar_t* path, std::vector<unsigned char>& out)
	{
		HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);

		if (hFile == INVALID_HANDLE_VALUE)
			return false;

		LARGE_INTEGER size;
		if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 || size.QuadPart > (LONGLONG)0x7FFFFFFF)
		{
			CloseHandle(hFile);
			return false;
		}

		out.resize((size_t)size.QuadPart);

		// One ReadFile for the lot: the entire point of the cache is to pay a single
		// sequential read instead of one seek per palette file.
		size_t total = 0;
		while (total < out.size())
		{
			DWORD got = 0;
			if (!ReadFile(hFile, out.data() + total, (DWORD)(out.size() - total), &got, NULL) || got == 0)
			{
				CloseHandle(hFile);
				return false;
			}
			total += got;
		}

		CloseHandle(hFile);
		return true;
	}

	bool WriteAll(HANDLE hFile, const void* data, size_t len)
	{
		const unsigned char* p = (const unsigned char*)data;
		size_t total = 0;

		while (total < len)
		{
			DWORD written = 0;
			if (!WriteFile(hFile, p + total, (DWORD)(len - total), &written, NULL) || written == 0)
				return false;

			total += written;
		}

		return true;
	}
}

unsigned long long PaletteCache_HashBytes(const void* data, size_t len, unsigned long long seed)
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

bool PaletteCache_Read(PaletteSet& out)
{
	std::vector<unsigned char> raw;
	if (!ReadWholeFile(PALCACHE_PATH, raw))
	{
		LOG(2, "[PaletteCache] no cache file to read\n");
		return false;
	}

	Reader reader(raw.data(), raw.size());

	PalCacheHeader header = {};
	if (!reader.Take(&header, sizeof(header)))
	{
		LOG(1, "[PaletteCache] cache too small for its header, ignoring\n");
		return false;
	}

	if (memcmp(header.magic, PALCACHE_MAGIC, sizeof(header.magic)) != 0 ||
		header.version != PALCACHE_VERSION)
	{
		LOG(1, "[PaletteCache] cache magic/version mismatch, ignoring\n");
		return false;
	}

	// A struct-layout change between builds would make the stored blobs mean something
	// else entirely, so treat it as a different format rather than reinterpreting it.
	if (header.implDataSize != (unsigned int)sizeof(IMPL_data_t))
	{
		LOG(1, "[PaletteCache] cache holds %u-byte palettes but this build uses %u, ignoring\n",
			header.implDataSize, (unsigned int)sizeof(IMPL_data_t));
		return false;
	}

	if (header.charCount == 0 || header.charCount > PALCACHE_MAX_CHARS)
	{
		LOG(1, "[PaletteCache] cache claims %u characters, ignoring\n", header.charCount);
		return false;
	}

	PaletteSet parsed;
	parsed.resize(header.charCount);

	for (unsigned int i = 0; i < header.charCount; i++)
	{
		unsigned long long keyHash = 0;
		unsigned int palCount = 0;
		unsigned int logBytes = 0;

		if (!reader.Take(&keyHash, sizeof(keyHash)) ||
			!reader.Take(&palCount, sizeof(palCount)) ||
			!reader.Take(&logBytes, sizeof(logBytes)))
		{
			LOG(1, "[PaletteCache] cache truncated in character %u's header, ignoring\n", i);
			return false;
		}

		if (palCount > PALCACHE_MAX_PALS_PER_CHAR || logBytes > PALCACHE_MAX_LOG_BYTES)
		{
			LOG(1, "[PaletteCache] cache character %u has implausible counts, ignoring\n", i);
			return false;
		}

		parsed[i].keyHash = keyHash;
		parsed[i].palettes.resize(palCount);

		if (palCount > 0 &&
			!reader.Take(parsed[i].palettes.data(), (size_t)palCount * sizeof(IMPL_data_t)))
		{
			LOG(1, "[PaletteCache] cache truncated in character %u's palettes, ignoring\n", i);
			return false;
		}

		if (!reader.TakeString(parsed[i].log, logBytes))
		{
			LOG(1, "[PaletteCache] cache truncated in character %u's log, ignoring\n", i);
			return false;
		}
	}

	if (!reader.AtEnd())
	{
		LOG(1, "[PaletteCache] cache has trailing bytes, ignoring\n");
		return false;
	}

	out.swap(parsed);
	LOG(1, "[PaletteCache] read cache for %u characters (%u bytes)\n",
		header.charCount, (unsigned int)raw.size());
	return true;
}

bool PaletteCache_Write(const PaletteSet& set)
{
	if (set.empty() || set.size() > PALCACHE_MAX_CHARS)
		return false;

	// Written to a temp file and moved into place: a crash or a kill mid-write leaves the
	// previous cache intact instead of a half-file the next boot would have to reject.
	HANDLE hFile = CreateFileW(PALCACHE_TEMP_PATH, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);

	if (hFile == INVALID_HANDLE_VALUE)
	{
		LOG(1, "[PaletteCache] couldn't create the temp cache file (err %lu)\n", GetLastError());
		return false;
	}

	PalCacheHeader header = {};
	memcpy(header.magic, PALCACHE_MAGIC, sizeof(header.magic));
	header.version = PALCACHE_VERSION;
	header.implDataSize = (unsigned int)sizeof(IMPL_data_t);
	header.charCount = (unsigned int)set.size();
	header.reserved = 0;

	bool ok = WriteAll(hFile, &header, sizeof(header));

	for (size_t i = 0; ok && i < set.size(); i++)
	{
		const PaletteCharSet& charSet = set[i];

		if (charSet.palettes.size() > PALCACHE_MAX_PALS_PER_CHAR ||
			charSet.log.size() > PALCACHE_MAX_LOG_BYTES)
		{
			ok = false;
			break;
		}

		const unsigned long long keyHash = charSet.keyHash;
		const unsigned int palCount = (unsigned int)charSet.palettes.size();
		const unsigned int logBytes = (unsigned int)charSet.log.size();

		ok = WriteAll(hFile, &keyHash, sizeof(keyHash)) &&
			WriteAll(hFile, &palCount, sizeof(palCount)) &&
			WriteAll(hFile, &logBytes, sizeof(logBytes));

		if (ok && palCount > 0)
			ok = WriteAll(hFile, charSet.palettes.data(), (size_t)palCount * sizeof(IMPL_data_t));

		if (ok && logBytes > 0)
			ok = WriteAll(hFile, charSet.log.data(), logBytes);
	}

	CloseHandle(hFile);

	if (!ok)
	{
		LOG(1, "[PaletteCache] failed writing the temp cache file (err %lu)\n", GetLastError());
		DeleteFileW(PALCACHE_TEMP_PATH);
		return false;
	}

	if (!MoveFileExW(PALCACHE_TEMP_PATH, PALCACHE_PATH, MOVEFILE_REPLACE_EXISTING))
	{
		LOG(1, "[PaletteCache] couldn't replace the cache file (err %lu)\n", GetLastError());
		DeleteFileW(PALCACHE_TEMP_PATH);
		return false;
	}

	LOG(1, "[PaletteCache] wrote cache for %u characters\n", (unsigned int)set.size());
	return true;
}

void PaletteCache_Delete()
{
	DeleteFileW(PALCACHE_PATH);
	DeleteFileW(PALCACHE_TEMP_PATH);
}
