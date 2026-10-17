#include "TestProbes.h"

#include "interfaces.h"
#include "logger.h"

#include "Game/gamestates.h"

#include <Windows.h>

namespace
{
	bool Enabled()
	{
		static int enabled = -1;
		if (enabled < 0)
		{
			wchar_t value[4] = {};
			const DWORD len = GetEnvironmentVariableW(L"BBCF_IM_STATE_TRACE", value, 4);
			enabled = (len > 0 && len < 4 && value[0] == L'1') ? 1 : 0;
		}
		return enabled == 1;
	}
}

namespace TestProbes
{
	void OnGameFrame()
	{
		if (!Enabled() || g_interfaces.player1.IsCharDataNullPtr() || g_interfaces.player2.IsCharDataNullPtr())
			return;

		static int lastHp1 = -1;
		static int lastHp2 = -1;
		const CharData* p1 = g_interfaces.player1.GetData();
		const CharData* p2 = g_interfaces.player2.GetData();
		if (p1->currentHP == lastHp1 && p2->currentHP == lastHp2)
			return;
		lastHp1 = p1->currentHP;
		lastHp2 = p2->currentHP;
		LOG(1, "[Probe] hp %d %d pos %d,%d %d,%d\n", p1->currentHP, p2->currentHP,
			p1->position_x, p1->position_y, p2->position_x, p2->position_y);
	}

	void OnRenderFrame()
	{
		if (!Enabled() || !g_gameVals.pGameState || *g_gameVals.pGameState != GameState_CharacterSelectionScreen ||
			!g_gameVals.stageSelect_X || !g_gameVals.stageSelect_Y)
			return;

		static int lastX = -1000;
		static int lastY = -1000;
		const int x = *g_gameVals.stageSelect_X;
		const int y = *g_gameVals.stageSelect_Y;
		if (x == lastX && y == lastY)
			return;
		lastX = x;
		lastY = y;
		LOG(1, "[Probe] stage x=%d y=%d\n", x, y);
	}
}
