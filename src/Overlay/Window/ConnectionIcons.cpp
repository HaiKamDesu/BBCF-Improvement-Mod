#include "ConnectionIcons.h"

#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/utils.h"
#include "Game/PerformanceMonitor.h"
#include "Network/RoomManager.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

using namespace PerformanceMonitor;

namespace
{
	// Same colours as the stats window (PerfStatsWindow.cpp).
	const ImVec4 kGood(0.45f, 0.90f, 0.45f, 1.0f);
	const ImVec4 kWarn(1.00f, 0.65f, 0.20f, 1.0f);
	const ImVec4 kBad(1.00f, 0.38f, 0.32f, 1.0f);
	const ImVec4 kDim(0.62f, 0.62f, 0.62f, 1.0f);

	constexpr int kMinSize = 12;
	constexpr int kMaxSize = 256;

	// The size while a corner is being dragged; written to settings.ini on release.
	float g_dragSize = -1.0f;

	struct SideData
	{
		bool hasReport = false;
		bool stale = false;
		bool isLocal = false;
		OpponentState state = Opp_NotOnline;
		Report report = {};
		uint64_t steamId = 0;
	};

	struct Rating
	{
		ImVec4 color = kDim;
		std::string state;   // "Good", "Struggling", ... (the tooltip's first line)
		std::string detail;  // what it means, with the numbers
	};

	bool HasFrames(const Report& r) { return (r.flags & Flag_HasFrames) != 0 && r.gameDrawP90Us > 0; }

	// No numbers will ever come from this player: draw the question mark, not a grey icon
	// that looks like "loading".
	bool CannotShare(const SideData& side)
	{
		return side.state == Opp_NoMod || side.state == Opp_OldMod || side.state == Opp_NotSharing;
	}

	int IconSize()
	{
		const int size = g_dragSize > 0.0f ? (int)g_dragSize : Settings::settingsIni.connectionIconsSize;
		return (std::min)(kMaxSize, (std::max)(kMinSize, size));
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

	void FillRemote(SideData& side, const OpponentView& view)
	{
		side.state = view.state;
		side.steamId = view.steamId;
		side.report = view.report;
		side.hasReport = (view.state == Opp_Live || view.state == Opp_Stale) && HasFrames(view.report);
		side.stale = view.state == Opp_Stale;
	}

	// Both sides, from whichever role this client has in the match (the fake-players test
	// counts as spectating). Returns false outside an online match: the icons are then only a
	// preview for arranging them.
	bool GatherSides(SideData sides[2])
	{
		if (IsOnlineMatchAsPlayer())
		{
			RoomManager* room = g_interfaces.pRoomManager;
			int local = room && room->IsRoomFunctional() ? (int)room->GetThisPlayerMatchPlayerIndex() : 0;
			if (local < 0 || local > 1)
				local = 0;
			SideData& me = sides[local];
			me.isLocal = true;
			me.report = LocalReport();
			me.hasReport = HasFrames(me.report);
			me.state = Opp_Live;
			if (room && room->GetThisPlayerRoomMemberEntry())
				me.steamId = room->GetThisPlayerRoomMemberEntry()->steamId;
			FillRemote(sides[1 - local], GetOpponentView());
			return true;
		}
		if (IsSpectating())
		{
			for (int side = 0; side < 2; ++side)
				FillRemote(sides[side], GetPlayerView(side));
			return true;
		}
		return false;
	}

	std::string WhyNoNumbers(const SideData& side, bool inMatch)
	{
		if (!inMatch)
			return L("Shown here so you can place it. During online matches it shows this player's state.");
		switch (side.state)
		{
		case Opp_NoMod:      return L("This player does not run the mod, so their numbers are unknown.");
		case Opp_OldMod:     return L("This player runs an older mod version that does not share these numbers.");
		case Opp_NotSharing: return L("This player's mod is not sending its numbers.");
		case Opp_NoOpponent: return L("No opponent found yet.");
		default:             return L("Waiting for numbers...");
		}
	}

	Rating RatePcSide(const SideData& side, bool inMatch)
	{
		Rating r;
		if (CannotShare(side))
		{
			r.state = L("No mod");
			r.detail = WhyNoNumbers(side, inMatch);
			return r;
		}
		if (!side.hasReport)
		{
			r.state = inMatch ? L("Unknown") : L("Preview");
			r.detail = WhyNoNumbers(side, inMatch);
			return r;
		}
		const double capacity = CapacityPercent(side.report.gameDrawP90Us);
		const double fps = side.report.intervalAvgUs ? 1000000.0 / side.report.intervalAvgUs : 0.0;
		const std::string numbers = FormatText(L("%.0f%% capacity, %.0f FPS").c_str(), capacity, fps);
		switch (RatePc(side.report))
		{
		case Pc_Struggling:
			r.color = kBad;
			r.state = L("Struggling");
			r.detail = L("This PC can't hold 60 FPS, so the match slows down for both players.") + "\n" + numbers;
			break;
		case Pc_Good:
			r.color = capacity < 120.0 ? kWarn : kGood;
			r.state = capacity < 120.0 ? L("Borderline") : L("Good");
			r.detail = (capacity < 120.0 ? L("Keeps up, with little room to spare.") : L("Runs the game with room to spare.")) + "\n" + numbers;
			break;
		default:
			r.state = L("Checking");
			r.detail = L("Give it a few seconds of play.");
			break;
		}
		return r;
	}

	Rating RateNetSide(const SideData& side, const SideData& other, bool inMatch)
	{
		Rating r;
		if (CannotShare(side))
		{
			r.state = L("No mod");
			r.detail = WhyNoNumbers(side, inMatch);
			return r;
		}
		if (!side.hasReport)
		{
			r.state = inMatch ? L("Unknown") : L("Preview");
			r.detail = WhyNoNumbers(side, inMatch);
			return r;
		}
		const Report* measuredByOther = other.hasReport ? &other.report : nullptr;
		const char* link = side.report.linkType == Link_WiFi ? "Wi-Fi" : (side.report.linkType == Link_Wired ? "Wired" : "");
		const std::string linkText = *link ? " (" + L(link) + ")" : "";
		std::string numbers = FormatText(L("Ping %u ms, jitter %u ms").c_str(), (unsigned)side.report.pingMs, (unsigned)side.report.jitterMs);
		if (measuredByOther)
			numbers += FormatText(L(", longest gap in their packets %u ms").c_str(), (unsigned)measuredByOther->maxRecvGapMs);
		if (side.report.relayPingMs)
			numbers += FormatText(L(", nearest Valve server %u ms").c_str(), (unsigned)side.report.relayPingMs);
		switch (RateConnection(&side.report, measuredByOther))
		{
		case Conn_Good:
			r.color = kGood;
			r.state = L("Good") + linkText;
			r.detail = L("Packets arrive steadily.") + "\n" + numbers;
			break;
		case Conn_SlowLine:
			r.color = kWarn;
			r.state = L("Slow line") + linkText;
			r.detail = L("High ping to the nearest Valve server: this side's line adds delay.") + "\n" + numbers;
			break;
		case Conn_Unstable:
			r.color = kBad;
			r.state = L("Unstable") + linkText;
			r.detail = L("Packets arrive with long gaps: this side causes stutter and rollbacks.") + "\n" + numbers;
			break;
		default:
			r.state = L("Checking") + linkText;
			r.detail = L("Needs the other player's numbers too.") + "\n" + numbers;
			break;
		}
		return r;
	}

	ImU32 ToU32(ImVec4 color, float alpha)
	{
		color.w *= alpha;
		return ImGui::GetColorU32(color);
	}

	// ---- icons, drawn in a size x size box at p ----
	// Each shape is drawn twice: a dark wider stroke, then the colour, so it reads on any
	// part of the stage or HUD without a background.
	//
	// All icons share one grid so they come out the same size: the same stroke and outline, a
	// live area inset kMargin from the box, and shapes placed in 0..1 units of the "centreline
	// box", the live area inset by half an outlined stroke. A stroke on the centreline box's edge
	// therefore puts its dark edge exactly on the live area, wherever the shape is.

	float Stroke(float s) { return (std::max)(1.5f, s * 0.09f); }
	float Outline(float s) { return (std::max)(2.0f, s * 0.07f); }

	struct Grid
	{
		ImVec2 origin;   // centreline box, top left
		float size;      // centreline box side, in pixels
		float t;         // colour stroke width
		float o;         // extra width of the dark outline stroke
		float half;      // half the outlined stroke: how far a stroke's dark edge reaches past its centreline

		ImVec2 At(float u, float v) const { return ImVec2(origin.x + u * size, origin.y + v * size); }
		float Len(float u) const { return u * size; }
		float Width(int pass) const { return pass == 0 ? t + o : t; }
	};

	Grid MakeGrid(ImVec2 p, float s)
	{
		const float kMargin = 0.06f;
		Grid g;
		g.t = Stroke(s);
		g.o = Outline(s);
		g.half = (g.t + g.o) * 0.5f;
		const float inset = s * kMargin + g.half;
		g.origin = ImVec2(p.x + inset, p.y + inset);
		g.size = s - inset * 2.0f;
		return g;
	}

	// The dot of the Wi-Fi and question mark icons: a little wider than the stroke.
	float DotRadius(const Grid& g) { return g.t * 0.7f; }

	void DrawDot(ImDrawList* dl, const Grid& g, ImVec2 center, int pass, ImU32 c)
	{
		dl->AddCircleFilled(center, pass == 0 ? DotRadius(g) + g.o * 0.5f : DotRadius(g), c, 16);
	}

	void DrawPc(ImDrawList* dl, ImVec2 p, float s, ImU32 color, ImU32 shadow)
	{
		// A monitor: the screen across the full width, its neck and base below.
		const Grid g = MakeGrid(p, s);
		const ImVec2 a = g.At(0.0f, 0.0f), b = g.At(1.0f, 0.66f);
		for (int pass = 0; pass < 2; ++pass)
		{
			const ImU32 c = pass == 0 ? shadow : color;
			const float w = g.Width(pass);
			dl->AddRect(a, b, c, g.Len(0.10f), 0, w);
			dl->AddLine(g.At(0.5f, 0.66f), g.At(0.5f, 1.0f), c, w);
			dl->AddLine(g.At(0.2f, 1.0f), g.At(0.8f, 1.0f), c, w);
		}
		// Screen fill, faint, so the colour reads even at small sizes.
		const float in = g.t * 0.5f;
		dl->AddRectFilled(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), (color & 0x00FFFFFF) | 0x50000000);
	}

	void DrawWifi(ImDrawList* dl, ImVec2 p, float s, ImU32 color, ImU32 shadow)
	{
		// Three arcs fanning 45 degrees either side of straight up, over a dot. The outer arc's
		// ends reach the box's sides; the whole fan is centred vertically.
		const Grid g = MakeGrid(p, s);
		const float pi = 3.14159265f;
		const float outer = g.Len(0.75f), step = g.Len(0.24f);
		const float dotBottom = DotRadius(g) + g.o * 0.5f;
		// Centre the span from the outer arc's top edge to the dot's bottom edge.
		const float cy = g.origin.y + (g.size + outer + g.half - dotBottom) * 0.5f;
		const ImVec2 center(g.origin.x + g.size * 0.5f, cy);
		for (int pass = 0; pass < 2; ++pass)
		{
			const ImU32 c = pass == 0 ? shadow : color;
			for (int i = 0; i < 3; ++i)
			{
				dl->PathArcTo(center, outer - step * (2 - i), -pi * 0.75f, -pi * 0.25f, 24);
				dl->PathStroke(c, 0, g.Width(pass));
			}
			DrawDot(dl, g, center, pass, c);
		}
	}

	void DrawCable(ImDrawList* dl, ImVec2 p, float s, ImU32 color, ImU32 shadow)
	{
		// An ethernet cable: the plug at the top (head with its contacts), its boot, and the
		// cable curling away below to the bottom left.
		const Grid g = MakeGrid(p, s);
		const ImVec2 headA = g.At(0.34f, 0.0f), headB = g.At(0.82f, 0.42f);
		const ImVec2 bootA = g.At(0.44f, 0.42f), bootB = g.At(0.72f, 0.58f);
		const ImVec2 c0 = g.At(0.58f, 0.58f), c1 = g.At(0.58f, 0.86f), c2 = g.At(0.20f, 0.70f), c3 = g.At(0.18f, 1.0f);
		for (int pass = 0; pass < 2; ++pass)
		{
			const ImU32 c = pass == 0 ? shadow : color;
			const float w = g.Width(pass);
			dl->AddBezierCubic(c0, c1, c2, c3, c, w, 24);
			dl->AddRect(headA, headB, c, g.Len(0.05f), 0, w);
			const float grow = pass == 0 ? g.o * 0.5f : 0.0f;
			dl->AddRectFilled(ImVec2(bootA.x - grow, bootA.y), ImVec2(bootB.x + grow, bootB.y + grow), c, g.Len(0.04f));
			for (int i = 0; i < 4; ++i)
			{
				const float x = headA.x + (headB.x - headA.x) * (0.2f + 0.2f * i);
				dl->AddLine(ImVec2(x, headA.y + g.Len(0.10f)), ImVec2(x, headA.y + g.Len(0.24f)), c, pass == 0 ? w * 0.7f : g.t * 0.6f);
			}
		}
	}

	void DrawSignalBars(ImDrawList* dl, ImVec2 p, float s, ImU32 color, ImU32 shadow)
	{
		// Link type unknown (preview, or not measured yet): plain signal bars. Filled shapes
		// reach as far as a stroke's colour would (half a stroke past the centreline box).
		const Grid g = MakeGrid(p, s);
		const float edge = g.t * 0.5f;
		const float left = g.origin.x - edge, span = g.size + edge * 2.0f;
		const float gap = span * 0.16f, barW = (span - gap * 3.0f) / 4.0f;
		const float top = g.origin.y - edge, bottom = g.origin.y + g.size + edge;
		for (int pass = 0; pass < 2; ++pass)
		{
			const ImU32 c = pass == 0 ? shadow : color;
			const float grow = pass == 0 ? g.o * 0.5f : 0.0f;
			for (int i = 0; i < 4; ++i)
			{
				const float x0 = left + (barW + gap) * i;
				const float y0 = top + (bottom - top) * (0.75f - 0.25f * i);
				dl->AddRectFilled(ImVec2(x0 - grow, y0 - grow), ImVec2(x0 + barW + grow, bottom + grow), c, 1.0f);
			}
		}
	}

	void DrawQuestionMark(ImDrawList* dl, ImVec2 p, float s, ImU32 color, ImU32 shadow)
	{
		// A player without the mod: their state cannot be known.
		const Grid g = MakeGrid(p, s);
		const float pi = 3.14159265f;
		const float r = g.Len(0.32f);
		const ImVec2 hook = g.At(0.5f, 0.32f);
		const ImVec2 dot(hook.x, g.origin.y + g.size + g.half - DotRadius(g) - g.o * 0.5f);
		for (int pass = 0; pass < 2; ++pass)
		{
			const ImU32 c = pass == 0 ? shadow : color;
			dl->PathArcTo(hook, r, -pi * 1.0f, pi * 0.45f, 24);
			dl->PathLineTo(g.At(0.5f, 0.74f));
			dl->PathStroke(c, 0, g.Width(pass));
			DrawDot(dl, g, dot, pass, c);
		}
	}

	enum IconKind { Icon_Pc, Icon_Net };

	// One draggable icon window. The label says whose it is ("P1" or "P2"; the tooltip says which is you).
	void DrawIconWindow(int side, IconKind kind, const SideData& data, const SideData& other, bool inMatch, bool locked)
	{
		ImGuiIO& io = ImGui::GetIO();
		const float s = (float)IconSize();

		// First-use spots: under each player's barrier gauge, PC then connection, mirrored.
		// The connection icon starts one icon-plus-label further right.
		const float x = io.DisplaySize.x * (side == 0 ? 0.172f : 0.800f) + (kind == Icon_Net ? s * 2.8f : 0.0f);
		ImGui::SetNextWindowPos(ImVec2(x, io.DisplaySize.y * 0.165f), ImGuiCond_FirstUseEver);

		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
		if (locked)
			flags |= ImGuiWindowFlags_NoMove;

		static const char* const kIds[2][2] = {
			{ "###ConnIconP1Pc", "###ConnIconP1Net" },
			{ "###ConnIconP2Pc", "###ConnIconP2Net" },
		};
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.0f, 2.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
		const bool visible = ImGui::Begin(kIds[side][kind], nullptr, flags);
		ImGui::PopStyleVar(3);
		if (!visible)
		{
			ImGui::End();
			return;
		}

		// Above every other window, so the icons can be found and dragged with the mod menu open
		// (they start where the menu usually is). Not over a modal popup, which owns the input.
		if (!ImGui::GetTopMostPopupModal())
			ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

		const bool unknownPlayer = CannotShare(data);
		const Rating rating = kind == Icon_Pc ? RatePcSide(data, inMatch) : RateNetSide(data, other, inMatch);
		const float alpha = data.stale ? 0.5f : 1.0f;
		const ImU32 color = ToU32(rating.color, alpha);
		const ImU32 shadow = IM_COL32(0, 0, 0, (int)(170 * alpha));

		const std::string label = side == 0 ? "P1" : "P2";
		ImFont* font = ImGui::GetFont();
		const float labelSize = (std::max)(9.0f, s * 0.5f);
		const ImVec2 labelExtent = font->CalcTextSizeA(labelSize, FLT_MAX, 0.0f, label.c_str());

		const ImVec2 p = ImGui::GetCursorScreenPos();
		const ImVec2 contentMax(p.x + s + 3.0f + labelExtent.x, p.y + s);
		ImGui::Dummy(ImVec2(contentMax.x - p.x, s));
		ImDrawList* dl = ImGui::GetWindowDrawList();

		if (unknownPlayer)
			DrawQuestionMark(dl, p, s, color, shadow);
		else if (kind == Icon_Pc)
			DrawPc(dl, p, s, color, shadow);
		else if (data.hasReport && data.report.linkType == Link_WiFi)
			DrawWifi(dl, p, s, color, shadow);
		else if (data.hasReport && data.report.linkType == Link_Wired)
			DrawCable(dl, p, s, color, shadow);
		else
			DrawSignalBars(dl, p, s, color, shadow);

		const ImVec2 textPos(p.x + s + 3.0f, p.y + s - labelExtent.y);
		dl->AddText(font, labelSize, ImVec2(textPos.x + 1.0f, textPos.y + 1.0f), shadow, label.c_str());
		dl->AddText(font, labelSize, textPos, IM_COL32(255, 255, 255, (int)(230 * alpha)), label.c_str());

		const ImVec2 wmin = ImGui::GetWindowPos();
		const ImVec2 wmax(wmin.x + ImGui::GetWindowWidth(), wmin.y + ImGui::GetWindowHeight());

		// Resize grip in the bottom-right corner: dragging it sets the one size all four icons
		// share. An item, so ImGui does not also treat the press as a window move. Inside the
		// content, not on the window edge: the window sizes itself to its content, and an item
		// on the edge would grow it every frame.
		bool resizing = false;
		if (!locked)
		{
			const float grip = (std::max)(8.0f, s * 0.28f);
			ImGui::SetCursorScreenPos(ImVec2(contentMax.x - grip, contentMax.y - grip));
			ImGui::InvisibleButton("##resize", ImVec2(grip, grip));
			const bool gripHovered = ImGui::IsItemHovered();
			resizing = ImGui::IsItemActive();
			if (resizing)
			{
				if (g_dragSize < 0.0f)
					g_dragSize = (float)Settings::settingsIni.connectionIconsSize;
				// Diagonal drag: whichever way the mouse moved more.
				const float delta = (std::fabs(io.MouseDelta.x) > std::fabs(io.MouseDelta.y)) ? io.MouseDelta.x : io.MouseDelta.y;
				g_dragSize = (std::min)((float)kMaxSize, (std::max)((float)kMinSize, g_dragSize + delta));
			}
			if (ImGui::IsItemDeactivated() && g_dragSize > 0.0f)
			{
				Settings::settingsIni.connectionIconsSize = (int)g_dragSize;
				Settings::changeSetting("ConnectionIconsSize", std::to_string(Settings::settingsIni.connectionIconsSize));
				g_dragSize = -1.0f;
			}
			if (gripHovered || resizing)
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
			if (ImGui::IsWindowHovered() || resizing)
			{
				const ImU32 gripColor = IM_COL32(255, 255, 255, (gripHovered || resizing) ? 220 : 120);
				dl->AddTriangleFilled(ImVec2(contentMax.x, contentMax.y - grip), contentMax, ImVec2(contentMax.x - grip, contentMax.y), gripColor);
			}
		}

		const bool hovered = ImGui::IsWindowHovered() || resizing;
		if (hovered || !inMatch)
		{
			// Outside a match (arranging) and on hover: a faint frame so the grab area shows.
			dl->AddRect(wmin, wmax, IM_COL32(255, 255, 255, hovered ? 110 : 45), 3.0f);
		}
		if (hovered)
		{
			// The game hides the OS cursor: show ImGui's while over an icon (as the ranked bar does).
			io.MouseDrawCursor = true;
			if (!resizing)
			{
				const std::string who = std::string(side == 0 ? "P1" : "P2") + (data.isLocal ? " (" + L("You") + ")" : "");
				const std::string name = NameOf(data.steamId);
				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
				ImGui::TextUnformatted(who.c_str());
				ImGui::SameLine(0.0f, 0.0f);
				ImGui::TextUnformatted((" " + (kind == Icon_Pc ? L("PC State") : L("Connection State")) + ": ").c_str());
				ImGui::SameLine(0.0f, 0.0f);
				ImGui::TextColored(unknownPlayer ? kDim : rating.color, "%s", rating.state.c_str());
				if (!name.empty())
					ImGui::TextDisabled("%s", name.c_str());
				ImGui::Separator();
				ImGui::TextUnformatted(rating.detail.c_str());
				if (data.stale)
					ImGui::TextDisabled("%s", L("(no update for a few seconds)").c_str());
				ImGui::Spacing();
				ImGui::TextDisabled("%s", L(locked ? "Positions are locked (PC & connection stats window, Ctrl+F2)."
					: "Drag to move, drag the corner to resize all four. Ctrl+F2 opens the full stats.").c_str());
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
		}
		ImGui::End();
	}
}

void DrawConnectionIconsStandalone(bool modMenuOpen)
{
	if (!Settings::settingsIni.connectionIcons)
		return;

	SideData sides[2];
	const bool inMatch = GatherSides(sides);
	const bool hudHidden = g_gameVals.pIsHUDHidden && (*g_gameVals.pIsHUDHidden & 1) != 0;
	if (!((inMatch && !hudHidden) || modMenuOpen))
		return;

	// Keeps the other side sending its numbers while the icons are up (the route check stays
	// tied to the stats window, see PerformanceMonitor.h).
	if (inMatch)
		NoteWidgetsVisible();

	const bool locked = Settings::settingsIni.connectionIconsLocked;
	for (int side = 0; side < 2; ++side)
	{
		DrawIconWindow(side, Icon_Pc, sides[side], sides[1 - side], inMatch, locked);
		DrawIconWindow(side, Icon_Net, sides[side], sides[1 - side], inMatch, locked);
	}
}

void DrawConnectionIconsControls()
{
	if (ImGui::Checkbox((L("Show connection icons on screen") + "##conn_icons").c_str(), &Settings::settingsIni.connectionIcons))
		Settings::changeSetting("ConnectionIcons", Settings::settingsIni.connectionIcons ? "1" : "0");
	ImGui::ShowHelpMarkerSameLine(L("During online matches, a small PC icon and a connection icon (Wi-Fi or cable) for each player stay on screen: green is fine, orange is borderline, red is the cause of lag, grey is unknown, a question mark is a player without the mod. Hover one for its state. While the mod menu (F1) is open they show anywhere: drag one to move it, drag its corner to resize all four.").c_str());
	if (!Settings::settingsIni.connectionIcons)
		return;
	ImGui::SameLine();
	if (ImGui::Checkbox((L("Lock positions") + "##conn_icons_lock").c_str(), &Settings::settingsIni.connectionIconsLocked))
		Settings::changeSetting("ConnectionIconsLocked", Settings::settingsIni.connectionIconsLocked ? "1" : "0");
	ImGui::SameLine();
	if (ImGui::SmallButton((L("Reset size") + "##conn_icons_size").c_str()))
	{
		Settings::settingsIni.connectionIconsSize = 22;
		Settings::changeSetting("ConnectionIconsSize", "22");
	}
}
