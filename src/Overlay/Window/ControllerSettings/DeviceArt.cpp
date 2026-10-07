#include "DeviceArt.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace DeviceArt
{
	namespace
	{
		using namespace LocalInput;

		const ImU32 kOutline = IM_COL32(150, 160, 175, 255);
		const ImU32 kDim = IM_COL32(55, 60, 70, 255);
		const ImU32 kBody = IM_COL32(38, 42, 50, 255);
		const ImU32 kLit = IM_COL32(110, 240, 140, 255);
		const ImU32 kLabel = IM_COL32(255, 220, 120, 255);

		ImU32 Mix(ImU32 color, float alpha)
		{
			ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
			c.w *= alpha;
			return ImGui::ColorConvertFloat4ToU32(c);
		}

		// Short name of what a control is mapped to: "A", "B+C", "D", or "" when unmapped or
		// only directions (the picture shows those already).
		std::string MappedLabel(const Mapping& mapping, int control)
		{
			auto it = mapping.find(control);
			if (it == mapping.end())
				return std::string();
			std::string out;
			for (const GameButtonInfo& info : kGameButtons)
			{
				if (!(it->second & info.bit) || (info.bit & kDirectionMask))
					continue;
				if (!out.empty())
					out += "+";
				out += info.bit == Btn_Reset ? "Rst" : info.bit == Btn_Special ? "SP" : info.bit == Btn_Taunt ? "Tnt" : info.name;
			}
			return out;
		}

		void CenteredText(ImDrawList* draw, ImVec2 center, ImU32 color, const char* text, float scale = 1.0f)
		{
			ImFont* font = ImGui::GetFont();
			const float size = ImGui::GetFontSize() * scale;
			const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
			draw->AddText(font, size, ImVec2(center.x - extent.x * 0.5f, center.y - extent.y * 0.5f), color, text);
		}

		// One round button: lit fill when pressed, the game button it sends beside it.
		void RoundControl(ImDrawList* draw, ImVec2 c, float r, bool pressed, ImU32 tint, const char* glyph,
			const std::string& label, ImVec2 labelOffset)
		{
			draw->AddCircleFilled(c, r, pressed ? kLit : Mix(tint, 0.55f));
			draw->AddCircle(c, r, pressed ? IM_COL32_WHITE : kOutline, 0, 1.5f);
			if (glyph)
				CenteredText(draw, c, pressed ? IM_COL32(20, 20, 20, 255) : IM_COL32_WHITE, glyph, 0.85f);
			if (!label.empty())
				CenteredText(draw, ImVec2(c.x + labelOffset.x, c.y + labelOffset.y), kLabel, label.c_str(), 0.8f);
		}

		void RectControl(ImDrawList* draw, ImVec2 a, ImVec2 b, bool pressed, const char* glyph, const std::string& label, ImVec2 labelPos)
		{
			draw->AddRectFilled(a, b, pressed ? kLit : kDim, 4.0f);
			draw->AddRect(a, b, pressed ? IM_COL32_WHITE : kOutline, 4.0f, 0, 1.5f);
			if (glyph)
				CenteredText(draw, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), pressed ? IM_COL32(20, 20, 20, 255) : IM_COL32_WHITE, glyph, 0.8f);
			if (!label.empty())
				CenteredText(draw, labelPos, kLabel, label.c_str(), 0.8f);
		}

		void Stick(ImDrawList* draw, ImVec2 c, float r, float x, float y, bool clicked)
		{
			draw->AddCircleFilled(c, r, kBody);
			draw->AddCircle(c, r, kOutline, 0, 1.5f);
			const bool moved = std::fabs(x) > 0.2f || std::fabs(y) > 0.2f;
			draw->AddCircleFilled(ImVec2(c.x + x * r * 0.55f, c.y + y * r * 0.55f), r * 0.55f,
				clicked ? kLit : (moved ? IM_COL32(90, 200, 120, 255) : kDim));
		}

		void Dpad(ImDrawList* draw, ImVec2 c, float s, bool up, bool down, bool left, bool right, const Mapping* mapping,
			int upCode, int downCode, int leftCode, int rightCode)
		{
			(void)mapping; (void)upCode; (void)downCode; (void)leftCode; (void)rightCode;
			const float w = s * 0.42f;
			auto arm = [&](ImVec2 a, ImVec2 b, bool on)
			{
				draw->AddRectFilled(a, b, on ? kLit : kDim, 2.0f);
				draw->AddRect(a, b, on ? IM_COL32_WHITE : kOutline, 2.0f, 0, 1.2f);
			};
			arm(ImVec2(c.x - w * 0.5f, c.y - s), ImVec2(c.x + w * 0.5f, c.y - w * 0.5f), up);
			arm(ImVec2(c.x - w * 0.5f, c.y + w * 0.5f), ImVec2(c.x + w * 0.5f, c.y + s), down);
			arm(ImVec2(c.x - s, c.y - w * 0.5f), ImVec2(c.x - w * 0.5f, c.y + w * 0.5f), left);
			arm(ImVec2(c.x + w * 0.5f, c.y - w * 0.5f), ImVec2(c.x + s, c.y + w * 0.5f), right);
			draw->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - w * 0.5f), ImVec2(c.x + w * 0.5f, c.y + w * 0.5f), kDim);
		}

		void PadBody(ImDrawList* draw, ImVec2 c, float u)
		{
			draw->AddCircleFilled(ImVec2(c.x - 6.2f * u, c.y + 2.6f * u), 3.2f * u, kBody);
			draw->AddCircleFilled(ImVec2(c.x + 6.2f * u, c.y + 2.6f * u), 3.2f * u, kBody);
			draw->AddRectFilled(ImVec2(c.x - 7.5f * u, c.y - 4.0f * u), ImVec2(c.x + 7.5f * u, c.y + 2.8f * u), kBody, 3.0f * u);
		}

		void DrawXboxPad(ImDrawList* draw, ImVec2 origin, float width, const DeviceView& d, const Mapping& m)
		{
			const float u = width / 20.0f;
			const ImVec2 c(origin.x + width * 0.5f, origin.y + 7.5f * u);
			const auto on = [&](int code) { return d.controls[code] != false; };

			// Shoulders and triggers above the body.
			RectControl(draw, ImVec2(c.x - 7.6f * u, c.y - 7.0f * u), ImVec2(c.x - 4.4f * u, c.y - 5.9f * u), on(14), "LT", MappedLabel(m, 14), ImVec2(c.x - 9.2f * u, c.y - 6.45f * u));
			RectControl(draw, ImVec2(c.x + 4.4f * u, c.y - 7.0f * u), ImVec2(c.x + 7.6f * u, c.y - 5.9f * u), on(15), "RT", MappedLabel(m, 15), ImVec2(c.x + 9.2f * u, c.y - 6.45f * u));
			RectControl(draw, ImVec2(c.x - 7.6f * u, c.y - 5.5f * u), ImVec2(c.x - 4.0f * u, c.y - 4.5f * u), on(4), "LB", MappedLabel(m, 4), ImVec2(c.x - 9.2f * u, c.y - 5.0f * u));
			RectControl(draw, ImVec2(c.x + 4.0f * u, c.y - 5.5f * u), ImVec2(c.x + 7.6f * u, c.y - 4.5f * u), on(5), "RB", MappedLabel(m, 5), ImVec2(c.x + 9.2f * u, c.y - 5.0f * u));

			PadBody(draw, c, u);

			const float lx = d.axisCount > 1 ? d.axes[0] / 1000.0f : 0.0f;
			const float ly = d.axisCount > 1 ? d.axes[1] / 1000.0f : 0.0f;
			const float rx = d.axisCount > 3 ? d.axes[2] / 1000.0f : 0.0f;
			const float ry = d.axisCount > 3 ? d.axes[3] / 1000.0f : 0.0f;
			Stick(draw, ImVec2(c.x - 4.6f * u, c.y - 1.6f * u), 1.6f * u, lx, ly, on(8));
			Stick(draw, ImVec2(c.x + 2.3f * u, c.y + 1.4f * u), 1.4f * u, rx, ry, on(9));
			Dpad(draw, ImVec2(c.x - 2.3f * u, c.y + 1.4f * u), 1.3f * u, on(10), on(11), on(12), on(13), &m, 10, 11, 12, 13);

			const ImVec2 face(c.x + 4.8f * u, c.y - 1.6f * u);
			const float r = 0.75f * u;
			RoundControl(draw, ImVec2(face.x, face.y + 1.4f * u), r, on(0), IM_COL32(60, 170, 70, 255), "A", MappedLabel(m, 0), ImVec2(0, 1.5f * u));
			RoundControl(draw, ImVec2(face.x + 1.4f * u, face.y), r, on(1), IM_COL32(200, 60, 60, 255), "B", MappedLabel(m, 1), ImVec2(1.7f * u, 0));
			RoundControl(draw, ImVec2(face.x - 1.4f * u, face.y), r, on(2), IM_COL32(60, 110, 210, 255), "X", MappedLabel(m, 2), ImVec2(-1.7f * u, 0));
			RoundControl(draw, ImVec2(face.x, face.y - 1.4f * u), r, on(3), IM_COL32(210, 180, 50, 255), "Y", MappedLabel(m, 3), ImVec2(0, -1.5f * u));

			RectControl(draw, ImVec2(c.x - 1.6f * u, c.y - 2.0f * u), ImVec2(c.x - 0.5f * u, c.y - 1.3f * u), on(6), nullptr, MappedLabel(m, 6), ImVec2(c.x - 1.05f * u, c.y - 2.7f * u));
			RectControl(draw, ImVec2(c.x + 0.5f * u, c.y - 2.0f * u), ImVec2(c.x + 1.6f * u, c.y - 1.3f * u), on(7), nullptr, MappedLabel(m, 7), ImVec2(c.x + 1.05f * u, c.y - 2.7f * u));

			ImGui::Dummy(ImVec2(width, 13.0f * u));
		}

		void DrawArcadeStick(ImDrawList* draw, ImVec2 origin, float width, const DeviceView& d, const Mapping& m)
		{
			const float u = width / 20.0f;
			const ImVec2 a(origin.x + 0.5f * u, origin.y + 0.5f * u);
			const ImVec2 b(origin.x + width - 0.5f * u, origin.y + 9.0f * u);
			draw->AddRectFilled(a, b, kBody, 1.0f * u);
			draw->AddRect(a, b, kOutline, 1.0f * u, 0, 1.5f);

			// Lever: hat 0 if the device has one, else the X/Y axes.
			const bool up = d.controls[kDInputPovBase + 0] || d.controls[kDInputAxisBase + 2];
			const bool right = d.controls[kDInputPovBase + 1] || d.controls[kDInputAxisBase + 1];
			const bool down = d.controls[kDInputPovBase + 2] || d.controls[kDInputAxisBase + 3];
			const bool left = d.controls[kDInputPovBase + 3] || d.controls[kDInputAxisBase + 0];
			const ImVec2 lever(a.x + 4.0f * u, a.y + 4.3f * u);
			draw->AddCircle(lever, 2.2f * u, kOutline, 8, 1.5f);
			const ImVec2 ball(lever.x + (right ? 1.3f : left ? -1.3f : 0.0f) * u, lever.y + (down ? 1.3f : up ? -1.3f : 0.0f) * u);
			draw->AddLine(lever, ball, kOutline, 0.35f * u);
			draw->AddCircleFilled(ball, 1.1f * u, (up || down || left || right) ? kLit : IM_COL32(200, 50, 50, 255));

			// Buttons in the usual two rows of four, numbered as Windows numbers them.
			const float bx = a.x + 8.5f * u;
			for (int i = 0; i < 8; ++i)
			{
				const int col = i % 4;
				const int row = i / 4;
				const ImVec2 c(bx + col * 2.6f * u + (row == 0 ? 0.0f : 0.0f), a.y + 2.8f * u + row * 3.0f * u - (col == 0 ? 0.4f * u : 0.0f));
				char glyph[8];
				snprintf(glyph, sizeof(glyph), "%d", i + 1);
				RoundControl(draw, c, 1.0f * u, d.controls[i] != false, IM_COL32(80, 80, 95, 255), glyph, MappedLabel(m, i), ImVec2(0, 1.6f * u));
			}
			// Extra buttons (start/select/home) along the top edge.
			for (int i = 8; i < 12; ++i)
			{
				const ImVec2 c(a.x + 8.0f * u + (i - 8) * 2.2f * u, a.y + 0.9f * u);
				char glyph[8];
				snprintf(glyph, sizeof(glyph), "%d", i + 1);
				RoundControl(draw, c, 0.5f * u, d.controls[i] != false, IM_COL32(70, 70, 80, 255), nullptr, std::string(), ImVec2());
				if (d.controls[i])
					CenteredText(draw, ImVec2(c.x, c.y + 1.0f * u), kLit, glyph, 0.7f);
			}
			ImGui::Dummy(ImVec2(width, 10.0f * u));
		}

		void DrawGenericPad(ImDrawList* draw, ImVec2 origin, float width, const DeviceView& d, const Mapping& m)
		{
			const float u = width / 20.0f;
			const ImVec2 c(origin.x + width * 0.5f, origin.y + 7.5f * u);
			const auto on = [&](int code) { return d.controls[code] != false; };
			RectControl(draw, ImVec2(c.x - 7.6f * u, c.y - 7.0f * u), ImVec2(c.x - 4.4f * u, c.y - 5.9f * u), on(6), "7", MappedLabel(m, 6), ImVec2(c.x - 9.2f * u, c.y - 6.45f * u));
			RectControl(draw, ImVec2(c.x + 4.4f * u, c.y - 7.0f * u), ImVec2(c.x + 7.6f * u, c.y - 5.9f * u), on(7), "8", MappedLabel(m, 7), ImVec2(c.x + 9.2f * u, c.y - 6.45f * u));
			RectControl(draw, ImVec2(c.x - 7.6f * u, c.y - 5.5f * u), ImVec2(c.x - 4.0f * u, c.y - 4.5f * u), on(4), "5", MappedLabel(m, 4), ImVec2(c.x - 9.2f * u, c.y - 5.0f * u));
			RectControl(draw, ImVec2(c.x + 4.0f * u, c.y - 5.5f * u), ImVec2(c.x + 7.6f * u, c.y - 4.5f * u), on(5), "6", MappedLabel(m, 5), ImVec2(c.x + 9.2f * u, c.y - 5.0f * u));
			PadBody(draw, c, u);
			const bool up = on(kDInputPovBase + 0), right = on(kDInputPovBase + 1), down = on(kDInputPovBase + 2), left = on(kDInputPovBase + 3);
			Dpad(draw, ImVec2(c.x - 4.6f * u, c.y - 1.6f * u), 1.4f * u, up, down, left, right, &m, 0, 0, 0, 0);
			const float lx = d.axisCount > 1 ? d.axes[0] / 1000.0f : 0.0f;
			const float ly = d.axisCount > 1 ? d.axes[1] / 1000.0f : 0.0f;
			const float rx = d.axisCount > 5 ? d.axes[2] / 1000.0f : 0.0f;
			const float ry = d.axisCount > 5 ? d.axes[5] / 1000.0f : 0.0f;
			Stick(draw, ImVec2(c.x - 2.3f * u, c.y + 1.4f * u), 1.3f * u, lx, ly, on(10));
			Stick(draw, ImVec2(c.x + 2.3f * u, c.y + 1.4f * u), 1.3f * u, rx, ry, on(11));
			const ImVec2 face(c.x + 4.8f * u, c.y - 1.6f * u);
			const float r = 0.75f * u;
			RoundControl(draw, ImVec2(face.x - 1.4f * u, face.y), r, on(0), IM_COL32(90, 90, 110, 255), "1", MappedLabel(m, 0), ImVec2(-1.7f * u, 0));
			RoundControl(draw, ImVec2(face.x, face.y + 1.4f * u), r, on(1), IM_COL32(90, 90, 110, 255), "2", MappedLabel(m, 1), ImVec2(0, 1.5f * u));
			RoundControl(draw, ImVec2(face.x + 1.4f * u, face.y), r, on(2), IM_COL32(90, 90, 110, 255), "3", MappedLabel(m, 2), ImVec2(1.7f * u, 0));
			RoundControl(draw, ImVec2(face.x, face.y - 1.4f * u), r, on(3), IM_COL32(90, 90, 110, 255), "4", MappedLabel(m, 3), ImVec2(0, -1.5f * u));
			RectControl(draw, ImVec2(c.x - 1.6f * u, c.y - 2.0f * u), ImVec2(c.x - 0.5f * u, c.y - 1.3f * u), on(8), nullptr, MappedLabel(m, 8), ImVec2(c.x - 1.05f * u, c.y - 2.7f * u));
			RectControl(draw, ImVec2(c.x + 0.5f * u, c.y - 2.0f * u), ImVec2(c.x + 1.6f * u, c.y - 1.3f * u), on(9), nullptr, MappedLabel(m, 9), ImVec2(c.x + 1.05f * u, c.y - 2.7f * u));
			ImGui::Dummy(ImVec2(width, 13.0f * u));
		}

		struct Key
		{
			int vk;
			const char* label;
			float width; // in key units
		};

		void DrawKeyboard(ImDrawList* draw, ImVec2 origin, float width, const DeviceView& d, const Mapping& m)
		{
			static const Key kRows[5][15] = {
				{ {'1',"1",1}, {'2',"2",1}, {'3',"3",1}, {'4',"4",1}, {'5',"5",1}, {'6',"6",1}, {'7',"7",1}, {'8',"8",1}, {'9',"9",1}, {'0',"0",1}, {VK_OEM_MINUS,"-",1}, {VK_OEM_PLUS,"=",1}, {VK_BACK,"Bksp",1.8f}, {0,nullptr,0} },
				{ {VK_TAB,"Tab",1.4f}, {'Q',"Q",1}, {'W',"W",1}, {'E',"E",1}, {'R',"R",1}, {'T',"T",1}, {'Y',"Y",1}, {'U',"U",1}, {'I',"I",1}, {'O',"O",1}, {'P',"P",1}, {VK_OEM_4,"[",1}, {VK_OEM_6,"]",1}, {0,nullptr,0} },
				{ {VK_CAPITAL,"Caps",1.7f}, {'A',"A",1}, {'S',"S",1}, {'D',"D",1}, {'F',"F",1}, {'G',"G",1}, {'H',"H",1}, {'J',"J",1}, {'K',"K",1}, {'L',"L",1}, {VK_OEM_1,";",1}, {VK_OEM_7,"'",1}, {VK_RETURN,"Enter",1.9f}, {0,nullptr,0} },
				{ {VK_LSHIFT,"Shift",2.2f}, {'Z',"Z",1}, {'X',"X",1}, {'C',"C",1}, {'V',"V",1}, {'B',"B",1}, {'N',"N",1}, {'M',"M",1}, {VK_OEM_COMMA,",",1}, {VK_OEM_PERIOD,".",1}, {VK_OEM_2,"/",1}, {VK_RSHIFT,"Shift",2.4f}, {0,nullptr,0} },
				{ {VK_LCONTROL,"Ctrl",1.5f}, {VK_LMENU,"Alt",1.3f}, {VK_SPACE,"Space",6.4f}, {VK_RMENU,"Alt",1.3f}, {VK_RCONTROL,"Ctrl",1.5f}, {0,nullptr,0} },
			};
			const float unit = width / 18.5f;
			const float gap = unit * 0.1f;
			auto keyBox = [&](ImVec2 p, float w, int vk, const char* label)
			{
				const bool pressed = vk > 0 && vk < kMaxControls && d.controls[vk];
				const std::string mapped = MappedLabel(m, vk);
				const bool directional = m.count(vk) && (m.at(vk) & kDirectionMask) && mapped.empty();
				const ImVec2 b(p.x + w * unit - gap, p.y + unit - gap);
				ImU32 fill = pressed ? kLit : (!mapped.empty() || directional ? IM_COL32(60, 70, 95, 255) : kDim);
				draw->AddRectFilled(p, b, fill, 3.0f);
				draw->AddRect(p, b, pressed ? IM_COL32_WHITE : kOutline, 3.0f, 0, 1.0f);
				draw->AddText(ImVec2(p.x + 3.0f, p.y + 1.0f), pressed ? IM_COL32(20, 20, 20, 255) : IM_COL32(220, 220, 225, 255), label);
				if (!mapped.empty())
					CenteredText(draw, ImVec2((p.x + b.x) * 0.5f, b.y - unit * 0.3f), pressed ? IM_COL32(20, 20, 20, 255) : kLabel, mapped.c_str(), 0.85f);
			};
			for (int r = 0; r < 5; ++r)
			{
				float x = origin.x + (r == 0 ? unit * 0.5f : 0.0f);
				const float y = origin.y + r * unit;
				for (int k = 0; k < 15 && kRows[r][k].label; ++k)
				{
					keyBox(ImVec2(x, y), kRows[r][k].width, kRows[r][k].vk, kRows[r][k].label);
					x += kRows[r][k].width * unit;
				}
			}
			// Arrow cluster.
			const float ax = origin.x + 15.3f * unit;
			keyBox(ImVec2(ax + unit, origin.y + 3.0f * unit), 1.0f, VK_UP, "^");
			keyBox(ImVec2(ax, origin.y + 4.0f * unit), 1.0f, VK_LEFT, "<");
			keyBox(ImVec2(ax + unit, origin.y + 4.0f * unit), 1.0f, VK_DOWN, "v");
			keyBox(ImVec2(ax + 2.0f * unit, origin.y + 4.0f * unit), 1.0f, VK_RIGHT, ">");
			ImGui::Dummy(ImVec2(width, unit * 5.2f));

			// Anything held that the picture has no key for.
			std::string others;
			for (int vk = 1; vk < kMaxControls; ++vk)
			{
				if (!d.controls[vk])
					continue;
				bool drawn = false;
				for (int r = 0; r < 5 && !drawn; ++r)
					for (int k = 0; k < 15 && kRows[r][k].label; ++k)
						if (kRows[r][k].vk == vk) { drawn = true; break; }
				if (drawn || vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ||
					vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
					continue;
				if (!others.empty())
					others += ", ";
				others += Manager::ControlName(DeviceKind::Keyboard, vk);
			}
			if (!others.empty())
				ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f), "+ %s", others.c_str());
		}
	}

	IconStyle StyleFor(DeviceKind kind, const std::string& name)
	{
		if (kind == DeviceKind::Keyboard)
			return IconStyle::Keyboard;
		if (kind == DeviceKind::XInputPad)
			return IconStyle::XboxPad;
		std::string lower = name;
		for (char& c : lower)
			c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		for (const char* word : { "stick", "arcade", "hitbox", "hit box", "fight", "joystick", "vjoy", "qanba", "mayflash", "snack box", "leverless" })
			if (lower.find(word) != std::string::npos)
				return IconStyle::ArcadeStick;
		return IconStyle::GenericPad;
	}

	void DrawIcon(ImDrawList* draw, ImVec2 p, float h, IconStyle style, ImU32 color)
	{
		const float w = h * 1.6f;
		switch (style)
		{
		case IconStyle::Keyboard:
		{
			const ImVec2 a(p.x, p.y + h * 0.18f), b(p.x + w, p.y + h * 0.88f);
			draw->AddRect(a, b, color, h * 0.12f, 0, 1.6f);
			const float kw = (w - h * 0.24f) / 8.0f;
			for (int r = 0; r < 3; ++r)
			{
				const int keys = r == 2 ? 1 : 7 - r;
				for (int k = 0; k < keys; ++k)
				{
					float x = a.x + h * 0.14f + k * kw + r * kw * 0.5f;
					float kwidth = r == 2 ? kw * 4.0f : kw * 0.7f;
					if (r == 2) x = a.x + w * 0.5f - kwidth * 0.5f;
					const float y = a.y + h * 0.1f + r * h * 0.19f;
					draw->AddRectFilled(ImVec2(x, y), ImVec2(x + kwidth, y + h * 0.12f), color, 1.0f);
				}
			}
			break;
		}
		case IconStyle::XboxPad:
		case IconStyle::GenericPad:
		{
			const ImVec2 c(p.x + w * 0.5f, p.y + h * 0.5f);
			const float u = w / 16.0f;
			draw->AddCircleFilled(ImVec2(c.x - 5.0f * u, c.y + 2.4f * u), 2.6f * u, color);
			draw->AddCircleFilled(ImVec2(c.x + 5.0f * u, c.y + 2.4f * u), 2.6f * u, color);
			draw->AddRectFilled(ImVec2(c.x - 6.2f * u, c.y - 3.2f * u), ImVec2(c.x + 6.2f * u, c.y + 2.6f * u), color, 2.6f * u);
			const ImU32 hole = IM_COL32(25, 28, 34, 255);
			// d-pad
			draw->AddRectFilled(ImVec2(c.x - 4.6f * u, c.y - 1.0f * u), ImVec2(c.x - 2.0f * u, c.y - 0.2f * u), hole);
			draw->AddRectFilled(ImVec2(c.x - 3.7f * u, c.y - 1.9f * u), ImVec2(c.x - 2.9f * u, c.y + 0.7f * u), hole);
			if (style == IconStyle::XboxPad)
			{
				draw->AddCircleFilled(ImVec2(c.x + 3.3f * u, c.y - 1.7f * u), 0.65f * u, IM_COL32(230, 200, 60, 255));
				draw->AddCircleFilled(ImVec2(c.x + 3.3f * u, c.y + 0.5f * u), 0.65f * u, IM_COL32(80, 200, 90, 255));
				draw->AddCircleFilled(ImVec2(c.x + 2.2f * u, c.y - 0.6f * u), 0.65f * u, IM_COL32(70, 130, 230, 255));
				draw->AddCircleFilled(ImVec2(c.x + 4.4f * u, c.y - 0.6f * u), 0.65f * u, IM_COL32(220, 70, 70, 255));
			}
			else
			{
				for (int i = 0; i < 4; ++i)
				{
					const float ang = i * 1.5708f;
					draw->AddCircleFilled(ImVec2(c.x + 3.3f * u + std::cos(ang) * 1.1f * u, c.y - 0.6f * u + std::sin(ang) * 1.1f * u), 0.6f * u, hole);
				}
			}
			break;
		}
		case IconStyle::ArcadeStick:
		{
			const ImVec2 a(p.x, p.y + h * 0.45f), b(p.x + w, p.y + h * 0.95f);
			draw->AddRectFilled(a, b, color, h * 0.08f);
			draw->AddLine(ImVec2(p.x + w * 0.25f, a.y), ImVec2(p.x + w * 0.25f, p.y + h * 0.18f), color, h * 0.1f);
			draw->AddCircleFilled(ImVec2(p.x + w * 0.25f, p.y + h * 0.16f), h * 0.16f, IM_COL32(220, 60, 60, 255));
			const ImU32 hole = IM_COL32(25, 28, 34, 255);
			for (int i = 0; i < 4; ++i)
				draw->AddCircleFilled(ImVec2(p.x + w * (0.5f + i * 0.13f), a.y + h * (i == 0 ? 0.2f : 0.17f)), h * 0.07f, hole);
			for (int i = 0; i < 4; ++i)
				draw->AddCircleFilled(ImVec2(p.x + w * (0.5f + i * 0.13f), a.y + h * (i == 0 ? 0.38f : 0.35f)), h * 0.07f, hole);
			break;
		}
		}
	}

	ImU32 PlayerColor(Side side, float alpha)
	{
		// One accent for "this is being used" on both sides; the cards say which player.
		(void)side;
		return IM_COL32(64, 196, 160, static_cast<int>(255 * alpha));
	}

	void DrawGameInput(uint32_t buttons, float height)
	{
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float r = height * 0.42f;
		const ImVec2 gate(origin.x + r + 4.0f, origin.y + height * 0.5f);

		// Direction gate, numpad notation like the training display.
		draw->AddCircleFilled(gate, r, IM_COL32(30, 32, 40, 255), 8);
		draw->AddNgon(gate, r, kOutline, 8, 1.5f);
		const int dx = (buttons & Btn_Right) ? 1 : (buttons & Btn_Left) ? -1 : 0;
		const int dy = (buttons & Btn_Down) ? 1 : (buttons & Btn_Up) ? -1 : 0;
		for (int y = -1; y <= 1; ++y)
			for (int x = -1; x <= 1; ++x)
				draw->AddCircleFilled(ImVec2(gate.x + x * r * 0.62f, gate.y + y * r * 0.62f), r * 0.06f, kOutline);
		const ImVec2 ball(gate.x + dx * r * 0.62f, gate.y + dy * r * 0.62f);
		draw->AddCircleFilled(ball, r * 0.22f, IM_COL32(230, 70, 60, 255));
		draw->AddCircle(ball, r * 0.22f, IM_COL32_WHITE, 0, 1.5f);
		const int numpad = 5 + dx - dy * 3;
		char digit[4];
		snprintf(digit, sizeof(digit), "%d", numpad);
		CenteredText(draw, ImVec2(gate.x, gate.y + r + ImGui::GetFontSize() * 0.6f), IM_COL32(220, 220, 225, 255), digit);

		// A B C D in the game's colours.
		struct Face { uint32_t bit; const char* name; ImU32 color; ImVec2 offset; };
		const float s = r * 0.62f;
		const Face faces[4] = {
			{ Btn_A, "A", IM_COL32(225, 70, 70, 255),  ImVec2(-s, 0) },
			{ Btn_B, "B", IM_COL32(70, 120, 225, 255), ImVec2(0, -s) },
			{ Btn_C, "C", IM_COL32(70, 190, 90, 255),  ImVec2(s, 0) },
			{ Btn_D, "D", IM_COL32(230, 195, 60, 255), ImVec2(0, s) },
		};
		const ImVec2 diamond(gate.x + r * 2.0f + s + 12.0f, gate.y);
		for (const Face& f : faces)
		{
			const bool on = (buttons & f.bit) != 0;
			const ImVec2 c(diamond.x + f.offset.x, diamond.y + f.offset.y);
			draw->AddCircleFilled(c, r * 0.3f, on ? f.color : Mix(f.color, 0.22f));
			draw->AddCircle(c, r * 0.3f, on ? IM_COL32_WHITE : Mix(f.color, 0.7f), 0, 1.5f);
			CenteredText(draw, c, on ? IM_COL32(15, 15, 15, 255) : IM_COL32(230, 230, 230, 255), f.name);
		}

		// Everything else as chips to the right.
		float x = diamond.x + s + r * 0.5f + 14.0f;
		float y = origin.y + 4.0f;
		for (const GameButtonInfo& info : kGameButtons)
		{
			if (info.bit & (kDirectionMask | Btn_A | Btn_B | Btn_C | Btn_D))
				continue;
			const bool on = (buttons & info.bit) != 0;
			const ImVec2 size(ImGui::CalcTextSize(info.name).x + 12.0f, ImGui::GetFontSize() + 6.0f);
			if (y + size.y > origin.y + height)
			{
				y = origin.y + 4.0f;
				x += 86.0f;
			}
			draw->AddRectFilled(ImVec2(x, y), ImVec2(x + size.x, y + size.y), on ? kLit : kDim, 4.0f);
			draw->AddText(ImVec2(x + 6.0f, y + 3.0f), on ? IM_COL32(15, 15, 15, 255) : IM_COL32(200, 200, 205, 255), info.name);
			y += size.y + 4.0f;
		}
		ImGui::Dummy(ImVec2(x + 90.0f - origin.x, height));
	}

	void DrawDevice(const DeviceView& device, const Mapping& mapping, float width)
	{
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		switch (StyleFor(device.kind, device.defaultName))
		{
		case IconStyle::Keyboard: DrawKeyboard(draw, origin, width, device, mapping); break;
		case IconStyle::XboxPad: DrawXboxPad(draw, origin, width, device, mapping); break;
		case IconStyle::ArcadeStick: DrawArcadeStick(draw, origin, width, device, mapping); break;
		case IconStyle::GenericPad: DrawGenericPad(draw, origin, width, device, mapping); break;
		}
	}
}
