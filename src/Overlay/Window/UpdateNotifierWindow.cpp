#include "UpdateNotifierWindow.h"

#include "Overlay/WindowManager.h"
#include "Overlay/Widget/UpdateProgressWidget.h"
#include "Overlay/WindowContainer/WindowContainer.h"

#include "Core/info.h"
#include "Core/Localization.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Widget/MarkdownRenderer.h"
#include "Web/update_check.h"
#include "Updater/UpdateCoordinator.h"

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <shellapi.h>
#include <sstream>
#include <string>
#include <vector>

namespace
{

	std::string FormatGitHubDate(const std::string& value)
	{
		int year = 0;
		int month = 0;
		int day = 0;
		int hour = 0;
		int minute = 0;
		if (std::sscanf(value.c_str(), "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) != 5)
			return value;

		static const char* months[] = {
			"Jan", "Feb", "Mar", "Apr", "May", "Jun",
			"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
		};
		if (month < 1 || month > 12)
			return value;

		char buffer[64] = {};
		std::snprintf(buffer, sizeof(buffer), "%s %d, %d at %02d:%02d UTC", months[month - 1], day, year, hour, minute);
		return buffer;
	}


	void DrawReleaseNotes(const Updater::GitHubRelease& release)
	{
		const std::string title = release.name.empty() ? release.tagName : release.name;
		const ImVec4 titleColor = ImVec4(0.98f, 0.98f, 1.0f, 1.0f);
		const ImVec4 titleHoverColor = ImVec4(0.76f, 0.76f, 0.80f, 1.0f);

		ImGui::TextColoredAlignedHorizontalCenter(ImVec4(0.58f, 0.58f, 0.62f, 1.0f), release.tagName.c_str());

		// Bold, and sized via PushFont rather than the discouraged SetWindowFontScale, to match
		// how GitHub renders a release title (and the All Releases window).
		ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 1.18f);
		const ImVec2 textSize = ImGui::CalcTextSizeBold(title.c_str());
		ImGui::AlignItemHorizontalCenter(textSize.x);
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton(("##ReleaseLink" + release.tagName).c_str(), textSize);
		const bool hovered = ImGui::IsItemHovered();
		const bool clicked = ImGui::IsItemClicked();

		ImGui::AddTextBold(
			ImGui::GetWindowDrawList(),
			pos,
			ImGui::ColorConvertFloat4ToU32(hovered ? titleHoverColor : titleColor),
			title.c_str());
		if (hovered)
		{
			ImGui::GetWindowDrawList()->AddLine(
				ImVec2(pos.x, pos.y + textSize.y),
				ImVec2(pos.x + textSize.x, pos.y + textSize.y),
				ImGui::ColorConvertFloat4ToU32(titleHoverColor));
		}
		ImGui::PopFont();
		if (clicked && !release.htmlUrl.empty())
		{
			const std::wstring url(release.htmlUrl.begin(), release.htmlUrl.end());
			ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}

		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.58f, 0.62f, 1.0f));
		if (!release.publishedAt.empty())
			ImGui::TextAlignedHorizontalCenter("%s", FormatGitHubDate(release.publishedAt).c_str());
		ImGui::PopStyleColor();

		if (!release.body.empty())
		{
			ImGui::Spacing();
			ImGuiMarkdown::Render(release.body);
		}
	}

	// Both the opening size and the floor below which the window cannot be resized.
	const ImVec2 kWindowSize(760, 600);
}

void UpdateNotifierWindow::Update()
{
	if (!m_windowOpen)
		return;

	// Last in the exclusive order. An out-of-date install is exactly the install whose
	// settings.ini also predates the consent settings, so this prompt and a first-launch
	// prompt come up together - and before this check the two evicted each other from
	// ImGui's popup slot on every frame.
	if (WindowManager::GetInstance().GetWindowContainer()->ShouldDeferExclusivePopup(WindowType_UpdateNotifier))
		return;

	BeforeDraw();
	const char* popupTitle = L("Update available").c_str();
	ImGui::OpenPopup(popupTitle);
	if (ImGui::BeginPopupModal(popupTitle, nullptr, m_windowFlags))
	{
		Draw();
		ImGui::EndPopup();
	}
}

void UpdateNotifierWindow::BeforeDraw()
{
	const ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(kWindowSize, ImGuiCond_FirstUseEver);

	// FirstUseEver alone was not enough: the size is remembered in imgui.ini, so anyone who
	// once shrank this window - or inherited a small size from an older build - reopens it
	// too small to read the release notes, with no hint that it is meant to be bigger. A
	// floor makes that unreachable. Clamped to the display so it stays usable when the game
	// renders at a low resolution.
	const ImVec2 minSize(
		(std::min)(kWindowSize.x, io.DisplaySize.x * 0.9f),
		(std::min)(kWindowSize.y, io.DisplaySize.y * 0.9f));
	ImGui::SetNextWindowSizeConstraints(minSize, ImVec2(FLT_MAX, FLT_MAX));
}

void UpdateNotifierWindow::Draw()
{
	Updater::UpdateUiSnapshot update = Updater::UpdateCoordinator::GetInstance().GetSnapshot();
	const char* tag = update.tag.empty() ? GetNewVersionNum().c_str() : update.tag.c_str();
	const bool busy = UpdateProgressWidget::IsBusy(update);
	if (ImGui::IsWindowAppearing() || m_lastReleaseNotesTag != tag)
	{
		m_lastReleaseNotesTag = tag;
		m_resetReleaseNotesScroll = true;
	}

	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 1.0f, 1.0f));
	ImGui::TextAlignedHorizontalCenter(L("BBCF Improvement Mod %s is available").c_str(), tag);
	ImGui::PopStyleColor();
	ImGui::Spacing();

	if (update.developmentChannel)
		ImGui::TextColoredAlignedHorizontalCenter(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), L("Development update channel").c_str());

	ImGui::Spacing();
	ImGui::Separator();
	const ImVec2 buttonSize = ImVec2(150, 24);
	const float rowWidth = (buttonSize.x * 3.0f) + (ImGui::GetStyle().ItemSpacing.x * 2.0f);
	const float wrapWidth = ImGui::GetContentRegionAvail().x;
	float bottomReserve = ImGui::GetStyle().ItemSpacing.y + buttonSize.y + ImGui::GetStyle().ItemSpacing.y;
	bottomReserve += UpdateProgressWidget::EstimateHeight(update, wrapWidth, true);
	bottomReserve += ImGui::GetStyle().ItemSpacing.y;

	if (!update.releaseNotes.empty())
	{
		ImGui::Spacing();
		ImGui::TextDisabled("%s", L("Release notes").c_str());
		ImGui::BeginChild("ReleaseNotes", ImVec2(0, -bottomReserve), true);
		if (m_resetReleaseNotesScroll)
			ImGui::SetScrollY(0.0f);
		for (size_t i = 0; i < update.releaseNotes.size(); ++i)
		{
			DrawReleaseNotes(update.releaseNotes[i]);
			if (i + 1 < update.releaseNotes.size())
			{
				ImGui::Spacing();
				ImGui::Separator();
				ImGui::Spacing();
			}
		}
		ImGui::EndChild();
	}
	else if (!update.body.empty())
	{
		ImGui::Spacing();
		ImGui::TextDisabled("%s", L("Release notes").c_str());
		ImGui::BeginChild("ReleaseNotes", ImVec2(0, -bottomReserve), true);
		if (m_resetReleaseNotesScroll)
			ImGui::SetScrollY(0.0f);
		ImGuiMarkdown::Render(update.body);
		ImGui::EndChild();
	}
	m_resetReleaseNotesScroll = false;

	UpdateProgressWidget::Draw(update, true);

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 7));

	ImGui::AlignItemHorizontalCenter(rowWidth);
	if (update.autoApplySupported)
	{
		if (!busy && ImGui::Button(L("Update").c_str(), buttonSize))
			Updater::UpdateCoordinator::GetInstance().StartUpdate();
		else if (busy)
			ImGui::Button(L("Update").c_str(), buttonSize);
	}
	else if (ImGui::ButtonUrl(L("Open release page"), GetNewVersionReleaseUrl(), buttonSize))
	{
		ImGui::CloseCurrentPopup();
		Close();
	}

	ImGui::SameLine();
	if (!busy && ImGui::Button(L("Later").c_str(), buttonSize))
	{
		ImGui::CloseCurrentPopup();
		Close();
	}
	else if (busy)
		ImGui::Button(L("Later").c_str(), buttonSize);

	ImGui::SameLine();
	if (!busy && ImGui::Button(L("Skip this version").c_str(), buttonSize))
	{
		Updater::UpdateCoordinator::GetInstance().SkipCurrentVersion();
		ImGui::CloseCurrentPopup();
		Close();
	}
	else if (busy)
		ImGui::Button(L("Skip this version").c_str(), buttonSize);

	ImGui::PopStyleVar();

	ImGui::Spacing();
}
