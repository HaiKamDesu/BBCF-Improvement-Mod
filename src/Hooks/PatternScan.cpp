#include "PatternScan.h"

#include <Psapi.h>

namespace
{
	// Deliberately the pre-existing rule, byte for byte: the old scanner tested
	// `mask[j] == '?' || pattern[j] == ...`, so '?' is the only wildcard and every other
	// character means "must match". Every mask in the hook table uses only 'x', 'X' and '?',
	// so the two readings agree today - keeping the original one means a mask that ever grows
	// a third character cannot silently resolve to a different address than it used to.
	inline bool IsSignificant(char maskChar)
	{
		return maskChar != '?';
	}
}

ScanRange PatternScan_WholeImage(HMODULE module)
{
	ScanRange range = { nullptr, 0 };

	MODULEINFO info = { 0 };
	if (module == nullptr ||
		!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
	{
		return range;
	}

	range.base = (const unsigned char*)info.lpBaseOfDll;
	range.size = (size_t)info.SizeOfImage;
	return range;
}

int PatternScan_ExecutableRanges(HMODULE module, ScanRange* out, int maxRanges)
{
	if (out == nullptr || maxRanges <= 0)
		return 0;

	const ScanRange whole = PatternScan_WholeImage(module);
	if (whole.base == nullptr || whole.size == 0)
		return 0;

	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)whole.base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
		dos->e_lfanew <= 0 || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > whole.size)
	{
		out[0] = whole;
		return 1;
	}

	const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(whole.base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
	{
		out[0] = whole;
		return 1;
	}

	const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	const int sectionCount = nt->FileHeader.NumberOfSections;
	int written = 0;

	for (int i = 0; i < sectionCount && written < maxRanges; i++, section++)
	{
		if ((section->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)) == 0)
			continue;

		// VirtualSize is what is actually mapped; SizeOfRawData can be larger when the
		// section is padded on disk, and smaller when it is zero-filled at load time.
		size_t size = section->Misc.VirtualSize;
		if (size == 0)
			size = section->SizeOfRawData;

		if (section->VirtualAddress >= whole.size)
			continue;

		// Never let a malformed header walk us off the end of the mapping.
		if (size > whole.size - section->VirtualAddress)
			size = whole.size - section->VirtualAddress;

		if (size == 0)
			continue;

		out[written].base = whole.base + section->VirtualAddress;
		out[written].size = size;
		written++;
	}

	if (written == 0)
	{
		out[0] = whole;
		return 1;
	}

	return written;
}

bool PatternScan_MatchesAt(const unsigned char* addr, const char* pattern, const char* mask)
{
	if (addr == nullptr || pattern == nullptr || mask == nullptr)
		return false;

	for (size_t j = 0; mask[j] != '\0'; j++)
	{
		if (IsSignificant(mask[j]) && (unsigned char)pattern[j] != addr[j])
			return false;
	}

	return true;
}

const unsigned char* PatternScan_Find(const ScanRange& range, const char* pattern, const char* mask)
{
	if (range.base == nullptr || pattern == nullptr || mask == nullptr)
		return nullptr;

	const size_t patternLength = strlen(mask);
	if (patternLength == 0 || range.size < patternLength)
		return nullptr;

	// Skip leading wildcards when picking the byte to search on, so a pattern that starts
	// with '?' still gets a cheap first-byte reject instead of degenerating to a full compare
	// at every offset.
	size_t anchor = 0;
	while (anchor < patternLength && !IsSignificant(mask[anchor]))
		anchor++;

	const size_t lastStart = range.size - patternLength;

	if (anchor == patternLength)
	{
		// Every byte is a wildcard: the original loop matched at the first offset, so do that.
		return range.base;
	}

	const unsigned char anchorByte = (unsigned char)pattern[anchor];

	for (size_t i = 0; i <= lastStart; i++)
	{
		// One byte test rejects almost every offset, and memchr lets the CRT do it a word or
		// a vector at a time instead of one comparison per offset.
		const unsigned char* candidate = (const unsigned char*)memchr(
			range.base + i + anchor, anchorByte, lastStart - i + 1);

		if (candidate == nullptr)
			return nullptr;

		i = (size_t)(candidate - (range.base + anchor));

		if (PatternScan_MatchesAt(range.base + i, pattern, mask))
			return range.base + i;
	}

	return nullptr;
}
