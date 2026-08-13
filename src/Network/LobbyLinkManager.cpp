#include "LobbyLinkManager.h"

#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/logger.h"
#include "Core/HotkeyManager.h"
#include "Core/Settings.h"
#include "Core/utils.h"
#include "Network/RoomManager.h"
#include "Overlay/NotificationBar/NotificationBar.h"
#include "SteamApiWrapper/SteamMatchmakingWrapper.h"
#include "SteamApiWrapper/SteamUserWrapper.h"

#include <Windows.h>
#include <shellapi.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace
{
	const char* const kBbcfAppId = "586140";

	// A LobbyEnter_t failure is only attributed to our paste if it lands inside this
	// window; beyond it, the failure is more likely from something the user did in the
	// menus and reporting it as a link failure would be misleading.
	const unsigned long long kPasteOutcomeWindowMs = 15000;

	const char* ChatRoomEnterResponseName(unsigned int response)
	{
		switch (response)
		{
		case k_EChatRoomEnterResponseSuccess:          return "Success";
		case k_EChatRoomEnterResponseDoesntExist:      return "DoesntExist";
		case k_EChatRoomEnterResponseNotAllowed:       return "NotAllowed";
		case k_EChatRoomEnterResponseFull:             return "Full";
		case k_EChatRoomEnterResponseError:            return "Error";
		case k_EChatRoomEnterResponseBanned:           return "Banned";
		case k_EChatRoomEnterResponseLimited:          return "Limited";
		case k_EChatRoomEnterResponseClanDisabled:     return "ClanDisabled";
		case k_EChatRoomEnterResponseCommunityBan:     return "CommunityBan";
		case k_EChatRoomEnterResponseMemberBlockedYou: return "MemberBlockedYou";
		case k_EChatRoomEnterResponseYouBlockedMember: return "YouBlockedMember";
		default:                                       return "Unknown";
		}
	}

	void Notify(std::function<std::string()> formatter)
	{
		if (g_notificationBar)
		{
			g_notificationBar->AddLocalizedNotification(std::move(formatter));
		}
	}

	// Formats a localized string that carries a single %s placeholder.
	std::string FormatLocalized(const char* format, const std::string& arg)
	{
		if (!format)
		{
			return arg;
		}

		const int size = std::snprintf(nullptr, 0, format, arg.c_str()) + 1;
		if (size <= 1)
		{
			return arg;
		}

		std::string formatted(static_cast<size_t>(size), ' ');
		std::snprintf(&formatted[0], static_cast<size_t>(size), format, arg.c_str());
		formatted.resize(static_cast<size_t>(size) - 1); // drop the NUL snprintf wrote
		return formatted;
	}

	// The clipboard is a process-wide lock that browsers and Discord grab in bursts, so a
	// single OpenClipboard failure is normal rather than fatal. Retry briefly.
	bool OpenClipboardWithRetry(HWND owner)
	{
		for (int attempt = 0; attempt < 5; ++attempt)
		{
			if (OpenClipboard(owner))
			{
				return true;
			}
			Sleep(10);
		}
		return false;
	}

	bool SetClipboardText(const std::string& text)
	{
		const int wideLength = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
		if (wideLength <= 0)
		{
			return false;
		}

		if (!OpenClipboardWithRetry(g_gameProc.hWndGameWindow))
		{
			return false;
		}

		bool succeeded = false;
		if (EmptyClipboard())
		{
			// GMEM_MOVEABLE + GlobalLock is required: the clipboard takes ownership of
			// the handle, so it must not be freed on success.
			const HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wideLength) * sizeof(wchar_t));
			if (handle)
			{
				if (wchar_t* const dest = static_cast<wchar_t*>(GlobalLock(handle)))
				{
					MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, dest, wideLength);
					GlobalUnlock(handle);

					if (SetClipboardData(CF_UNICODETEXT, handle))
					{
						succeeded = true;
					}
				}

				if (!succeeded)
				{
					GlobalFree(handle);
				}
			}
		}

		CloseClipboard();
		return succeeded;
	}

	bool GetClipboardText(std::string* outText)
	{
		if (!outText)
		{
			return false;
		}

		if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
		{
			return false;
		}

		if (!OpenClipboardWithRetry(g_gameProc.hWndGameWindow))
		{
			return false;
		}

		bool succeeded = false;
		if (const HANDLE handle = GetClipboardData(CF_UNICODETEXT))
		{
			if (const wchar_t* const source = static_cast<const wchar_t*>(GlobalLock(handle)))
			{
				const int size = WideCharToMultiByte(CP_UTF8, 0, source, -1, nullptr, 0, nullptr, nullptr);
				if (size > 0)
				{
					std::string converted(static_cast<size_t>(size), '\0');
					WideCharToMultiByte(CP_UTF8, 0, source, -1, &converted[0], size, nullptr, nullptr);
					converted.resize(static_cast<size_t>(size) - 1); // drop the NUL
					*outText = std::move(converted);
					succeeded = true;
				}
				GlobalUnlock(handle);
			}
		}

		CloseClipboard();
		return succeeded;
	}

	// Reads a run of decimal digits starting at `pos`, advancing it past them.
	uint64_t ReadDigits(const std::string& text, size_t* pos)
	{
		const size_t start = *pos;
		uint64_t value = 0;
		while (*pos < text.size() && std::isdigit(static_cast<unsigned char>(text[*pos])))
		{
			value = value * 10 + static_cast<uint64_t>(text[*pos] - '0');
			++(*pos);
		}
		return (*pos > start) ? value : 0;
	}

}

std::string LobbyLink::FormatJoinUrl(uint64_t lobbyId, uint64_t memberSteamId)
{
	if (lobbyId == 0 || memberSteamId == 0)
	{
		return std::string();
	}

	char buffer[128] = {};
	std::snprintf(buffer, sizeof(buffer), "steam://joinlobby/%s/%llu/%llu",
		kBbcfAppId,
		static_cast<unsigned long long>(lobbyId),
		static_cast<unsigned long long>(memberSteamId));
	return std::string(buffer);
}

uint64_t LobbyLink::ParseLobbyIdFromText(const std::string& text, uint64_t* outMemberSteamId)
{
	if (outMemberSteamId)
	{
		*outMemberSteamId = 0;
	}

	// Deliberately scans for the marker anywhere rather than matching the whole string:
	// people paste a Discord message with words wrapped around the link, and Steam's own
	// copy sometimes carries a trailing newline.
	static const std::string marker = "joinlobby/";

	size_t searchFrom = 0;
	while (true)
	{
		const size_t markerPos = text.find(marker, searchFrom);
		if (markerPos == std::string::npos)
		{
			return 0;
		}

		size_t pos = markerPos + marker.size();

		const size_t appIdStart = pos;
		const uint64_t appId = ReadDigits(text, &pos);
		const bool appIdPresent = (pos > appIdStart);

		// Only BBCF links. A link for another game would otherwise be handed to Steam and
		// silently launch or focus that game instead.
		if (appIdPresent && appId == std::strtoull(kBbcfAppId, nullptr, 10) &&
			pos < text.size() && text[pos] == '/')
		{
			++pos;
			const size_t lobbyStart = pos;
			const uint64_t lobbyId = ReadDigits(text, &pos);
			if (pos > lobbyStart && lobbyId != 0)
			{
				// The trailing member ID is optional: Steam's own links always carry it,
				// but a hand-trimmed link still identifies the lobby well enough.
				if (outMemberSteamId && pos < text.size() && text[pos] == '/')
				{
					++pos;
					*outMemberSteamId = ReadDigits(text, &pos);
				}
				return lobbyId;
			}
		}

		searchFrom = markerPos + marker.size();
	}
}

bool LobbyLink::IsShareableRoomType(RoomType type)
{
	// Deny-list rather than allow-list, deliberately.
	//
	// The room the community actually shares -- a player-created room with match and
	// spectator slots -- reports RoomType_MatchSpectate (0x11), not RoomType_Lobby. An
	// allow-list built from the names in Room.h got that wrong, and since that enum is
	// incomplete reverse engineering, any type we haven't catalogued yet is far more
	// likely to be another player-room variant than a matchmaking one.
	//
	// So block only what is known not to be invitable: ranked (matchmaking-owned; handing
	// out that link would drop a stranger into your ranked set) and the replay theater
	// (nothing to join). Everything else is treated as shareable.
	switch (type)
	{
	case RoomType_Ranked:
	case RoomType_Replay:
		return false;
	default:
		return true;
	}
}

LobbyLinkManager& LobbyLinkManager::GetInstance()
{
	// Function-local static: the STEAM_CALLBACK members register themselves on
	// construction, so the instance must not exist before SteamAPI is up. Every caller
	// (the frame hook, the matchmaking wrapper) runs well after initialization.
	static LobbyLinkManager instance;
	return instance;
}

void LobbyLinkManager::CacheLobbyId(uint64_t lobbyId, const char* source)
{
	if (lobbyId == 0 || m_currentLobbyId == lobbyId)
	{
		return;
	}

	m_currentLobbyId = lobbyId;
	LOG(1, "[LobbyLink] current lobby = %llu (via %s)\n",
		static_cast<unsigned long long>(lobbyId), source ? source : "?");
}

bool LobbyLinkManager::IsLocalPlayerInLobby(uint64_t lobbyId) const
{
	if (lobbyId == 0 || !g_interfaces.pSteamMatchmakingWrapper || !g_interfaces.pSteamUserWrapper)
	{
		return false;
	}

	const CSteamID localId = g_interfaces.pSteamUserWrapper->GetSteamID();
	const CSteamID lobby(lobbyId);
	const int memberCount = g_interfaces.pSteamMatchmakingWrapper->GetNumLobbyMembers(lobby);
	for (int i = 0; i < memberCount; ++i)
	{
		if (g_interfaces.pSteamMatchmakingWrapper->GetLobbyMemberByIndex(lobby, i) == localId)
		{
			return true;
		}
	}
	return false;
}

void LobbyLinkManager::OnLobbyEnter(LobbyEnter_t* pParam)
{
	if (!pParam)
	{
		return;
	}

	const unsigned int response = static_cast<unsigned int>(pParam->m_EChatRoomEnterResponse);
	if (response == k_EChatRoomEnterResponseSuccess)
	{
		CacheLobbyId(pParam->m_ulSteamIDLobby, "LobbyEnter");
		m_pasteJoinTickMs = 0;
		return;
	}

	// Only speak up for a join we started. The game issues plenty of its own joins
	// (ranked search, lobby list) whose failures it already reports in its own UI.
	if (m_pasteJoinTickMs == 0 || GetTickCount64() - m_pasteJoinTickMs > kPasteOutcomeWindowMs)
	{
		return;
	}
	m_pasteJoinTickMs = 0;

	const std::string reason = ChatRoomEnterResponseName(response);
	LOG(1, "[LobbyLink] pasted link join failed: response=%u(%s) lobby=%llu\n",
		response, reason.c_str(), static_cast<unsigned long long>(pParam->m_ulSteamIDLobby));

	Notify([reason]() {
		return FormatLocalized(Messages.Lobby_link_join_failed(), reason);
	});
}

void LobbyLinkManager::OnLobbyDataUpdate(LobbyDataUpdate_t* pParam)
{
	// Recovery path for a missed LobbyEnter_t. This fires repeatedly for the lobby you
	// are sitting in, so membership is re-checked rather than trusted blindly -- the same
	// callback also fires for lobbies we merely queried from the lobby list.
	if (!pParam || m_currentLobbyId != 0 || pParam->m_bSuccess == 0)
	{
		return;
	}

	if (IsLocalPlayerInLobby(pParam->m_ulSteamIDLobby))
	{
		CacheLobbyId(pParam->m_ulSteamIDLobby, "LobbyDataUpdate recovery");
	}
}

void LobbyLinkManager::OnLeaveLobby(uint64_t lobbyId)
{
	if (lobbyId != 0 && lobbyId != m_currentLobbyId)
	{
		return;
	}

	if (m_currentLobbyId != 0)
	{
		LOG(1, "[LobbyLink] cleared cached lobby %llu (left)\n",
			static_cast<unsigned long long>(m_currentLobbyId));
	}
	m_currentLobbyId = 0;
}

bool LobbyLinkManager::CanShareCurrentRoom() const
{
	if (m_currentLobbyId == 0 || !g_interfaces.pRoomManager)
	{
		return false;
	}

	if (!g_interfaces.pRoomManager->IsRoomFunctional())
	{
		return false;
	}

	RoomType roomType = RoomType_Ranked;
	if (!g_interfaces.pRoomManager->TryGetRoomType(&roomType))
	{
		return false;
	}

	return LobbyLink::IsShareableRoomType(roomType);
}

void LobbyLinkManager::CopyCurrentLobbyLink()
{
	if (!CanShareCurrentRoom())
	{
		RoomType roomType = RoomType_Ranked;
		const bool roomTypeKnown = g_interfaces.pRoomManager &&
			g_interfaces.pRoomManager->TryGetRoomType(&roomType);

		LOG(1, "[LobbyLink] copy rejected: lobby=%llu roomFunctional=%d roomType=%s(0x%02X)\n",
			static_cast<unsigned long long>(m_currentLobbyId),
			(g_interfaces.pRoomManager && g_interfaces.pRoomManager->IsRoomFunctional()) ? 1 : 0,
			roomTypeKnown ? g_interfaces.pRoomManager->GetRoomTypeName().c_str() : "<unreadable>",
			roomTypeKnown ? static_cast<unsigned int>(roomType) : 0u);
		Notify([]() { return std::string(Messages.Lobby_link_no_room()); });
		return;
	}

	const uint64_t localSteamId = g_interfaces.pSteamUserWrapper
		? g_interfaces.pSteamUserWrapper->GetSteamID().ConvertToUint64()
		: 0ull;

	const std::string url = LobbyLink::FormatJoinUrl(m_currentLobbyId, localSteamId);
	if (url.empty())
	{
		LOG(1, "[LobbyLink] copy failed: could not format url (lobby=%llu steamId=%llu)\n",
			static_cast<unsigned long long>(m_currentLobbyId),
			static_cast<unsigned long long>(localSteamId));
		Notify([]() { return std::string(Messages.Lobby_link_no_room()); });
		return;
	}

	if (!SetClipboardText(url))
	{
		LOG(1, "[LobbyLink] copy failed: clipboard unavailable (GLE=%lu)\n", GetLastError());
		Notify([]() { return std::string(Messages.Lobby_link_clipboard_failed()); });
		return;
	}

	LOG(1, "[LobbyLink] copied %s\n", url.c_str());
	Notify([]() { return std::string(Messages.Lobby_link_copied()); });
}

void LobbyLinkManager::JoinLobbyFromClipboard()
{
	std::string clipboard;
	if (!GetClipboardText(&clipboard))
	{
		LOG(1, "[LobbyLink] paste failed: clipboard unavailable or not text\n");
		Notify([]() { return std::string(Messages.Lobby_link_clipboard_failed()); });
		return;
	}

	uint64_t linkMemberSteamId = 0;
	const uint64_t lobbyId = LobbyLink::ParseLobbyIdFromText(clipboard, &linkMemberSteamId);
	if (lobbyId == 0)
	{
		LOG(1, "[LobbyLink] paste failed: no BBCF lobby link in clipboard\n");
		Notify([]() { return std::string(Messages.Lobby_link_not_found()); });
		return;
	}

	if (lobbyId == m_currentLobbyId)
	{
		LOG(1, "[LobbyLink] paste ignored: already in lobby %llu\n",
			static_cast<unsigned long long>(lobbyId));
		Notify([]() { return std::string(Messages.Lobby_link_already_here()); });
		return;
	}

	// Rebuild the URL from the parsed fields rather than forwarding the clipboard text
	// verbatim: whatever was copied may carry trailing punctuation from a chat message,
	// and this guarantees the app ID we hand to the Steam client is BBCF's. The member ID
	// is kept as-is (it names someone already in the target lobby); when the link had
	// none, fall back to the lobby ID, which is what Steam resolves the join from anyway.
	const std::string url = LobbyLink::FormatJoinUrl(
		lobbyId, linkMemberSteamId != 0 ? linkMemberSteamId : lobbyId);

	std::wstring wideUrl(url.begin(), url.end()); // ASCII by construction

	// Hand off to the Steam client, which posts GameLobbyJoinRequested_t to the game.
	// BBCF then drives its own leave/join and shows its own error popup on failure --
	// exactly the path a browser-pasted link takes today.
	const HINSTANCE result = ShellExecute(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(result) <= 32)
	{
		LOG(1, "[LobbyLink] paste failed: ShellExecute returned %d for %s\n",
			static_cast<int>(reinterpret_cast<INT_PTR>(result)), url.c_str());
		Notify([]() { return std::string(Messages.Lobby_link_open_failed()); });
		return;
	}

	m_pasteJoinTickMs = GetTickCount64();
	LOG(1, "[LobbyLink] requested join for lobby %llu via %s\n",
		static_cast<unsigned long long>(lobbyId), url.c_str());
	Notify([]() { return std::string(Messages.Lobby_link_joining()); });
}

void LobbyLinkManager::Tick()
{
	if (!Settings::settingsIni.lobbyLinkHotkeysEnabled)
	{
		return;
	}

	// Foreground-window and typing gating, exact modifier matching and press-edge tracking
	// all live in HotkeyManager now, which is also why the Ctrl these shortcuts need is a
	// real part of the binding instead of a hardcoded check here.
	if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_CopyLobbyLink))
	{
		CopyCurrentLobbyLink();
	}

	if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_JoinLobbyLink))
	{
		JoinLobbyFromClipboard();
	}
}
