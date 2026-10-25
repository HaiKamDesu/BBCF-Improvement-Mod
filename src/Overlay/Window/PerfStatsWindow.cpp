#include "PerfStatsWindow.h"

#include "Core/info.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/utils.h"
#include "Game/PerformanceMonitor.h"
#include "Game/Room/RoomMemberEntry.h"
#include "Network/RoomManager.h"
#include "Overlay/imgui_utils.h"

#include "imgui_internal.h"

#include <cfloat>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace PerformanceMonitor;

	// ---- remembered layout (menus.ini, next to the window's own position) ----------------

	bool g_detailsOpen = false; // the full table, closed by default

	void* Layout_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
	{
		return strcmp(name, "Layout") == 0 ? (void*)1 : nullptr;
	}

	void Layout_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
	{
		int value = 0;
		if (sscanf_s(line, "Details=%d", &value) == 1)
			g_detailsOpen = value != 0;
		// Anything else (the dev builds' "Detailed=") is ignored.
	}

	void Layout_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
	{
		buf->appendf("[%s][Layout]\n", handler->TypeName);
		buf->appendf("Details=%d\n\n", g_detailsOpen ? 1 : 0);
	}

	// HoverTooltip does not wrap, and these are sentences: same delay, wrapped like the (?) markers.
	void WrappedTip(const char* text)
	{
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
			ImGui::SetTooltipWrapped(text);
	}

	const ImVec4 kGood(0.45f, 0.90f, 0.45f, 1.0f);
	const ImVec4 kWarn(1.00f, 0.65f, 0.20f, 1.0f);
	const ImVec4 kBad(1.00f, 0.38f, 0.32f, 1.0f);
	const ImVec4 kDim(0.62f, 0.62f, 0.62f, 1.0f);

	ImVec4 CapacityColor(double percent)
	{
		if (percent < 100.0) return kBad;
		if (percent < 120.0) return kWarn;
		return kGood;
	}

	// Rollbacks in BBCF go up to 8 frames deep, all re-simulated inside one 60 FPS frame.
	ImVec4 HeadroomColor(int frames)
	{
		if (frames < 0) return kDim;
		if (frames < 3) return kBad;
		if (frames < 8) return kWarn;
		return kGood;
	}

	ImVec4 PingColor(unsigned ms)
	{
		if (ms < 80) return kGood;
		if (ms < 150) return kWarn;
		return kBad;
	}

	ImVec4 GapColor(unsigned ms)
	{
		if (ms < 60) return kGood;
		if (ms < 120) return kWarn;
		return kBad;
	}

	float BarWidth() { return 120.0f * ImGui::GetFontSize() / 13.0f; }

	// A thin bar under a number. mark is a tick (100 % / an 8-frame rollback).
	void DrawBar(float fraction, float mark, const ImVec4& color)
	{
		const float width = BarWidth();
		const float height = 4.0f;
		if (fraction < 0.0f) fraction = 0.0f;
		if (fraction > 1.0f) fraction = 1.0f;

		ImDrawList* const draw = ImGui::GetWindowDrawList();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		draw->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(255, 255, 255, 40));
		draw->AddRectFilled(p, ImVec2(p.x + width * fraction, p.y + height), ImGui::GetColorU32(color));
		const float mx = p.x + width * mark;
		draw->AddLine(ImVec2(mx, p.y - 2.0f), ImVec2(mx, p.y + height + 2.0f), IM_COL32(255, 255, 255, 200), 1.0f);
		ImGui::Dummy(ImVec2(width, height + 2.0f));
	}

	bool HasFrames(const Report& r) { return (r.flags & Flag_HasFrames) != 0 && r.gameDrawP90Us > 0; }
	bool HasResim(const Report& r) { return (r.flags & Flag_HasResim) != 0 && r.resimFrameAvgUs > 0; }
	bool HasNet(const Report& r) { return (r.flags & Flag_HasNet) != 0; }

	// A value that is not there yet for a normal reason, in plain grey words, with the reason
	// in the tooltip. A bare "-" is kept for real failures only.
	void Pending(const char* word, const char* why)
	{
		ImGui::TextColored(kDim, "%s", L(word).c_str());
		if (why && *why && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
			ImGui::SetTooltipWrapped(L(why).c_str());
	}

	void Measuring() { Pending("measuring...", "Collecting the first numbers; this takes a few seconds."); }
	void NoRollbacksYet()
	{
		Pending("no rollbacks yet", "No rollbacks have happened this match - that is good news: inputs are arriving in time. This fills in after the first rollback.");
	}
	void NetMeasuring()
	{
		Pending("measuring...", "The game measures the connection about once a second; the first numbers arrive shortly after the match starts.");
	}

	// Resim-type values: either there were no rollbacks yet (good) or the frames are not in.
	void NoResim(const Report& r)
	{
		if ((r.flags & Flag_HasFrames) != 0 && r.totalRollbacks == 0 && r.rollbackSamples == 0)
			NoRollbacksYet();
		else
			Measuring();
	}

	void Dash() { Measuring(); }

	// Every cell draws the same number of lines whatever the data, so the table does not
	// jump when a value appears or a status changes.
	void StatusCell(const Report& r, bool online)
	{
		if (!HasFrames(r))
		{
			ImGui::TextColored(kDim, "%s", L("Measuring...").c_str());
			ImGui::TextUnformatted(" ");
			return;
		}

		const double percent = CapacityPercent(r.gameDrawP90Us);
		if (IsStruggling(r))
			ImGui::TextColored(kBad, "%s", r.gameDrawP90Us > (uint32_t)kBudgetUs
				? L("Too slow for 60 FPS").c_str() : L("Dropping frames").c_str());
		else if (percent < 120.0)
			ImGui::TextColored(kWarn, "%s", L("OK, little headroom").c_str());
		else
			ImGui::TextColored(kGood, "%s", L("Good").c_str());

		// Judged against the rollback depth this connection really produces, and only once
		// there are enough rollbacks to trust; a perfect connection may have none at all.
		const int capacity = RollbackCapacityOk(r);
		if (!online)
			ImGui::TextUnformatted(" ");
		else if (capacity > 0)
			ImGui::TextColored(kGood, "%s", L("Rollback: OK").c_str());
		else if (capacity < 0)
			ImGui::TextColored(kWarn, "%s", L("Rollback: may lag").c_str());
		else if (r.totalRollbacks == 0 && r.rollbackSamples == 0)
			ImGui::TextColored(kGood, "%s", L("Rollback: none yet").c_str());
		else
			ImGui::TextColored(kDim, "%s", L("Rollback: too few to judge").c_str());
	}

	void GameDrawCell(const Report& r)
	{
		if (!HasFrames(r)) { Dash(); ImGui::Dummy(ImVec2(BarWidth(), 6.0f)); return; }
		const double percent = CapacityPercent(r.gameDrawP90Us);
		const ImVec4 color = CapacityColor(percent);
		ImGui::TextColored(color, "%.0f%% (%.0f FPS)", percent, 1000000.0 / (double)r.gameDrawP90Us);
		DrawBar((float)(percent / 300.0), 1.0f / 3.0f, color);
	}

	void ResimCell(const Report& r)
	{
		if (!HasResim(r)) { NoResim(r); ImGui::Dummy(ImVec2(BarWidth(), 6.0f)); return; }
		const double percent = CapacityPercent(r.resimFrameAvgUs);
		const int headroom = RollbackHeadroomFrames(r);
		const int capacity = RollbackCapacityOk(r);
		const ImVec4 color = capacity < 0 ? kWarn : (capacity > 0 ? kGood : ImGui::GetStyleColorVec4(ImGuiCol_Text));
		ImGui::TextColored(color, "%.0f%% (%.0f FPS)", percent, 1000000.0 / (double)r.resimFrameAvgUs);
		DrawBar(headroom < 0 ? 0.0f : (float)headroom / 16.0f, 0.5f, color);
	}

	void PingCell(const Report& r)
	{
		if (!HasNet(r)) { NetMeasuring(); return; }
		ImGui::TextColored(PingColor(r.pingMs), "%u ms", (unsigned)r.pingMs);
	}

	void RecentRollbacksCell(const Report& r)
	{
		if (!HasFrames(r)) { Dash(); return; }
		if (r.rollbacksRecent == 0) { ImGui::TextColored(kGood, "0"); return; }
		const ImVec4 color = (r.rollbacksRecent >= 4 || r.maxDepthRecent >= 3) ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text);
		ImGui::TextColored(color, L("%u, deepest %u f").c_str(), (unsigned)r.rollbacksRecent, (unsigned)r.maxDepthRecent);
	}

	void FrameCostCell(const Report& r)
	{
		if (!HasFrames(r)) { Dash(); return; }
		ImGui::Text("%.1f / %.1f / %.1f ms", r.gameDrawAvgUs / 1000.0, r.gameDrawP90Us / 1000.0, r.gameDrawMaxUs / 1000.0);
	}

	void ModCostCell(const Report& r)
	{
		if (!HasFrames(r)) { Dash(); return; }
		ImGui::Text("%.1f ms", r.modAvgUs / 1000.0);
	}

	void RealFpsCell(const Report& r)
	{
		if (!HasFrames(r) || r.intervalAvgUs == 0) { Dash(); return; }
		const double fps = 1000000.0 / (double)r.intervalAvgUs;
		ImGui::TextColored(fps < 55.0 ? kBad : (fps < 59.0 ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text)),
			"%.1f (%s %.0f ms)", fps, L("longest").c_str(), r.intervalMaxUs / 1000.0);
	}

	void ResimCostCell(const Report& r)
	{
		if (!HasResim(r)) { NoResim(r); return; }
		ImGui::Text(L("%.2f ms + %.2f ms/frame").c_str(), r.resimLoadUs / 1000.0, r.resimFrameAvgUs / 1000.0);
	}

	void HeadroomCell(const Report& r)
	{
		const int headroom = RollbackHeadroomFrames(r);
		if (headroom < 0) { NoResim(r); return; }
		const int capacity = RollbackCapacityOk(r);
		const ImVec4 color = capacity < 0 ? kWarn : (capacity > 0 ? kGood : ImGui::GetStyleColorVec4(ImGuiCol_Text));
		if (headroom >= 99)
			ImGui::TextColored(color, L("99+ (needs %d)").c_str(), RealisticRollbackDepth(r));
		else
			ImGui::TextColored(color, L("%d (needs %d)").c_str(), headroom, RealisticRollbackDepth(r));
	}

	void MatchRollbacksCell(const Report& r)
	{
		if (!HasFrames(r)) { Dash(); return; }
		ImGui::Text(L("%u (%u frames)").c_str(), (unsigned)r.totalRollbacks, (unsigned)r.totalResimFrames);
	}

	void JitterCell(const Report& r)
	{
		if (!HasNet(r)) { NetMeasuring(); return; }
		ImGui::TextColored(r.jitterMs >= 20 ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%u ms", (unsigned)r.jitterMs);
	}

	void SilenceCell(const Report& r)
	{
		if (!HasNet(r)) { NetMeasuring(); return; }
		ImGui::TextColored(GapColor(r.maxRecvGapMs), "%u ms", (unsigned)r.maxRecvGapMs);
	}

	void PredictionCell(const Report& r)
	{
		if (!HasNet(r)) { NetMeasuring(); return; }
		ImGui::Text("%u f", (unsigned)r.maxPrediction);
	}

	void SendQueueCell(const Report& r)
	{
		if (!HasNet(r)) { NetMeasuring(); return; }
		ImGui::Text("%u", (unsigned)r.sendQueue);
	}

	void AdvantageCell(const Report& r)
	{
		if (!HasNet(r)) { NetMeasuring(); return; }
		ImGui::Text("%+d / %+d", (int)r.localFramesBehind, (int)r.remoteFramesBehind);
	}

	void RelayPingCell(const Report& r)
	{
		if (r.relayPingMs == 0) { Pending("measuring...", "Steam is still measuring the ping to its servers; it starts when you enter online mode and takes a few seconds."); return; }
		ImGui::TextColored(r.relayPingMs >= kSlowLineRelayMs ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%u ms", (unsigned)r.relayPingMs);
	}

	void RouteCell(const Report& r)
	{
		if (!HasNet(r)) { NetMeasuring(); return; }
		ImGui::TextUnformatted((r.flags & Flag_UsingRelay) ? L("Steam relay").c_str() : L("Direct").c_str());
	}

	typedef void (*CellFn)(const Report&);

	struct Row
	{
		const char* label;
		const char* help;
		CellFn fn;
		bool detailedOnly;
	};

	// Labels and help are localization keys. Help is written for a player, not a programmer.
	const Row kRows[] = {
		{ "Game+Draw", "How fast this PC runs one full game frame - the game update and the drawing - compared with what 60 FPS needs. 100% is exactly enough; below 100% the game cannot hold 60 FPS and both players feel it. Measured without the frame limiter or vsync wait, as the typical (90th percentile) frame of the last 2 seconds.", GameDrawCell, false },
		{ "Resim", "How fast this PC re-simulates frames during a rollback, compared with a 60 FPS frame. A rollback redoes several frames inside a single frame, so this needs to be well above 100%. The bar shows how many rollback frames fit in one frame; the tick is 8, the most the game ever does.", ResimCell, false },
		{ "Ping", "Round trip time between the two of you, as the game's netcode measures it once a second. Higher ping means deeper rollbacks; that is distance, not a fault.", PingCell, false },
		{ "Rollbacks (2 s)", "How many times the game rolled back in the last 2 seconds, and the deepest one in frames. A few shallow rollbacks are normal online.", RecentRollbacksCell, false },
		{ "Frame cost", "Game+Draw time in milliseconds: average / typical (90th percentile) / worst, over the last 2 seconds. A 60 FPS frame is 16.7 ms.", FrameCostCell, true },
		{ "Mod overlay", "The part of the average frame cost that is this mod's own work: its overlay windows and per-frame managers. Included in Game+Draw, since the PC has to do it too.", ModCostCell, true },
		{ "Real FPS", "Frames actually shown per second, and the longest gap between two frames, over the last 2 seconds. This also catches a slow graphics card, which Game+Draw cannot see.", RealFpsCell, true },
		{ "Resim cost", "What one rollback costs: a fixed part (loading the saved moment) plus a part per re-simulated frame, worked out from the last 64 rollbacks.", ResimCostCell, true },
		{ "Rollback headroom", "How many rollback frames fit in one 60 FPS frame on top of a normal frame, and how many this connection realistically needs (from the ping, plus a margin). Below that, deep rollbacks could slow the game down. Judged only after 8 rollbacks.", HeadroomCell, true },
		{ "Rollbacks (match)", "Rollbacks this match, and how many frames they re-simulated in total.", MatchRollbacksCell, true },
		{ "Jitter", "How much the ping changes from one second to the next. A steady connection stays under about 10 ms; big swings mean an unstable connection (Wi-Fi, a busy line).", JitterCell, true },
		{ "Longest silence", "The longest stretch in the last 2 seconds with no packet from the other player. The game sends every frame, so anything past about 120 ms is a hiccup: packets lost or held up on the way. In your column it is their packets reaching you; in theirs, yours reaching them.", SilenceCell, true },
		{ "Unconfirmed frames", "The most frames the game had to guess the other player's input for, in the last 2 seconds. This is how deep a rollback could get right now.", PredictionCell, true },
		{ "Unacked inputs", "Inputs this side has sent that the other side has not confirmed yet. It climbs when packets are lost or the connection stalls.", SendQueueCell, true },
		{ "Frame advantage", "The netcode's estimate of how far ahead each side's game runs (this side / the other side). The side ahead waits for the other now and then; a large steady number usually means one PC is slow.", AdvantageCell, true },
		{ "Route", "Whether Steam sends your traffic directly to the other player or through Valve's relay servers. A relay adds some ping but is not itself a problem.", RouteCell, true },
		{ "Ping to nearest Valve server", "This player's own line: the ping from their PC to the closest of Valve's servers, measured by Steam. It does not depend on the opponent, so it shows whose line adds delay. Above about 60 ms the line itself is slow (Wi-Fi, a long distance to the nearest server, or a slow connection).", RelayPingCell, true },
	};

	// ---- the plain view -------------------------------------------------------------------

	const char* OpponentStateLabel(OpponentState s)
	{
		switch (s)
		{
		case Opp_NoOpponent: return "No opponent";
		case Opp_NoMod:      return "No mod";
		case Opp_OldMod:     return "Older mod";
		case Opp_Waiting:    return "Waiting...";
		case Opp_NotSharing: return "No reply";
		case Opp_Stale:      return "No update";
		default:             return "n/a";
		}
	}

	const char* StateHelp(OpponentState s)
	{
		switch (s)
		{
		case Opp_NoOpponent: return "This player has not been found in the room yet.";
		case Opp_NoMod:      return "This player is not running the Improvement Mod, so there are no numbers from their side. The ping and your own side still work.";
		case Opp_OldMod:     return "This player runs a version of the mod older than 8.9.2, which cannot share these numbers. They need to update.";
		case Opp_Waiting:    return "Asking this player's mod for its numbers. They arrive about a second after this window opens.";
		case Opp_NotSharing: return "This player's mod did not answer. It is probably an early test version.";
		case Opp_Stale:      return "This player's numbers stopped arriving.";
		default:             return "";
		}
	}

	std::string NameOf(uint64_t id)
	{
		if (!id || !g_interfaces.pSteamFriendsWrapper)
			return std::string();
		if (IsSpectatorTestMode())
			return (id & 1) ? "Test P1" : "Test P2";
		const char* name = g_interfaces.pSteamFriendsWrapper->GetFriendPersonaName(CSteamID(id));
		return name ? name : "";
	}

	std::string VersionOf(uint64_t id)
	{
		IMPlayer player;
		if (id && g_interfaces.pRoomManager && g_interfaces.pRoomManager->FindIMPlayer(id, &player) && player.versionKnown)
			return player.modVersion;
		return std::string();
	}

	// Who is being shown, in the viewer's words: You / your opponent, or P1 / P2 by name.
	struct Perspective
	{
		bool spectator = false;
		std::string name[2]; // spectator: "P1 (name)" / "P2 (name)"
	};

	std::string SideHeadline(const LagVerdict& v, const Perspective& who)
	{
		// For players side 0 is you; for spectators side 0 is whichever player's report was
		// judged first, and who.name says which.
		auto pick = [&](const char* you, const char* opp, const char* named) -> std::string
		{
			if (who.spectator)
				return FormatText(L(named).c_str(), who.name[v.side].c_str());
			return L(v.side == 0 ? you : opp);
		};
		switch (v.kind)
		{
		case Lag_Checking: return L("Checking... give it a few seconds of play.");
		case Lag_None:     return L("Nobody's - it is running smoothly.");
		case Lag_Pc:
			return pick("Your side: your PC can't keep up with 60 FPS.",
				"Your opponent's side: their PC can't keep up with 60 FPS.",
				"%s's side: their PC can't keep up with 60 FPS.");
		case Lag_Unstable:
			return pick("Your side: your connection drops out now and then.",
				"Your opponent's side: their connection drops out now and then.",
				"%s's side: their connection drops out now and then.");
		case Lag_UnstableUnknown: return L("The connection is unsteady, but it is not clear whose side it is.");
		case Lag_SlowLine:
			return pick("Your side: your internet line adds most of the delay.",
				"Your opponent's side: their internet line adds most of the delay.",
				"%s's side: their internet line adds most of the delay.");
		case Lag_Relayed:  return L("The route: Steam can't connect you directly and detours through its servers.");
		case Lag_Distance: return L("Nobody's fault - you are just far apart.");
		case Lag_LongRoute: return L("The route: the connection takes a detour - it should be faster.");
		default:           return std::string();
		}
	}

	const char* SideAdvice(LagKind kind)
	{
		switch (kind)
		{
		case Lag_None:            return "Both PCs keep up and the connection is steady. A few small rollbacks are normal online.";
		case Lag_Pc:              return "The game has to wait for the slower PC, so both players feel it. Closing other programs or lowering the graphics settings can help.";
		case Lag_Unstable:        return "Messages from that side arrive with long pauses - usually Wi-Fi or someone else using the connection. A cable instead of Wi-Fi usually fixes it.";
		case Lag_UnstableUnknown: return "Messages arrive unevenly. Wi-Fi or a busy connection on either side can do this; a cable instead of Wi-Fi is the first thing to try.";
		case Lag_SlowLine:        return "That side's internet is slow to reach even Valve's nearest server. A cable helps if it is on Wi-Fi; otherwise it is the internet provider, or a long way to the nearest server.";
		case Lag_Relayed:         return "Opening the game's ports on the router, or a VPN, can let Steam connect you directly, which usually lowers the ping.";
		case Lag_Distance:        return "Both internet lines look fine; the delay is the distance between you. A VPN rarely helps here.";
		case Lag_LongRoute:       return "Steam's map of where you both are says the ping could be clearly lower. See the recommendation below.";
		default:                  return "";
		}
	}

	ImVec4 HeadlineColor(LagKind kind)
	{
		switch (kind)
		{
		case Lag_None:     return kGood;
		case Lag_Distance: return kWarn;
		case Lag_Checking: return kDim;
		default:           return kBad;
		}
	}

	const char* HowItIsDecided()
	{
		return "How this is decided: a PC that can't hold 60 FPS comes first. Then the connection: long pauses between messages from one side point at that side; a high ping (over 100 ms) is blamed on whichever player's own line to Valve's nearest server is slow, on Steam's detour servers if it is using them, or else on distance. Limits: it cannot see a bad route between two good internet lines, and the other side's numbers need them to run this mod.";
	}

	std::string PcText(PcRating r)
	{
		switch (r)
		{
		case Pc_Good:       return L("Good");
		case Pc_Struggling: return L("Struggling (can't hold 60 FPS)");
		default:            return L("Checking...");
		}
	}

	ImVec4 PcColor(PcRating r)
	{
		return r == Pc_Good ? kGood : (r == Pc_Struggling ? kBad : kDim);
	}

	const char* LinkWords(uint8_t link)
	{
		return link == Link_WiFi ? "on Wi-Fi" : (link == Link_Wired ? "wired" : "");
	}

	// "Good (wired)", "Unstable (on Wi-Fi)"; just the rating when the link type is unknown.
	std::string ConnWithLink(const std::string& rating, const Report* own)
	{
		const char* link = own ? LinkWords(own->linkType) : "";
		return *link ? rating + " (" + L(link) + ")" : rating;
	}

	std::string ConnText(ConnRating r)
	{
		switch (r)
		{
		case Conn_Good:     return L("Good");
		case Conn_Unstable: return L("Unstable");
		case Conn_SlowLine: return L("Slow line");
		default:            return L("Checking...");
		}
	}

	ImVec4 ConnColor(ConnRating r)
	{
		return r == Conn_Good ? kGood : (r == Conn_Unknown ? kDim : kWarn);
	}

	// One column of the plain view and of the details table.
	struct PerfColumn
	{
		std::string header;
		std::string headerTip;
		bool isLocal = false;
		OpponentView view; // for a remote column
		Report display = {}; // what the cells draw: the report, with pair-wide values shared
		bool hasDisplay = false;
	};

	bool HasData(const PerfColumn& c) { return c.isLocal || c.view.state == Opp_Live || c.view.state == Opp_Stale; }

	std::string DescribeReport(const std::string& who, const Report& r)
	{
		std::string line = who + ":";
		if (HasFrames(r))
			line += FormatText(" Game+Draw %.0f%% (%.0f FPS), real %.1f FPS",
				CapacityPercent(r.gameDrawP90Us), 1000000.0 / r.gameDrawP90Us,
				r.intervalAvgUs ? 1000000.0 / r.intervalAvgUs : 0.0);
		if (HasResim(r))
			line += FormatText(", resim %.0f%%", CapacityPercent(r.resimFrameAvgUs));
		if (HasNet(r))
			line += FormatText(", ping %u ms, jitter %u ms, longest silence %u ms, %s",
				(unsigned)r.pingMs, (unsigned)r.jitterMs, (unsigned)r.maxRecvGapMs,
				(r.flags & Flag_UsingRelay) ? "Steam relay" : "direct");
		if (r.relayPingMs)
			line += FormatText(", nearest Valve server %u ms", (unsigned)r.relayPingMs);
		if (HasFrames(r))
			line += FormatText(", rollbacks %u in 2 s (deepest %u f), %u this match",
				(unsigned)r.rollbacksRecent, (unsigned)r.maxDepthRecent, (unsigned)r.totalRollbacks);
		return line + "\n";
	}

	// Everything the window shows, gathered once per frame.
	struct Model
	{
		bool online = false;
		bool spectating = false;
		bool offlineTest = false;
		std::vector<PerfColumn> columns;
		Perspective who;
		LagVerdict verdict;
		bool hasVerdict = false;
		const Report* pingSource = nullptr;
		RouteCheck route;
		bool bothCapable = false; // the other side's numbers can exist at all
	};

	Model BuildModel()
	{
		Model m;
		m.online = IsOnlineMatchAsPlayer();
		m.spectating = IsSpectating();
		m.offlineTest = !m.online && !m.spectating && IsInAnyMatch() && Settings::settingsIni.enableInDevelopmentFeatures;
		const Report& local = LocalReport();

		if (m.spectating)
		{
			m.who.spectator = true;
			for (int side = 0; side < 2; ++side)
			{
				PerfColumn c;
				c.view = GetPlayerView(side);
				const std::string name = NameOf(c.view.steamId);
				c.header = std::string(side == 0 ? "P1" : "P2");
				const std::string version = VersionOf(c.view.steamId);
				c.headerTip = name + (version.empty() ? "" : " - " + version);
				m.columns.push_back(c);
			}
			// Judge from whichever player's report is in, naming the sides accordingly.
			for (int first = 0; first < 2; ++first)
			{
				const PerfColumn& a = m.columns[first];
				if (!HasData(a) || !HasFrames(a.view.report))
					continue;
				m.verdict = JudgeLag(a.view.report, m.columns[1 - first].view);
				if (m.verdict.side >= 0)
					m.verdict.side = m.verdict.side == 0 ? first : 1 - first;
				m.hasVerdict = true;
				m.pingSource = &m.columns[first].view.report;
				m.bothCapable = true;
				const PerfColumn& b = m.columns[1 - first];
				m.route = CheckRoute(a.view.report, HasData(b) ? &b.view.report : nullptr);
				if (m.route.side >= 0)
					m.route.side = m.route.side == 0 ? first : 1 - first;
				if (first == 1)
				{
					std::swap(m.route.lastMile[0], m.route.lastMile[1]);
					std::swap(m.route.excess[0], m.route.excess[1]);
				}
				break;
			}
			for (int side = 0; side < 2; ++side)
			{
				const std::string name = NameOf(m.columns[side].view.steamId);
				m.who.name[side] = std::string(side == 0 ? "P1" : "P2") + (name.empty() ? "" : " (" + name + ")");
			}
			return m;
		}

		PerfColumn you;
		you.header = L("You");
		you.isLocal = true;
		m.columns.push_back(you);
		if (m.online)
		{
			PerfColumn opp;
			opp.view = GetOpponentView();
			opp.header = L("Opponent");
			const std::string name = NameOf(opp.view.steamId);
			const std::string version = VersionOf(opp.view.steamId);
			opp.headerTip = name + (version.empty() ? "" : " - " + version);
			m.columns.push_back(opp);
			m.verdict = JudgeLag(local, m.columns[1].view);
			m.hasVerdict = true;
			const PerfColumn& o = m.columns[1];
			m.route = CheckRoute(local, HasData(o) ? &o.view.report : nullptr);
			m.bothCapable = o.view.state != Opp_NoMod && o.view.state != Opp_OldMod && o.view.state != Opp_NoOpponent;
		}
		m.pingSource = &local;
		return m;
	}

	const Report* ReportOf(const PerfColumn& c)
	{
		if (c.hasDisplay)
			return &c.display;
		return c.isLocal ? &LocalReport() : (HasData(c) ? &c.view.report : nullptr);
	}

	// Steam's expected ping is one number for the pair: once either side has it, show it in
	// both columns.
	void ShareSymmetricValues(std::vector<PerfColumn>& columns)
	{
		for (PerfColumn& c : columns)
		{
			c.hasDisplay = false;
			const Report* r = ReportOf(c);
			if (r)
			{
				c.display = *r;
				c.hasDisplay = true;
			}
		}
		if (columns.size() == 2 && columns[0].hasDisplay && columns[1].hasDisplay)
		{
			Report& a = columns[0].display;
			Report& b = columns[1].display;
			if (!a.expectedPingMs) a.expectedPingMs = b.expectedPingMs;
			if (!b.expectedPingMs) b.expectedPingMs = a.expectedPingMs;
		}
	}

	const char* RouteCheckExplained()
	{
		return "How this works: when the game connects you directly, each mod sends the other a rough location from Steam (never shown or saved) and the code of its nearest Valve server. From those it works out what ping your two locations should allow, how fast each line reaches the nearest server, and how much each side's internet provider adds to reach the other side's region. If the real ping is much higher than that, the side whose provider adds more should turn on a VPN such as Cloudflare WARP, which carries that side's traffic into a faster network. Limits: it needs both players on this mod version and a direct connection - if a VPN makes Steam route the connection through its own servers, the check switches off. It is an estimate, not a guarantee.";
	}

	// "Recommendation: ..." in the viewer's words.
	std::string RecommendationText(const Model& m, ImVec4* color)
	{
		const RouteCheck& r = m.route;
		auto who = [&](int side, const char* you, const char* opp, const char* named) -> std::string
		{
			if (m.who.spectator)
				return FormatText(L(named).c_str(), m.who.name[side].c_str());
			return L(side == 0 ? you : opp);
		};
		switch (r.state)
		{
		case Route_Fine:
		{
			// The route is as short as the locations allow, but one side's own line can still be
			// slow; a VPN does not shorten that, so say so instead of a bare "no VPN needed".
			const bool slow0 = r.lastMile[0] >= kSlowLineRelayMs, slow1 = r.lastMile[1] >= kSlowLineRelayMs;
			if (slow0 != slow1)
			{
				*color = kWarn;
				return who(slow0 ? 0 : 1,
					"Recommendation: no VPN needed - the route is as short as it can be. Your own line is slow to reach the internet; only a better connection lowers that.",
					"Recommendation: no VPN needed - the route is as short as it can be. Your opponent's line is slow to reach the internet; only a better connection lowers that.",
					"Recommendation: no VPN needed - the route is as short as it can be. %s's line is slow to reach the internet; only a better connection lowers that.");
			}
			*color = kGood;
			return L("Recommendation: no VPN needed - the ping is what your locations allow.");
		}
		case Route_VpnSide:
			*color = kBad;
			return who(r.side,
				"Recommendation: you should turn on your VPN (e.g. Cloudflare WARP) - your internet provider takes a long way to reach your opponent.",
				"Recommendation: your opponent should turn on their VPN (e.g. Cloudflare WARP) - their internet provider takes a long way to reach you.",
				"Recommendation: %s should turn on their VPN (e.g. Cloudflare WARP) - their internet provider takes a long way to reach the other player.");
		case Route_VpnEither:
			*color = kWarn;
			if (r.side < 0)
				return L("Recommendation: either of you could try a VPN (e.g. Cloudflare WARP) - the path between your two providers is the slow part.");
			return who(r.side,
				"Recommendation: either of you could try a VPN (e.g. Cloudflare WARP) - start with you, and if it doesn't help, your opponent.",
				"Recommendation: either of you could try a VPN (e.g. Cloudflare WARP) - start with your opponent, and if it doesn't help, you.",
				"Recommendation: either player could try a VPN (e.g. Cloudflare WARP) - start with %s.");
		case Route_LineSide:
			*color = kWarn;
			return who(r.side,
				"Recommendation: your own line is slow even to Valve's nearest server - a cable or a better connection will help more than a VPN.",
				"Recommendation: your opponent's line is slow even to Valve's nearest server - a cable or a better connection will help them more than a VPN.",
				"Recommendation: %s's line is slow even to Valve's nearest server - a cable or a better connection will help more than a VPN.");
		case Route_CantCheckRelayed:
			*color = kDim;
			return L("Route check off: the connection goes through Steam's servers (no direct connection), so routes can't be compared.");
		default:
			*color = kDim;
			return m.bothCapable ? L("Route check: waiting for both sides' numbers.")
				: L("Route check: needs both players on this mod version.");
		}
	}

	void LinkCell(const Report& r)
	{
		ImGui::TextUnformatted(r.linkType == Link_WiFi ? L("Wi-Fi").c_str() : (r.linkType == Link_Wired ? L("Wired").c_str() : L("Unknown").c_str()));
	}

	void ExpectedPingCell(const Report& r)
	{
		if (!r.expectedPingMs)
		{
			if (r.flags & Flag_UsingRelay)
				Pending("n/a (relayed)", "Steam routes this connection through its own servers, so the two locations are not exchanged and this cannot be worked out.");
			else
				Pending("waiting for opponent", "Needs the other player's rough location from Steam; it is exchanged while either of you has this window open and the connection is direct.");
			return;
		}
		ImGui::Text("%u ms", (unsigned)r.expectedPingMs);
	}

	void RouteExcessCell(const Report& r)
	{
		if (r.flags & Flag_UsingRelay)
		{
			Pending("n/a (relayed)", "Steam routes this connection through its own servers, so the two locations are not exchanged and this cannot be worked out.");
			return;
		}
		if (!r.relayPingMs)
		{
			Pending("measuring...", "Steam is still measuring the ping to its servers; it starts when you enter online mode and takes a few seconds.");
			return;
		}
		if (!r.crossPingMs)
		{
			Pending("waiting for opponent", "Needs the code of the other player's nearest Valve server; it is exchanged while either of you has this window open and the connection is direct.");
			return;
		}
		const int excess = (std::max)(0, (int)r.crossPingMs - (int)r.relayPingMs);
		ImGui::Text("%d ms", excess);
	}

	// Rows for the details table that need the helpers above; drawn after kRows.
	const Row kRouteRows[] = {
		{ "Network", "Whether this player's PC reaches the internet over Wi-Fi or a cable, as Windows reports it for the adapter that carries the match. Wi-Fi is the most common cause of an unstable connection.", LinkCell, true },
		{ "Steam's expected ping", "What Steam's map of both players' locations says the ping between them should be. Only available when the game connects them directly.", ExpectedPingCell, true },
		{ "Route excess", "How much this player's internet provider adds on the way to the other player's region, beyond its own line to the nearest Valve server. A clearly higher number on one side is the sign that a VPN on that side should help.", RouteExcessCell, true },
	};
}

void PerfStatsWindow::RegisterLayoutSettings()
{
	// AddSettingsHandler asserts on a duplicate type, and init can run again after a device loss.
	if (ImGui::FindSettingsHandler("BBCFIMPerfStats"))
		return;

	ImGuiSettingsHandler handler;
	handler.TypeName = "BBCFIMPerfStats";
	handler.TypeHash = ImHashStr("BBCFIMPerfStats");
	handler.ReadOpenFn = Layout_ReadOpen;
	handler.ReadLineFn = Layout_ReadLine;
	handler.WriteAllFn = Layout_WriteAll;
	ImGui::AddSettingsHandler(&handler);
}

void PerfStatsWindow::OpenFocused()
{
	Open();
	m_focusNextFrame = true;
}

void PerfStatsWindow::ToggleFromHotkey()
{
	if (IsOpen())
		Close();
	else
		OpenFocused();
}

namespace
{
	// The details table's widths, which also fix the window's width: everything in the window
	// is laid out to this, so it never changes size as numbers come and go.
	struct Widths
	{
		float label;
		float value;
		float table; // with 2 value columns
	};

	Widths ComputeWidths()
	{
		Widths w;
		w.label = ImGui::CalcTextSize(L("Status").c_str()).x;
		for (const Row& row : kRows)
			w.label = (std::max)(w.label, ImGui::CalcTextSize(L(row.label).c_str()).x);
		for (const Row& row : kRouteRows)
			w.label = (std::max)(w.label, ImGui::CalcTextSize(L(row.label).c_str()).x);
		w.value = BarWidth();
		for (const char* key : { "Unstable", "Slow line" })
			w.value = (std::max)(w.value, ImGui::CalcTextSize((L(key) + " (" + L("on Wi-Fi") + ")").c_str()).x + 12.0f);
		for (const char* sample : { "8888% (888 FPS)", "88.8 / 88.8 / 88.8 ms", "88.8 (longest 888 ms)" })
			w.value = (std::max)(w.value, ImGui::CalcTextSize(sample).x);
		for (const char* key : { "Rollback: too few to judge", "Too slow for 60 FPS", "OK, little headroom", "Steam relay",
			"No opponent", "Older mod", "No update", "Waiting...", "Struggling (can't hold 60 FPS)", "%.2f ms + %.2f ms/frame" })
			w.value = (std::max)(w.value, ImGui::CalcTextSize(L(key).c_str()).x + 12.0f);
		const ImGuiStyle& style = ImGui::GetStyle();
		w.table = w.label + w.value * 2.0f + style.CellPadding.x * 2.0f * 3.0f + 2.0f;
		return w;
	}
}

void PerfStatsWindow::BeforeDraw()
{
	ImGui::SetNextWindowPos(ImVec2(760, 200), ImGuiCond_FirstUseEver);
	// Not resizable: it fits its content. The width is pinned to the details table so the
	// window does not jump when numbers or wording change; only the height follows content.
	const float width = ComputeWidths().table + ImGui::GetStyle().WindowPadding.x * 2.0f;
	ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
	if (m_focusNextFrame)
	{
		ImGui::SetNextWindowFocus();
		m_focusNextFrame = false;
	}
}

void PerfStatsWindow::Draw()
{
	// Open (even collapsed) = we want the others' numbers: this is what turns the exchange on,
	// for players and spectators alike, and off again ~1.5 s after the window closes.
	PerformanceMonitor::NoteStatsWindowOpen();

	if (ImGui::SmallButton((L("Copy stats") + "##perf_copy").c_str()))
		ImGui::SetClipboardText(BuildStatsText().c_str());
	WrappedTip(L("Copies what this window says, with each player's mod version and all the numbers, as text ready to paste into Discord.").c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("%s", MOD_VERSION);

	DrawStats();
}

void PerfStatsWindow::DrawStats()
{
	Model m = BuildModel();
	ShareSymmetricValues(m.columns);
	const Widths w = ComputeWidths();
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w.table);

	if (!m.online && !m.spectating && !m.offlineTest)
	{
		ImGui::TextDisabled("%s", L("Shows during an online match, for both players and for spectators: whether each PC keeps up, how good each connection is, and whose side any lag is on.").c_str());
		ImGui::PopTextWrapPos();
		return;
	}

	// ---- the headline: "If it lags, whose side is it on?"
	ImGui::SeparatorText(L("If it lags, whose side is it on?").c_str());
	const float answerTop = ImGui::GetCursorPosY();
	if (m.hasVerdict)
	{
		ImGui::TextColored(HeadlineColor(m.verdict.kind), "%s", SideHeadline(m.verdict, m.who).c_str());
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
			ImGui::SetTooltipWrapped(L(HowItIsDecided()).c_str());
		std::string advice = *SideAdvice(m.verdict.kind) ? L(SideAdvice(m.verdict.kind)) : std::string(" ");
		if (m.verdict.kind == Lag_Unstable && m.verdict.side >= 0 && m.verdict.side < (int)m.columns.size())
		{
			// The side whose messages arrive late: say whether it is on Wi-Fi.
			const Report* r = ReportOf(m.columns[m.verdict.side]);
			if (r && r->linkType == Link_WiFi)
				advice = L("That side is on Wi-Fi: a cable will most likely fix it.");
			else if (r && r->linkType == Link_Wired)
				advice = L("That side is already wired, so it is the line itself or something else using it (downloads, streams, other devices).");
		}
		ImGui::TextDisabled("%s", advice.c_str());
	}
	else
	{
		ImGui::TextColored(kDim, "%s", m.offlineTest ? L("Offline test: your side only").c_str() : L("Checking... give it a few seconds of play.").c_str());
		ImGui::TextDisabled(" ");
	}
	// Reserve three lines for the answer and its advice whatever their length, so the rows
	// below (and the Details header) stay put when the answer changes.
	{
		const float used = ImGui::GetCursorPosY() - answerTop;
		const float want = ImGui::GetTextLineHeightWithSpacing() * 3.0f;
		if (used < want)
			ImGui::Dummy(ImVec2(1.0f, want - used - ImGui::GetStyle().ItemSpacing.y));
	}

	// ---- per player: PC and Connection in plain words
	const ImGuiTableFlags simpleFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH;
	if (ImGui::BeginTable("##perf_simple", 1 + (int)m.columns.size(), simpleFlags, ImVec2(w.table, 0.0f)))
	{
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, w.label);
		for (const PerfColumn& c : m.columns)
			ImGui::TableSetupColumn(c.header.c_str(), ImGuiTableColumnFlags_WidthFixed, w.value);
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int i = 0; i <= (int)m.columns.size(); ++i)
		{
			ImGui::TableSetColumnIndex(i);
			ImGui::TableHeader(i == 0 ? "" : m.columns[i - 1].header.c_str());
			if (i > 0 && !m.columns[i - 1].headerTip.empty() && ImGui::IsItemHovered())
				ImGui::SetTooltipWrapped(m.columns[i - 1].headerTip.c_str());
		}

		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(L("PC").c_str());
		WrappedTip(L("Whether this PC runs the game at a full 60 frames per second. If one PC can't, the game slows down for both players.").c_str());
		for (int i = 0; i < (int)m.columns.size(); ++i)
		{
			ImGui::TableSetColumnIndex(i + 1);
			const PerfColumn& c = m.columns[i];
			if (!HasData(c))
			{
				ImGui::TextColored(kDim, "%s", L(OpponentStateLabel(c.view.state)).c_str());
				const char* help = StateHelp(c.view.state);
				if (*help)
					WrappedTip(L(help).c_str());
				continue;
			}
			const PcRating r = RatePc(*ReportOf(c));
			ImGui::TextColored(PcColor(r), "%s", PcText(r).c_str());
		}

		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(L("Connection").c_str());
		WrappedTip(L("Good: this player's messages arrive steadily and their line is quick. Unstable: their messages arrive with long pauses (often Wi-Fi or a busy line). Slow line: their internet is slow to reach even Valve's nearest server (measured by Steam), so it adds delay to every match.").c_str());
		for (int i = 0; i < (int)m.columns.size(); ++i)
		{
			ImGui::TableSetColumnIndex(i + 1);
			const PerfColumn& c = m.columns[i];
			const Report* own = ReportOf(c);
			// How late this player's messages arrive is measured at the other end.
			const Report* other = m.columns.size() > 1 ? ReportOf(m.columns[1 - i]) : nullptr;
			if (!own && !other)
			{
				ImGui::TextColored(kDim, "-");
				continue;
			}
			const ConnRating r = RateConnection(own, other);
			ImGui::TextColored(ConnColor(r), "%s", ConnWithLink(ConnText(r), own).c_str());
		}
		ImGui::EndTable();
	}

	// ---- the shared numbers: ping and route
	const Report* net = m.pingSource && HasNet(*m.pingSource) ? m.pingSource : nullptr;
	if (net)
	{
		const unsigned ping = net->pingMs;
		const char* distance = ping < 60 ? "close" : (ping < kHighPingMs ? "medium distance" : "far apart");
		if (m.route.expectedMs > 0)
			ImGui::Text("%s", FormatText(L("Ping between you: %u ms (%s) - your locations allow about %d ms").c_str(), ping, L(distance).c_str(), m.route.expectedMs).c_str());
		else
			ImGui::Text("%s", FormatText(L("Ping between you: %u ms (%s)").c_str(), ping, L(distance).c_str()).c_str());
		WrappedTip(L("How long a message takes to go to the other player and back. Under about 60 ms feels local; over about 100 ms the game has to correct more often. The ping is shared by both players - the Connection row says whose line adds to it.").c_str());
		ImGui::TextDisabled("%s", (net->flags & Flag_UsingRelay) ? L("Route: through Steam's servers (no direct connection)").c_str() : L("Route: direct").c_str());
	}
	else
	{
		ImGui::TextDisabled("%s", L("Ping between you: not measured yet").c_str());
		ImGui::TextDisabled(" ");
	}

	// ---- the route check's recommendation: who should turn on a VPN
	if (m.online || m.spectating)
	{
		const float recTop = ImGui::GetCursorPosY();
		ImVec4 color = kDim;
		const std::string rec = RecommendationText(m, &color);
		ImGui::TextColored(color, "%s", rec.c_str());
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
			ImGui::SetTooltipWrapped(L(RouteCheckExplained()).c_str());
		const float used = ImGui::GetCursorPosY() - recTop;
		const float want = ImGui::GetTextLineHeightWithSpacing() * 3.0f;
		if (used < want)
			ImGui::Dummy(ImVec2(1.0f, want - used - ImGui::GetStyle().ItemSpacing.y));
	}

	// ---- the full table, for whoever wants the numbers
	ImGui::SetNextItemOpen(g_detailsOpen, ImGuiCond_Always);
	const bool details = ImGui::CollapsingHeader((L("Details") + "##perf_details").c_str());
	if (details != g_detailsOpen)
	{
		g_detailsOpen = details;
		ImGui::MarkIniSettingsDirty();
	}
	if (details)
	{
		const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg;
		if (ImGui::BeginTable("##perf_table", 1 + (int)m.columns.size(), flags, ImVec2(w.table, 0.0f)))
		{
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, w.label);
			for (const PerfColumn& c : m.columns)
				ImGui::TableSetupColumn(c.header.c_str(), ImGuiTableColumnFlags_WidthFixed, w.value);
			ImGui::TableHeadersRow();

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(L("Status").c_str());
			WrappedTip(L("Whether each PC keeps up with 60 FPS (first line) and can absorb the rollbacks this connection produces without slowing down (second line).").c_str());
			for (int i = 0; i < (int)m.columns.size(); ++i)
			{
				ImGui::TableSetColumnIndex(i + 1);
				const PerfColumn& c = m.columns[i];
				if (HasData(c))
					StatusCell(*ReportOf(c), !c.isLocal || m.online);
				else
				{
					ImGui::TextColored(kDim, "%s", L(OpponentStateLabel(c.view.state)).c_str());
					ImGui::TextUnformatted(" ");
				}
			}

			const Row* const tables[2] = { kRows, kRouteRows };
			const int counts[2] = { (int)(sizeof(kRows) / sizeof(kRows[0])), (int)(sizeof(kRouteRows) / sizeof(kRouteRows[0])) };
			for (int t = 0; t < 2; ++t)
			{
				for (int k = 0; k < counts[t]; ++k)
				{
					const Row& row = tables[t][k];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(L(row.label).c_str());
					WrappedTip(L(row.help).c_str());
					for (int i = 0; i < (int)m.columns.size(); ++i)
					{
						ImGui::TableSetColumnIndex(i + 1);
						if (HasData(m.columns[i]))
							row.fn(*ReportOf(m.columns[i]));
						else
							ImGui::TextColored(kDim, "%s", L(OpponentStateLabel(m.columns[i].view.state)).c_str());
					}
				}
			}
			ImGui::EndTable();
		}

		std::string ages;
		for (const PerfColumn& c : m.columns)
		{
			if (c.isLocal || (c.view.state != Opp_Live && c.view.state != Opp_Stale))
				continue;
			if (!ages.empty())
				ages += "   ";
			ages += FormatText(L("%s's numbers: %.0f s ago").c_str(), c.header.c_str(), c.view.ageSeconds);
		}
		ImGui::TextDisabled("%s", ages.empty() ? " " : ages.c_str());
	}

	if (Settings::settingsIni.enableInDevelopmentFeatures && (m.offlineTest || IsSpectatorTestMode()))
	{
		bool test = IsSpectatorTestMode();
		if (ImGui::Checkbox((L("Test: spectator view with fake players") + "##perf_spectest").c_str(), &test))
			SetSpectatorTestMode(test);
		WrappedTip(L("In-development check, offline only: shows the spectator layout fed by made-up player reports pushed through the same receive code as real ones. It cycles through three situations every 6 seconds.").c_str());
	}
	ImGui::PopTextWrapPos();
}

std::string PerfStatsWindow::BuildStatsText() const
{
	std::string out = std::string("BBCF Improvement Mod ") + MOD_VERSION;
	RoomManager* const room = g_interfaces.pRoomManager;
	if (room && room->IsRoomFunctional())
	{
		out += " - " + room->GetRoomTypeName() + "\n";
		const RoomMemberEntry* const self = room->GetThisPlayerRoomMemberEntry();
		for (uint16_t i = 0; i < MAX_PLAYERS_IN_ROOM; ++i)
		{
			const RoomMemberEntry* const entry = room->GetRoomMemberEntryByIndex(i);
			if (!entry)
				continue;
			const char* name = g_interfaces.pSteamFriendsWrapper ? g_interfaces.pSteamFriendsWrapper->GetFriendPersonaName(CSteamID(entry->steamId)) : "";
			std::string mod;
			IMPlayer imPlayer;
			if (self && entry->steamId == self->steamId)
				mod = MOD_VERSION;
			else if (room->FindIMPlayer(entry->steamId, &imPlayer))
				mod = imPlayer.versionKnown ? imPlayer.modVersion : L("Older mod");
			else
				mod = L("No mod");
			out += FormatText("  %s: %s\n", name ? name : "?", mod.c_str());
		}
	}
	else
	{
		out += "\n";
	}

	Model m = BuildModel();
	ShareSymmetricValues(m.columns);
	if (m.hasVerdict)
	{
		out += L("If it lags, whose side is it on?") + " " + SideHeadline(m.verdict, m.who) + "\n";
		ImVec4 unused;
		out += RecommendationText(m, &unused) + "\n";
		if (m.route.expectedMs > 0)
			out += FormatText("Route: actual %d ms, expected about %d ms (Steam estimate %d ms), last mile %d / %d ms, route excess %d / %d ms\n",
				m.route.actualMs, m.route.expectedMs, m.route.steamEstimateMs, m.route.lastMile[0], m.route.lastMile[1],
				m.route.excess[0], m.route.excess[1]);
	}
	for (int i = 0; i < (int)m.columns.size(); ++i)
	{
		const PerfColumn& c = m.columns[i];
		const std::string who = m.who.spectator ? m.who.name[i] : c.header;
		const Report* own = ReportOf(c);
		if (!own)
		{
			out += who + ": " + L(OpponentStateLabel(c.view.state)) + "\n";
			continue;
		}
		const Report* other = m.columns.size() > 1 ? ReportOf(m.columns[1 - i]) : nullptr;
		out += who + ": " + L("PC") + " " + PcText(RatePc(*own)) + ", " + L("Connection") + " " + ConnWithLink(ConnText(RateConnection(own, other)), own) + "\n";
		out += DescribeReport("  " + who, *own);
	}
	return out;
}
