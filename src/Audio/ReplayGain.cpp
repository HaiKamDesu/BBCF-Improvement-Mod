#include "ReplayGain.h"

#include "Core/utils.h"

#include <Windows.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	// Tags live at one end of the file or the other, never in the middle, so there is no
	// reason to read a 40 MB song to find 20 bytes.
	const size_t kHeadBytes = 1024 * 1024;   // ID3v2 can be large when art is embedded
	const size_t kTailBytes = 256 * 1024;    // APEv2 sits at the end, after any ID3v1

	// Opus records its gain against -23 LUFS where ReplayGain uses -18, so an Opus tag has
	// to be shifted by the difference to mean the same thing as everything else here.
	const float kOpusReferenceOffsetDb = 5.0f;

	std::string LowerAscii(const std::string& text)
	{
		std::string out(text);
		for (size_t i = 0; i < out.size(); i++)
		{
			if (out[i] >= 'A' && out[i] <= 'Z')
				out[i] = (char)(out[i] - 'A' + 'a');
		}
		return out;
	}

	// Tag values look like "-6.48 dB". strtod stops at the space on its own, so the suffix
	// needs no special handling - but a value that is not a number at all must not read as
	// a legitimate 0 dB.
	bool ParseGain(const std::string& text, float& out)
	{
		const char* begin = text.c_str();
		char* end = NULL;
		const double value = strtod(begin, &end);
		if (end == begin)
			return false;
		out = (float)value;
		return true;
	}

	bool ReadPart(const std::string& path, std::vector<unsigned char>& head,
		std::vector<unsigned char>& tail, long long& fileSize)
	{
		HANDLE hFile = CreateFileW(utf8_to_utf16(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
			NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE)
			return false;

		LARGE_INTEGER size;
		if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0)
		{
			CloseHandle(hFile);
			return false;
		}
		fileSize = size.QuadPart;

		const DWORD headWanted = (DWORD)((fileSize < (long long)kHeadBytes) ? fileSize : kHeadBytes);
		head.resize(headWanted);
		DWORD got = 0;
		if (!ReadFile(hFile, head.data(), headWanted, &got, NULL) || got != headWanted)
		{
			CloseHandle(hFile);
			return false;
		}

		const DWORD tailWanted = (DWORD)((fileSize < (long long)kTailBytes) ? fileSize : kTailBytes);
		LARGE_INTEGER from;
		from.QuadPart = fileSize - tailWanted;
		if (SetFilePointerEx(hFile, from, NULL, FILE_BEGIN))
		{
			tail.resize(tailWanted);
			if (!ReadFile(hFile, tail.data(), tailWanted, &got, NULL) || got != tailWanted)
				tail.clear();
		}

		CloseHandle(hFile);
		return true;
	}

	void TakeField(const std::string& key, const std::string& value, ReplayGain::Tag& tag)
	{
		const std::string lower = LowerAscii(key);
		if (lower == "replaygain_track_gain")
		{
			float gain = 0.0f;
			if (ParseGain(value, gain))
			{
				tag.trackGainDb = gain;
				tag.found = true;
			}
		}
		else if (lower == "replaygain_track_peak")
		{
			float peak = 0.0f;
			if (ParseGain(value, peak))
			{
				tag.trackPeak = peak;
				tag.hasPeak = true;
			}
		}
		else if (lower == "r128_track_gain")
		{
			// Opus: a whole number in Q7.8 dB, against a different reference.
			float raw = 0.0f;
			if (ParseGain(value, raw))
			{
				tag.trackGainDb = raw / 256.0f + kOpusReferenceOffsetDb;
				tag.found = true;
			}
		}
	}

	unsigned int ReadBE32(const unsigned char* p)
	{
		return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
			((unsigned int)p[2] << 8) | (unsigned int)p[3];
	}

	unsigned int ReadLE32(const unsigned char* p)
	{
		return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
			((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
	}

	// ID3v2 sizes are "synchsafe": seven bits per byte, so a size can never contain a byte
	// that looks like the start of an MPEG frame.
	unsigned int ReadSynchsafe(const unsigned char* p)
	{
		return ((unsigned int)(p[0] & 0x7F) << 21) | ((unsigned int)(p[1] & 0x7F) << 14) |
			((unsigned int)(p[2] & 0x7F) << 7) | (unsigned int)(p[3] & 0x7F);
	}

	// ID3v2 TXXX frames: an encoding byte, a NUL-terminated description, then the value.
	// This is where foobar2000 and most taggers put ReplayGain.
	void ScanId3v2(const std::vector<unsigned char>& data, ReplayGain::Tag& tag)
	{
		if (data.size() < 10 || memcmp(data.data(), "ID3", 3) != 0)
			return;

		const unsigned char majorVersion = data[3];
		if (majorVersion < 3 || majorVersion > 4)
			return; // v2.2 uses 3-character frame ids and no TXXX as we know it

		const unsigned int tagSize = ReadSynchsafe(data.data() + 6);
		size_t offset = 10;
		const size_t limit = (std::min)(data.size(), (size_t)10 + tagSize);

		while (offset + 10 <= limit)
		{
			const char* id = (const char*)data.data() + offset;
			if (id[0] == '\0')
				break; // padding

			// v2.4 made frame sizes synchsafe as well; v2.3 left them plain.
			const unsigned int frameSize = (majorVersion == 4)
				? ReadSynchsafe(data.data() + offset + 4)
				: ReadBE32(data.data() + offset + 4);
			if (frameSize == 0 || offset + 10 + frameSize > limit)
				break;

			if (memcmp(id, "TXXX", 4) == 0 && frameSize > 1)
			{
				const unsigned char* frame = data.data() + offset + 10;
				const unsigned char encoding = frame[0];
				const unsigned char* body = frame + 1;
				const size_t bodySize = frameSize - 1;

				// Only the single-byte encodings are handled; ReplayGain is written as
				// ASCII by every tagger that produces it, and decoding UTF-16 here would
				// be work for a case that does not occur.
				if (encoding == 0 || encoding == 3)
				{
					size_t split = 0;
					while (split < bodySize && body[split] != '\0')
						split++;
					if (split < bodySize)
					{
						const std::string key((const char*)body, split);
						size_t valueStart = split + 1;
						size_t valueEnd = valueStart;
						while (valueEnd < bodySize && body[valueEnd] != '\0')
							valueEnd++;
						TakeField(key, std::string((const char*)body + valueStart, valueEnd - valueStart), tag);
					}
				}
			}

			offset += 10 + frameSize;
		}
	}

	// APEv2, which is what MP3Gain itself writes. The footer is at the very end, except
	// when an ID3v1 tag has been appended after it.
	void ScanApev2(const std::vector<unsigned char>& tail, ReplayGain::Tag& tag)
	{
		if (tail.size() < 32)
			return;

		size_t footer = tail.size() - 32;
		if (memcmp(tail.data() + footer, "APETAGEX", 8) != 0)
		{
			if (tail.size() < 32 + 128)
				return;
			footer = tail.size() - 128 - 32; // step back over an ID3v1 tag
			if (memcmp(tail.data() + footer, "APETAGEX", 8) != 0)
				return;
		}

		const unsigned int tagSize = ReadLE32(tail.data() + footer + 12);
		const unsigned int itemCount = ReadLE32(tail.data() + footer + 16);
		if (tagSize < 32 || tagSize > footer + 32 || itemCount == 0 || itemCount > 1024)
			return;

		// tagSize covers the items plus this footer, so the items start that far back.
		size_t offset = footer + 32 - tagSize;
		for (unsigned int i = 0; i < itemCount && offset + 8 < footer; i++)
		{
			const unsigned int valueSize = ReadLE32(tail.data() + offset);
			offset += 8; // value size and flags

			size_t keyEnd = offset;
			while (keyEnd < footer && tail[keyEnd] != '\0')
				keyEnd++;
			if (keyEnd >= footer)
				return;

			const std::string key((const char*)tail.data() + offset, keyEnd - offset);
			const size_t valueStart = keyEnd + 1;
			if (valueStart + valueSize > footer)
				return;

			TakeField(key, std::string((const char*)tail.data() + valueStart, valueSize), tag);
			offset = valueStart + valueSize;
		}
	}

	// A Vorbis comment block: a vendor string, then a count, then "KEY=value" entries.
	// The same structure appears in FLAC, Ogg Vorbis and Opus.
	void ScanVorbisComments(const unsigned char* data, size_t size, ReplayGain::Tag& tag)
	{
		if (size < 8)
			return;

		const unsigned int vendorLength = ReadLE32(data);
		size_t offset = 4 + (size_t)vendorLength;
		if (offset + 4 > size)
			return;

		const unsigned int count = ReadLE32(data + offset);
		offset += 4;
		if (count > 1024)
			return;

		for (unsigned int i = 0; i < count && offset + 4 <= size; i++)
		{
			const unsigned int length = ReadLE32(data + offset);
			offset += 4;
			if (offset + length > size)
				return;

			const std::string entry((const char*)data + offset, length);
			offset += length;

			const size_t equals = entry.find('=');
			if (equals != std::string::npos)
				TakeField(entry.substr(0, equals), entry.substr(equals + 1), tag);
		}
	}

	void ScanFlac(const std::vector<unsigned char>& data, ReplayGain::Tag& tag)
	{
		if (data.size() < 8 || memcmp(data.data(), "fLaC", 4) != 0)
			return;

		size_t offset = 4;
		while (offset + 4 <= data.size())
		{
			const unsigned char header = data[offset];
			const bool last = (header & 0x80) != 0;
			const unsigned char type = header & 0x7F;
			const unsigned int length = ((unsigned int)data[offset + 1] << 16) |
				((unsigned int)data[offset + 2] << 8) | (unsigned int)data[offset + 3];
			offset += 4;
			if (offset + length > data.size())
				return;

			if (type == 4) // VORBIS_COMMENT
			{
				ScanVorbisComments(data.data() + offset, length, tag);
				return;
			}

			if (last)
				return;
			offset += length;
		}
	}

	// Ogg: the comment header is in the second packet. Only the first few pages are worth
	// walking - a tag is never buried in the middle of the audio.
	void ScanOgg(const std::vector<unsigned char>& data, ReplayGain::Tag& tag)
	{
		if (data.size() < 27 || memcmp(data.data(), "OggS", 4) != 0)
			return;

		size_t offset = 0;
		int pages = 0;
		while (offset + 27 <= data.size() && pages < 8)
		{
			if (memcmp(data.data() + offset, "OggS", 4) != 0)
				return;

			const unsigned char segments = data[offset + 26];
			if (offset + 27 + segments > data.size())
				return;

			size_t bodySize = 0;
			for (unsigned int s = 0; s < segments; s++)
				bodySize += data[offset + 27 + s];

			const size_t bodyStart = offset + 27 + segments;
			if (bodyStart + bodySize > data.size())
				return;

			const unsigned char* body = data.data() + bodyStart;
			if (bodySize > 8 && memcmp(body, "OpusTags", 8) == 0)
			{
				ScanVorbisComments(body + 8, bodySize - 8, tag);
				return;
			}
			// Vorbis comment packet: type byte 3 then the "vorbis" signature.
			if (bodySize > 7 && body[0] == 3 && memcmp(body + 1, "vorbis", 6) == 0)
			{
				ScanVorbisComments(body + 7, bodySize - 7, tag);
				return;
			}

			offset = bodyStart + bodySize;
			pages++;
		}
	}
}

namespace ReplayGain
{
	Tag Read(const std::string& path)
	{
		Tag tag;

		std::vector<unsigned char> head;
		std::vector<unsigned char> tail;
		long long fileSize = 0;
		if (!ReadPart(path, head, tail, fileSize))
			return tag;

		// Order matters only in that the first hit wins per field, and these containers
		// are mutually exclusive in practice. ID3v2 is checked first because it can be
		// prepended to almost anything.
		ScanId3v2(head, tag);
		if (!tag.found)
			ScanFlac(head, tag);
		if (!tag.found)
			ScanOgg(head, tag);
		if (!tag.found)
			ScanApev2(tail, tag);

		return tag;
	}
}
