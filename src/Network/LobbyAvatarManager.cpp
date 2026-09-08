#include "LobbyAvatarManager.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/Settings.h"
#include "Game/gamestates.h"
#include "Network/LobbyAvatarSource.h"
#include "Network/ProfileBlobSeal.h"

#include <Windows.h>

#include <string>

namespace
{
	// Both copies must go this long without moving under us before the blob is touched. It
	// has to outlast the gap between the two steps of the game's profile load, which fills
	// the blob with zeros and then the stored profile.
	const unsigned long long kQuietMs = 3000;

	// The phase always lasts at least this long, so a game that has not started its profile
	// load yet cannot be mistaken for one that has finished it. In a healthy session the
	// network handshake is done well inside this.
	const unsigned long long kMinMs = 8000;

	// Backstop for something keeping either copy moving forever. Writing the source costs
	// nothing, so reaching this is cheap -- unlike the 120s the old blob fight needed.
	const unsigned long long kMaxMs = 30000;

	// How long the live values must sit unchanged before they are written to settings.ini.
	const unsigned long long kSaveDebounceMs = 750;

	// Source re-applies allowed after the phase, and how long after it they are allowed.
	// Bounded only so a genuine equip-menu change is not fought for the whole session.
	const int kSourceReapplyBudget = 3;
	const unsigned long long kReapplyGraceMs = 10000;

	// Blob writes allowed per connect, including the first one. This is the ration that
	// matters: each one is a chance to land in the upload path's seal -> verify window, and
	// an unbounded supply of them is what held lobby entry for two minutes.
	const int kBlobWriteBudget = 4;
	const unsigned long long kBlobGraceMs = 30000;

	// While this feature is being validated, log both copies on a slow heartbeat for the
	// first stretch of a connect even when nothing changes, so a capture shows what the game
	// was doing between events rather than only at them.
	const unsigned long long kHeartbeatMs = 5000;
	const unsigned long long kHeartbeatWindowMs = 90000;

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

bool LobbyAvatarManager::BlobReady()
{
	return g_gameVals.playerAvatarAddr != nullptr
		&& g_gameVals.playerAvatarColAddr != nullptr
		&& g_gameVals.playerAvatarAcc1 != nullptr
		&& g_gameVals.playerAvatarAcc2 != nullptr;
}

LobbyAvatarManager::AvatarValues LobbyAvatarManager::ReadBlob()
{
	AvatarValues values;
	if (!BlobReady())
	{
		return values;
	}
	values.icon = *g_gameVals.playerAvatarAddr;
	values.color = *g_gameVals.playerAvatarColAddr;
	values.accessory1 = *g_gameVals.playerAvatarAcc1;
	values.accessory2 = *g_gameVals.playerAvatarAcc2;
	return values;
}

bool LobbyAvatarManager::WriteBlob(const AvatarValues& values)
{
	if (!BlobReady() || !IsComplete(values))
	{
		return false;
	}

	*g_gameVals.playerAvatarAddr = values.icon;
	*g_gameVals.playerAvatarColAddr = values.color;
	*g_gameVals.playerAvatarAcc1 = static_cast<BYTE>(values.accessory1);
	*g_gameVals.playerAvatarAcc2 = static_cast<BYTE>(values.accessory2);

	// These four fields live inside the checksummed profile blob the game uploads to Steam.
	// Leaving it unsealed is what broke network profiles before -- see ProfileBlobSeal.h.
	ProfileBlobSeal::Reseal();
	return true;
}

bool LobbyAvatarManager::IsComplete(const AvatarValues& values)
{
	// Anything outside the ranges the game can handle is treated as "nothing remembered"
	// rather than clamped: a bad value there is more likely a hand-edit or a half-written
	// file than an avatar the user wants back. The accessory ids reach an unbounded table
	// index in the draw path, so guessing is not worth an out-of-bounds read.
	return LobbyAvatarSource::InRange(values);
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

void LobbyAvatarManager::LogBoth(unsigned long long now, const char* what) const
{
	AvatarValues source;
	const bool haveSource = LobbyAvatarSource::Read(&source);
	const AvatarValues blob = ReadBlob();

	LOG(2, "LobbyAvatarManager: %s %.1fs | source %d/%d/%d/%d%s | blob %d/%d/%d/%d | target %d/%d/%d/%d%s | blobValid=%d blobWritesLeft=%d\n",
		what, (now - m_applyStartedTick) / 1000.0,
		source.icon, source.color, source.accessory1, source.accessory2,
		haveSource ? "" : " (UNAVAILABLE)",
		blob.icon, blob.color, blob.accessory1, blob.accessory2,
		m_target.icon, m_target.color, m_target.accessory1, m_target.accessory2,
		m_hasTarget ? "" : " (none)",
		ProfileBlobSeal::IsValid() ? 1 : 0, m_blobWriteBudget);
}

void LobbyAvatarManager::BeginApplying(const char* reason)
{
	m_applying = true;
	m_applyStartedTick = GetTickCount64();
	m_lastSourceChangeTick = m_applyStartedTick;
	m_lastBlobChangeTick = m_applyStartedTick;
	m_lastHeartbeatTick = m_applyStartedTick;
	m_hasPendingSave = false;
	m_sourceReapplyBudget = kSourceReapplyBudget;
	m_blobWriteBudget = kBlobWriteBudget;
	m_sawGameRebuildBlob = false;

	if (!LobbyAvatarSource::Read(&m_lastSource))
	{
		// No save data manager, or its slot flags did not read back the way the game's init
		// leaves them. Writing the blob alone would not stick, so do nothing at all.
		m_applying = false;
		m_applyEndedTick = m_applyStartedTick;
		m_hasTarget = false;
		m_haveLastSource = false;
		if (!m_sourceUnavailableLogged)
		{
			m_sourceUnavailableLogged = true;
			LOG(2, "LobbyAvatarManager: avatar source unavailable, not applying (%s)\n", reason);
		}
		return;
	}
	m_haveLastSource = true;
	m_lastBlob = ReadBlob();
	m_haveLastBlob = true;

	// Logged before anything is written, because "what did the source and blob hold when we
	// got here" is the question a relaunch answers: the source comes back from bbsave.dat
	// and the blob from the stored profile, and whether either kept the last set is the
	// whole point of the feature.
	LOG(2, "LobbyAvatarManager: on arrival source %d/%d/%d/%d, blob %d/%d/%d/%d, settings.ini %d/%d/%d/%d\n",
		m_lastSource.icon, m_lastSource.color, m_lastSource.accessory1, m_lastSource.accessory2,
		m_lastBlob.icon, m_lastBlob.color, m_lastBlob.accessory1, m_lastBlob.accessory2,
		Settings::settingsIni.lobbyAvatarIcon, Settings::settingsIni.lobbyAvatarColor,
		Settings::settingsIni.lobbyAvatarAccessory1, Settings::settingsIni.lobbyAvatarAccessory2);

	const AvatarValues remembered = LoadRemembered();
	m_hasTarget = IsComplete(remembered);
	if (m_hasTarget)
	{
		m_target = remembered;
		// The source is written straight away because it can never fail the handshake, and
		// because a rebuild that happens before we are done would otherwise undo us. The
		// blob is deliberately left alone until the phase ends -- see the header.
		if (LobbyAvatarSource::Write(m_target))
		{
			m_lastSource = m_target;
		}
	}

	LogBoth(m_applyStartedTick, m_hasTarget ? "applying, source written" : "watching, nothing remembered yet");
	LOG(2, "LobbyAvatarManager:   reason: %s\n", reason);
}

void LobbyAvatarManager::TickApplying(unsigned long long now)
{
	AvatarValues source;
	if (!LobbyAvatarSource::Read(&source))
	{
		m_applying = false;
		m_applyEndedTick = now;
		return;
	}
	const AvatarValues blob = ReadBlob();

	if (source != m_lastSource)
	{
		LOG(2, "LobbyAvatarManager: game moved SOURCE to %d/%d/%d/%d %.1fs into applying\n",
			source.icon, source.color, source.accessory1, source.accessory2,
			(now - m_applyStartedTick) / 1000.0);
		m_lastSource = source;
		m_lastSourceChangeTick = now;
	}

	if (blob != m_lastBlob)
	{
		// Worth naming when the blob lands exactly on the source: that is the signature of
		// the game rebuilding it (FUN_004924B0), the thing the previous design assumed
		// happened on a connect and which the capture said it does not.
		const bool matchesSource = (blob == source);
		LOG(2, "LobbyAvatarManager: game moved BLOB to %d/%d/%d/%d %.1fs into applying%s\n",
			blob.icon, blob.color, blob.accessory1, blob.accessory2,
			(now - m_applyStartedTick) / 1000.0,
			matchesSource ? "  <== equals source, game rebuilt it" : "");
		if (matchesSource)
		{
			m_sawGameRebuildBlob = true;
		}
		m_lastBlob = blob;
		m_lastBlobChangeTick = now;
	}

	if (m_hasTarget && source != m_target)
	{
		if (LobbyAvatarSource::Write(m_target))
		{
			m_lastSource = m_target;
		}
	}

	if ((now - m_lastHeartbeatTick) >= kHeartbeatMs
		&& (now - m_applyStartedTick) < kHeartbeatWindowMs)
	{
		m_lastHeartbeatTick = now;
		LogBoth(now, "applying, heartbeat");
	}

	const unsigned long long quietSince =
		m_lastSourceChangeTick > m_lastBlobChangeTick ? m_lastSourceChangeTick : m_lastBlobChangeTick;
	const bool quiet = (now - quietSince) >= kQuietMs
		&& (now - m_applyStartedTick) >= kMinMs;
	const bool timedOut = (now - m_applyStartedTick) >= kMaxMs;
	if (!quiet && !timedOut)
	{
		return;
	}

	m_applying = false;
	m_applyEndedTick = now;
	LOG(2, "LobbyAvatarManager: both copies quiet after %.1fs%s, gameRebuiltBlobOnItsOwn=%d\n",
		(now - m_applyStartedTick) / 1000.0,
		timedOut ? " (gave up waiting)" : "", m_sawGameRebuildBlob ? 1 : 0);

	// Now, and only now, the blob. The profile load has finished and the handshake is past,
	// so this is the calm period the old code never waited for.
	ApplyBlob(now, "apply phase ended");
}

void LobbyAvatarManager::ApplyBlob(unsigned long long now, const char* reason)
{
	if (!m_hasTarget)
	{
		return;
	}

	const AvatarValues blob = ReadBlob();
	if (blob == m_target)
	{
		LOG(2, "LobbyAvatarManager: blob already matches target, nothing to write (%s)\n", reason);
		return;
	}

	if (m_blobWriteBudget <= 0)
	{
		LOG(2, "LobbyAvatarManager: blob is %d/%d/%d/%d, target %d/%d/%d/%d, but the blob write budget is spent -- leaving it alone (%s)\n",
			blob.icon, blob.color, blob.accessory1, blob.accessory2,
			m_target.icon, m_target.color, m_target.accessory1, m_target.accessory2, reason);
		return;
	}

	if (!WriteBlob(m_target))
	{
		return;
	}
	--m_blobWriteBudget;
	m_lastBlob = m_target;
	m_lastBlobChangeTick = now;

	LOG(2, "LobbyAvatarManager: wrote BLOB %d/%d/%d/%d over %d/%d/%d/%d (%s), blobValid=%d, %d write(s) left\n",
		m_target.icon, m_target.color, m_target.accessory1, m_target.accessory2,
		blob.icon, blob.color, blob.accessory1, blob.accessory2, reason,
		ProfileBlobSeal::IsValid() ? 1 : 0, m_blobWriteBudget);
}

void LobbyAvatarManager::TickSettled(unsigned long long now)
{
	AvatarValues source;
	if (!LobbyAvatarSource::Read(&source))
	{
		return;
	}
	const AvatarValues blob = ReadBlob();

	if (blob != m_lastBlob)
	{
		const bool matchesSource = (blob == source);
		LOG(2, "LobbyAvatarManager: game moved BLOB to %d/%d/%d/%d after settling%s\n",
			blob.icon, blob.color, blob.accessory1, blob.accessory2,
			matchesSource ? "  <== equals source, game rebuilt it" : "");
		if (matchesSource)
		{
			m_sawGameRebuildBlob = true;
		}
		m_lastBlob = blob;
		m_lastBlobChangeTick = now;
	}

	if ((now - m_lastHeartbeatTick) >= kHeartbeatMs
		&& (now - m_applyStartedTick) < kHeartbeatWindowMs)
	{
		m_lastHeartbeatTick = now;
		LogBoth(now, "settled, heartbeat");
	}

	// Put the blob back if the game moved it off target, on the fixed ration and only for a
	// while after the phase. Once either runs out the blob is left as the game wants it: a
	// wrong accessory is a cosmetic miss, an unbounded fight blocks people out of lobbies.
	if (m_hasTarget && blob != m_target && (now - m_applyEndedTick) < kBlobGraceMs)
	{
		ApplyBlob(now, "game moved the blob off target");
	}

	if (!IsComplete(source))
	{
		return;
	}

	// A source divergence inside the grace window is more likely the game copying a
	// downloaded profile over it than the user changing their accessory, so put ours back a
	// bounded number of times before believing it.
	const bool inReapplyGrace = m_sourceReapplyBudget > 0
		&& (now - m_applyEndedTick) < kReapplyGraceMs;
	if (m_hasTarget && source != m_target && inReapplyGrace)
	{
		if (LobbyAvatarSource::Write(m_target))
		{
			--m_sourceReapplyBudget;
			m_lastSource = m_target;
			LOG(2, "LobbyAvatarManager: source moved to %d/%d/%d/%d after settling, re-applied (%d left)\n",
				source.icon, source.color, source.accessory1, source.accessory2, m_sourceReapplyBudget);
		}
		m_hasPendingSave = false;
		return;
	}

	if (!m_hasPendingSave || source != m_pendingSave)
	{
		m_pendingSave = source;
		m_pendingSaveSinceTick = now;
		m_hasPendingSave = true;
		return;
	}

	if (now - m_pendingSaveSinceTick < kSaveDebounceMs)
	{
		return;
	}

	m_hasPendingSave = false;

	if (source == LoadRemembered())
	{
		return;
	}

	// Whatever the source holds now is what the user last chose -- on the Online page, which
	// writes it through, or in the game's own equip menu, which writes it directly.
	m_target = source;
	m_hasTarget = true;
	SaveRemembered(source);
}

void LobbyAvatarManager::OnUserEdited()
{
	if (!BlobReady())
	{
		return;
	}

	if (m_applying)
	{
		LOG(2, "LobbyAvatarManager: applying ended early, user edited the avatar\n");
		m_applying = false;
	}
	m_applyEndedTick = GetTickCount64();
	m_sourceReapplyBudget = 0;

	// The sliders edit the blob, so that is where the user's intent is. Only the fields they
	// actually moved are adopted: taking all four would drag in whatever the game happens to
	// have left in the others, which is how a remembered colour got replaced by the game's
	// during testing.
	const AvatarValues blob = ReadBlob();
	AvatarValues edited = m_hasTarget ? m_target : blob;
	if (!m_haveLastBlob || blob.icon != m_lastBlob.icon) { edited.icon = blob.icon; }
	if (!m_haveLastBlob || blob.color != m_lastBlob.color) { edited.color = blob.color; }
	if (!m_haveLastBlob || blob.accessory1 != m_lastBlob.accessory1) { edited.accessory1 = blob.accessory1; }
	if (!m_haveLastBlob || blob.accessory2 != m_lastBlob.accessory2) { edited.accessory2 = blob.accessory2; }

	if (!IsComplete(edited))
	{
		return;
	}

	// Through to the source as well, or the game's next rebuild puts the old value back and
	// the edit looks like it never happened.
	LobbyAvatarSource::Write(edited);
	m_target = edited;
	m_hasTarget = true;
	m_lastSource = edited;
	m_lastBlob = blob;
	m_haveLastBlob = true;
	m_pendingSave = edited;
	m_pendingSaveSinceTick = GetTickCount64();
	m_hasPendingSave = true;
}

void LobbyAvatarManager::Tick()
{
	if (!Settings::settingsIni.rememberLobbyAvatar)
	{
		return;
	}

	if (!BlobReady())
	{
		// Before the first network connection there is nothing to put back on. The game
		// never tears these back down once built, so this only covers the pre-connect part
		// of a launch.
		return;
	}

	if (!m_addressesSeen)
	{
		m_addressesSeen = true;
		BeginApplying("connected to network mode");
	}

	// A lobby entry is the other moment the game rewrites the profile, e.g. after leaving
	// network mode and going back in within the same launch.
	if (g_gameVals.pGameState)
	{
		const int gameState = *g_gameVals.pGameState;
		if (gameState != m_lastGameState)
		{
			if (gameState == GameState_Lobby && m_lastGameState != -1)
			{
				BeginApplying("entered a lobby");
			}
			m_lastGameState = gameState;
		}
	}

	const unsigned long long now = GetTickCount64();

	if (m_applying)
	{
		TickApplying(now);
		return;
	}

	TickSettled(now);
}
