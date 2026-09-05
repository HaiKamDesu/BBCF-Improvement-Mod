#include "ProfileBlobSeal.h"

#include "Core/interfaces.h"
#include "Core/logger.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>

namespace
{
	// The blob the avatar fields live in. playerAvatarBaseAddr is its base -- verified
	// against a live session on 2026-09-05, where netUserData was 0x0092D0C0 and the
	// reported avatar base 0x0092D190 (= netUserData + 0xD0).
	const size_t kProfileBlobSize = 0x6800;

	uint8_t* BlobPtr()
	{
		if (g_gameVals.playerAvatarBaseAddr == 0)
		{
			return nullptr;
		}

		uint8_t* const blob = reinterpret_cast<uint8_t*>(g_gameVals.playerAvatarBaseAddr);
		if (IsBadWritePtr(blob, kProfileBlobSize))
		{
			return nullptr;
		}
		return blob;
	}

	// Replica of the game's sum, shared by its sealer (FUN_0040DEC0) and its verifier
	// (FUN_0040DF10): 16-bit ones'-complement with end-around carry over size/2 words.
	uint16_t Checksum16(const uint8_t* buf, size_t size)
	{
		uint32_t sum = 0;
		const size_t words = size / 2;
		for (size_t i = 0; i < words; ++i)
		{
			uint16_t word;
			memcpy(&word, buf + i * 2, sizeof(word));
			sum += word;
			sum = (sum & 0xFFFF) + (sum >> 16);
		}
		return static_cast<uint16_t>(sum);
	}
}

bool ProfileBlobSeal::Reseal()
{
	uint8_t* const blob = BlobPtr();
	if (blob == nullptr)
	{
		return false;
	}

	// The sealer clears the checksum field as a dword before summing, so +0x02 ends up
	// zero as well. Matching that exactly matters: those two bytes are part of the sum.
	uint32_t zero = 0;
	memcpy(blob, &zero, sizeof(zero));

	const uint16_t checksum = static_cast<uint16_t>(~Checksum16(blob, kProfileBlobSize));
	memcpy(blob, &checksum, sizeof(checksum));

	return true;
}

bool ProfileBlobSeal::IsValid()
{
	const uint8_t* const blob = BlobPtr();
	if (blob == nullptr)
	{
		return false;
	}

	return Checksum16(blob, kProfileBlobSize) == 0xFFFF;
}
