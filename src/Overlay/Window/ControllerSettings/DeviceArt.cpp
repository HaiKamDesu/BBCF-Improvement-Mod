#include "DeviceArt.h"

#include <Windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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

		// The printed symbol of a face button: PlayStation shapes, otherwise the letter or
		// number on it.
		void FaceGlyph(ImDrawList* draw, ImVec2 c, float r, PadFamily family, int role, const char* name, bool pressed)
		{
			const bool face = role <= Role_North;
			if (family == PadFamily::PlayStation && face)
			{
				static const ImU32 kShape[4] = {
					IM_COL32(120, 160, 240, 255), // Cross
					IM_COL32(235, 85, 90, 255),   // Circle
					IM_COL32(225, 130, 200, 255), // Square
					IM_COL32(60, 205, 170, 255),  // Triangle
				};
				const ImU32 col = pressed ? IM_COL32(20, 20, 20, 255) : kShape[role];
				const float s = r * 0.42f;
				const float t = (std::max)(1.5f, r * 0.14f);
				switch (role)
				{
				case Role_South:
					draw->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, t);
					draw->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), col, t);
					break;
				case Role_East:
					draw->AddCircle(c, s, col, 0, t);
					break;
				case Role_West:
					draw->AddRect(ImVec2(c.x - s * 0.85f, c.y - s * 0.85f), ImVec2(c.x + s * 0.85f, c.y + s * 0.85f), col, 0.0f, 0, t);
					break;
				default:
					draw->AddTriangle(ImVec2(c.x, c.y - s), ImVec2(c.x - s * 0.95f, c.y + s * 0.7f), ImVec2(c.x + s * 0.95f, c.y + s * 0.7f), col, t);
					break;
				}
				return;
			}
			if (name)
				CenteredText(draw, c, pressed ? IM_COL32(20, 20, 20, 255) : IM_COL32_WHITE, name, 0.85f);
		}

		// Xbox face buttons are coloured; everything else is a dark button with a symbol on it.
		ImU32 FaceTint(PadFamily family, int role)
		{
			if (family == PadFamily::Xbox && role <= Role_North)
			{
				static const ImU32 kXbox[4] = {
					IM_COL32(60, 170, 70, 255), IM_COL32(200, 60, 60, 255), IM_COL32(60, 110, 210, 255), IM_COL32(210, 180, 50, 255),
				};
				return kXbox[role];
			}
			return IM_COL32(70, 74, 88, 255);
		}

		// One round button: lit fill when pressed, the game button it sends beside it.
		void RoundButton(ImDrawList* draw, ImVec2 c, float r, bool pressed, PadFamily family, int role, const char* name,
			const std::string& label, ImVec2 labelOffset)
		{
			draw->AddCircleFilled(c, r, pressed ? kLit : Mix(FaceTint(family, role), 0.55f));
			draw->AddCircle(c, r, pressed ? IM_COL32_WHITE : kOutline, 0, 1.5f);
			FaceGlyph(draw, c, r, family, role, name, pressed);
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

		void Dpad(ImDrawList* draw, ImVec2 c, float s, bool up, bool down, bool left, bool right)
		{
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

		// A device's controls by place, from its layout.
		struct Controls
		{
			const DeviceView& d;
			const Mapping& m;
			const PadLayout& layout;
			int Code(int role) const { return layout.control[role]; }
			bool On(int role) const { const int k = Code(role); return k >= 0 && k < kMaxControls && d.controls[k]; }
			std::string Label(int role) const { const int k = Code(role); return k < 0 ? std::string() : MappedLabel(m, k); }
			const char* Name(int role) const { return layout.name[role]; }
			bool Has(int role) const { return Code(role) >= 0; }
			float Axis(int index) const { return index >= 0 && index < d.axisCount ? d.axes[index] / 1000.0f : 0.0f; }
			// The left stick's halves as control codes, for an arcade lever on XInput or axes.
			bool LeftStick(int dir) const // 0 up 1 down 2 left 3 right
			{
				if (d.kind == DeviceKind::XInputPad)
					return d.controls[16 + dir] != false;
				static const int kHalf[4] = { 2, 3, 0, 1 };
				return d.controls[kDInputAxisBase + kHalf[dir]] != false;
			}
		};

		// A small centre button (Select/Start style) with its printed name under it.
		void CentreButton(ImDrawList* draw, const Controls& k, int role, ImVec2 a, ImVec2 b, float u)
		{
			if (!k.Has(role))
				return;
			RectControl(draw, a, b, k.On(role), nullptr, k.Label(role), ImVec2((a.x + b.x) * 0.5f, a.y - 0.6f * u));
			if (k.Name(role))
				CenteredText(draw, ImVec2((a.x + b.x) * 0.5f, b.y + 0.35f * u), IM_COL32(150, 155, 168, 255), k.Name(role), 0.62f);
		}

		void SmallRoundButton(ImDrawList* draw, const Controls& k, int role, ImVec2 c, float r, float u)
		{
			if (!k.Has(role))
				return;
			const bool on = k.On(role);
			draw->AddCircleFilled(c, r, on ? kLit : kDim);
			draw->AddCircle(c, r, on ? IM_COL32_WHITE : kOutline, 0, 1.2f);
			const std::string label = k.Label(role);
			std::string text = k.Name(role) ? k.Name(role) : "";
			CenteredText(draw, ImVec2(c.x, c.y + r + 0.35f * u), IM_COL32(150, 155, 168, 255), text.c_str(), 0.62f);
			if (!label.empty())
				CenteredText(draw, ImVec2(c.x, c.y + r + 0.85f * u), kLabel, label.c_str(), 0.75f);
		}

		void DrawPad(ImDrawList* draw, ImVec2 origin, float width, const DeviceView& d, const Mapping& m)
		{
			const Controls k{ d, m, *d.model.layout };
			const PadFamily family = k.layout.family;
			const float u = width / 20.0f;
			const ImVec2 c(origin.x + width * 0.5f, origin.y + 7.5f * u);

			// Shoulders and triggers above the body.
			RectControl(draw, ImVec2(c.x - 7.6f * u, c.y - 7.0f * u), ImVec2(c.x - 4.4f * u, c.y - 5.9f * u), k.On(Role_L2), k.Name(Role_L2), k.Label(Role_L2), ImVec2(c.x - 9.2f * u, c.y - 6.45f * u));
			RectControl(draw, ImVec2(c.x + 4.4f * u, c.y - 7.0f * u), ImVec2(c.x + 7.6f * u, c.y - 5.9f * u), k.On(Role_R2), k.Name(Role_R2), k.Label(Role_R2), ImVec2(c.x + 9.2f * u, c.y - 6.45f * u));
			RectControl(draw, ImVec2(c.x - 7.6f * u, c.y - 5.5f * u), ImVec2(c.x - 4.0f * u, c.y - 4.5f * u), k.On(Role_L1), k.Name(Role_L1), k.Label(Role_L1), ImVec2(c.x - 9.2f * u, c.y - 5.0f * u));
			RectControl(draw, ImVec2(c.x + 4.0f * u, c.y - 5.5f * u), ImVec2(c.x + 7.6f * u, c.y - 4.5f * u), k.On(Role_R1), k.Name(Role_R1), k.Label(Role_R1), ImVec2(c.x + 9.2f * u, c.y - 5.0f * u));

			PadBody(draw, c, u);

			// Xbox and Nintendo put the left stick above the d-pad; PlayStation and most generic
			// pads put the d-pad there and both sticks at the bottom.
			const bool offset = family == PadFamily::Xbox || family == PadFamily::Nintendo;
			const ImVec2 upperLeft(c.x - 4.6f * u, c.y - 1.6f * u);
			const ImVec2 lowerLeft(c.x - 2.3f * u, c.y + 1.4f * u);
			Stick(draw, offset ? upperLeft : lowerLeft, (offset ? 1.6f : 1.35f) * u, k.Axis(0), k.Axis(1), k.On(Role_L3));
			Stick(draw, ImVec2(c.x + 2.3f * u, c.y + 1.4f * u), 1.35f * u, k.Axis(k.layout.rightX), k.Axis(k.layout.rightY), k.On(Role_R3));
			Dpad(draw, offset ? lowerLeft : upperLeft, 1.3f * u, k.On(Role_DpadUp), k.On(Role_DpadDown), k.On(Role_DpadLeft), k.On(Role_DpadRight));

			struct Face { int role; ImVec2 at; ImVec2 label; };
			const Face faces[4] = {
				{ Role_South, ImVec2(0, 1.4f), ImVec2(0, 1.5f) },
				{ Role_East, ImVec2(1.4f, 0), ImVec2(1.7f, 0) },
				{ Role_West, ImVec2(-1.4f, 0), ImVec2(-1.7f, 0) },
				{ Role_North, ImVec2(0, -1.4f), ImVec2(1.9f, -0.9f) }, // beside it: above is the shoulder
			};
			const ImVec2 face(c.x + 4.8f * u, c.y - 1.6f * u);
			for (const Face& f : faces)
				RoundButton(draw, ImVec2(face.x + f.at.x * u, face.y + f.at.y * u), 0.75f * u, k.On(f.role), family, f.role,
					k.Name(f.role), k.Label(f.role), ImVec2(f.label.x * u, f.label.y * u));

			if (family == PadFamily::PlayStation)
			{
				// Touchpad in the middle, Share/Create and Options either side, PS between the sticks.
				const ImVec2 ta(c.x - 1.5f * u, c.y - 3.7f * u), tb(c.x + 1.5f * u, c.y - 1.7f * u);
				if (k.Has(Role_Touchpad))
				{
					RectControl(draw, ta, tb, k.On(Role_Touchpad), nullptr, k.Label(Role_Touchpad), ImVec2(c.x, (ta.y + tb.y) * 0.5f));
					if (k.Label(Role_Touchpad).empty())
						CenteredText(draw, ImVec2(c.x, (ta.y + tb.y) * 0.5f), IM_COL32(150, 155, 168, 255), k.Name(Role_Touchpad), 0.62f);
				}
				CentreButton(draw, k, Role_Select, ImVec2(c.x - 2.6f * u, c.y - 3.5f * u), ImVec2(c.x - 1.9f * u, c.y - 2.9f * u), u);
				CentreButton(draw, k, Role_Start, ImVec2(c.x + 1.9f * u, c.y - 3.5f * u), ImVec2(c.x + 2.6f * u, c.y - 2.9f * u), u);
				SmallRoundButton(draw, k, Role_Home, ImVec2(c.x, c.y + 0.3f * u), 0.45f * u, u);
			}
			else
			{
				CentreButton(draw, k, Role_Select, ImVec2(c.x - 1.6f * u, c.y - 2.0f * u), ImVec2(c.x - 0.5f * u, c.y - 1.3f * u), u);
				CentreButton(draw, k, Role_Start, ImVec2(c.x + 0.5f * u, c.y - 2.0f * u), ImVec2(c.x + 1.6f * u, c.y - 1.3f * u), u);
				SmallRoundButton(draw, k, Role_Capture, ImVec2(c.x - 0.8f * u, c.y - 0.2f * u), 0.35f * u, u);
				SmallRoundButton(draw, k, Role_Home, ImVec2(c.x + 0.8f * u, c.y - 0.2f * u), 0.35f * u, u);
			}

			ImGui::Dummy(ImVec2(width, 13.0f * u));
		}

		void DrawArcadeStick(ImDrawList* draw, ImVec2 origin, float width, const DeviceView& d, const Mapping& m)
		{
			const Controls k{ d, m, *d.model.layout };
			const PadFamily family = k.layout.family;
			const float u = width / 20.0f;
			const ImVec2 a(origin.x + 0.5f * u, origin.y + 0.5f * u);
			const ImVec2 b(origin.x + width - 0.5f * u, origin.y + 9.0f * u);
			draw->AddRectFilled(a, b, kBody, 1.0f * u);
			draw->AddRect(a, b, kOutline, 1.0f * u, 0, 1.5f);

			// Lever: the d-pad (hat or buttons), or the left stick on sticks that send that.
			const bool up = k.On(Role_DpadUp) || k.LeftStick(0);
			const bool down = k.On(Role_DpadDown) || k.LeftStick(1);
			const bool left = k.On(Role_DpadLeft) || k.LeftStick(2);
			const bool right = k.On(Role_DpadRight) || k.LeftStick(3);
			const ImVec2 lever(a.x + 4.0f * u, a.y + 4.3f * u);
			draw->AddCircle(lever, 2.2f * u, kOutline, 8, 1.5f);
			const ImVec2 ball(lever.x + (right ? 1.3f : left ? -1.3f : 0.0f) * u, lever.y + (down ? 1.3f : up ? -1.3f : 0.0f) * u);
			draw->AddLine(lever, ball, kOutline, 0.35f * u);
			draw->AddCircleFilled(ball, 1.1f * u, (up || down || left || right) ? kLit : IM_COL32(200, 50, 50, 255));

			// Eight buttons in the usual two rows of four. A known stick shows them where they
			// are (Square Triangle R1 L1 / Cross Circle R2 L2); an unknown one by Windows' numbers.
			static const int kRows[2][4] = {
				{ Role_West, Role_North, Role_R1, Role_L1 },
				{ Role_South, Role_East, Role_R2, Role_L2 },
			};
			const bool known = family != PadFamily::Generic;
			const float bx = a.x + 8.5f * u;
			for (int i = 0; i < 8; ++i)
			{
				const int col = i % 4;
				const int row = i / 4;
				const ImVec2 c(bx + col * 2.6f * u, a.y + 2.8f * u + row * 3.0f * u - (col == 0 ? 0.4f * u : 0.0f));
				if (known)
				{
					const int role = kRows[row][col];
					RoundButton(draw, c, 1.0f * u, k.On(role), family, role, k.Name(role), k.Label(role), ImVec2(0, 1.6f * u));
				}
				else
				{
					char glyph[8];
					snprintf(glyph, sizeof(glyph), "%d", i + 1);
					RoundButton(draw, c, 1.0f * u, d.controls[i] != false, family, Role_Count, glyph, MappedLabel(m, i), ImVec2(0, 1.6f * u));
				}
			}

			// The small buttons along the top edge, with their names and what they send.
			struct Extra { bool on; const char* name; std::string label; };
			std::vector<Extra> extras;
			char numbers[4][8];
			if (known)
			{
				for (int role : { Role_Select, Role_Start, Role_Home, Role_Touchpad, Role_Capture })
					if (k.Has(role))
						extras.push_back({ k.On(role), k.Name(role), k.Label(role) });
			}
			else
			{
				for (int i = 8; i < 12; ++i)
				{
					snprintf(numbers[i - 8], sizeof(numbers[i - 8]), "%d", i + 1);
					extras.push_back({ d.controls[i] != false, numbers[i - 8], MappedLabel(m, i) });
				}
			}
			for (size_t i = 0; i < extras.size(); ++i)
			{
				const ImVec2 c(a.x + 1.4f * u + i * 3.8f * u, a.y + 0.9f * u);
				draw->AddCircleFilled(c, 0.4f * u, extras[i].on ? kLit : IM_COL32(70, 70, 80, 255));
				draw->AddCircle(c, 0.4f * u, extras[i].on ? IM_COL32_WHITE : kOutline, 0, 1.2f);
				std::string text = extras[i].name ? extras[i].name : "";
				ImFont* font = ImGui::GetFont();
				const float size = ImGui::GetFontSize() * 0.62f;
				const ImVec2 at(c.x + 0.6f * u, c.y - size * 0.5f);
				draw->AddText(font, size, at, IM_COL32(170, 175, 188, 255), text.c_str());
				if (!extras[i].label.empty())
				{
					const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
					draw->AddText(font, size, ImVec2(at.x + w + 0.25f * u, at.y), kLabel, extras[i].label.c_str());
				}
			}
			ImGui::Dummy(ImVec2(width, 10.0f * u));
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

	IconStyle StyleFor(const DeviceView& device)
	{
		if (device.kind == DeviceKind::Keyboard)
			return IconStyle::Keyboard;
		if (device.model.arcadeStick)
			return IconStyle::ArcadeStick;
		switch (device.model.Family())
		{
		case PadFamily::Xbox: return IconStyle::XboxPad;
		case PadFamily::PlayStation: return IconStyle::PlayStationPad;
		case PadFamily::Nintendo: return IconStyle::NintendoPad;
		default: return IconStyle::GenericPad;
		}
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
		case IconStyle::PlayStationPad:
		case IconStyle::NintendoPad:
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
			// Face buttons: north, south, west, east.
			const ImVec2 at[4] = {
				ImVec2(c.x + 3.3f * u, c.y - 1.7f * u), ImVec2(c.x + 3.3f * u, c.y + 0.5f * u),
				ImVec2(c.x + 2.2f * u, c.y - 0.6f * u), ImVec2(c.x + 4.4f * u, c.y - 0.6f * u),
			};
			const float r = 0.65f * u;
			if (style == IconStyle::XboxPad)
			{
				const ImU32 xbox[4] = { IM_COL32(230, 200, 60, 255), IM_COL32(80, 200, 90, 255), IM_COL32(70, 130, 230, 255), IM_COL32(220, 70, 70, 255) };
				for (int i = 0; i < 4; ++i)
					draw->AddCircleFilled(at[i], r, xbox[i]);
			}
			else if (style == IconStyle::PlayStationPad)
			{
				// The four shapes in their colours, and the touchpad.
				const float s = r * 0.95f, t = (std::max)(1.0f, u * 0.35f);
				draw->AddTriangleFilled(ImVec2(at[0].x, at[0].y - s), ImVec2(at[0].x - s, at[0].y + s * 0.75f), ImVec2(at[0].x + s, at[0].y + s * 0.75f), IM_COL32(60, 205, 170, 255));
				draw->AddLine(ImVec2(at[1].x - s, at[1].y - s), ImVec2(at[1].x + s, at[1].y + s), IM_COL32(120, 160, 240, 255), t);
				draw->AddLine(ImVec2(at[1].x - s, at[1].y + s), ImVec2(at[1].x + s, at[1].y - s), IM_COL32(120, 160, 240, 255), t);
				draw->AddRectFilled(ImVec2(at[2].x - s * 0.8f, at[2].y - s * 0.8f), ImVec2(at[2].x + s * 0.8f, at[2].y + s * 0.8f), IM_COL32(225, 130, 200, 255));
				draw->AddCircleFilled(at[3], s * 0.9f, IM_COL32(235, 85, 90, 255));
				draw->AddRectFilled(ImVec2(c.x - 1.3f * u, c.y - 2.8f * u), ImVec2(c.x + 1.3f * u, c.y - 1.0f * u), hole, 0.4f * u);
			}
			else if (style == IconStyle::NintendoPad)
			{
				// Plain buttons with - and + between the halves.
				for (int i = 0; i < 4; ++i)
					draw->AddCircleFilled(at[i], r, hole);
				const float t = (std::max)(1.0f, u * 0.3f);
				draw->AddLine(ImVec2(c.x - 1.5f * u, c.y - 1.7f * u), ImVec2(c.x - 0.6f * u, c.y - 1.7f * u), hole, t);
				draw->AddLine(ImVec2(c.x + 0.6f * u, c.y - 1.7f * u), ImVec2(c.x + 1.5f * u, c.y - 1.7f * u), hole, t);
				draw->AddLine(ImVec2(c.x + 1.05f * u, c.y - 2.15f * u), ImVec2(c.x + 1.05f * u, c.y - 1.25f * u), hole, t);
			}
			else
			{
				for (int i = 0; i < 4; ++i)
					draw->AddCircleFilled(at[i], r * 0.9f, hole);
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
		if (device.kind == DeviceKind::Keyboard || !device.model.layout)
			DrawKeyboard(draw, origin, width, device, mapping);
		else if (device.model.arcadeStick)
			DrawArcadeStick(draw, origin, width, device, mapping);
		else
			DrawPad(draw, origin, width, device, mapping);
	}
}
