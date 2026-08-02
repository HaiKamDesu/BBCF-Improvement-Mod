#include "RankedLeaderboardWindow.h"

#include "RankedProgressWindow.h" // FormatVisibleRankLabel / GetVisibleRankColor / ComputeTotalLpFromPackedScore

#include "Core/interfaces.h"      // g_interfaces + (transitively) the Steamworks headers
#include "Core/Localization.h"
#include "Core/logger.h"
#include "Game/characters.h"      // getCharacterNameByIndexA

#include "Overlay/imgui_utils.h"

#include <imgui.h>

#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	// Character-id sentinel for the overall "RANK_ALL" board. Mirrors
	// kRankAllCharacterId in RankedProgressWindow.cpp.
	constexpr uint32_t kAllCharacterId = 64u;
	constexpr int kNumRankedCharacters = 36; // entries in the RANK_<code> table

	// Per-character 2-letter Steam leaderboard codes, indexed by BBCF character id.
	// Kept in sync with GetRankLeaderboardCode() in RankedProgressWindow.cpp.
	const char* LeaderboardCode(uint32_t characterId)
	{
		static const char* kCodes[] =
		{
			"RG", "JN", "NL", "RC", "TK", "TG",
			"LI", "AR", "BG", "CA", "HK", "NU",
			"TB", "HZ", "MU", "MK", "VN", "PL",
			"RL", "IY", "AM", "BL", "AZ", "KG",
			"KK", "TM", "CE", "LA", "HB", "NI",
			"NT", "IZ", "SU", "ES", "MA", "JB",
		};
		if (characterId < (sizeof(kCodes) / sizeof(kCodes[0])))
			return kCodes[characterId];
		if (characterId == kAllCharacterId)
			return "ALL";
		return nullptr;
	}

	std::string LeaderboardName(uint32_t characterId)
	{
		const char* code = LeaderboardCode(characterId);
		return code ? (std::string("RANK_") + code) : std::string();
	}

	std::string CharacterLabel(uint32_t characterId)
	{
		if (characterId == kAllCharacterId)
			return L("All characters");
		if (characterId < static_cast<uint32_t>(kNumRankedCharacters))
			return getCharacterNameByIndexA(static_cast<int>(characterId));
		return "?";
	}

	struct LeaderboardRow
	{
		uint64_t steamId = 0;
		int32_t  globalRank = 0;
		int32_t  score = 0;
		uint16_t internalRank = 0; // (score >> 16)
		uint16_t packedSubscore = 0; // (score & 0xFFFF), within-tier raw LP, NOT the total
		uint32_t totalLp = 0;      // decoded total LP, same number the ranked progress overlay shows
		bool     hasTotalLp = false;
		uint8_t  characterId = 0xFF;
		std::string name;
		int      personaState = 0; // EPersonaState; 0 = offline/unknown
	};

	// Owns all Steam async state for the leaderboard window. A single instance
	// backs the (singleton) window; CCallResult members stay valid because the
	// instance lives for the whole process.
	class LeaderboardModel
	{
	public:
		enum class Scope { Global, Friends };

		// --- user-controlled view state ---
		uint32_t selectedCharacterId = kAllCharacterId;
		Scope    scope = Scope::Global;
		int      pageSize = 50;
		int      pageStart = 1; // 1-based global rank of the first row on the page

		// --- exposed status ---
		int         totalEntries = 0;
		bool        downloadPending = false;
		bool        findPending = false;
		bool        selfRankPending = false;
		std::string statusError;
		std::vector<LeaderboardRow> rows;

		void MarkNeedsDownload() { m_dirty = true; }

		void RequestJumpToMe() { m_jumpRequested = true; }

		// Called every frame while the window is visible.
		void Tick()
		{
			statusError.clear();

			SteamUserStatsWrapper* stats = g_interfaces.pSteamUserStatsWrapper;
			SteamUserWrapper* user = g_interfaces.pSteamUserWrapper;
			if (!stats || !user)
			{
				statusError = L("Steam is not available.");
				return;
			}

			const double now = ImGui::GetTime();

			// Resolve the leaderboard handle for the current character selection.
			auto it = m_handles.find(selectedCharacterId);
			if (it != m_handles.end() && it->second)
			{
				if (m_currentHandle != it->second)
				{
					m_currentHandle = it->second;
					totalEntries = stats->GetLeaderboardEntryCount(m_currentHandle);
					m_dirty = true;
				}
			}
			else
			{
				m_currentHandle = 0;
				if (!findPending && now > m_lastFindAttempt + 5.0)
				{
					const std::string name = LeaderboardName(selectedCharacterId);
					SteamAPICall_t call = name.empty() ? 0 : stats->FindLeaderboard(name.c_str());
					if (call)
					{
						findPending = true;
						m_lastFindAttempt = now;
						m_findCharacterId = selectedCharacterId;
						m_findResult.Set(call, this, &LeaderboardModel::OnLeaderboardFound);
					}
				}
				statusError = L("Loading leaderboard...");
				return;
			}

			// Refresh the total count (cheap, cached client-side by Steam).
			if (m_currentHandle)
			{
				const int c = stats->GetLeaderboardEntryCount(m_currentHandle);
				if (c > 0)
					totalEntries = c;
			}

			// Jump-to-me: look up the local player's position, then page to it.
			if (m_jumpRequested && !selfRankPending && m_currentHandle)
			{
				CSteamID localId = user->GetSteamID();
				SteamAPICall_t call = stats->DownloadLeaderboardEntriesForUsers(m_currentHandle, &localId, 1);
				if (call)
				{
					selfRankPending = true;
					m_jumpRequested = false;
					m_selfRankResult.Set(call, this, &LeaderboardModel::OnSelfRankDownloaded);
				}
			}

			// Download the current page / friends set.
			if (m_dirty && m_currentHandle && !downloadPending)
			{
				ClampPage();
				ELeaderboardDataRequest req = (scope == Scope::Friends)
					? k_ELeaderboardDataRequestFriends
					: k_ELeaderboardDataRequestGlobal;
				const int start = (scope == Scope::Friends) ? 0 : pageStart;
				const int end = (scope == Scope::Friends) ? 0 : (pageStart + pageSize - 1);

				SteamAPICall_t call = stats->DownloadLeaderboardEntries(m_currentHandle, req, start, end);
				if (call)
				{
					downloadPending = true;
					m_dirty = false;
					m_downloadResult.Set(call, this, &LeaderboardModel::OnEntriesDownloaded);
				}
			}

			ResolvePersonaInfo();
		}

		int PageCount() const
		{
			if (pageSize <= 0 || totalEntries <= 0)
				return 1;
			return (totalEntries + pageSize - 1) / pageSize;
		}

		uint64_t LocalSteamId() const
		{
			return g_interfaces.pSteamUserWrapper
				? g_interfaces.pSteamUserWrapper->GetSteamID().ConvertToUint64()
				: 0ull;
		}

	private:
		void ClampPage()
		{
			if (pageStart < 1)
				pageStart = 1;
			if (totalEntries > 0 && pageStart > totalEntries)
				pageStart = ((totalEntries - 1) / pageSize) * pageSize + 1;
		}

		void OnLeaderboardFound(LeaderboardFindResult_t* cb, bool ioFailure)
		{
			findPending = false;
			if (ioFailure || !cb || !cb->m_bLeaderboardFound)
			{
				statusError = L("Leaderboard not found.");
				LOG(1, "[RANK][LeaderboardWin] find failed char=%u ioFailure=%d\n",
					static_cast<unsigned int>(m_findCharacterId), ioFailure ? 1 : 0);
				return;
			}
			m_handles[m_findCharacterId] = cb->m_hSteamLeaderboard;
			m_dirty = true;
		}

		void OnEntriesDownloaded(LeaderboardScoresDownloaded_t* cb, bool ioFailure)
		{
			downloadPending = false;
			rows.clear();
			if (ioFailure || !cb)
			{
				statusError = L("Failed to download leaderboard entries.");
				return;
			}

			SteamUserStatsWrapper* stats = g_interfaces.pSteamUserStatsWrapper;
			if (!stats)
				return;

			for (int i = 0; i < cb->m_cEntryCount; ++i)
			{
				LeaderboardEntry_t entry{};
				int32 details[4] = {};
				if (!stats->GetDownloadedLeaderboardEntryQuiet(cb->m_hSteamLeaderboardEntries, i, &entry, details, 4))
					continue;

				LeaderboardRow row;
				row.steamId = entry.m_steamIDUser.ConvertToUint64();
				row.globalRank = entry.m_nGlobalRank;
				row.score = entry.m_nScore;
				row.internalRank = static_cast<uint16_t>((static_cast<uint32_t>(entry.m_nScore) >> 16) & 0xFFFFu);
				row.packedSubscore = static_cast<uint16_t>(static_cast<uint32_t>(entry.m_nScore) & 0xFFFFu);
				row.hasTotalLp = ComputeTotalLpFromPackedScore(row.internalRank, row.packedSubscore, &row.totalLp);

				// details[0] only carries a meaningful character id on RANK_ALL (the
				// board covers every character, so the game tags each entry with the
				// character last used). On a per-character board every entry already
				// belongs to that character - details[0] there is unrelated/stale and
				// reading it here is what caused every row to show as Ragna (index 0).
				if (selectedCharacterId != kAllCharacterId)
					row.characterId = static_cast<uint8_t>(selectedCharacterId);
				else
					row.characterId = (details[0] >= 0 && details[0] < 64) ? static_cast<uint8_t>(details[0]) : 0xFFu;

				rows.push_back(std::move(row));
			}

			LOG(2, "[RANK][LeaderboardWin] downloaded %d entries (char=%u scope=%d page=%d)\n",
				static_cast<int>(rows.size()), static_cast<unsigned int>(selectedCharacterId),
				scope == Scope::Friends ? 1 : 0, pageStart);

			ResolvePersonaInfo();
		}

		void OnSelfRankDownloaded(LeaderboardScoresDownloaded_t* cb, bool ioFailure)
		{
			selfRankPending = false;
			if (ioFailure || !cb || cb->m_cEntryCount <= 0)
			{
				statusError = L("You have no entry on this leaderboard yet.");
				return;
			}
			SteamUserStatsWrapper* stats = g_interfaces.pSteamUserStatsWrapper;
			if (!stats)
				return;

			LeaderboardEntry_t entry{};
			int32 details[4] = {};
			if (stats->GetDownloadedLeaderboardEntryQuiet(cb->m_hSteamLeaderboardEntries, 0, &entry, details, 4)
				&& entry.m_nGlobalRank > 0)
			{
				scope = Scope::Global;
				pageStart = ((entry.m_nGlobalRank - 1) / pageSize) * pageSize + 1;
				m_dirty = true;
			}
		}

		// Fill in persona name / Steam level / online state for the visible rows.
		// Values are cached lazily by the Steam client; we re-poll each frame and
		// ask for missing info once, so names fill in over the next few frames.
		void ResolvePersonaInfo()
		{
			SteamFriendsWrapper* friends = g_interfaces.pSteamFriendsWrapper;
			if (!friends)
				return;

			for (LeaderboardRow& row : rows)
			{
				CSteamID id(static_cast<uint64>(row.steamId));

				const char* name = friends->GetFriendPersonaName(id);
				if (name && name[0] != '\0' && std::string(name) != "[unknown]")
					row.name = name;

				row.personaState = static_cast<int>(friends->GetFriendPersonaState(id));

				if (row.name.empty() && m_infoRequested.find(row.steamId) == m_infoRequested.end())
				{
					friends->RequestUserInformation(id, true);
					m_infoRequested.insert(row.steamId);
				}
			}
		}

		SteamLeaderboard_t m_currentHandle = 0;
		uint32_t m_findCharacterId = kAllCharacterId;
		bool m_dirty = true;
		bool m_jumpRequested = false;
		double m_lastFindAttempt = -5.0;

		std::unordered_map<uint32_t, SteamLeaderboard_t> m_handles;
		std::unordered_set<uint64_t> m_infoRequested;

		CCallResult<LeaderboardModel, LeaderboardFindResult_t> m_findResult;
		CCallResult<LeaderboardModel, LeaderboardScoresDownloaded_t> m_downloadResult;
		CCallResult<LeaderboardModel, LeaderboardScoresDownloaded_t> m_selfRankResult;
	};

	LeaderboardModel& Model()
	{
		static LeaderboardModel model;
		return model;
	}

	ImU32 OnlineStateColor(int personaState)
	{
		// EPersonaState: 0 offline, 1 online, 2 busy, 3 away, 4 snooze, ...
		switch (personaState)
		{
		case 1: return IM_COL32(87, 200, 77, 255);   // online - green
		case 2: return IM_COL32(220, 90, 70, 255);   // busy   - red
		case 3:
		case 4: return IM_COL32(220, 190, 70, 255);  // away/snooze - yellow
		case 0: return IM_COL32(110, 110, 118, 255); // offline - gray
		default: return IM_COL32(70, 150, 220, 255); // otherwise (looking to play/trade) - blue
		}
	}

	void DrawOnlineDot(int personaState)
	{
		const float lineHeight = ImGui::GetTextLineHeight();
		const float radius = 4.0f;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddCircleFilled(
			ImVec2(p.x + radius + 2.0f, p.y + lineHeight * 0.5f), radius, OnlineStateColor(personaState));
		ImGui::Dummy(ImVec2(radius * 2.0f + 4.0f, lineHeight));
	}

	void DrawFilterBar()
	{
		LeaderboardModel& m = Model();

		// Character filter.
		ImGui::TextUnformatted(L("Character:").c_str());
		ImGui::SameLine();
		ImGui::PushItemWidth(180.0f);
		if (ImGui::BeginCombo("###LbChar", CharacterLabel(m.selectedCharacterId).c_str()))
		{
			if (ImGui::Selectable(CharacterLabel(kAllCharacterId).c_str(), m.selectedCharacterId == kAllCharacterId))
			{
				m.selectedCharacterId = kAllCharacterId;
				m.pageStart = 1;
			}
			for (int i = 0; i < kNumRankedCharacters; ++i)
			{
				const bool selected = m.selectedCharacterId == static_cast<uint32_t>(i);
				if (ImGui::Selectable(getCharacterNameByIndexA(i).c_str(), selected))
				{
					m.selectedCharacterId = static_cast<uint32_t>(i);
					m.pageStart = 1;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::PopItemWidth();

		// Scope filter (all players vs friends only).
		ImGui::SameLine();
		ImGui::TextUnformatted(L("Show:").c_str());
		ImGui::SameLine();
		bool globalScope = m.scope == LeaderboardModel::Scope::Global;
		if (ImGui::RadioButton(L("Everyone").c_str(), globalScope))
		{
			if (!globalScope)
			{
				m.scope = LeaderboardModel::Scope::Global;
				m.MarkNeedsDownload();
			}
		}
		ImGui::SameLine();
		if (ImGui::RadioButton(L("Friends").c_str(), !globalScope))
		{
			if (globalScope)
			{
				m.scope = LeaderboardModel::Scope::Friends;
				m.MarkNeedsDownload();
			}
		}

		ImGui::SameLine();
		if (ImGui::Button(L("Jump to me").c_str()))
			m.RequestJumpToMe();
		ImGui::SameLine();
		ImGui::ShowHelpMarker(L("Scrolls the list to your own position on this leaderboard.").c_str());
	}

	void DrawPager()
	{
		LeaderboardModel& m = Model();
		if (m.scope == LeaderboardModel::Scope::Friends)
		{
			ImGui::Text(L("%d friends on this leaderboard").c_str(), static_cast<int>(m.rows.size()));
			return;
		}

		const int last = m.totalEntries > 0
			? (m.pageStart + m.pageSize - 1 < m.totalEntries ? m.pageStart + m.pageSize - 1 : m.totalEntries)
			: m.pageStart + m.pageSize - 1;

		if (ImGui::Button(L("<< First").c_str()))
		{
			m.pageStart = 1;
			m.MarkNeedsDownload();
		}
		ImGui::SameLine();
		if (ImGui::Button(L("< Prev").c_str()))
		{
			m.pageStart -= m.pageSize;
			if (m.pageStart < 1)
				m.pageStart = 1;
			m.MarkNeedsDownload();
		}
		ImGui::SameLine();
		ImGui::Text(L("Ranks %d - %d of %d").c_str(), m.pageStart, last, m.totalEntries);
		ImGui::SameLine();
		if (ImGui::Button(L("Next >").c_str()))
		{
			if (m.totalEntries <= 0 || m.pageStart + m.pageSize <= m.totalEntries)
			{
				m.pageStart += m.pageSize;
				m.MarkNeedsDownload();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(L("Last >>").c_str()))
		{
			if (m.totalEntries > 0)
			{
				m.pageStart = ((m.totalEntries - 1) / m.pageSize) * m.pageSize + 1;
				m.MarkNeedsDownload();
			}
		}
	}

	void DrawTable()
	{
		LeaderboardModel& m = Model();
		const uint64_t localId = m.LocalSteamId();

		ImGui::Separator();
		ImGui::Columns(7, "###LbCols", true);
		const float fullWidth = (ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x);
		ImGui::SetColumnWidth(0, fullWidth * 0.07f); // #
		ImGui::SetColumnWidth(1, fullWidth * 0.30f); // player
		ImGui::SetColumnWidth(2, fullWidth * 0.16f); // rank tier
		ImGui::SetColumnWidth(3, fullWidth * 0.12f); // total LP
		ImGui::SetColumnWidth(4, fullWidth * 0.18f); // character
		ImGui::SetColumnWidth(5, fullWidth * 0.06f); // online
		ImGui::SetColumnWidth(6, fullWidth * 0.11f); // profile

		ImGui::TextUnformatted(L("#").c_str());                ImGui::NextColumn();
		ImGui::TextUnformatted(L("Player").c_str());           ImGui::NextColumn();
		ImGui::TextUnformatted(L("Rank").c_str());             ImGui::NextColumn();
		ImGui::TextUnformatted(L("Total LP").c_str());         ImGui::NextColumn();
		ImGui::TextUnformatted(L("Character").c_str());        ImGui::NextColumn();
		ImGui::TextUnformatted("");                            ImGui::NextColumn();
		ImGui::TextUnformatted("");                            ImGui::NextColumn();
		ImGui::Separator();

		for (size_t i = 0; i < m.rows.size(); ++i)
		{
			const LeaderboardRow& row = m.rows[i];
			const bool isLocal = row.steamId == localId && localId != 0;

			if (isLocal)
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.92f, 0.45f, 1.0f));

			ImGui::Text("%d", row.globalRank);
			ImGui::NextColumn();

			ImGui::TextUnformatted(row.name.empty() ? L("(loading...)").c_str() : row.name.c_str());
			ImGui::NextColumn();

			// Rank tier label + colour, decoded from the packed score.
			{
				const uint32_t visibleRank = static_cast<uint32_t>(row.internalRank) + 1u;
				if (isLocal)
				{
					ImGui::TextUnformatted(FormatVisibleRankLabel(visibleRank, false).c_str());
				}
				else
				{
					ImGui::TextColored(GetVisibleRankColor(visibleRank, false), "%s",
						FormatVisibleRankLabel(visibleRank, false).c_str());
				}
			}
			ImGui::NextColumn();

			if (row.hasTotalLp)
				ImGui::Text("%u", static_cast<unsigned int>(row.totalLp));
			else
				ImGui::TextUnformatted("-");
			ImGui::NextColumn();

			if (row.characterId < static_cast<uint8_t>(kNumRankedCharacters))
				ImGui::TextUnformatted(getCharacterNameByIndexA(row.characterId).c_str());
			else
				ImGui::TextUnformatted("-");
			ImGui::NextColumn();

			DrawOnlineDot(row.personaState);
			ImGui::NextColumn();

			char btnId[32];
			std::snprintf(btnId, sizeof(btnId), "%s###LbProf%zu", L("Profile").c_str(), i);
			if (ImGui::SmallButton(btnId))
			{
				if (g_interfaces.pSteamFriendsWrapper)
					g_interfaces.pSteamFriendsWrapper->ActivateGameOverlayToUser("steamid", CSteamID(static_cast<uint64>(row.steamId)));
			}
			ImGui::NextColumn();

			if (isLocal)
				ImGui::PopStyleColor();
		}

		ImGui::Columns(1);
	}
}

void RankedLeaderboardWindow::BeforeDraw()
{
	ImGui::SetNextWindowSize(ImVec2(720.0f, 560.0f), ImGuiCond_FirstUseEver);
}

void RankedLeaderboardWindow::Draw()
{
	LeaderboardModel& m = Model();
	m.Tick();

	DrawFilterBar();

	if (!m.statusError.empty())
	{
		ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.35f, 1.0f), "%s", m.statusError.c_str());
	}

	DrawPager();

	if (m.downloadPending && m.rows.empty())
		ImGui::TextUnformatted(L("Loading...").c_str());

	ImGui::BeginChild("###LbScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
	DrawTable();
	ImGui::EndChild();
}
