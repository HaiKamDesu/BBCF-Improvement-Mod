#include "LargeAddressPopupWindow.h"

#include "Core/LaaRedirect.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Overlay/imgui_utils.h"

#include <imgui.h>

#include <string>

void LargeAddressPopupWindow::Update()
{
	if (!m_windowOpen)
		return;

	if (m_pWindowContainer->ShouldDeferExclusivePopup(WindowType_LargeAddressPopup))
		return;

	BeforeDraw();
	// Everything lives in the modal; see PaletteSharePopupWindow::Update for why there is no
	// wrapper window.
	Draw();
	AfterDraw();
}

void LargeAddressPopupWindow::Draw()
{
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.060f, 0.060f, 0.060f, 1.0f));
	const std::string title = L("Run the game in 4 GB mode?") + "##largeaddresspopup";
	ImGui::OpenPopup(title.c_str());

	const ImVec2 buttonSize = ImVec2(220, 23);
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);

	if (ImGui::BeginPopupModal(title.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::PushTextWrapPos(540);
		ImGui::TextUnformatted(L("BBCF is a 32-bit game and only gets 2 GB of memory. In long sessions that memory runs out, and the game closes by itself when a match or a replay loads.").c_str());
		ImGui::Spacing();
		ImGui::TextUnformatted(L("The mod can run the game with 4 GB instead. If you accept:").c_str());
		// BulletText does not wrap; a bullet followed by wrapped text does.
		for (const char* line : {
			"The mod makes a copy of BBCF.exe called BBCF_IM_4GB.exe, with one setting changed so Windows gives it 4 GB. Your BBCF.exe is not modified.",
			"From then on, starting the game from Steam starts that copy. Your saves, settings and Steam work as before.",
			"The game closes and starts again by itself, once, right now." })
		{
			ImGui::Bullet();
			ImGui::TextWrapped("%s", L(line).c_str());
		}
		ImGui::Spacing();
		ImGui::TextUnformatted(L("If you ever remove the mod, the game just starts normally again. You can turn this off at any time in the mod's Settings (4 GB mode).").c_str());
		ImGui::PopTextWrapPos();

		if (!m_error.empty())
		{
			ImGui::Spacing();
			ImGui::PushTextWrapPos(540);
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s %s", L("Could not switch to 4 GB mode:").c_str(), m_error.c_str());
			ImGui::PopTextWrapPos();
		}
		ImGui::Separator();

		ImGui::AlignItemHorizontalCenter(buttonSize.x);
		if (ImGui::Button((L("Restart in 4 GB mode") + "##largeaddresspopup").c_str(), buttonSize))
		{
			// Does not return when it works: the game closes and the updater starts it again.
			LaaRedirect::AcceptAndRestart(m_error);
		}

		ImGui::AlignItemHorizontalCenter(buttonSize.x);
		if (ImGui::Button((L("Not now") + "##largeaddresspopup").c_str(), buttonSize))
		{
			ImGui::CloseCurrentPopup();
			Close();
		}

		ImGui::AlignItemHorizontalCenter(buttonSize.x);
		if (ImGui::Button((L("No, don't ask again") + "##largeaddresspopup").c_str(), buttonSize))
		{
			Settings::changeSetting("LargeAddressSpace", "0");
			Settings::settingsIni.largeAddressSpace = 0;
			ImGui::CloseCurrentPopup();
			Close();
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
}
