#include "PacFile.h"

#include <Windows.h>

// Same reasoning as the updater's PackageStager: stb_image's inflate is already in the
// DLL but rejects valid deflate streams depending on what the compressor picked, and
// without this define miniz declares a bare `crc32` macro that rewrites unrelated fields.
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <cstdio>
#include <cstring>

namespace PacFile
{

bool IsCompressed(const void* data, size_t size)
{
	return size >= 0x10 && memcmp(data, "DFASFPAC", 8) == 0;
}

bool Decompress(const std::vector<unsigned char>& in,
                std::vector<unsigned char>& out,
                std::string* errorOut)
{
	auto fail = [&](const std::string& msg) {
		if (errorOut) *errorOut = msg;
		return false;
	};

	if (!IsCompressed(in.data(), in.size()))
		return fail("Not a DFASFPAC container");

	unsigned int uncompressedSize = 0;
	memcpy(&uncompressedSize, in.data() + 0x08, sizeof(uncompressedSize));
	if (uncompressedSize < 0x20 || uncompressedSize > kMaxUncompressedSize) {
		char buf[128];
		sprintf_s(buf, "Compressed .pac declares an implausible size (%u bytes)", uncompressedSize);
		return fail(buf);
	}

	// The size at +0x0C is the compressed length, but at least one repack tool writes the
	// whole file's length there instead, which would run the decompressor off the end. The
	// zlib stream knows where it stops, so feed it everything after the envelope and let
	// the declared output size be the thing that has to match.
	const size_t available = in.size() - 0x10;

	out.assign(uncompressedSize, 0);
	const size_t produced = tinfl_decompress_mem_to_mem(
		&out[0], out.size(), in.data() + 0x10, available, TINFL_FLAG_PARSE_ZLIB_HEADER);

	if (produced == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || produced != (size_t)uncompressedSize) {
		out.clear();
		return fail("Could not decompress the .pac - the zlib stream is damaged");
	}
	return true;
}

bool Read(const std::string& path, std::vector<unsigned char>& out, std::string* errorOut)
{
	auto fail = [&](const std::string& msg) {
		if (errorOut) *errorOut = msg;
		return false;
	};

	HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		char buf[320];
		sprintf_s(buf, "Could not open '%s' (error %lu)", path.c_str(), GetLastError());
		return fail(buf);
	}

	const DWORD size = GetFileSize(hFile, NULL);
	if (size == INVALID_FILE_SIZE || size == 0) {
		CloseHandle(hFile);
		return fail("'" + path + "' is empty or unreadable");
	}
	if (size > kMaxUncompressedSize) {
		CloseHandle(hFile);
		return fail("'" + path + "' is far too large to be a .pac");
	}

	std::vector<unsigned char> raw(size);
	DWORD got = 0;
	const BOOL readOk = ReadFile(hFile, raw.data(), size, &got, NULL);
	CloseHandle(hFile);
	if (!readOk || got != size)
		return fail("Short read on '" + path + "'");

	if (!IsCompressed(raw.data(), raw.size())) {
		out.swap(raw);
		return true;
	}
	return Decompress(raw, out, errorOut);
}

std::string DescribeHeader(const std::vector<unsigned char>& pac)
{
	if (pac.size() < 4)
		return "the file is only " + std::to_string(pac.size()) + " bytes";

	char magic[5] = {};
	for (int i = 0; i < 4; ++i) {
		const unsigned char c = pac[i];
		magic[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
	}

	char buf[192];
	if (pac.size() >= 0x20) {
		unsigned int dataStart = 0, fileCount = 0, nameField = 0;
		memcpy(&dataStart, &pac[0x04], sizeof(dataStart));
		memcpy(&fileCount, &pac[0x0C], sizeof(fileCount));
		memcpy(&nameField, &pac[0x14], sizeof(nameField));
		sprintf_s(buf, "magic '%s' (%02X %02X %02X %02X), dataStart 0x%X, %u sub-files, "
		               "nameField %u, %zu bytes",
			magic, pac[0], pac[1], pac[2], pac[3], dataStart, fileCount, nameField, pac.size());
	}
	else {
		sprintf_s(buf, "magic '%s' (%02X %02X %02X %02X), %zu bytes",
			magic, pac[0], pac[1], pac[2], pac[3], pac.size());
	}
	return buf;
}

}
