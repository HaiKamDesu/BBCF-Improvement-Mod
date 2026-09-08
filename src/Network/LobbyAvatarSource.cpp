#include "LobbyAvatarSource.h"

#include "Core/logger.h"
#include "Core/utils.h"
#include "Game/GhidraDefs.h"

#include <Windows.h>

#include <cstdint>

namespace
{
	// The icon source holds icon + 1, and 0 is the game's "nothing chosen" value there.
	// FUN_004924B0 decrements before clamping, so writing icon + 1 is what the game's own
	// equip menu writes.
	const int kIconSourceBias = 1;

	uint8_t* SaveDataManager()
	{
		const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
		if (moduleBase == 0)
		{
			return nullptr;
		}

		// Constructed lazily on the first call to its getter. Reading the guard is how we
		// stay out of a manager that has not been built yet without calling into the game
		// to build it: this runs off the render thread, and the getter registers atexit
		// handlers on first use.
		const uint32_t* const guard =
			reinterpret_cast<const uint32_t*>(moduleBase + ADDR_SaveDataManagerGuard);
		if (IsBadReadPtr(guard, sizeof(*guard)) || (*guard & 1) == 0)
		{
			return nullptr;
		}

		uint8_t* const manager = reinterpret_cast<uint8_t*>(moduleBase + ADDR_SaveDataManager);
		// 0x8264 is the furthest field this file touches (slot B's target flag).
		if (IsBadWritePtr(manager, OFFSET_AvatarAccSlotB + OFFSET_AccSlotTargetsHigh + sizeof(uint32_t)))
		{
			return nullptr;
		}
		return manager;
	}

	int32_t* FieldAt(uint8_t* manager, uintptr_t offset)
	{
		return reinterpret_cast<int32_t*>(manager + offset);
	}

	int32_t* SlotId(uint8_t* manager, uintptr_t slotOffset)
	{
		return FieldAt(manager, slotOffset + OFFSET_AccSlotEquippedId);
	}

	// The game's init writes 0 into slot A's target flag and 1 into slot B's. Checking both
	// is a cheap proof that the base address is right and the manager is initialised, which
	// is worth having before writing into somebody's save state.
	bool SlotFlagsLookRight(uint8_t* manager)
	{
		const int32_t slotA = *FieldAt(manager, OFFSET_AvatarAccSlotA + OFFSET_AccSlotTargetsHigh);
		const int32_t slotB = *FieldAt(manager, OFFSET_AvatarAccSlotB + OFFSET_AccSlotTargetsHigh);
		return slotA == 0 && slotB == 1;
	}

	bool Within(int value, int max)
	{
		return value >= 0 && value <= max;
	}
}

bool LobbyAvatarSource::Available()
{
	uint8_t* const manager = SaveDataManager();
	return manager != nullptr && SlotFlagsLookRight(manager);
}

bool LobbyAvatarSource::InRange(const Values& values)
{
	return Within(values.icon, MAX_AvatarIcon)
		&& Within(values.color, MAX_AvatarColor)
		&& Within(values.accessory1, MAX_AvatarAccessoryId)
		&& Within(values.accessory2, MAX_AvatarAccessoryId);
}

bool LobbyAvatarSource::Read(Values* out)
{
	if (out == nullptr)
	{
		return false;
	}

	uint8_t* const manager = SaveDataManager();
	if (manager == nullptr || !SlotFlagsLookRight(manager))
	{
		return false;
	}

	const int32_t rawIcon = *FieldAt(manager, OFFSET_AvatarSourceIcon);
	// Mirror the game: it decrements and treats anything at or past the icon count as 0.
	// Reporting the biased value raw would make a never-chosen icon read as -1 and be
	// mistaken for "nothing remembered".
	out->icon = rawIcon >= kIconSourceBias ? rawIcon - kIconSourceBias : 0;
	out->color = *FieldAt(manager, OFFSET_AvatarSourceColor);
	out->accessory1 = *SlotId(manager, OFFSET_AvatarAccSlotA);
	out->accessory2 = *SlotId(manager, OFFSET_AvatarAccSlotB);
	return true;
}

bool LobbyAvatarSource::Write(const Values& values)
{
	if (!InRange(values))
	{
		// Refused as a set rather than clamped. A value outside these ranges is a
		// hand-edited settings.ini or a half-written file, and the accessory ids reach an
		// unbounded table index -- guessing at what was meant is not worth an out-of-bounds
		// read in the avatar draw path, on our machine or on anyone else's in the lobby.
		LOG(2, "LobbyAvatarSource: refused out-of-range set avatar %d, colour %d, accessories %d/%d\n",
			values.icon, values.color, values.accessory1, values.accessory2);
		return false;
	}

	uint8_t* const manager = SaveDataManager();
	if (manager == nullptr || !SlotFlagsLookRight(manager))
	{
		return false;
	}

	*FieldAt(manager, OFFSET_AvatarSourceIcon) = values.icon + kIconSourceBias;
	*FieldAt(manager, OFFSET_AvatarSourceColor) = values.color;
	// Written as dwords because that is what the game's own blob -> source path
	// (FUN_0048F310) writes, even though the refresh reads only the low byte back.
	*SlotId(manager, OFFSET_AvatarAccSlotA) = values.accessory1;
	*SlotId(manager, OFFSET_AvatarAccSlotB) = values.accessory2;
	return true;
}
