#pragma once

#include <imgui.h>
#include <string>

namespace ImGui
{
	// ImGui dropped support for indexing keys by Win32 virtual-key code (obsolete since 1.87,
	// removed in 1.91.5). Our hotkeys are stored in settings.ini as VK codes via
	// Settings::getButtonValue, so translate before querying.
	IMGUI_API ImGuiKey VirtualKeyToImGuiKey(int virtualKey);
	IMGUI_API bool IsVirtualKeyPressed(int virtualKey, bool repeat = true);
	IMGUI_API bool IsVirtualKeyDown(int virtualKey);

	// Faux bold. No bold face is bundled and neither stb_truetype nor ImGui synthesizes weight,
	// so bold is an overdraw whose offset scales with the current font size (a fixed 1px reads
	// as bold at body size and as nothing at heading size).
	IMGUI_API float GetBoldOverdrawOffset();
	IMGUI_API ImVec2 CalcTextSizeBold(const char* text, const char* textEnd = NULL);
	IMGUI_API void AddTextBold(ImDrawList* drawList, const ImVec2& pos, ImU32 col, const char* text, const char* textEnd = NULL);

	IMGUI_API void HoverTooltip(const char* text);
	IMGUI_API bool ButtonUrl(const std::string& label, const wchar_t* url, const ImVec2& btnSize = ImVec2(0, 0));
	IMGUI_API void TextAlignedHorizontalCenter(const char* text, ...);
	IMGUI_API void TextColoredAlignedHorizontalCenter(const ImVec4 color, const char* text);
	IMGUI_API void AlignItemHorizontalCenter(float itemWidth);
	IMGUI_API void Spacing(ImVec2& size);
	IMGUI_API void VerticalSpacing(float height = 1);
	IMGUI_API void HorizontalSpacing(float width = 1);

	IMGUI_API bool SliderByte(const char* label, unsigned char* v, unsigned char v_min, unsigned char v_max, const char* display_format = "%.0f");
	IMGUI_API bool DragByte(const char* label, unsigned char* v, float v_speed, unsigned char v_min, unsigned char v_max, const char* display_format = "%.0f");

	IMGUI_API bool ColorButtonIndexed(const char* desc_id, int idx, const ImVec4& col, ImGuiColorEditFlags flags = 0, ImVec2 size = ImVec2(0, 0));
	IMGUI_API void ColorTooltipIndexed(const char* text, int idx, const float* col, ImGuiColorEditFlags flags);
	IMGUI_API bool ColorEdit4On32Bit(const char* label, int idx, unsigned char val[4], ImGuiColorEditFlags flags = 0);
	IMGUI_API bool ColorButtonOn32Bit(const char* desc_id, int idx, unsigned char val[4], ImGuiColorEditFlags flags = 0, ImVec2 size = ImVec2(0, 0));
	IMGUI_API bool ColorPicker4On32Bit(const char* label, unsigned char col[4], ImGuiColorEditFlags flags = 0, const float* ref_col = NULL);
	IMGUI_API void ShowHelpMarker(const char* desc);

	// SameLine(), but only when the next item actually fits. The mod menu is resizable, and a
	// plain SameLine() chain simply runs off the right edge and clips whatever came last -
	// help markers, the second button of a pair, a checkbox. These start a new line instead.
	IMGUI_API void SameLineOrWrap(float nextItemWidth);
	IMGUI_API float ButtonWidth(const char* label);
	IMGUI_API float CheckboxWidth(const char* label);
	IMGUI_API float HelpMarkerWidth();
	IMGUI_API void ShowHelpMarkerSameLine(const char* desc);

	/* Wrapping variants of the coloured/disabled text calls. Plain TextDisabled and
	   TextColored do NOT wrap, so a long sentence runs off the side of the window and
	   forces a horizontal scrollbar (or is simply lost, in a NoScrollbar window). Use
	   these for anything longer than a label. */
	IMGUI_API void TextDisabledWrapped(const char* fmt, ...);
	IMGUI_API void TextColoredWrapped(const ImVec4& color, const char* fmt, ...);

	/* SetTooltip does not wrap either; this matches ShowHelpMarker's wrap width. */
	IMGUI_API void SetTooltipWrapped(const char* text);
}
