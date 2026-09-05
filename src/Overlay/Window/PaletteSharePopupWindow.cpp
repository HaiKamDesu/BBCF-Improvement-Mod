#include "PaletteSharePopupWindow.h"

#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Overlay/imgui_utils.h"

#include <imgui.h>

#include <string>

void PaletteSharePopupWindow::Update()
{
	if (!m_windowOpen)
		return;

	if (m_pWindowContainer->ShouldDeferExclusivePopup(WindowType_PaletteSharePopup))
		return;

	BeforeDraw();

	// No wrapper Begin: everything this window draws lives inside the modal in Draw(), so a
	// wrapper is an empty title-less ImGui window - a small box in the corner with nothing in
	// it, which cannot be moved, resized or closed and which the mod menu hotkey does not
	// touch. UpdateNotifierWindow has always done it this way.
	Draw();

	AfterDraw();
}

void PaletteSharePopupWindow::Draw()
{
	ImVec4 black = ImVec4(0.060f, 0.060f, 0.060f, 1.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_PopupBg, black);
	const char* popupTitle = Messages.Share_your_custom_palettes();
	ImGui::OpenPopup(popupTitle);

	const ImVec2 buttonSize = ImVec2(160, 23);
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (ImGui::BeginPopupModal(popupTitle, NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted(Messages.Palette_share_popup_description());
		ImGui::Separator();

		ImGui::AlignItemHorizontalCenter(buttonSize.x);
		if (ImGui::Button((std::string(Messages.Yes_share_them()) + "##palettesharepopup").c_str(), buttonSize))
		{
			Settings::changeSetting("AllowPaletteDownloads", "1");
			Settings::settingsIni.allowPaletteDownloads = 1;
			g_modVals.allowPaletteDownloads = 1;
			ImGui::CloseCurrentPopup();
			Close();
		}

		ImGui::AlignItemHorizontalCenter(buttonSize.x);
		if (ImGui::Button((std::string(Messages.No_dont_share()) + "##palettesharepopup").c_str(), buttonSize))
		{
			Settings::changeSetting("AllowPaletteDownloads", "0");
			Settings::settingsIni.allowPaletteDownloads = 0;
			g_modVals.allowPaletteDownloads = 0;
			ImGui::CloseCurrentPopup();
			Close();
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
}
