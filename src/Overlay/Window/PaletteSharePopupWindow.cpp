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

	// Same one-popup-at-a-time rule as WinePopupWindow: wait until the other
	// first-launch popups are answered before showing this one.
	if (m_pWindowContainer->GetWindow(WindowType_ReplayDBPopup)->IsOpen() ||
		m_pWindowContainer->GetWindow(WindowType_WinePopup)->IsOpen())
		return;

	BeforeDraw();

	ImGui::Begin(m_windowTitle.c_str(), &m_windowOpen, m_windowFlags);
	Draw();
	ImGui::End();

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
	ImGui::SetNextWindowPosCenter(ImGuiCond_Appearing);

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
