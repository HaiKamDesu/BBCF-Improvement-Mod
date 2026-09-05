#include "LobbyAvatarManager.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/Settings.h"
#include "Game/gamestates.h"
#include "Network/ProfileBlobSeal.h"

#include <Windows.h>

#include <string>

namespace
{
	// Settling is finished once the avatar fields have gone this long without changing.
	// The game fills them in a few seconds after they become readable, in two steps (zeros,
	// then the profile's real avatar), so the quiet period has to outlast the gap between
	// those steps -- measured at ~1.5s -- rather than the delay before the first one.
	const unsigned long long kSettleQuietMs = 5000;

	// Settling always lasts at least this long, so a game that has not started its load yet
	// cannot be mistaken for one that has finished it. Anyone who reaches the Online page
	// inside this window ends settling by touching a slider, so it costs nothing.
	const unsigned long long kSettleMinMs = 15000;

	// Backstop for the case where something keeps the values moving forever: settling ends
	// regardless, rather than leaving the mod writing over the user for the whole session.
	const unsigned long long kSettleMaxMs = 120000;

	// How long the live values must sit unchanged before they are written to settings.ini.
	const unsigned long long kSaveDebounceMs = 750;

	// The ranges the Online page's sliders offer. Anything outside them in settings.ini is
	// treated as "nothing remembered" rather than clamped: a bad value there is more likely
	// a hand-edit or a half-written file than an avatar the user wants back.
	const int kMaxIcon = 0x2F;
	const int kMaxColor = 0x3;
	const int kMaxAccessory = 0xCF;

	bool InRange(int value, int max)
	{
		return value >= 0 && value <= max;
	}

	void SaveInt(const char* iniKey, int value, int& settingsField)
	{
		Settings::changeSetting(iniKey, std::to_string(value));
		settingsField = value;
	}
}

LobbyAvatarManager& LobbyAvatarManager::GetInstance()
{
	static LobbyAvatarManager instance;
	return instance;
}

bool LobbyAvatarManager::AddressesReady()
{
	return g_gameVals.playerAvatarAddr != nullptr
		&& g_gameVals.playerAvatarColAddr != nullptr
		&& g_gameVals.playerAvatarAcc1 != nullptr
		&& g_gameVals.playerAvatarAcc2 != nullptr;
}

LobbyAvatarManager::AvatarValues LobbyAvatarManager::ReadLive()
{
	AvatarValues values;
	values.icon = *g_gameVals.playerAvatarAddr;
	values.color = *g_gameVals.playerAvatarColAddr;
	values.accessory1 = *g_gameVals.playerAvatarAcc1;
	values.accessory2 = *g_gameVals.playerAvatarAcc2;
	return values;
}

void LobbyAvatarManager::WriteLive(const AvatarValues& values)
{
	*g_gameVals.playerAvatarAddr = values.icon;
	*g_gameVals.playerAvatarColAddr = values.color;
	*g_gameVals.playerAvatarAcc1 = static_cast<BYTE>(values.accessory1);
	*g_gameVals.playerAvatarAcc2 = static_cast<BYTE>(values.accessory2);

	// These four fields live inside the checksummed profile blob the game uploads to Steam.
	// Leaving it unsealed is what broke network profiles before -- see ProfileBlobSeal.h.
	ProfileBlobSeal::Reseal();
}

bool LobbyAvatarManager::IsComplete(const AvatarValues& values)
{
	return InRange(values.icon, kMaxIcon)
		&& InRange(values.color, kMaxColor)
		&& InRange(values.accessory1, kMaxAccessory)
		&& InRange(values.accessory2, kMaxAccessory);
}

LobbyAvatarManager::AvatarValues LobbyAvatarManager::LoadRemembered() const
{
	AvatarValues values;
	values.icon = Settings::settingsIni.lobbyAvatarIcon;
	values.color = Settings::settingsIni.lobbyAvatarColor;
	values.accessory1 = Settings::settingsIni.lobbyAvatarAccessory1;
	values.accessory2 = Settings::settingsIni.lobbyAvatarAccessory2;
	return values;
}

void LobbyAvatarManager::SaveRemembered(const AvatarValues& values)
{
	SaveInt("LobbyAvatarIcon", values.icon, Settings::settingsIni.lobbyAvatarIcon);
	SaveInt("LobbyAvatarColor", values.color, Settings::settingsIni.lobbyAvatarColor);
	SaveInt("LobbyAvatarAccessory1", values.accessory1, Settings::settingsIni.lobbyAvatarAccessory1);
	SaveInt("LobbyAvatarAccessory2", values.accessory2, Settings::settingsIni.lobbyAvatarAccessory2);

	LOG(2, "LobbyAvatarManager: remembered avatar %d, colour %d, accessories %d/%d\n",
		values.icon, values.color, values.accessory1, values.accessory2);
}

void LobbyAvatarManager::BeginSettling(const char* reason)
{
	m_settling = true;
	m_settlingStartedTick = GetTickCount64();
	m_lastChangeTick = m_settlingStartedTick;
	m_lastSeen = ReadLive();
	m_hasPendingSave = false;

	const AvatarValues remembered = LoadRemembered();
	m_hasTarget = IsComplete(remembered);
	if (m_hasTarget)
	{
		m_target = remembered;
		WriteLive(m_target);
		// Our own write must not read back as the game having moved the values, or the
		// first tick of settling would restart the quiet period for no reason.
		m_lastSeen = m_target;
		LOG(2, "LobbyAvatarManager: profile blob resealed, valid=%d\n", ProfileBlobSeal::IsValid() ? 1 : 0);
		LOG(2, "LobbyAvatarManager: settling, re-applying avatar %d, colour %d, accessories %d/%d (%s)\n",
			m_target.icon, m_target.color, m_target.accessory1, m_target.accessory2, reason);
	}
	else
	{
		// Nothing saved yet (a first run, or a hand-edited settings.ini). Still settle:
		// whatever the game loads must not be written back to settings.ini as though the
		// user had picked it, and it must not be picked up mid-load either.
		LOG(2, "LobbyAvatarManager: settling, nothing remembered yet (%s)\n", reason);
	}
}

void LobbyAvatarManager::TickSettling(unsigned long long now)
{
	const AvatarValues live = ReadLive();

	if (live != m_lastSeen)
	{
		// The game wrote to the struct. This is the event the whole phase exists to wait
		// for, and it happens more than once per connect.
		LOG(2, "LobbyAvatarManager: game set avatar %d, colour %d, accessories %d/%d %.1fs into settling\n",
			live.icon, live.color, live.accessory1, live.accessory2,
			(now - m_settlingStartedTick) / 1000.0);
		m_lastSeen = live;
		m_lastChangeTick = now;
	}

	if (m_hasTarget && live != m_target)
	{
		WriteLive(m_target);
		m_lastSeen = m_target;
	}

	const bool quiet = (now - m_lastChangeTick) >= kSettleQuietMs
		&& (now - m_settlingStartedTick) >= kSettleMinMs;
	const bool timedOut = (now - m_settlingStartedTick) >= kSettleMaxMs;
	if (!quiet && !timedOut)
	{
		return;
	}

	m_settling = false;
	m_lastSeen = ReadLive();
	// blobValid is the thing to check if network profiles ever misbehave again: 0 here means
	// something wrote into the profile blob without resealing it, and uploads will fail for
	// the rest of the process. See ProfileBlobSeal.h.
	LOG(2, "LobbyAvatarManager: settled after %.1fs on avatar %d, colour %d, accessories %d/%d, blobValid=%d%s\n",
		(now - m_settlingStartedTick) / 1000.0,
		m_lastSeen.icon, m_lastSeen.color, m_lastSeen.accessory1, m_lastSeen.accessory2,
		ProfileBlobSeal::IsValid() ? 1 : 0,
		timedOut ? " (gave up waiting for it to hold still)" : "");
}

void LobbyAvatarManager::TickRemember(unsigned long long now)
{
	// Settling is over, so the live values are whatever the user last chose -- on the
	// Online page or in the game's own equip menu -- rather than a half-finished load.
	const AvatarValues live = ReadLive();
	if (!IsComplete(live))
	{
		return;
	}

	if (!m_hasPendingSave || live != m_pendingSave)
	{
		m_pendingSave = live;
		m_pendingSaveSinceTick = now;
		m_hasPendingSave = true;
		return;
	}

	if (now - m_pendingSaveSinceTick < kSaveDebounceMs)
	{
		return;
	}

	m_hasPendingSave = false;

	if (live == LoadRemembered())
	{
		return;
	}

	SaveRemembered(live);
}

void LobbyAvatarManager::OnUserEdited()
{
	if (!AddressesReady())
	{
		return;
	}

	// The user's hand beats a re-apply that is still running: end settling rather than
	// spend the rest of it dragging the slider back.
	if (m_settling)
	{
		LOG(2, "LobbyAvatarManager: settling ended early, user edited the avatar\n");
		m_settling = false;
	}

	m_pendingSave = ReadLive();
	m_pendingSaveSinceTick = GetTickCount64();
	m_hasPendingSave = true;
}

void LobbyAvatarManager::Tick()
{
	if (!Settings::settingsIni.rememberLobbyAvatar)
	{
		return;
	}

	if (!AddressesReady())
	{
		// Before the first network connection there is nothing to read or write. The game
		// never tears these back down once built, so this only covers the pre-connect part
		// of a launch.
		return;
	}

	if (!m_addressesSeen)
	{
		m_addressesSeen = true;
		BeginSettling("connected to network mode");
	}

	// A lobby entry is the other moment the game can load the profile's avatar over the
	// top, e.g. after leaving network mode and going back in within the same launch.
	if (g_gameVals.pGameState)
	{
		const int gameState = *g_gameVals.pGameState;
		if (gameState != m_lastGameState)
		{
			if (gameState == GameState_Lobby && m_lastGameState != -1)
			{
				BeginSettling("entered a lobby");
			}
			m_lastGameState = gameState;
		}
	}

	const unsigned long long now = GetTickCount64();

	if (m_settling)
	{
		TickSettling(now);
		return;
	}

	TickRemember(now);
}
