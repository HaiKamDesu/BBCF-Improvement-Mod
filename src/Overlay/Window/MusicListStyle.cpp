#include "MusicListStyle.h"

#include "Overlay/imgui_utils.h"

#include "imgui_internal.h"

#include <cstdio>

namespace MusicListStyle
{
	ImVec4 CategoryColor(const std::string& category)
	{
		if (category == "btl") return ImVec4(1.0f, 0.4f, 0.4f, 1.0f);              // battle: red
		if (category == "vs") return ImVec4(0.4f, 0.6f, 1.0f, 1.0f);               // versus: blue
		if (category == "boss") return ImVec4(0.8f, 0.4f, 1.0f, 1.0f);             // boss: purple
		if (category == "sys" || category == "sysex") return ImVec4(1.0f, 1.0f, 0.4f, 1.0f); // system: yellow
		if (category == "old") return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);             // older versions: grey
		if (category == "story") return ImVec4(0.4f, 1.0f, 0.6f, 1.0f);           // story: light green
		if (category == "astral") return ImVec4(1.0f, 0.6f, 0.2f, 1.0f);          // Astral Finish: orange
		if (category == "custom") return ImVec4(0.2f, 0.8f, 1.0f, 1.0f);          // custom songs: cyan
		if (category == "replacements") return ImVec4(0.6f, 1.0f, 0.4f, 1.0f);    // replacements: green
		return ImVec4(1, 1, 1, 1);
	}

	HeaderResult CategoryHeader(const std::string& category, int checkState, bool checkboxEnabled,
		bool open, const char* checkboxTip)
	{
		HeaderResult result;
		ImGui::PushID(category.c_str());
		bool checked = checkState == 1;
		ImGui::BeginDisabled(!checkboxEnabled);
		ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, checkState == -1);
		if (ImGui::Checkbox("##all", &checked))
		{
			result.checkboxClicked = true;
			// From mixed, a click always means "all on".
			result.checkTo = checkState == -1 ? true : checked;
		}
		ImGui::PopItemFlag();
		ImGui::EndDisabled();
		if (checkboxTip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltipWrapped(checkboxTip);
		ImGui::SameLine();

		ImGui::SetNextItemOpen(open, ImGuiCond_Always);
		ImGui::PushStyleColor(ImGuiCol_Text, CategoryColor(category));
		char header[96];
		snprintf(header, sizeof(header), "%s##header", category.c_str());
		result.open = ImGui::CollapsingHeader(header);
		ImGui::PopStyleColor();
		ImGui::PopID();
		return result;
	}

	bool CategoryHeaderPlain(const std::string& category, bool open)
	{
		ImGui::PushID(category.c_str());
		ImGui::SetNextItemOpen(open, ImGuiCond_Always);
		ImGui::PushStyleColor(ImGuiCol_Text, CategoryColor(category));
		char header[96];
		snprintf(header, sizeof(header), "%s##header", category.c_str());
		const bool result = ImGui::CollapsingHeader(header);
		ImGui::PopStyleColor();
		ImGui::PopID();
		return result;
	}

	bool VolumeButton(float db, const char* id)
	{
		char label[48];
		if (db == 0.0f)
			snprintf(label, sizeof(label), "vol##%s", id);
		else
			snprintf(label, sizeof(label), "%+.0f dB##%s", db, id);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		const bool clicked = ImGui::SmallButton(label);
		ImGui::PopStyleColor();
		return clicked;
	}

	bool DropdownButton(const char* label, const char* id, float width, const ImVec4& textColor)
	{
		ImGui::PushID(id);
		const ImVec2 start = ImGui::GetCursorScreenPos();
		const float height = ImGui::GetFrameHeight();
		const bool clicked = ImGui::InvisibleButton("##dropdown", ImVec2(width, height));
		const bool hovered = ImGui::IsItemHovered();
		ImDrawList* draw = ImGui::GetWindowDrawList();
		// A quiet frame like a combo box's, a little brighter under the mouse.
		ImVec4 frame = ImGui::GetStyleColorVec4(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
		frame.w *= hovered ? 0.9f : 0.45f;
		draw->AddRectFilled(start, ImVec2(start.x + width, start.y + height), ImGui::GetColorU32(frame),
			ImGui::GetStyle().FrameRounding);
		// The arrow on the right, the label cut to the space left of it.
		const float arrowSize = ImGui::GetFontSize();
		const float padX = ImGui::GetStyle().FramePadding.x;
		const float textY = start.y + ImGui::GetStyle().FramePadding.y;
		ImGui::RenderArrow(draw, ImVec2(start.x + width - arrowSize - padX * 0.5f, textY),
			ImGui::GetColorU32(ImGuiCol_TextDisabled), ImGuiDir_Down, 0.65f);
		const float textRight = start.x + width - arrowSize - padX;
		ImGui::PushClipRect(ImVec2(start.x + padX, start.y), ImVec2(textRight, start.y + height), true);
		draw->AddText(ImVec2(start.x + padX, textY), ImGui::GetColorU32(textColor), label);
		ImGui::PopClipRect();
		ImGui::PopID();
		return clicked;
	}

	void AnchorNextPopupBelowLastItem()
	{
		ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y), ImGuiCond_Appearing);
	}

	OpaquePopup::OpaquePopup()
	{
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(22, 24, 30, 255));
		ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 140));
	}

	OpaquePopup::~OpaquePopup()
	{
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar();
	}
}
