#include "BrandingLayout.h"

#include "Overlay/imgui_utils.h"

#include <cstdio>

namespace Branding
{
	namespace
	{
		// ====================================================================
		//  DEFAULTS - paste the tuner's "Copy defaults to clipboard" output
		//  over this block to bake in whatever you settled on.
		// ====================================================================
		const Layout kDefaults = {
			/* titleBarExtraPadding  */ 0.00f,
			/* titleTextOffsetX      */ -0.00f,
			/* titleTextOffsetY      */ 0.00f,
			/* oceanyaHeight         */ 0.916f,
			/* laboratoriesHeight    */ 0.781f,
			/* oceanyaOffsetY        */ -3.00f,
			/* laboratoriesOffsetY   */ -1.00f,
			/* gapAfterVersion       */ 5.20f,
			/* gapAfterOceanya       */ 0.00f,
			/* gapAfterLaboratories  */ 0.00f,
			/* showEditionSuffix     */ true,
			/* watermarkOpacity      */ 0.072f,
			/* watermarkScale        */ 1.000f,
			/* watermarkOffsetX      */ 0.00f,
			/* watermarkOffsetY      */ -41.30f,
			/* watermarkTint         */ ImVec4(1.000f, 1.000f, 1.000f, 1.0f),
		};

		Layout g_layout = kDefaults;

		std::string FormatDefaults(const Layout& l)
		{
			char buffer[1400];
			snprintf(buffer, sizeof(buffer),
				"\t\tconst Layout kDefaults = {\n"
				"\t\t\t/* titleBarExtraPadding  */ %.2ff,\n"
				"\t\t\t/* titleTextOffsetX      */ %.2ff,\n"
				"\t\t\t/* titleTextOffsetY      */ %.2ff,\n"
				"\t\t\t/* oceanyaHeight         */ %.3ff,\n"
				"\t\t\t/* laboratoriesHeight    */ %.3ff,\n"
				"\t\t\t/* oceanyaOffsetY        */ %.2ff,\n"
				"\t\t\t/* laboratoriesOffsetY   */ %.2ff,\n"
				"\t\t\t/* gapAfterVersion       */ %.2ff,\n"
				"\t\t\t/* gapAfterOceanya       */ %.2ff,\n"
				"\t\t\t/* gapAfterLaboratories  */ %.2ff,\n"
				"\t\t\t/* showEditionSuffix     */ %s,\n"
				"\t\t\t/* watermarkOpacity      */ %.3ff,\n"
				"\t\t\t/* watermarkScale        */ %.3ff,\n"
				"\t\t\t/* watermarkOffsetX      */ %.2ff,\n"
				"\t\t\t/* watermarkOffsetY      */ %.2ff,\n"
				"\t\t\t/* watermarkTint         */ ImVec4(%.3ff, %.3ff, %.3ff, 1.0f),\n"
				"\t\t};\n",
				l.titleBarExtraPadding, l.titleTextOffsetX, l.titleTextOffsetY,
				l.oceanyaHeight, l.laboratoriesHeight,
				l.oceanyaOffsetY, l.laboratoriesOffsetY,
				l.gapAfterVersion, l.gapAfterOceanya, l.gapAfterLaboratories,
				l.showEditionSuffix ? "true" : "false",
				l.watermarkOpacity, l.watermarkScale,
				l.watermarkOffsetX, l.watermarkOffsetY,
				l.watermarkTint.x, l.watermarkTint.y, l.watermarkTint.z);
			return buffer;
		}
	}

	Layout& GetLayout()
	{
		return g_layout;
	}

	void DrawLayoutTuner()
	{
		if (!ImGui::CollapsingHeader("Branding layout"))
			return;

		ImGui::TextDisabledWrapped(
			"Drag anything here and the mod menu's title bar and watermark update as you go. "
			"Nothing is saved: when it looks right, copy the defaults out and paste them over "
			"kDefaults in src/Overlay/BrandingLayout.cpp.");

		ImGui::VerticalSpacing(6);
		ImGui::SeparatorText("Title bar");

		ImGui::SliderFloat("Bar extra padding", &g_layout.titleBarExtraPadding, 0.0f, 24.0f, "%.1f px");
		ImGui::ShowHelpMarkerSameLine(
			"Makes the title bar taller. The wordmarks are sized as a fraction of the bar, so "
			"this is what to raise if they are still too small at a height fraction of 1.0.");

		ImGui::SliderFloat("Version text X", &g_layout.titleTextOffsetX, -40.0f, 80.0f, "%.1f px");
		ImGui::SliderFloat("Version text Y", &g_layout.titleTextOffsetY, -20.0f, 20.0f, "%.1f px");

		ImGui::VerticalSpacing(4);
		ImGui::SliderFloat("OCEANYA height", &g_layout.oceanyaHeight, 0.2f, 3.0f, "%.3f x bar");
		ImGui::ShowHelpMarkerSameLine(
			"Fraction of the title bar's height. Above 1.0 the artwork is taller than the bar "
			"and gets clipped by it, so raise 'Bar extra padding' alongside it.");
		ImGui::SliderFloat("OCEANYA Y", &g_layout.oceanyaOffsetY, -30.0f, 30.0f, "%.1f px");

		ImGui::VerticalSpacing(4);
		ImGui::SliderFloat("Laboratories height", &g_layout.laboratoriesHeight, 0.2f, 3.0f, "%.3f x bar");
		ImGui::SliderFloat("Laboratories Y", &g_layout.laboratoriesOffsetY, -30.0f, 30.0f, "%.1f px");

		ImGui::VerticalSpacing(4);
		ImGui::SliderFloat("Gap after version", &g_layout.gapAfterVersion, 0.0f, 60.0f, "%.1f px");
		ImGui::SliderFloat("Gap after OCEANYA", &g_layout.gapAfterOceanya, 0.0f, 60.0f, "%.1f px");
		ImGui::SliderFloat("Gap after Laboratories", &g_layout.gapAfterLaboratories, 0.0f, 60.0f, "%.1f px");
		ImGui::Checkbox("Show \"Edition\" suffix", &g_layout.showEditionSuffix);

		ImGui::VerticalSpacing(6);
		ImGui::SeparatorText("Background watermark");

		ImGui::SliderFloat("Opacity", &g_layout.watermarkOpacity, 0.0f, 1.0f, "%.3f");
		ImGui::SliderFloat("Scale", &g_layout.watermarkScale, 0.1f, 3.0f, "%.3f x fit");
		ImGui::ShowHelpMarkerSameLine(
			"1.0 fits the mark inside the window. Above 1.0 it overflows and is clipped by the "
			"window edges, which is how you get a cropped, zoomed-in look.");
		ImGui::SliderFloat("Offset X", &g_layout.watermarkOffsetX, -400.0f, 400.0f, "%.1f px");
		ImGui::SliderFloat("Offset Y", &g_layout.watermarkOffsetY, -400.0f, 400.0f, "%.1f px");
		ImGui::ColorEdit3("Tint", &g_layout.watermarkTint.x);

		ImGui::VerticalSpacing(8);
		ImGui::Separator();

		if (ImGui::Button("Copy defaults to clipboard"))
			ImGui::SetClipboardText(FormatDefaults(g_layout).c_str());
		ImGui::SameLineOrWrap(ImGui::ButtonWidth("Reset to built-in defaults"));
		if (ImGui::Button("Reset to built-in defaults"))
			g_layout = kDefaults;

		ImGui::VerticalSpacing(4);
		const std::string preview = FormatDefaults(g_layout);
		ImGui::InputTextMultiline("##branding_defaults", const_cast<char*>(preview.c_str()),
			preview.size() + 1, ImVec2(-1.0f, ImGui::GetTextLineHeight() * 18.0f),
			ImGuiInputTextFlags_ReadOnly);
	}
}
