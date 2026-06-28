#include "UpdateCoordinator.h"

#include "FileUtil.h"
#include "GitHubReleaseClient.h"
#include "PackageStager.h"
#include "UpdateStateStore.h"

#include "Core/info.h"
#include "Core/Localization.h"
#include "Core/RuntimePlatform.h"
#include "Core/Settings.h"
#include "Core/logger.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include "Overlay/WindowContainer/WindowType.h"
#include "Overlay/WindowManager.h"

#include <imgui.h>
#include <handleapi.h>
#include <processthreadsapi.h>
#include <cstdio>

namespace Updater
{
	namespace
	{
		std::wstring GetUpdaterRoot()
		{
			return CombinePath(GetInstallRoot(), L"BBCF_IM\\Updater");
		}

		std::wstring GetStatePath()
		{
			return CombinePath(GetUpdaterRoot(), L"state.ini");
		}

		std::string BuildUserAgent()
		{
			return std::string("BBCFIM/") + MOD_VERSION;
		}

		bool ShouldUseDevelopmentUpdateChannel()
		{
			return IsDebuggerPresent() || Settings::settingsIni.enableInDevelopmentFeatures;
		}
	}

	UpdateCoordinator& UpdateCoordinator::GetInstance()
	{
		static UpdateCoordinator instance;
		return instance;
	}

	UpdateCoordinator::UpdateCoordinator()
	{
		InitializeCriticalSection(&m_lock);
	}

	UpdateCoordinator::~UpdateCoordinator()
	{
		DeleteCriticalSection(&m_lock);
	}

	void UpdateCoordinator::StartAsyncCheck()
	{
		if (MOD_FORCE_DISABLE_UPDATE_CHECK || !Settings::settingsIni.checkupdates)
			return;

		if (IsWineOrProton())
		{
			EnterCriticalSection(&m_lock);
			m_snapshot.state = UpdateUiState_Idle;
			m_snapshot.statusText = "Update checks are disabled under Wine/Proton.";
			m_snapshot.autoApplySupported = false;
			m_snapshot.autoApplyDisabledReason = "Auto-update is disabled under Wine/Proton. Open GitHub releases manually.";
			LeaveCriticalSection(&m_lock);
			LOG(2, "Update check skipped under Wine/Proton.\n");
			return;
		}

		EnterCriticalSection(&m_lock);
		if (m_checkStarted)
		{
			LeaveCriticalSection(&m_lock);
			return;
		}
		m_checkStarted = true;
		m_snapshot.state = UpdateUiState_Checking;
		LeaveCriticalSection(&m_lock);

		CloseHandle(CreateThread(nullptr, 0, CheckThreadProc, this, 0, nullptr));
	}

	void UpdateCoordinator::OpenPopup()
	{
		WindowManager::GetInstance().GetWindowContainer()->GetWindow(WindowType_UpdateNotifier)->Open();
	}

	void UpdateCoordinator::SkipCurrentVersion()
	{
		EnterCriticalSection(&m_lock);
		if (!m_hasUpdate)
		{
			LeaveCriticalSection(&m_lock);
			return;
		}

		UpdateState state;
		UpdateStateStore store(GetStatePath());
		store.Load(state);
		state.skippedReleaseTag = m_update.release.tagName;
		state.skippedReleaseVersion = m_update.manifest.version;
		store.Save(state);

		m_snapshot.skipped = true;
		m_snapshot.state = UpdateUiState_Skipped;
		LeaveCriticalSection(&m_lock);
	}

	void UpdateCoordinator::StartUpdate()
	{
		EnterCriticalSection(&m_lock);
		if (!m_hasUpdate || m_applyStarted)
		{
			LeaveCriticalSection(&m_lock);
			return;
		}
		m_applyStarted = true;
		m_progressPercent = 0;
		m_snapshot.progressPercent = 0;
		m_snapshot.state = UpdateUiState_Downloading;
		m_snapshot.statusText = "Downloading update package...";
		m_snapshot.errorText.clear();
		LeaveCriticalSection(&m_lock);

		CloseHandle(CreateThread(nullptr, 0, ApplyThreadProc, this, 0, nullptr));
	}

	UpdateUiSnapshot UpdateCoordinator::GetSnapshot()
	{
		EnterCriticalSection(&m_lock);
		m_snapshot.progressPercent = static_cast<int>(m_progressPercent);
		UpdateUiSnapshot copy = m_snapshot;
		LeaveCriticalSection(&m_lock);
		return copy;
	}

	void UpdateCoordinator::DrawSkippedLink()
	{
	}

	void UpdateCoordinator::DrawSkippedMainMenuLink()
	{
		UpdateUiSnapshot snapshot = GetSnapshot();
		if (!snapshot.hasUpdate)
			return;

		ImGui::Spacing();

		char text[128] = {};
		const std::string format = L("Update to %s");
		std::snprintf(text, sizeof(text), format.c_str(), snapshot.tag.c_str());

		const ImVec4 linkColor = ImVec4(0.58f, 0.58f, 0.62f, 1.0f);
		const ImVec4 hoverColor = ImVec4(0.76f, 0.76f, 0.80f, 1.0f);

		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 textSize = ImGui::CalcTextSize(text);
		ImGui::InvisibleButton("##BBCFIMUpdateMainMenuLink", textSize);
		const bool hovered = ImGui::IsItemHovered();
		const bool clicked = ImGui::IsItemClicked();

		ImGui::GetWindowDrawList()->AddText(
			pos,
			ImGui::ColorConvertFloat4ToU32(hovered ? hoverColor : linkColor),
			text);
		if (hovered)
		{
			ImGui::GetWindowDrawList()->AddLine(
				ImVec2(pos.x, pos.y + textSize.y),
				ImVec2(pos.x + textSize.x, pos.y + textSize.y),
				ImGui::ColorConvertFloat4ToU32(hoverColor));
		}

		if (clicked)
		{
			EnterCriticalSection(&m_lock);
			m_snapshot.skipped = false;
			m_snapshot.state = UpdateUiState_Available;
			LeaveCriticalSection(&m_lock);
			OpenPopup();
		}
	}

	DWORD WINAPI UpdateCoordinator::CheckThreadProc(LPVOID param)
	{
		static_cast<UpdateCoordinator*>(param)->CheckThread();
		return 0;
	}

	DWORD WINAPI UpdateCoordinator::ApplyThreadProc(LPVOID param)
	{
		static_cast<UpdateCoordinator*>(param)->ApplyThread();
		return 0;
	}

	void UpdateCoordinator::CheckThread()
	{
		SemVersion currentVersion;
		if (!TryParseSemVersion(MOD_VERSION, currentVersion))
			return;

		const bool developmentChannel = ShouldUseDevelopmentUpdateChannel();
		GitHubReleaseClient client;
		UpdateCheckResult result = client.CheckForUpdates(currentVersion, developmentChannel);
		UpdateState state;
		UpdateStateStore store(GetStatePath());
		store.Load(state);
		state.lastCheckUtc = GetUtcTimestampForFileName();
		if (result.status != UpdateCheckStatus_UpdateAvailable)
		{
			if (result.status != UpdateCheckStatus_NoUpdate)
				state.lastFailureUtc = state.lastCheckUtc;
			store.Save(state);
			return;
		}

		state.lastSeenReleaseTag = result.update.release.tagName;
		state.lastSeenReleaseVersion = result.update.manifest.version;
		store.Save(state);

		EnterCriticalSection(&m_lock);
		const bool skipped = state.skippedReleaseTag == result.update.release.tagName &&
			state.skippedReleaseVersion == result.update.manifest.version;
		SetAvailableLocked(result.update, skipped);
		LeaveCriticalSection(&m_lock);

		if (!skipped)
			OpenPopup();
	}

	void UpdateCoordinator::ApplyThread()
	{
		AvailableUpdate update;
		EnterCriticalSection(&m_lock);
		update = m_update;
		LeaveCriticalSection(&m_lock);

		if (!update.autoApplySupported)
		{
			SetErrorLocked(update.autoApplyDisabledReason);
			return;
		}

		std::string error;
		const std::wstring updaterRoot = GetUpdaterRoot();
		const std::wstring downloadsRoot = CombinePath(updaterRoot, L"downloads");
		const std::wstring stageRoot = CombinePath(updaterRoot, L"stage");
		EnsureDirectoryRecursive(downloadsRoot);
		EnsureDirectoryRecursive(stageRoot);

		const std::wstring packagePath = CombinePath(downloadsRoot, Utf8ToWide(update.manifest.assetName));
		const std::wstring tempPath = packagePath + L".tmp";
		if (!DownloadFileWithProgress(update.packageAsset.browserDownloadUrl, tempPath, packagePath, BuildUserAgent(), &m_progressPercent, error))
		{
			SetErrorLocked(error);
			return;
		}

		EnterCriticalSection(&m_lock);
		m_snapshot.state = UpdateUiState_Verifying;
		m_snapshot.statusText = "Verifying SHA-256...";
		LeaveCriticalSection(&m_lock);

		std::string sha256;
		if (!ComputeFileSha256Hex(packagePath, sha256, error))
		{
			SetErrorLocked(error);
			return;
		}
		if (_stricmp(sha256.c_str(), update.manifest.sha256.c_str()) != 0)
		{
			SetErrorLocked("Downloaded package SHA-256 did not match update manifest.");
			return;
		}

		ZipValidationResult zip = ValidateUpdateZip(packagePath);
		if (!zip.valid)
		{
			SetErrorLocked(zip.error);
			return;
		}

		EnterCriticalSection(&m_lock);
		m_snapshot.state = UpdateUiState_Staging;
		m_snapshot.statusText = "Extracting staged update...";
		LeaveCriticalSection(&m_lock);

		if (!ExtractZipWithShell(packagePath, stageRoot, error))
		{
			SetErrorLocked(error);
			return;
		}

		std::wstring handoff;
		if (!WriteUpdaterHandoff(update, stageRoot, packagePath, handoff, error))
		{
			SetErrorLocked(error);
			return;
		}

		EnterCriticalSection(&m_lock);
		m_snapshot.state = UpdateUiState_LaunchingUpdater;
		m_snapshot.statusText = "Launching updater and closing BBCF...";
		LeaveCriticalSection(&m_lock);

		if (!LaunchUpdaterAndExitGame(handoff, error))
		{
			SetErrorLocked(error);
			return;
		}
	}

	void UpdateCoordinator::StartInstallRelease(const GitHubRelease& release)
	{
		EnterCriticalSection(&m_lock);
		if (m_applyStarted)
		{
			LeaveCriticalSection(&m_lock);
			return;
		}
		m_applyStarted = true;
		m_pendingInstallRelease = release;
		m_progressPercent = 0;
		m_snapshot.progressPercent = 0;
		m_snapshot.state = UpdateUiState_Downloading;
		m_snapshot.statusText = "Fetching update manifest...";
		m_snapshot.errorText.clear();
		m_snapshot.hasUpdate = true;
		m_hasUpdate = true;
		m_snapshot.tag = release.tagName;
		m_snapshot.name = release.name;
		m_snapshot.body = release.body;
		m_snapshot.publishedAt = release.publishedAt;
		m_snapshot.releaseUrl = release.htmlUrl;
		m_snapshot.releaseNotes.clear();
		m_snapshot.autoApplySupported = true;
		m_snapshot.autoApplyDisabledReason.clear();
		m_snapshot.developmentChannel = false;
		LeaveCriticalSection(&m_lock);

		CloseHandle(CreateThread(nullptr, 0, InstallReleaseThreadProc, this, 0, nullptr));
	}

	DWORD WINAPI UpdateCoordinator::InstallReleaseThreadProc(LPVOID param)
	{
		static_cast<UpdateCoordinator*>(param)->InstallReleaseThread();
		return 0;
	}

	void UpdateCoordinator::InstallReleaseThread()
	{
		EnterCriticalSection(&m_lock);
		GitHubRelease release = m_pendingInstallRelease;
		LeaveCriticalSection(&m_lock);

		const GitHubReleaseAsset* manifestAsset = nullptr;
		for (size_t i = 0; i < release.assets.size(); ++i)
		{
			if (release.assets[i].name == "update-manifest.json")
			{
				manifestAsset = &release.assets[i];
				break;
			}
		}
		if (!manifestAsset || manifestAsset->browserDownloadUrl.compare(0, 8, "https://") != 0)
		{
			SetErrorLocked("Release is missing a valid update-manifest.json.");
			return;
		}

		GitHubReleaseClient client;
		std::string manifestJson, error;
		const std::wstring manifestUrl(manifestAsset->browserDownloadUrl.begin(),
			manifestAsset->browserDownloadUrl.end());
		if (!client.FetchText(manifestUrl, manifestJson, error))
		{
			SetErrorLocked(error);
			return;
		}

		UpdateManifest manifest;
		if (!ParseUpdateManifestJson(manifestJson, manifest, error))
		{
			SetErrorLocked(error);
			return;
		}
		if (!ValidateUpdateManifest(manifest, error))
		{
			SetErrorLocked(error);
			return;
		}

		const GitHubReleaseAsset* packageAsset = nullptr;
		for (size_t i = 0; i < release.assets.size(); ++i)
		{
			if (release.assets[i].name == manifest.assetName)
			{
				packageAsset = &release.assets[i];
				break;
			}
		}
		if (!packageAsset || packageAsset->browserDownloadUrl.compare(0, 8, "https://") != 0)
		{
			SetErrorLocked("Release is missing the package asset referenced by its manifest.");
			return;
		}

		EnterCriticalSection(&m_lock);
		m_update.release = release;
		m_update.manifest = manifest;
		m_update.manifestAsset = *manifestAsset;
		m_update.packageAsset = *packageAsset;
		m_update.autoApplySupported = true;
		m_update.autoApplyDisabledReason.clear();
		m_snapshot.statusText = "Downloading update package...";
		m_snapshot.state = UpdateUiState_Downloading;
		LeaveCriticalSection(&m_lock);

		ApplyThread();
	}

	void UpdateCoordinator::SetErrorLocked(const std::string& error)
	{
		EnterCriticalSection(&m_lock);
		m_snapshot.state = UpdateUiState_Error;
		m_snapshot.errorText = error;
		m_snapshot.statusText = "Update failed.";
		m_applyStarted = false;
		LeaveCriticalSection(&m_lock);
	}

	void UpdateCoordinator::SetAvailableLocked(const AvailableUpdate& update, bool skipped)
	{
		m_update = update;
		m_hasUpdate = true;
		m_snapshot.hasUpdate = true;
		m_snapshot.skipped = skipped;
		m_snapshot.autoApplySupported = update.autoApplySupported;
		m_snapshot.autoApplyDisabledReason = update.autoApplyDisabledReason;
		m_snapshot.state = skipped ? UpdateUiState_Skipped : UpdateUiState_Available;
		m_snapshot.progressPercent = 0;
		m_snapshot.tag = update.release.tagName;
		m_snapshot.version = update.manifest.version;
		m_snapshot.name = update.release.name;
		m_snapshot.body = update.release.body;
		m_snapshot.publishedAt = update.release.publishedAt;
		m_snapshot.releaseUrl = update.release.htmlUrl;
		m_snapshot.releaseNotes = update.releaseNotes;
		m_snapshot.developmentChannel = ShouldUseDevelopmentUpdateChannel();
		m_snapshot.statusText.clear();
		m_snapshot.errorText.clear();
	}
}
