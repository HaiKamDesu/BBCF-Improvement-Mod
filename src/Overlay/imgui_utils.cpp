#include "imgui_utils.h"

#include "imgui_internal.h"

#include <windows.h>

///////////////////////////////////////////////////// HELPERS /////////////////////////////////////////////////////

// Covers every key Settings::getButtonValue can produce (see src/Core/keycodes.h) plus the
// obvious neighbours. Deliberately not delegating to the Win32 backend's
// ImGui_ImplWin32_KeyEventToImGuiKey(): that one resolves modifiers and most OEM keys through
// the scancode in lParam, which we don't have here, so it would silently return ImGuiKey_None
// for SHIFT/CTRL/ALT/TILDE/MINUS/EQUAL/brackets/backslash/semicolon/quote/slash.
ImGuiKey ImGui::VirtualKeyToImGuiKey(int virtualKey)
{
	if (virtualKey >= '0' && virtualKey <= '9')
		return (ImGuiKey)(ImGuiKey_0 + (virtualKey - '0'));
	if (virtualKey >= 'A' && virtualKey <= 'Z')
		return (ImGuiKey)(ImGuiKey_A + (virtualKey - 'A'));
	if (virtualKey >= VK_F1 && virtualKey <= VK_F24)
		return (ImGuiKey)(ImGuiKey_F1 + (virtualKey - VK_F1));
	if (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_NUMPAD9)
		return (ImGuiKey)(ImGuiKey_Keypad0 + (virtualKey - VK_NUMPAD0));

	switch (virtualKey)
	{
	// Modifiers: use the mod flags rather than the Left* keys, so either side fires -
	// this matches the pre-1.87 behaviour where the backend set KeysDown[VK_SHIFT] for both.
	case VK_SHIFT:      return (ImGuiKey)ImGuiMod_Shift;
	case VK_CONTROL:    return (ImGuiKey)ImGuiMod_Ctrl;
	case VK_MENU:       return (ImGuiKey)ImGuiMod_Alt;
	case VK_LSHIFT:     return ImGuiKey_LeftShift;
	case VK_RSHIFT:     return ImGuiKey_RightShift;
	case VK_LCONTROL:   return ImGuiKey_LeftCtrl;
	case VK_RCONTROL:   return ImGuiKey_RightCtrl;
	case VK_LMENU:      return ImGuiKey_LeftAlt;
	case VK_RMENU:      return ImGuiKey_RightAlt;

	case VK_TAB:        return ImGuiKey_Tab;
	case VK_RETURN:     return ImGuiKey_Enter;
	case VK_BACK:       return ImGuiKey_Backspace;
	case VK_SPACE:      return ImGuiKey_Space;
	case VK_ESCAPE:     return ImGuiKey_Escape;
	case VK_CAPITAL:    return ImGuiKey_CapsLock;

	case VK_LEFT:       return ImGuiKey_LeftArrow;
	case VK_UP:         return ImGuiKey_UpArrow;
	case VK_RIGHT:      return ImGuiKey_RightArrow;
	case VK_DOWN:       return ImGuiKey_DownArrow;

	case VK_INSERT:     return ImGuiKey_Insert;
	case VK_DELETE:     return ImGuiKey_Delete;
	case VK_HOME:       return ImGuiKey_Home;
	case VK_END:        return ImGuiKey_End;
	case VK_PRIOR:      return ImGuiKey_PageUp;
	case VK_NEXT:       return ImGuiKey_PageDown;
	case VK_SNAPSHOT:   return ImGuiKey_PrintScreen;
	case VK_SCROLL:     return ImGuiKey_ScrollLock;
	case VK_PAUSE:      return ImGuiKey_Pause;
	case VK_NUMLOCK:    return ImGuiKey_NumLock;

	case VK_OEM_3:      return ImGuiKey_GraveAccent;   // TILDE
	case VK_OEM_MINUS:  return ImGuiKey_Minus;
	case VK_OEM_PLUS:   return ImGuiKey_Equal;         // EQUAL
	case VK_OEM_4:      return ImGuiKey_LeftBracket;
	case VK_OEM_6:      return ImGuiKey_RightBracket;
	case VK_OEM_5:      return ImGuiKey_Backslash;
	case VK_OEM_1:      return ImGuiKey_Semicolon;
	case VK_OEM_7:      return ImGuiKey_Apostrophe;    // QUOTE
	case VK_OEM_COMMA:  return ImGuiKey_Comma;
	case VK_OEM_PERIOD: return ImGuiKey_Period;
	case VK_OEM_2:      return ImGuiKey_Slash;

	case VK_ADD:        return ImGuiKey_KeypadAdd;
	case VK_SUBTRACT:   return ImGuiKey_KeypadSubtract;
	case VK_MULTIPLY:   return ImGuiKey_KeypadMultiply;
	case VK_DIVIDE:     return ImGuiKey_KeypadDivide;
	case VK_DECIMAL:    return ImGuiKey_KeypadDecimal;

	default:            return ImGuiKey_None;
	}
}

bool ImGui::IsVirtualKeyPressed(int virtualKey, bool repeat)
{
	const ImGuiKey key = VirtualKeyToImGuiKey(virtualKey);
	return key != ImGuiKey_None && ImGui::IsKeyPressed(key, repeat);
}

bool ImGui::IsVirtualKeyDown(int virtualKey)
{
	const ImGuiKey key = VirtualKeyToImGuiKey(virtualKey);
	return key != ImGuiKey_None && ImGui::IsKeyDown(key);
}

// Helpers copied from imgui.cpp
#define IM_F32_TO_INT8_SAT(_VAL)        ((int)(ImSaturate(_VAL) * 255.0f + 0.5f))               // Saturated, always output 0..255

// The mod stores palette colors as 32-bit BGRA bytes; ImGui works in normalized RGBA floats.
static inline void BGRA8ToFloat4(const unsigned char val[4], float out[4])
{
	out[0] = val[2] / 255.0f;
	out[1] = val[1] / 255.0f;
	out[2] = val[0] / 255.0f;
	out[3] = val[3] / 255.0f;
}

static inline void Float4ToBGRA8(const float in[4], unsigned char val[4], bool writeAlpha)
{
	val[0] = (unsigned char)IM_F32_TO_INT8_SAT(in[2]);
	val[1] = (unsigned char)IM_F32_TO_INT8_SAT(in[1]);
	val[2] = (unsigned char)IM_F32_TO_INT8_SAT(in[0]);
	if (writeAlpha)
		val[3] = (unsigned char)IM_F32_TO_INT8_SAT(in[3]);
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

float ImGui::GetBoldOverdrawOffset()
{
	const float scaled = GetFontSize() * 0.07f;
	return scaled > 1.0f ? scaled : 1.0f;
}

ImVec2 ImGui::CalcTextSizeBold(const char* text, const char* textEnd)
{
	ImVec2 size = CalcTextSize(text, textEnd);
	size.x += GetBoldOverdrawOffset();
	return size;
}

void ImGui::AddTextBold(ImDrawList* drawList, const ImVec2& pos, ImU32 col, const char* text, const char* textEnd)
{
	ImFont* const font = GetFont();
	const float fontSize = GetFontSize();
	const float offset = GetBoldOverdrawOffset();

	// Three passes at 0, half and full offset: the half step fills the gap a single full step
	// would leave once the font is large, so the stroke thickens evenly instead of ghosting.
	drawList->AddText(font, fontSize, pos, col, text, textEnd);
	drawList->AddText(font, fontSize, ImVec2(pos.x + offset * 0.5f, pos.y), col, text, textEnd);
	drawList->AddText(font, fontSize, ImVec2(pos.x + offset, pos.y), col, text, textEnd);
}

void ImGui::HoverTooltip(const char* text)
{
	if (IsItemHovered())
	{
		BeginTooltip();
		TextUnformatted(text);
		EndTooltip();
	}
}

bool ImGui::ButtonUrl(const std::string& label, const wchar_t* url, const ImVec2& btnSize)
{
	if (Button(label.c_str(), btnSize))
	{
		ShellExecute(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
		return true;
	}

	return false;
}

void ImGui::TextAlignedHorizontalCenter(const char* text, ...)
{
	const ImVec4 COLOR_TRANSPARENT(0.000f, 0.000f, 0.000f, 0.000f);

	va_list args;
	va_start(args, text);

	TextColoredV(COLOR_TRANSPARENT, text, args);
	float width = GetItemRectSize().x;
	float height = GetItemRectSize().y;
	AlignItemHorizontalCenter(width);
	SetCursorPosY(GetCursorPosY() - height);

	TextV(text, args);
	va_end(args);
}

void ImGui::TextColoredAlignedHorizontalCenter(const ImVec4 color, const char* text)
{
	const ImVec4 COLOR_TRANSPARENT(0.000f, 0.000f, 0.000f, 0.000f);
	TextColored(COLOR_TRANSPARENT, text);

	float width = GetItemRectSize().x;
	float height = GetItemRectSize().y;
	AlignItemHorizontalCenter(width);
	SetCursorPosY(GetCursorPosY() - height);

	TextColored(color, text);
}

void ImGui::AlignItemHorizontalCenter(float itemWidth)
{
	SetCursorPosX(GetWindowSize().x / 2 - (itemWidth / 2));
}

void ImGui::Spacing(ImVec2& size)
{
	ImGuiWindow* window = GetCurrentWindow();
	if (window->SkipItems)
		return;

	PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
	ItemSize(size);
	PopStyleVar();
}

void ImGui::VerticalSpacing(float height)
{
	ImGuiWindow* window = GetCurrentWindow();
	if (window->SkipItems)
		return;

	PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
	ItemSize(ImVec2(0, height));
	PopStyleVar();
}

void ImGui::HorizontalSpacing(float width)
{
	ImGuiWindow* window = GetCurrentWindow();
	if (window->SkipItems)
		return;

	PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
	ItemSize(ImVec2(width, 0));
	PopStyleVar();

	SameLine();
}

bool ImGui::SliderByte(const char* label, unsigned char* v, unsigned char v_min, unsigned char v_max, const char* display_format)
{
    if (!display_format)
        display_format = "%.0f";
    float v_f = (float)*v;
    // The trailing 1.0f used to be the 'power' argument (linear). That parameter was obsoleted in
    // 1.78 and the slot now holds ImGuiSliderFlags, where a value of 1 falls inside
    // ImGuiSliderFlags_InvalidMask_ and trips an assert. Linear is the default, so just drop it.
    bool value_changed = SliderFloat(label, &v_f, (float)v_min, (float)v_max, display_format);
    *v = (unsigned char)v_f;
    return value_changed;
}

bool ImGui::DragByte(const char* label, unsigned char* v, float v_speed, unsigned char v_min, unsigned char v_max, const char* display_format)
{
    if (!display_format)
        display_format = "%.0f";
    float v_f = (float)*v;
    bool value_changed = DragFloat(label, &v_f, v_speed, (float)v_min, (float)v_max, display_format);
    *v = (unsigned char)v_f;
    return value_changed;
}


// ---------------------------------------------------------------------------------------------
// Palette color widgets.
//
// These used to be verbatim copies of ImGui 1.53's internal ColorButton/ColorEdit4/ColorPicker4
// carrying two changes: they operate on the game's 32-bit BGRA palette entries, and their
// tooltip shows the palette slot number. Both are now expressed as thin wrappers over the
// public API instead, so future ImGui updates don't require re-forking the widget internals.
// ---------------------------------------------------------------------------------------------

void ImGui::ColorTooltipIndexed(const char* text, int idx, const float* col, ImGuiColorEditFlags flags)
{
	ImGuiContext& g = *GImGui;

	const int cr = IM_F32_TO_INT8_SAT(col[0]), cg = IM_F32_TO_INT8_SAT(col[1]), cb = IM_F32_TO_INT8_SAT(col[2]);
	const int ca = (flags & ImGuiColorEditFlags_NoAlpha) ? 255 : IM_F32_TO_INT8_SAT(col[3]);

	if (!BeginTooltip())
		return;

	const char* text_end = text ? FindRenderedTextEnd(text, NULL) : text;
	if (text_end > text)
	{
		TextUnformatted(text, text_end);
		Separator();
	}

	const ImVec2 sz(g.FontSize * 4 + g.Style.FramePadding.y * 2, g.FontSize * 4 + g.Style.FramePadding.y * 2);
	ColorButton("##preview", ImVec4(col[0], col[1], col[2], col[3]),
		(flags & ImGuiColorEditFlags_AlphaMask_) | ImGuiColorEditFlags_NoTooltip, sz);
	SameLine();

	if (flags & ImGuiColorEditFlags_NoAlpha)
		idx ? Text("%.3d\n#%02X%02X%02X\nR: %d, G: %d, B: %d\n(%.3f, %.3f, %.3f)", idx, cr, cg, cb, cr, cg, cb, col[0], col[1], col[2])
		: Text("#%02X%02X%02X\nR: %d, G: %d, B: %d\n(%.3f, %.3f, %.3f)", cr, cg, cb, cr, cg, cb, col[0], col[1], col[2]);
	else
		idx ? Text("%.3d\n#%02X%02X%02X%02X\nR:%d, G:%d, B:%d, A:%d\n(%.3f, %.3f, %.3f, %.3f)", idx, cr, cg, cb, ca, cr, cg, cb, ca, col[0], col[1], col[2], col[3])
		: Text("#%02X%02X%02X%02X\nR:%d, G:%d, B:%d, A:%d\n(%.3f, %.3f, %.3f, %.3f)", cr, cg, cb, ca, cr, cg, cb, ca, col[0], col[1], col[2], col[3]);

	EndTooltip();
}

bool ImGui::ColorButtonIndexed(const char* desc_id, int idx, const ImVec4& col, ImGuiColorEditFlags flags, ImVec2 size)
{
	// Suppress the stock tooltip so we can substitute the one that shows the palette slot number.
	// Everything else - sizing, alpha checkerboard, border and the color drag-drop source - is
	// stock ColorButton behaviour.
	const bool wantTooltip = !(flags & ImGuiColorEditFlags_NoTooltip);
	const bool pressed = ColorButton(desc_id, col, flags | ImGuiColorEditFlags_NoTooltip, size);

	if (wantTooltip && IsItemHovered())
		ColorTooltipIndexed(desc_id, idx, &col.x, flags & ImGuiColorEditFlags_AlphaMask_);

	return pressed;
}

bool ImGui::ColorButtonOn32Bit(const char* desc_id, int idx, unsigned char val[4], ImGuiColorEditFlags flags, ImVec2 size)
{
	float col[4];
	BGRA8ToFloat4(val, col);
	return ColorButtonIndexed(desc_id, idx, ImVec4(col[0], col[1], col[2], col[3]), flags, size);
}

bool ImGui::ColorEdit4On32Bit(const char* label, int idx, unsigned char val[4], ImGuiColorEditFlags flags)
{
	float col[4];
	BGRA8ToFloat4(val, col);

	// Only take over the tooltip when there is a slot number worth showing; otherwise the stock
	// one is fine and stays correctly scoped to the preview swatch.
	const bool indexedTooltip = (idx != 0) && !(flags & ImGuiColorEditFlags_NoTooltip);
	const bool changed = ColorEdit4(label, col, indexedTooltip ? (flags | ImGuiColorEditFlags_NoTooltip) : flags);

	if (indexedTooltip && IsItemHovered())
		ColorTooltipIndexed(label, idx, col, flags & ImGuiColorEditFlags_AlphaMask_);

	if (changed)
		Float4ToBGRA8(col, val, (flags & ImGuiColorEditFlags_NoAlpha) == 0);

	return changed;
}

bool ImGui::ColorPicker4On32Bit(const char* label, unsigned char val[4], ImGuiColorEditFlags flags, const float* ref_col)
{
	float col[4];
	BGRA8ToFloat4(val, col);

	const bool changed = ColorPicker4(label, col, flags, ref_col);
	if (changed)
		Float4ToBGRA8(col, val, (flags & ImGuiColorEditFlags_NoAlpha) == 0);

	return changed;
}


void ImGui::ShowHelpMarker(const char* desc)
{
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(450.0f);
		ImGui::TextUnformatted(desc);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}