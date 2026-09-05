#pragma once

#include <string>
#include <vector>

// Reading BBCF .pac archives off disk.
//
// A .pac comes in two shapes. Most of what the game ships under data/Sound/BGM is a bare
// FPAC container. Everything else (characters, backgrounds) - and anything a repack tool
// has been through - is that same FPAC wrapped in a DFASFPAC zlib envelope:
//
//   +0x00 "DFASFPAC"; +0x08 uncompressed size; +0x0C compressed size; +0x10 zlib stream
//
// The game reads both, so a music mod shipping compressed BGM pacs is perfectly valid,
// and the mod has to be able to read the same files the game can. Everything here hands
// back the FPAC bytes, envelope already off.
namespace PacFile
{
	// The largest .pac we will inflate. The biggest thing the game ships is a few MB;
	// this only exists so a corrupt size field cannot ask for a gigabyte allocation.
	const unsigned int kMaxUncompressedSize = 128u * 1024u * 1024u;

	// True if `data` starts with the DFASFPAC envelope.
	bool IsCompressed(const void* data, size_t size);

	// Inflate a DFASFPAC buffer into its FPAC bytes.
	bool Decompress(const std::vector<unsigned char>& in,
	                std::vector<unsigned char>& out,
	                std::string* errorOut);

	// Read a .pac and hand back its FPAC bytes, inflating the envelope if there is one.
	bool Read(const std::string& path, std::vector<unsigned char>& out, std::string* errorOut);

	// What the first bytes of a buffer actually are, for error messages that would
	// otherwise leave the user guessing which of a dozen things went wrong.
	std::string DescribeHeader(const std::vector<unsigned char>& pac);
}
