#include "Game/TrainingSide.h"

#include "Core/interfaces.h"
#include "Core/utils.h"

namespace
{
	constexpr int kTrainingSideOffset = 0x891A38;
}

namespace TrainingSide
{
	unsigned char* SidePointer()
	{
		char* base = GetBbcfBaseAdress();
		return base ? reinterpret_cast<unsigned char*>(base + kTrainingSideOffset) : nullptr;
	}

	int HumanIndex()
	{
		const unsigned char* side = SidePointer();
		return (side && *side == 1) ? 1 : 0;
	}

	int DummyIndex()
	{
		return 1 - HumanIndex();
	}

	Player& Human()
	{
		return HumanIndex() == 0 ? g_interfaces.player1 : g_interfaces.player2;
	}

	Player& Dummy()
	{
		return HumanIndex() == 0 ? g_interfaces.player2 : g_interfaces.player1;
	}

	int DummyPlayerNum()
	{
		return DummyIndex() + 1;
	}
}
