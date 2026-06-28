#include "FrameHistoryWindow.h"
#include "Game/gamestates.h"

#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Game/gamestates.h"
#include "imgui_internal.h"
#include "Core/utils.h"

#include <array>
#include <vector>
#include <algorithm>
#include <cmath>

#define FH_MAX(a,b) (((a) > (b)) ? (a) : (b))
#define FH_MIN(a,b) (((a) > (b)) ? (b) : (a))


static void DrawBox(ImDrawList* dl, ImVec2 pos, float w, float h, ImColor col) {
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), (ImU32)col);
}

static bool IsHovering(ImVec2 pos, float w, float h) {
	return ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + w, pos.y + h));
}

bool FrameHistoryWindow::hasWorldTimeMoved() {
	bool res = *g_gameVals.pFrameCount > last_frame;
	last_frame = *g_gameVals.pFrameCount;
	return res;
}

void FrameHistoryWindow::Update()
{
	if (!m_windowOpen || !isFrameHistoryEnabledInCurrentState())
	{
		history.clear();
		return;
	}

	if (!g_interfaces.player1.IsCharDataNullPtr() &&
		!g_interfaces.player2.IsCharDataNullPtr() && hasWorldTimeMoved()) {
		history.updateHistory(resetting, countEmptyFrames, maxHistoryFrames);
	}

	BeforeDraw();

	// Enforce minimum window size: padding + P1 block + padding + P2 block + padding
	// padding = height * 0.5, each player block = 2*height + spacing
	{
		ImGuiStyle& style = ImGui::GetStyle();
		float pad = height * 0.5f;
		float block_h = 2.f * height + spacing;
		float min_inner_h = 3.f * pad + 2.f * block_h;
		// label col ~22px, at least 1 frame col, X button ~20px
		float min_inner_w = 22.f + (width + spacing) + 20.f;
		ImGui::SetNextWindowSizeConstraints(
			ImVec2(min_inner_w + style.WindowPadding.x * 2.f, min_inner_h + style.WindowPadding.y * 2.f),
			ImVec2(FLT_MAX, FLT_MAX));
	}

	ImGui::Begin("##FrameHistory", nullptr, m_windowFlags);
	Draw();
	ImGui::End();

	// Close requested via the in-window X button
	if (!m_windowOpen) {
		Settings::settingsIni.frameHistoryEnabled = false;
		Settings::changeSetting("FrameHistoryEnabled", "0");
	}

	AfterDraw();
}

void FrameHistoryWindow::BeforeDraw() {}
void FrameHistoryWindow::AfterDraw() {}

void FrameHistoryWindow::Draw()
{
	ImDrawList* dl = ImGui::GetWindowDrawList();

	// --- Layout constants ---
	const float pad        = height * 0.5f;   // half box height, used as top/bottom/mid padding
	const float block_h    = 2.f * height + spacing;  // one player's two-row block height
	const float label_w    = ImGui::CalcTextSize("P1").x + 8.f;
	const float btn_size   = 12.f;
	const float btn_margin = 4.f;

	// Available content area
	const ImVec2 content_start = ImGui::GetCursorScreenPos();
	const float avail_w = ImGui::GetContentRegionAvail().x;
	const float avail_h = ImGui::GetContentRegionAvail().y;

	// Bars area: content minus label column and X button column
	const float bars_w = avail_w - label_w - btn_size - btn_margin * 2.f;

	// Update max frames dynamically from window width (used next Update tick)
	{
		int newMax = (int)(bars_w / (width + spacing));
		if (newMax < 1) newMax = 1;
		if (newMax > HISTORY_DEPTH_DEFAULT) newMax = HISTORY_DEPTH_DEFAULT;
		maxHistoryFrames = newMax;
	}

	// --- Anchor positions ---
	// P1: top-anchored with half-box padding
	const float p1_y = content_start.y + pad;
	// P2: bottom-anchored with half-box padding
	float p2_y = content_start.y + avail_h - pad - block_h;
	// Guard against overlap when window is too small
	p2_y = FH_MAX(p2_y, p1_y + block_h + pad);

	const float bars_x = content_start.x + label_w;

	// Divider line extents: slightly beyond P1 top and P2 bottom
	const float div_top    = p1_y - height * 0.25f;
	const float div_bottom = p2_y + block_h + height * 0.25f;

	// --- Reserve content area for ImGui layout ---
	ImGui::Dummy(ImVec2(avail_w, avail_h));

	// --- Draw P1 / P2 labels (vertically centered in their respective blocks) ---
	{
		ImVec2 sz = ImGui::CalcTextSize("P1");
		dl->AddText(ImVec2(content_start.x + (label_w - sz.x) * 0.5f,
		                   p1_y + (block_h - sz.y) * 0.5f),
		            IM_COL32(255, 255, 255, 255), "P1");
	}
	{
		ImVec2 sz = ImGui::CalcTextSize("P2");
		dl->AddText(ImVec2(content_start.x + (label_w - sz.x) * 0.5f,
		                   p2_y + (block_h - sz.y) * 0.5f),
		            IM_COL32(255, 255, 255, 255), "P2");
	}

	// --- X (close) button, right-aligned, vertically centered ---
	{
		ImVec2 btn_min = ImVec2(content_start.x + avail_w - btn_margin - btn_size,
		                        content_start.y + (avail_h - btn_size) * 0.5f);
		ImVec2 btn_max = ImVec2(btn_min.x + btn_size, btn_min.y + btn_size);

		bool hovered = ImGui::IsMouseHoveringRect(btn_min, btn_max);
		ImU32 bg_col = hovered ? IM_COL32(180, 50, 50, 220) : IM_COL32(120, 120, 120, 150);
		dl->AddRectFilled(btn_min, btn_max, bg_col, 2.f);

		float xm = 2.5f;
		dl->AddLine(ImVec2(btn_min.x + xm, btn_min.y + xm),
		            ImVec2(btn_max.x - xm, btn_max.y - xm),
		            IM_COL32(255, 255, 255, 255), 1.5f);
		dl->AddLine(ImVec2(btn_max.x - xm, btn_min.y + xm),
		            ImVec2(btn_min.x + xm, btn_max.y - xm),
		            IM_COL32(255, 255, 255, 255), 1.5f);

		if (hovered && ImGui::IsMouseClicked(0)) {
			m_windowOpen = false;
		}
	}

	// --- Frame columns ---
	StatePairQueue& queue = history.read();

	const ImColor color_inv(255, 255, 255);
	const ImColor color_gp(122, 85, 61);

	float col_x = bars_x;
	int frame_idx = 0;

	for (StatePairQueue::reverse_iterator elem = queue.rbegin(); elem != queue.rend(); ++elem) {
		const PlayerFrameState& p1s = elem->front();
		const PlayerFrameState& p2s = elem->back();

		// Colors for main state row
		std::array<float, 3> c1 = kindtoColor(p1s.kind);
		std::array<float, 3> c2 = kindtoColor(p2s.kind);

		ImColor inv_p1 = bool(p1s.invul & Attribute::GP) ? color_gp : color_inv;
		ImColor inv_p2 = bool(p2s.invul & Attribute::GP) ? color_gp : color_inv;

		// --- P1 row 1: main state ---
		{
			ImVec2 pos = ImVec2(col_x, p1_y);
			DrawBox(dl, pos, width, height, ImColor(c1[0], c1[1], c1[2]));
			if (IsHovering(pos, width, height))
				ImGui::SetTooltip("P1: %s", kindToString(p1s.kind));
		}

		// --- P1 row 2: invul ---
		{
			ImVec2 pos = ImVec2(col_x, p1_y + height + spacing);
			DrawBox(dl, pos, width, height, ImColor(0, 0, 0));
			if (IsHovering(pos, width, height)) {
				std::string s = attributeToString(p1s.invul);
				ImGui::SetTooltip("P1 invul: %s", s.c_str());
			}
			if (bool(p1s.invul & Attribute::T))
				DrawBox(dl, ImVec2(pos.x + width - width/5.f, pos.y), width/5.f, height, inv_p1);
			if (bool(p1s.invul & Attribute::P))
				DrawBox(dl, pos, width/5.f, height, inv_p1);
			if (bool(p1s.invul & Attribute::H))
				DrawBox(dl, pos, width, height/5.f, inv_p1);
			if (bool(p1s.invul & Attribute::B))
				DrawBox(dl, ImVec2(pos.x, pos.y + height/2.f - height/10.f), width, height/5.f, inv_p1);
			if (bool(p1s.invul & Attribute::F))
				DrawBox(dl, ImVec2(pos.x, pos.y + height - height/5.f), width, height/5.f, inv_p1);
		}

		// --- P2 row 1: main state ---
		{
			ImVec2 pos = ImVec2(col_x, p2_y);
			DrawBox(dl, pos, width, height, ImColor(c2[0], c2[1], c2[2]));
			if (IsHovering(pos, width, height))
				ImGui::SetTooltip("P2: %s", kindToString(p2s.kind));
		}

		// --- P2 row 2: invul ---
		{
			ImVec2 pos = ImVec2(col_x, p2_y + height + spacing);
			DrawBox(dl, pos, width, height, ImColor(0, 0, 0));
			if (IsHovering(pos, width, height)) {
				std::string s = attributeToString(p2s.invul);
				ImGui::SetTooltip("P2 invul: %s", s.c_str());
			}
			if (bool(p2s.invul & Attribute::T))
				DrawBox(dl, ImVec2(pos.x + width - width/5.f, pos.y), width/5.f, height, inv_p2);
			if (bool(p2s.invul & Attribute::P))
				DrawBox(dl, pos, width/5.f, height, inv_p2);
			if (bool(p2s.invul & Attribute::H))
				DrawBox(dl, pos, width, height/5.f, inv_p2);
			if (bool(p2s.invul & Attribute::B))
				DrawBox(dl, ImVec2(pos.x, pos.y + height/2.f - height/10.f), width, height/5.f, inv_p2);
			if (bool(p2s.invul & Attribute::F))
				DrawBox(dl, ImVec2(pos.x, pos.y + height - height/5.f), width, height/5.f, inv_p2);
		}

		// --- Dividers every 5 frames (major every 25) ---
		if ((frame_idx + 1) % 5 == 0) {
			float div_x = col_x + width + spacing * 0.5f;
			bool major = (frame_idx + 1) % 25 == 0;
			ImU32 div_col  = major ? IM_COL32(255, 255, 255, 210) : IM_COL32(255, 255, 255, 90);
			float div_thick = major ? 1.5f : 1.0f;
			dl->AddLine(ImVec2(div_x, div_top), ImVec2(div_x, div_bottom), div_col, div_thick);
		}

		col_x += width + spacing;
		++frame_idx;
	}
}
