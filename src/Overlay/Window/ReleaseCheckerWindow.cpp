#include "ReleaseCheckerWindow.h"

#include "Core/info.h"
#include "Core/RuntimePlatform.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Widget/MarkdownRenderer.h"
#include "imgui_internal.h"
#include "Updater/GitHubReleaseClient.h"
#include "Updater/SemVersion.h"
#include "Updater/UpdateCoordinator.h"

#include <algorithm>
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
		int year = 0, month = 0, day = 0, hour = 0, minute = 0;
		if (std::sscanf(value.c_str(), "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) != 5)
			return value;

		static const char* months[] = {
			"Jan", "Feb", "Mar", "Apr", "May", "Jun",
			"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
		};
		if (month < 1 || month > 12)
			return value;

		char buffer[64] = {};
		std::snprintf(buffer, sizeof(buffer), "%s %d, %d at %02d:%02d UTC",
			months[month - 1], day, year, hour, minute);
		return buffer;
	}


	bool HasManifestAsset(const Updater::GitHubRelease& release)
	{
		for (size_t i = 0; i < release.assets.size(); ++i)
			if (release.assets[i].name == "update-manifest.json")
				return true;
		return false;
	}

	bool IsBusyState(Updater::UpdateUiState state)
	{
		return state == Updater::UpdateUiState_Downloading ||
			state == Updater::UpdateUiState_Verifying ||
			state == Updater::UpdateUiState_Staging ||
			state == Updater::UpdateUiState_LaunchingUpdater;
	}
} // namespace

ReleaseCheckerWindow::ReleaseCheckerWindow(const std::string& windowTitle, bool windowClosable,
	ImGuiWindowFlags windowFlags)
	: IWindow(windowTitle, windowClosable, windowFlags)
{
	InitializeCriticalSection(&m_lock);
}

ReleaseCheckerWindow::~ReleaseCheckerWindow()
{
	DeleteCriticalSection(&m_lock);
}

void ReleaseCheckerWindow::Update()
{
	if (!m_windowOpen)
		return;

	BeforeDraw();
	const char* popupTitle = "BBCF IM - All Releases##checkermodal";
	ImGui::OpenPopup(popupTitle);
	if (ImGui::BeginPopupModal(popupTitle, &m_windowOpen, m_windowFlags))
	{
		Draw();
		ImGui::EndPopup();
	}
}

void ReleaseCheckerWindow::BeforeDraw()
{
	const ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(
		ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
		ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(760, 620), ImGuiCond_FirstUseEver);
}

DWORD WINAPI ReleaseCheckerWindow::FetchThreadProc(LPVOID param)
{
	static_cast<ReleaseCheckerWindow*>(param)->FetchThread();
	return 0;
}

void ReleaseCheckerWindow::FetchThread()
{
	Updater::GitHubReleaseClient client;
	std::vector<Updater::GitHubRelease> releases;
	std::string error;
	EnterCriticalSection(&m_lock);
	const bool forceRefresh = m_forceRefresh;
	LeaveCriticalSection(&m_lock);

	const bool ok = client.FetchAllReleases(releases, error, forceRefresh);

	EnterCriticalSection(&m_lock);
	if (ok)
	{
		m_releases = releases;
		m_fetchState = FetchState::Loaded;
	}
	else
	{
		m_fetchError = error;
		m_fetchState = FetchState::Error;
	}
	LeaveCriticalSection(&m_lock);
}

void ReleaseCheckerWindow::StartFetch(bool forceRefresh)
{
	EnterCriticalSection(&m_lock);
	m_forceRefresh = forceRefresh;
	m_fetchState = FetchState::Fetching;
	m_releases.clear();
	m_fetchError.clear();
	m_fetchStarted = true;
	LeaveCriticalSection(&m_lock);
	CloseHandle(CreateThread(nullptr, 0, FetchThreadProc, this, 0, nullptr));
}

void ReleaseCheckerWindow::Draw()
{
	// Auto-start fetch on first open
	{
		EnterCriticalSection(&m_lock);
		const bool needsFetch = !m_fetchStarted;
		LeaveCriticalSection(&m_lock);
		if (needsFetch)
			StartFetch();
	}

	// Snapshot fetch state under lock
	FetchState fetchState;
	std::vector<Updater::GitHubRelease> releases;
	std::string fetchError;
	{
		EnterCriticalSection(&m_lock);
		fetchState = m_fetchState;
		releases = m_releases;
		fetchError = m_fetchError;
		LeaveCriticalSection(&m_lock);
	}

	// Coordinator snapshot for install progress / busy state
	Updater::UpdateUiSnapshot coordSnap = Updater::UpdateCoordinator::GetInstance().GetSnapshot();
	const bool coordinatorBusy = IsBusyState(coordSnap.state);

	// Header
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 1.0f, 1.0f));
	ImGui::TextAlignedHorizontalCenter("BBCF Improvement Mod - All Releases");
	ImGui::PopStyleColor();
	ImGui::Spacing();

	// Status line
	if (fetchState == FetchState::Fetching)
	{
		ImGui::TextColoredAlignedHorizontalCenter(
			ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "Fetching releases from GitHub...");
	}
	else if (fetchState == FetchState::Loaded)
	{
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%d release%s found",
			static_cast<int>(releases.size()), releases.size() == 1 ? "" : "s");
		ImGui::TextColoredAlignedHorizontalCenter(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), buf);
	}
	else if (fetchState == FetchState::Error)
	{
		const std::string errLine = "Error: " + fetchError;
		ImGui::TextColoredAlignedHorizontalCenter(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), errLine.c_str());
	}
	else
	{
		ImGui::Spacing();
	}

	ImGui::Spacing();
	ImGui::Separator();

	// Calculate bottom reserve height
	const ImVec2 buttonSize = ImVec2(110, 24);
	float bottomReserve = ImGui::GetStyle().ItemSpacing.y * 3.0f + buttonSize.y;
	if (coordinatorBusy)
	{
		if (!coordSnap.statusText.empty())
			bottomReserve += ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeightWithSpacing();
		bottomReserve += ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeight();
	}
	if (!coordSnap.errorText.empty())
		bottomReserve += ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeightWithSpacing();

	// Scrollable release list
	ImGui::Spacing();
	ImGui::BeginChild("##releases_list", ImVec2(0, -bottomReserve), true);

	if (fetchState == FetchState::Fetching)
	{
		ImGui::Spacing();
		ImGui::TextColoredAlignedHorizontalCenter(
			ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "Fetching releases from GitHub...");
	}
	else if (fetchState == FetchState::Error)
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
		ImGui::TextWrapped("Failed to fetch releases: %s", fetchError.c_str());
		ImGui::PopStyleColor();
		ImGui::Spacing();
		ImGui::TextDisabled("Check your internet connection and click Refresh.");
	}
	else if (fetchState == FetchState::Loaded)
	{
		if (releases.empty())
		{
			ImGui::Spacing();
			ImGui::TextColoredAlignedHorizontalCenter(
				ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "No releases found.");
		}
		else
		{
			Updater::SemVersion currentVersion;
			Updater::TryParseSemVersion(MOD_VERSION, currentVersion);

			for (size_t i = 0; i < releases.size(); ++i)
			{
				const Updater::GitHubRelease& release = releases[i];
				Updater::SemVersion releaseVersion;
				const bool canParse = Updater::TryParseSemVersion(release.tagName, releaseVersion);
				const int versionCmp = canParse
					? Updater::CompareSemVersion(releaseVersion, currentVersion)
					: -2;
				const bool isCurrent = canParse && (versionCmp == 0);

				DrawRelease(release, i, isCurrent, versionCmp, coordinatorBusy);

				if (i + 1 < releases.size())
				{
					ImGui::Spacing();
					ImGui::Separator();
					ImGui::Spacing();
				}
			}
		}
	}
	// FetchState::Idle: nothing shown yet

	ImGui::EndChild();

	// Install progress (when coordinator is busy)
	if (coordinatorBusy)
	{
		if (!coordSnap.statusText.empty())
		{
			ImGui::Spacing();
			ImGui::TextWrapped("%s", coordSnap.statusText.c_str());
		}
		ImGui::ProgressBar(coordSnap.progressPercent / 100.0f, ImVec2(-1, 0));
	}
	if (!coordSnap.errorText.empty())
	{
		ImGui::Spacing();
		ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", coordSnap.errorText.c_str());
	}

	// Manual install nested popup
	if (m_openManualPopup)
	{
		ImGui::OpenPopup("Not Compatible##manualinstall");
		m_openManualPopup = false;
	}
	if (ImGui::BeginPopupModal("Not Compatible##manualinstall", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextWrapped("Release %s does not include an auto-updater manifest\n"
			"and must be installed manually.", m_manualPopupTag.c_str());
		ImGui::Spacing();
		if (ImGui::Button("Go to GitHub Release", ImVec2(180, 0)))
		{
			if (!m_manualPopupUrl.empty())
			{
				const std::wstring url(m_manualPopupUrl.begin(), m_manualPopupUrl.end());
				ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(80, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// Footer buttons
	ImGui::Spacing();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 7));

	const bool isFetching = (fetchState == FetchState::Fetching);
	if (isFetching)
	{
		ImGui::BeginDisabled();
	}
	if (ImGui::Button("Refresh", buttonSize))
		StartFetch(true);
	if (isFetching)
	{
		ImGui::EndDisabled();
	}

	ImGui::SameLine();

	// Right-align Close button
	const float availX = ImGui::GetContentRegionAvail().x;
	const float closeOffset = availX - buttonSize.x;
	if (closeOffset > 0.0f)
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + closeOffset);

	if (ImGui::Button("Close", buttonSize))
	{
		ImGui::CloseCurrentPopup();
		Close();
	}

	ImGui::PopStyleVar();
}

void ReleaseCheckerWindow::DrawRelease(const Updater::GitHubRelease& release, size_t idx,
	bool isCurrent, int versionCmp, bool coordinatorBusy)
{
	const bool wine = IsWineOrProton();
	const bool hasManifest = HasManifestAsset(release);
	const bool autoUpdaterOk = hasManifest && !wine;

	// Status tag and header color
	ImVec4 headerColor;
	std::string statusTag;
	if (release.draft)
	{
		statusTag = "[DRAFT]";
		headerColor = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
	}
	else if (isCurrent)
	{
		statusTag = "[CURRENT]";
		headerColor = ImVec4(0.98f, 0.85f, 0.35f, 1.0f);
	}
	else if (versionCmp > 0)
	{
		statusTag = release.prerelease ? "[NEWER-PRE]" : "[NEWER]";
		headerColor = ImVec4(0.45f, 0.85f, 0.95f, 1.0f);
	}
	else if (versionCmp < 0 && versionCmp != -2)
	{
		statusTag = release.prerelease ? "[OLDER-PRE]" : "[OLDER]";
		headerColor = ImVec4(0.58f, 0.58f, 0.62f, 1.0f);
	}
	else
	{
		statusTag = "[UNKNOWN]";
		headerColor = ImVec4(0.58f, 0.58f, 0.62f, 1.0f);
	}

	const std::string& displayTitle = release.name.empty() ? release.tagName : release.name;
	const std::string headerLabel = statusTag + " " + release.tagName + " - " + displayTitle
		+ "###rel_" + release.tagName;

	ImGuiTreeNodeFlags flags = 0;
	if (isCurrent || (versionCmp > 0 && idx == 0))
		flags |= ImGuiTreeNodeFlags_DefaultOpen;

	ImGui::PushStyleColor(ImGuiCol_Text, headerColor);
	const bool expanded = ImGui::CollapsingHeader(headerLabel.c_str(), flags);
	ImGui::PopStyleColor();

	if (!expanded)
		return;

	ImGui::Indent(16.0f);
	ImGui::PushID(static_cast<int>(idx));

	// Tag (dimmed, centered)
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.58f, 0.62f, 1.0f));
	ImGui::TextAlignedHorizontalCenter("%s", release.tagName.c_str());
	ImGui::PopStyleColor();

	// Clickable title link
	{
		const ImVec4 titleColor = ImVec4(0.98f, 0.98f, 1.0f, 1.0f);
		const ImVec4 titleHoverColor = ImVec4(0.76f, 0.76f, 0.80f, 1.0f);

		// PushFont(NULL, size) rather than SetWindowFontScale: the latter is discouraged since
		// 1.92 and scales a baked bitmap, where this rasterizes at the real size.
		ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 1.12f);
		const ImVec2 titleSize = ImGui::CalcTextSizeBold(displayTitle.c_str());
		ImGui::AlignItemHorizontalCenter(titleSize.x);
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton(("##titlelink_" + release.tagName).c_str(), titleSize);
		const bool hovered = ImGui::IsItemHovered();
		const bool clicked = ImGui::IsItemClicked();
		ImGui::AddTextBold(
			ImGui::GetWindowDrawList(),
			pos,
			ImGui::ColorConvertFloat4ToU32(hovered ? titleHoverColor : titleColor),
			displayTitle.c_str());
		if (hovered)
		{
			ImGui::GetWindowDrawList()->AddLine(
				ImVec2(pos.x, pos.y + titleSize.y),
				ImVec2(pos.x + titleSize.x, pos.y + titleSize.y),
				ImGui::ColorConvertFloat4ToU32(titleHoverColor));
		}
		ImGui::PopFont();
		if (clicked && !release.htmlUrl.empty())
		{
			const std::wstring url(release.htmlUrl.begin(), release.htmlUrl.end());
			ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
	}

	// Date (dimmed, centered)
	if (!release.publishedAt.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.58f, 0.62f, 1.0f));
		ImGui::TextAlignedHorizontalCenter("%s", FormatGitHubDate(release.publishedAt).c_str());
		ImGui::PopStyleColor();
	}

	// Pre-release badge inline
	if (!release.draft && release.prerelease)
	{
		ImGui::TextColoredAlignedHorizontalCenter(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "[PRE-RELEASE]");
	}

	// Release notes body
	if (!release.body.empty())
	{
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		ImGuiMarkdown::Render(release.body);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Auto-updater compatibility badge
	if (wine)
	{
		ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f),
			"Auto-updates disabled under Wine/Proton.");
	}
	else if (autoUpdaterOk)
	{
		ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.35f, 1.0f),
			"Auto-updater compatible");
	}
	else
	{
		ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f),
			"No auto-updater manifest - manual install required");
	}

	ImGui::Spacing();

	// Action buttons
	const ImVec2 actionBtnSize = ImVec2(100, 22);

	if (autoUpdaterOk)
	{
		// Install button (disabled when coordinator is busy)
		if (coordinatorBusy)
		{
			ImGui::BeginDisabled();
		}
		if (ImGui::Button(("Install##" + release.tagName).c_str(), actionBtnSize))
		{
			Updater::UpdateCoordinator::GetInstance().StartInstallRelease(release);
			Updater::UpdateCoordinator::GetInstance().OpenPopup();
			ImGui::CloseCurrentPopup();
			Close();
		}
		if (coordinatorBusy)
		{
			ImGui::EndDisabled();
		}
	}
	else
	{
		// Manual install: open info popup
		if (ImGui::Button(("Install##" + release.tagName).c_str(), actionBtnSize))
		{
			m_manualPopupTag = release.tagName;
			m_manualPopupUrl = release.htmlUrl;
			m_openManualPopup = true;
		}
	}

	ImGui::SameLine();

	if (ImGui::Button(("GitHub##" + release.tagName).c_str(), actionBtnSize))
	{
		if (!release.htmlUrl.empty())
		{
			const std::wstring url(release.htmlUrl.begin(), release.htmlUrl.end());
			ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
	}

	ImGui::PopID();
	ImGui::Unindent(16.0f);
}
