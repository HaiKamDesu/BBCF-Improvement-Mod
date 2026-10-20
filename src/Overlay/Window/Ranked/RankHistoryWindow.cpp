#include "RankHistoryWindow.h"

#include "RankHistory.h"
#include "RankedProgressWindow.h"

#include "Core/Localization.h"
#include "Game/characters.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <ctime>
#include <set>
#include <string>
#include <vector>

namespace
{
	const ImVec4 kWinColor = ImVec4(0.31f, 0.92f, 0.41f, 1.0f);
	const ImVec4 kLossColor = ImVec4(0.97f, 0.32f, 0.32f, 1.0f);
	const ImVec4 kNeutralColor = ImVec4(0.70f, 0.72f, 0.76f, 1.0f);
	const ImVec4 kMutedColor = ImVec4(0.56f, 0.58f, 0.63f, 1.0f);
	const ImVec4 kRankUpColor = ImVec4(1.00f, 0.82f, 0.25f, 1.0f);
	const ImVec4 kCardColor = ImVec4(0.11f, 0.12f, 0.15f, 1.0f);
	const ImVec4 kDayHeaderColor = ImVec4(0.13f, 0.14f, 0.18f, 1.0f);
	const ImVec4 kDayHeaderHoverColor = ImVec4(0.17f, 0.18f, 0.23f, 1.0f);

	std::string FormatThousands(long long value)
	{
		const bool negative = value < 0;
		unsigned long long magnitude = negative ? static_cast<unsigned long long>(-value) : static_cast<unsigned long long>(value);
		std::string digits = std::to_string(magnitude);
		std::string out;
		int count = 0;
		for (auto it = digits.rbegin(); it != digits.rend(); ++it)
		{
			if (count != 0 && count % 3 == 0)
			{
				out.insert(out.begin(), ',');
			}
			out.insert(out.begin(), *it);
			++count;
		}
		if (negative)
		{
			out.insert(out.begin(), '-');
		}
		return out;
	}

	std::string FormatSigned(long long value)
	{
		std::string text = FormatThousands(value);
		return value > 0 ? "+" + text : text;
	}

	ImVec4 DeltaColor(long long delta)
	{
		return delta > 0 ? kWinColor : (delta < 0 ? kLossColor : kNeutralColor);
	}

	struct Totals
	{
		int matches = 0;
		int wins = 0;
		int losses = 0;
		long long net = 0;

		void Add(const RankHistory::Entry& entry)
		{
			++matches;
			if (entry.result == 'W') ++wins;
			if (entry.result == 'L') ++losses;
			net += entry.LpDelta();
		}

		std::string Record() const
		{
			char buffer[96];
			const int decided = wins + losses;
			if (decided > 0)
			{
				std::snprintf(buffer, sizeof(buffer), "%dW %dL  \xC2\xB7  %d%%", wins, losses, (wins * 100 + decided / 2) / decided);
			}
			else
			{
				std::snprintf(buffer, sizeof(buffer), "%dW %dL", wins, losses);
			}
			return buffer;
		}
	};

	struct DayGroup
	{
		int key = 0;                 // yyyymmdd, local time
		std::string label;
		std::vector<const RankHistory::Entry*> entries; // newest first
		Totals totals;
	};

	// Rebuilt only when the history or the filter changes.
	struct ViewCache
	{
		unsigned revision = ~0u;
		int characterFilter = -2;
		std::vector<DayGroup> days;
		Totals today;
		Totals week;
		Totals allTime;
		std::vector<int> charactersPlayed;
	};
	ViewCache g_view;
	int g_characterFilter = -1; // -1 = all characters
	std::set<int> g_openDays;
	bool g_openDaysSeeded = false;

	int LocalDayKey(int64_t unixTime, std::tm* outTm = nullptr)
	{
		std::tm tm = {};
		const time_t t = static_cast<time_t>(unixTime);
		localtime_s(&tm, &t);
		if (outTm)
		{
			*outTm = tm;
		}
		return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
	}

	std::string DayLabel(const std::tm& tm, int key, int todayKey, int yesterdayKey)
	{
		char date[64];
		std::strftime(date, sizeof(date), "%A %d %B %Y", &tm);
		if (key == todayKey)
		{
			return L("Today") + "  \xC2\xB7  " + date;
		}
		if (key == yesterdayKey)
		{
			return L("Yesterday") + "  \xC2\xB7  " + date;
		}
		return date;
	}

	void RebuildView()
	{
		const std::vector<RankHistory::Entry>& entries = RankHistory::GetEntries();
		const unsigned revision = RankHistory::GetRevision();
		if (g_view.revision == revision && g_view.characterFilter == g_characterFilter)
		{
			return;
		}

		g_view = ViewCache();
		g_view.revision = revision;
		g_view.characterFilter = g_characterFilter;

		const uint64_t localSteamId = RankHistory::GetLocalSteamId();
		const time_t now = std::time(nullptr);
		const int todayKey = LocalDayKey(static_cast<int64_t>(now));
		const int yesterdayKey = LocalDayKey(static_cast<int64_t>(now) - 24 * 60 * 60);
		const int64_t weekStart = static_cast<int64_t>(now) - 7 * 24 * 60 * 60;

		std::set<int> characters;
		for (auto it = entries.rbegin(); it != entries.rend(); ++it)
		{
			const RankHistory::Entry& entry = *it;
			// Other Steam accounts on this PC keep their own history in the same file.
			if (localSteamId != 0 && entry.localSteamId != 0 && entry.localSteamId != localSteamId)
			{
				continue;
			}
			characters.insert(static_cast<int>(entry.characterId));
			if (g_characterFilter >= 0 && static_cast<int>(entry.characterId) != g_characterFilter)
			{
				continue;
			}

			std::tm tm = {};
			const int key = LocalDayKey(entry.unixTime, &tm);
			if (g_view.days.empty() || g_view.days.back().key != key)
			{
				DayGroup group;
				group.key = key;
				group.label = DayLabel(tm, key, todayKey, yesterdayKey);
				g_view.days.push_back(group);
			}
			g_view.days.back().entries.push_back(&entry);
			g_view.days.back().totals.Add(entry);

			g_view.allTime.Add(entry);
			if (key == todayKey)
			{
				g_view.today.Add(entry);
			}
			if (entry.unixTime >= weekStart)
			{
				g_view.week.Add(entry);
			}
		}
		g_view.charactersPlayed.assign(characters.begin(), characters.end());

		if (!g_openDaysSeeded && !g_view.days.empty())
		{
			// The most recent day starts open; everything older is one click away.
			g_openDays.insert(g_view.days.front().key);
			g_openDaysSeeded = true;
		}
	}

	// A small rounded tag in a rank's colour: the same colour language as the progress bar.
	void DrawRankChip(uint32_t visibleRank, bool unranked)
	{
		const std::string label = FormatVisibleRankLabel(visibleRank, unranked);
		const ImVec4 color = GetVisibleRankColor(visibleRank, unranked);
		const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
		const ImVec2 padding(6.0f, 1.0f);
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
			ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, 0.16f)), 4.0f);
		drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
			ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, 0.75f)), 4.0f, ImDrawFlags_RoundCornersAll, 1.0f);
		drawList->AddText(ImVec2(pos.x + padding.x, pos.y + padding.y), ImGui::ColorConvertFloat4ToU32(color), label.c_str());
		ImGui::Dummy(size);
	}

	// A filled pill with dark text: used for the result and for rank changes.
	void DrawPill(const char* text, const ImVec4& color)
	{
		const ImVec2 textSize = ImGui::CalcTextSize(text);
		const ImVec2 padding(8.0f, 1.0f);
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::ColorConvertFloat4ToU32(color), size.y * 0.5f);
		drawList->AddText(ImVec2(pos.x + padding.x, pos.y + padding.y), IM_COL32(16, 18, 22, 255), text);
		ImGui::Dummy(size);
	}

	void DrawSummaryCard(const char* id, const std::string& title, const Totals& totals, float width)
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, kCardColor);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
		const float height = ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().FontSizeBase * 1.6f + 22.0f;
		ImGui::BeginChild(id, ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);

		ImGui::TextColored(kMutedColor, "%s", title.c_str());

		ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 1.6f);
		const std::string net = totals.matches > 0 ? FormatSigned(totals.net) + " LP" : std::string("\xE2\x80\x94");
		ImGui::TextColored(totals.matches > 0 ? DeltaColor(totals.net) : kMutedColor, "%s", net.c_str());
		ImGui::PopFont();

		if (totals.matches > 0)
		{
			ImGui::TextColored(kNeutralColor, "%d %s  \xC2\xB7  %s", totals.matches,
				L(totals.matches == 1 ? "match" : "matches").c_str(), totals.Record().c_str());
		}
		else
		{
			ImGui::TextColored(kMutedColor, "%s", L("No matches").c_str());
		}

		ImGui::EndChild();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();
	}

	// The day's header: a full-width bar that opens and closes the day, with the day's record and
	// net LP on the right.
	bool DrawDayHeader(const DayGroup& day, bool open)
	{
		const float width = ImGui::GetContentRegionAvail().x;
		const float height = ImGui::GetTextLineHeight() + 12.0f;
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::PushID(day.key);
		const bool clicked = ImGui::InvisibleButton("##day", ImVec2(width, height));
		const bool hovered = ImGui::IsItemHovered();
		ImGui::PopID();

		ImDrawList* const drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
			ImGui::ColorConvertFloat4ToU32(hovered ? kDayHeaderHoverColor : kDayHeaderColor), 5.0f);

		const float textY = pos.y + 6.0f;
		ImGui::RenderArrow(drawList, ImVec2(pos.x + 8.0f, textY), ImGui::GetColorU32(ImGuiCol_Text),
			open ? ImGuiDir_Down : ImGuiDir_Right, 0.8f);
		drawList->AddText(ImVec2(pos.x + 26.0f, textY), ImGui::GetColorU32(ImGuiCol_Text), day.label.c_str());

		const std::string net = FormatSigned(day.totals.net) + " LP";
		char record[128];
		std::snprintf(record, sizeof(record), "%d %s  \xC2\xB7  %s     ", day.totals.matches,
			L(day.totals.matches == 1 ? "match" : "matches").c_str(), day.totals.Record().c_str());
		const float netWidth = ImGui::CalcTextSize(net.c_str()).x;
		const float recordWidth = ImGui::CalcTextSize(record).x;
		const float right = pos.x + width - 12.0f;
		drawList->AddText(ImVec2(right - netWidth - recordWidth, textY), ImGui::ColorConvertFloat4ToU32(kMutedColor), record);
		drawList->AddText(ImVec2(right - netWidth, textY), ImGui::ColorConvertFloat4ToU32(DeltaColor(day.totals.net)), net.c_str());

		return clicked;
	}

	void DrawDayTable(const DayGroup& day)
	{
		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingStretchProp;
		ImGui::PushID(day.key);
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 5.0f));
		if (ImGui::BeginTable("##matches", 6, flags))
		{
			ImGui::TableSetupColumn(L("Time").c_str(), ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00:00").x + 4.0f);
			ImGui::TableSetupColumn(L("Character").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn(L("Opponent").c_str(), ImGuiTableColumnFlags_WidthStretch, 2.0f);
			ImGui::TableSetupColumn(L("Result").c_str(), ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("LOSE").x + 22.0f);
			ImGui::TableSetupColumn(L("LP").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.1f);
			ImGui::TableSetupColumn(L("Rank").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.6f);
			// Plain labels rather than TableHeadersRow: the headers do nothing when clicked, so
			// they should not light up under the mouse as if they did.
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			for (int column = 0; column < ImGui::TableGetColumnCount(); ++column)
			{
				ImGui::TableSetColumnIndex(column);
				ImGui::TextColored(kMutedColor, "%s", ImGui::TableGetColumnName(column));
			}

			for (const RankHistory::Entry* entry : day.entries)
			{
				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(0);
				std::tm tm = {};
				LocalDayKey(entry->unixTime, &tm);
				char timeText[16];
				std::strftime(timeText, sizeof(timeText), "%H:%M", &tm);
				ImGui::TextColored(kMutedColor, "%s", timeText);

				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(getCharacterNameByIndexA(static_cast<int>(entry->characterId)).c_str());

				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(entry->opponentName.empty() ? "?" : entry->opponentName.c_str());
				if (entry->opponentCharacterId >= 0)
				{
					ImGui::SameLine(0.0f, 6.0f);
					ImGui::TextColored(kMutedColor, "\xC2\xB7 %s", getCharacterNameByIndexA(entry->opponentCharacterId).c_str());
				}
				if (entry->opponentVisibleRank >= 0)
				{
					ImGui::SameLine(0.0f, 6.0f);
					DrawRankChip(static_cast<uint32_t>(entry->opponentVisibleRank), entry->opponentVisibleRank == 0);
				}

				ImGui::TableSetColumnIndex(3);
				if (entry->result == 'W')
					DrawPill(L("WIN").c_str(), kWinColor);
				else if (entry->result == 'L')
					DrawPill(L("LOSE").c_str(), kLossColor);
				else
					DrawPill("?", kNeutralColor);

				ImGui::TableSetColumnIndex(4);
				const int32_t delta = entry->LpDelta();
				ImGui::TextColored(DeltaColor(delta), "%s", FormatSigned(delta).c_str());
				ImGui::SameLine(0.0f, 8.0f);
				ImGui::TextColored(kMutedColor, "%s", FormatThousands(entry->lpAfter).c_str());

				ImGui::TableSetColumnIndex(5);
				if (entry->RankedUp() || entry->RankedDown())
				{
					const bool up = entry->RankedUp();
					const std::string label = std::string(up ? L("RANK UP") : L("RANK DOWN")) + " (" +
						FormatVisibleRankLabel(entry->rankAfter, entry->unrankedAfter) + ")";
					DrawPill(label.c_str(), up ? kRankUpColor : kLossColor);
				}
				else
				{
					DrawRankChip(entry->rankAfter, entry->unrankedAfter);
				}
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleVar();
		ImGui::PopID();
	}
}

void RankHistoryWindow::BeforeDraw()
{
	ImGui::SetNextWindowSize(ImVec2(820.0f, 600.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 320.0f), ImVec2(4000.0f, 4000.0f));
	// Opaque, unlike the overlay's usual see-through windows: this one is read like a page, and
	// the game's menus showing through the table made it hard to read.
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.075f, 0.09f, 0.98f));
}

void RankHistoryWindow::AfterDraw()
{
	ImGui::PopStyleColor();
}

void RankHistoryWindow::Draw()
{
	RebuildView();

	// Filter: one character, or all of them.
	{
		const std::string current = g_characterFilter < 0 ? L("All characters") : getCharacterNameByIndexA(g_characterFilter);
		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::BeginCombo("##rank_history_character", current.c_str()))
		{
			if (ImGui::Selectable(L("All characters").c_str(), g_characterFilter < 0))
			{
				g_characterFilter = -1;
			}
			for (int character : g_view.charactersPlayed)
			{
				if (ImGui::Selectable(getCharacterNameByIndexA(character).c_str(), g_characterFilter == character))
				{
					g_characterFilter = character;
				}
			}
			ImGui::EndCombo();
		}
		RebuildView();

		// Clear: right-aligned on the same row, behind a confirmation.
		const char* const clearLabel = L("Clear history...").c_str();
		const float clearWidth = ImGui::CalcTextSize(clearLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::SameLine(ImGui::GetContentRegionMax().x - clearWidth);
		ImGui::BeginDisabled(RankHistory::CountForLocalAccount() == 0);
		if (ImGui::Button(clearLabel))
		{
			ImGui::OpenPopup(L("Clear rank history?###ClearRankHistory").c_str());
		}
		ImGui::EndDisabled();

		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (ImGui::BeginPopupModal(L("Clear rank history?###ClearRankHistory").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			const size_t count = RankHistory::CountForLocalAccount();
			ImGui::Text(L("This deletes all %zu ranked matches recorded for this Steam account.").c_str(), count);
			ImGui::TextColored(kMutedColor, "%s", L("Your rank and LP are not affected. This cannot be undone.").c_str());
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.18f, 0.18f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.76f, 0.22f, 0.22f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.86f, 0.26f, 0.26f, 1.0f));
			if (ImGui::Button(L("Clear").c_str(), ImVec2(120.0f, 0.0f)))
			{
				RankHistory::ClearLocalAccount();
				g_openDays.clear();
				g_openDaysSeeded = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::PopStyleColor(3);
			ImGui::SameLine();
			if (ImGui::Button(L("Cancel").c_str(), ImVec2(120.0f, 0.0f)))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		RebuildView();
	}

	ImGui::Spacing();

	// Totals.
	{
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float cardWidth = (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f;
		DrawSummaryCard("##today", L("Today"), g_view.today, cardWidth);
		ImGui::SameLine();
		DrawSummaryCard("##week", L("Last 7 days"), g_view.week, cardWidth);
		ImGui::SameLine();
		DrawSummaryCard("##all", L("All time"), g_view.allTime, cardWidth);
	}

	ImGui::Spacing();
	ImGui::Spacing();

	if (g_view.days.empty())
	{
		const char* const line1 = L("No ranked matches recorded yet.").c_str();
		const char* const line2 = L("Every ranked match you play from now on shows up here, with how it moved your LP.").c_str();
		const float avail = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(1.0f, 40.0f));
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - ImGui::CalcTextSize(line1).x) * 0.5f);
		ImGui::TextUnformatted(line1);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - ImGui::CalcTextSize(line2).x) * 0.5f);
		ImGui::TextColored(kMutedColor, "%s", line2);
		return;
	}

	ImGui::BeginChild("##rank_history_days", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
	for (const DayGroup& day : g_view.days)
	{
		const bool open = g_openDays.count(day.key) != 0;
		if (DrawDayHeader(day, open))
		{
			if (open)
				g_openDays.erase(day.key);
			else
				g_openDays.insert(day.key);
		}
		if (open)
		{
			DrawDayTable(day);
		}
		ImGui::Spacing();
	}
	ImGui::EndChild();
}
