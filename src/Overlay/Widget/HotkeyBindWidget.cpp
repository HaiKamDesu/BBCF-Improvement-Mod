#include "HotkeyBindWidget.h"

#include "Overlay/imgui_utils.h"

#include <imgui.h>

#include <algorithm>

namespace
{
	// Only one widget can be listening at a time, so the capture owner is tracked globally by
	// the ImGui id it was drawn under.
	ImGuiID g_capturingWidget = 0;
}

bool ImGuiHotkey::IsAnyCapturing()
{
	return g_capturingWidget != 0 && HotkeyManager::IsCapturing();
}

bool ImGuiHotkey::BindWidget(const char* id, HotkeyBinding& binding, const char* defaultBindingText,
	const char* warning)
{
	bool changed = false;

	ImGui::PushID(id);
	const ImGuiID widgetId = ImGui::GetID("##bind");
	const bool capturing = (g_capturingWidget == widgetId);

	if (capturing && !HotkeyManager::IsCapturing())
	{
		// Escape, or another widget stole the capture.
		g_capturingWidget = 0;
	}
	else if (capturing)
	{
		HotkeyBinding captured;
		if (HotkeyManager::PollCapture(captured))
		{
			binding = captured;
			g_capturingWidget = 0;
			changed = true;
		}
	}

	const bool stillCapturing = (g_capturingWidget == widgetId);
	const std::string label = stillCapturing
		? std::string("Press any key or button...")
		: HotkeyManager::DisplayString(binding);

	if (stillCapturing)
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.35f, 0.10f, 1.0f));
	else if (!binding.IsBound())
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
	else
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);

	if (ImGui::Button(label.c_str(), ImVec2(160.0f, 0.0f)))
	{
		if (stillCapturing)
		{
			HotkeyManager::CancelCapture();
			g_capturingWidget = 0;
		}
		else
		{
			HotkeyManager::BeginCapture();
			g_capturingWidget = widgetId;
		}
	}
	ImGui::PopStyleColor();

	if (!stillCapturing)
		ImGui::HoverTooltip("Click, then press the key or controller button you want. "
			"Hold Ctrl, Shift or Alt while pressing to include them. Escape cancels.");

	ImGui::SameLine();
	if (ImGui::Button("Clear"))
	{
		binding = HotkeyBinding();
		HotkeyManager::CancelCapture();
		g_capturingWidget = 0;
		changed = true;
	}
	ImGui::HoverTooltip("Removes this shortcut entirely. The feature still works from the menu, "
		"it just has no key.");

	ImGui::SameLine();
	if (ImGui::Button("Default"))
	{
		binding = HotkeyManager::BindingFromString(defaultBindingText ? defaultBindingText : "");
		HotkeyManager::CancelCapture();
		g_capturingWidget = 0;
		changed = true;
	}
	{
		const std::string defaultLabel = HotkeyManager::DisplayString(
			HotkeyManager::BindingFromString(defaultBindingText ? defaultBindingText : ""));
		ImGui::HoverTooltip(("Puts this shortcut back to " + defaultLabel + ".").c_str());
	}

	if (warning && warning[0])
	{
		// Wrapped, not TextColored: these warnings are full sentences and the widget lives in
		// a fixed-width table cell, so an unwrapped one is simply cut off. Wrapping also grows
		// the table row to fit, since table rows size to their content.
		const float wrapWidth = (std::max)(120.0f, ImGui::GetContentRegionAvail().x);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.25f, 1.0f));
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
		ImGui::TextUnformatted(warning);
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	}

	ImGui::PopID();
	return changed;
}
