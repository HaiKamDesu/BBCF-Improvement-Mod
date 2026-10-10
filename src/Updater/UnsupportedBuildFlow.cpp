#include "UnsupportedBuildFlow.h"

#include "FileUtil.h"
#include "GitHubReleaseClient.h"
#include "PackageStager.h"
#include "UpdateStateStore.h"
#include "Core/info.h"
#include "Core/Localization.h"
#include "Core/RuntimePlatform.h"
#include "Core/SupportedGameBuild.h"
#include "Core/logger.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace Updater
{
	namespace
	{
		typedef HRESULT(WINAPI* TaskDialogIndirectFn)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

		constexpr int kButtonUpdate = 1001;
		constexpr int kButtonLater = 1002;
		constexpr int kButtonSkip = 1003;
		constexpr size_t kMaxReleaseNotesChars = 1500;

		const wchar_t* const kWindowTitle = L"BBCF Improvement Mod";

		std::wstring W(const std::string& text)
		{
			return Utf8ToWide(text);
		}

		// Takes the format by value: va_start cannot follow a reference parameter.
		std::string Format(std::string format, ...)
		{
			char buffer[2048] = {};
			va_list args;
			va_start(args, format);
			vsnprintf(buffer, sizeof(buffer), format.c_str(), args);
			va_end(args);
			return buffer;
		}

		std::wstring SettingsIniPath()
		{
			return CombinePath(GetInstallRoot(), L"settings.ini");
		}

		int ReadSettingInt(const wchar_t* key, int fallback)
		{
			return static_cast<int>(GetPrivateProfileIntW(L"Settings", key, fallback, SettingsIniPath().c_str()));
		}

		std::string ReadSettingString(const wchar_t* key, const wchar_t* fallback)
		{
			wchar_t buffer[64] = {};
			GetPrivateProfileStringW(L"Settings", key, fallback, buffer, ARRAYSIZE(buffer), SettingsIniPath().c_str());
			return WideToUtf8(buffer);
		}

		std::wstring LinkMarkup(const std::string& url, const std::string& label)
		{
			return L"<a href=\"" + W(url) + L"\">" + W(label) + L"</a>";
		}

		std::string ReleasesPageUrl()
		{
			return WideToUtf8(GetGitHubReleasesPageUrl());
		}

		// TaskDialog lives in comctl32 v6, which a process only gets through an activation
		// context; BBCF.exe's manifest does not ask for one. shell32.dll carries a manifest
		// (resource 124) that does, which is the usual way for a DLL to borrow it.
		class TaskDialogApi
		{
		public:
			TaskDialogApi()
			{
				ACTCTXW actCtx = {};
				actCtx.cbSize = sizeof(actCtx);
				actCtx.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
				actCtx.hModule = LoadLibraryW(L"shell32.dll");
				actCtx.lpResourceName = MAKEINTRESOURCEW(124);
				if (actCtx.hModule)
				{
					m_context = CreateActCtxW(&actCtx);
					if (m_context != INVALID_HANDLE_VALUE && !ActivateActCtx(m_context, &m_cookie))
					{
						ReleaseActCtx(m_context);
						m_context = INVALID_HANDLE_VALUE;
					}
				}

				HMODULE comctl = LoadLibraryW(L"comctl32.dll");
				if (comctl)
					m_taskDialogIndirect = reinterpret_cast<TaskDialogIndirectFn>(GetProcAddress(comctl, "TaskDialogIndirect"));
			}

			~TaskDialogApi()
			{
				if (m_context != INVALID_HANDLE_VALUE)
				{
					DeactivateActCtx(0, m_cookie);
					ReleaseActCtx(m_context);
				}
			}

			bool Available() const { return m_taskDialogIndirect != nullptr; }

			HRESULT Show(const TASKDIALOGCONFIG& config, int* button, BOOL* verificationChecked) const
			{
				return m_taskDialogIndirect(&config, button, nullptr, verificationChecked);
			}

		private:
			HANDLE m_context = INVALID_HANDLE_VALUE;
			ULONG_PTR m_cookie = 0;
			TaskDialogIndirectFn m_taskDialogIndirect = nullptr;
		};

		// The game window is created while this thread is waiting on GitHub, and it takes the
		// foreground, so a dialog shown afterwards would open behind it.
		void BringToFront(HWND hwnd)
		{
			SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
			SetForegroundWindow(hwnd);
		}

		HRESULT CALLBACK BasicCallback(HWND hwnd, UINT msg, WPARAM, LPARAM lParam, LONG_PTR)
		{
			if (msg == TDN_CREATED)
				BringToFront(hwnd);
			else if (msg == TDN_HYPERLINK_CLICKED)
				ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lParam), nullptr, nullptr, SW_SHOWNORMAL);
			return S_OK;
		}

		// ---- "the mod is off for this game version" ----

		void ShowModOffNotice(const TaskDialogApi& api, const char* gameBuildId, const std::string& reason)
		{
			UpdateStateStore store(CombinePath(GetInstallRoot(), L"BBCF_IM\\Updater\\state.ini"));
			UpdateState state;
			store.Load(state);
			if (state.dismissedUnsupportedGameBuild == gameBuildId)
			{
				ForceLog("[BuildGuard] Mod-off notice not shown: dismissed for this game build.\n");
				return;
			}

			const std::wstring heading = W(L("The mod is off for this version of the game"));
			std::wstring content = W(Format(L("BlazBlue Centralfiction has been updated, and this version of the BBCF Improvement Mod (%s) does not support the new game version yet. The mod has turned itself off, so the game runs without it. Nothing in the game has been changed."), MOD_VERSION));
			content += L"\n\n" + W(reason);
			const std::wstring footer = LinkMarkup(ReleasesPageUrl(), L("Open the mod's releases on GitHub"));
			const std::wstring checkbox = W(L("Don't show this again for this game version"));

			BOOL dismissed = FALSE;
			if (api.Available())
			{
				TASKDIALOGCONFIG config = {};
				config.cbSize = sizeof(config);
				config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
				config.dwCommonButtons = TDCBF_OK_BUTTON;
				config.pszWindowTitle = kWindowTitle;
				config.pszMainIcon = TD_WARNING_ICON;
				config.pszMainInstruction = heading.c_str();
				config.pszContent = content.c_str();
				config.pszFooter = footer.c_str();
				config.pszVerificationText = checkbox.c_str();
				config.pfCallback = BasicCallback;
				int button = 0;
				api.Show(config, &button, &dismissed);
			}
			else
			{
				const std::wstring text = content + L"\n\n" + W(L("Press Cancel to stop showing this for this game version."));
				dismissed = MessageBoxW(nullptr, text.c_str(), kWindowTitle,
					MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND) == IDCANCEL;
			}

			if (dismissed)
			{
				state.dismissedUnsupportedGameBuild = gameBuildId;
				store.Save(state);
				ForceLog("[BuildGuard] Player turned off the mod-off notice for game build %s.\n", gameBuildId);
			}
		}

		// ---- the offer ----

		enum OfferChoice
		{
			OfferChoice_Later,
			OfferChoice_Update,
			OfferChoice_Skip
		};

		std::string TrimmedReleaseNotes(const GitHubRelease& release)
		{
			std::string notes = release.body;
			if (notes.size() > kMaxReleaseNotesChars)
			{
				// Do not cut a UTF-8 sequence in half.
				size_t cut = kMaxReleaseNotesChars;
				while (cut > 0 && (static_cast<unsigned char>(notes[cut]) & 0xC0) == 0x80)
					--cut;
				notes = notes.substr(0, cut) + "\n...\n\n" + L("The full release notes are on GitHub.");
			}
			return notes;
		}

		OfferChoice ShowOffer(const TaskDialogApi& api, const AvailableUpdate& update)
		{
			const std::string& tag = update.release.tagName;
			const std::wstring heading = W(L("A mod update for the new game version is available"));
			const std::wstring content = W(Format(L("BlazBlue Centralfiction has been updated, and this version of the BBCF Improvement Mod (%s) does not support it, so the mod is off for now. %s supports the new game version."), MOD_VERSION, tag.c_str()));

			if (!api.Available())
			{
				const std::wstring text = content + L"\n\n" + W(Format(L("Yes: update to %s now (the game closes and restarts)\nNo: ask me again next time\nCancel: skip this version"), tag.c_str()));
				switch (MessageBoxW(nullptr, text.c_str(), kWindowTitle, MB_YESNOCANCEL | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND))
				{
				case IDYES: return OfferChoice_Update;
				case IDCANCEL: return OfferChoice_Skip;
				default: return OfferChoice_Later;
				}
			}

			const std::wstring updateText = W(L("Update now")) + L"\n" +
				W(Format(L("Downloads %s, closes the game, installs the update and starts the game again."), tag.c_str()));
			const std::wstring laterText = W(L("Later")) + L"\n" +
				W(L("Play without the mod this time. You will be asked again the next time you start the game."));
			const std::wstring skipText = W(L("Skip this version")) + L"\n" +
				W(Format(L("Do not offer %s again. Newer releases will still be offered."), tag.c_str()));
			const TASKDIALOG_BUTTON buttons[] = {
				{ kButtonUpdate, updateText.c_str() },
				{ kButtonLater, laterText.c_str() },
				{ kButtonSkip, skipText.c_str() },
			};

			const std::wstring notes = W(TrimmedReleaseNotes(update.release));
			const std::wstring showNotes = W(L("Show release notes"));
			const std::wstring hideNotes = W(L("Hide release notes"));
			const std::wstring footer = LinkMarkup(update.release.htmlUrl.empty() ? ReleasesPageUrl() : update.release.htmlUrl,
				L("Open the release on GitHub"));

			TASKDIALOGCONFIG config = {};
			config.cbSize = sizeof(config);
			config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
			config.pszWindowTitle = kWindowTitle;
			config.pszMainIcon = TD_INFORMATION_ICON;
			config.pszMainInstruction = heading.c_str();
			config.pszContent = content.c_str();
			config.pButtons = buttons;
			config.cButtons = ARRAYSIZE(buttons);
			config.nDefaultButton = kButtonUpdate;
			if (!notes.empty())
			{
				config.pszExpandedInformation = notes.c_str();
				config.pszCollapsedControlText = showNotes.c_str();
				config.pszExpandedControlText = hideNotes.c_str();
			}
			config.pszFooter = footer.c_str();
			config.pfCallback = BasicCallback;

			int button = 0;
			if (FAILED(api.Show(config, &button, nullptr)))
				return OfferChoice_Later;
			if (button == kButtonUpdate)
				return OfferChoice_Update;
			if (button == kButtonSkip)
				return OfferChoice_Skip;
			return OfferChoice_Later;
		}

		// ---- the update itself ----

		struct InstallJob
		{
			AvailableUpdate update;
			volatile long progressPercent = 0;
			std::atomic<int> phase{ -1 };
			std::atomic<bool> finished{ false };
			bool succeeded = false;
			std::wstring handoff;
			std::string error;
			HWND dialog = nullptr;
		};

		DWORD WINAPI InstallThread(LPVOID param)
		{
			InstallJob& job = *static_cast<InstallJob*>(param);
			const std::string userAgent = std::string("BBCFIM/") + MOD_VERSION;
			job.succeeded = DownloadAndStageUpdate(job.update, userAgent, &job.progressPercent,
				[&job](StagePhase phase) { job.phase = static_cast<int>(phase); },
				job.handoff, job.error);
			job.finished = true;
			return 0;
		}

		std::wstring InstallStatusText(const InstallJob& job)
		{
			switch (job.phase.load())
			{
			case StagePhase_Verifying: return W(L("Checking the download..."));
			case StagePhase_Staging: return W(L("Preparing the update..."));
			default: return W(Format(L("Downloading... %d%%"), static_cast<int>(job.progressPercent)));
			}
		}

		HRESULT CALLBACK ProgressCallback(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR data)
		{
			InstallJob& job = *reinterpret_cast<InstallJob*>(data);
			switch (msg)
			{
			case TDN_CREATED:
				job.dialog = hwnd;
				BringToFront(hwnd);
				// Nothing can stop a download half way; the button only exists because a task
				// dialog needs one, and closes the dialog once the job is done.
				SendMessageW(hwnd, TDM_ENABLE_BUTTON, IDCANCEL, FALSE);
				SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 100));
				break;
			case TDN_TIMER:
			{
				SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, static_cast<int>(job.progressPercent), 0);
				const std::wstring status = InstallStatusText(job);
				SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(status.c_str()));
				if (job.finished)
				{
					SendMessageW(hwnd, TDM_ENABLE_BUTTON, IDCANCEL, TRUE);
					PostMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
				}
				break;
			}
			}
			return S_OK;
		}

		void ShowInstallError(const TaskDialogApi& api, const AvailableUpdate& update, const std::string& error)
		{
			const std::wstring heading = W(L("The update could not be installed"));
			const std::wstring content = W(error) + L"\n\n" +
				W(L("The game keeps running without the mod. You will be asked again the next time you start the game, or you can download the release from GitHub."));
			const std::wstring footer = LinkMarkup(update.release.htmlUrl.empty() ? ReleasesPageUrl() : update.release.htmlUrl,
				L("Open the release on GitHub"));

			if (!api.Available())
			{
				MessageBoxW(nullptr, content.c_str(), kWindowTitle, MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
				return;
			}

			TASKDIALOGCONFIG config = {};
			config.cbSize = sizeof(config);
			config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
			config.dwCommonButtons = TDCBF_OK_BUTTON;
			config.pszWindowTitle = kWindowTitle;
			config.pszMainIcon = TD_ERROR_ICON;
			config.pszMainInstruction = heading.c_str();
			config.pszContent = content.c_str();
			config.pszFooter = footer.c_str();
			config.pfCallback = BasicCallback;
			int button = 0;
			api.Show(config, &button, nullptr);
		}

		void Install(const TaskDialogApi& api, const AvailableUpdate& update)
		{
			InstallJob job;
			job.update = update;
			HANDLE thread = CreateThread(nullptr, 0, InstallThread, &job, 0, nullptr);
			if (!thread)
			{
				ShowInstallError(api, update, "Could not start the download.");
				return;
			}

			if (api.Available())
			{
				const std::wstring heading = W(Format(L("Updating to %s"), update.release.tagName.c_str()));
				const std::wstring status = InstallStatusText(job);
				TASKDIALOGCONFIG config = {};
				config.cbSize = sizeof(config);
				config.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_POSITION_RELATIVE_TO_WINDOW;
				config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
				config.pszWindowTitle = kWindowTitle;
				config.pszMainInstruction = heading.c_str();
				config.pszContent = status.c_str();
				config.pfCallback = ProgressCallback;
				config.lpCallbackData = reinterpret_cast<LONG_PTR>(&job);
				int button = 0;
				api.Show(config, &button, nullptr);
			}

			// The dialog only closes once the job has finished; without one, this is the wait.
			WaitForSingleObject(thread, INFINITE);
			CloseHandle(thread);

			if (!job.succeeded)
			{
				ForceLog("[BuildGuard] Update to %s failed: %s\n", update.release.tagName.c_str(), job.error.c_str());
				ShowInstallError(api, update, job.error);
				return;
			}

			ForceLog("[BuildGuard] %s staged; handing over to the updater, which closes and restarts the game.\n",
				update.release.tagName.c_str());
			closeLogger();
			std::string error;
			if (!LaunchUpdaterAndExitGame(job.handoff, error))
			{
				openLogger();
				ForceLog("[BuildGuard] Could not start the updater: %s\n", error.c_str());
				ShowInstallError(api, update, error);
			}
		}
	}

	void RunUnsupportedBuildFlow(const char* gameBuildId)
	{
		// The init thread never runs on this build, so the log is not open. Keep one, so a "the
		// mod stopped working" report still says why.
		openLogger();
		ForceLog("[BuildGuard] BBCF_IM %s supports BBCF.exe %s (Steam build %s) only; this exe is %s. "
			"Mod off for this session; the game runs unmodified.\n",
			MOD_VERSION_NUM, BBCF_SUPPORTED_GAME_BUILD_ID, BBCF_SUPPORTED_GAME_STEAM_BUILD, gameBuildId);

		Localization::Initialize(ReadSettingString(L"Language", L"en"));
		TaskDialogApi api;

		std::string reason;
		if (MOD_FORCE_DISABLE_UPDATE_CHECK || ReadSettingInt(L"CheckUpdates", 1) == 0)
		{
			reason = L("Update checks are turned off (CheckUpdates in settings.ini), so the mod cannot look for a release that supports this game version. Check the mod's releases on GitHub.");
		}
		else if (IsWineOrProton())
		{
			reason = L("Automatic updates are not available under Wine/Proton. Check the mod's releases on GitHub for one that supports this game version.");
		}
		else
		{
			SemVersion currentVersion;
			TryParseSemVersion(MOD_VERSION, currentVersion);
			const bool developmentChannel = IsDebuggerPresent() || ReadSettingInt(L"EnableInDevelopmentFeatures", 0) != 0;

			GitHubReleaseClient client;
			const UpdateCheckResult result = client.CheckForUpdates(currentVersion, developmentChannel);
			if (result.status == UpdateCheckStatus_UpdateAvailable &&
				ManifestSupportsGameBuild(result.update.manifest, gameBuildId))
			{
				const AvailableUpdate& update = result.update;
				UpdateStateStore store(CombinePath(GetInstallRoot(), L"BBCF_IM\\Updater\\state.ini"));
				UpdateState state;
				store.Load(state);
				if (state.skippedReleaseTag == update.release.tagName &&
					state.skippedReleaseVersion == update.manifest.version)
				{
					ForceLog("[BuildGuard] %s supports this game build but the player skipped it.\n", update.release.tagName.c_str());
					closeLogger();
					return;
				}

				ForceLog("[BuildGuard] %s supports this game build; offering it.\n", update.release.tagName.c_str());
				const OfferChoice choice = update.autoApplySupported ? ShowOffer(api, update) : OfferChoice_Later;
				if (choice == OfferChoice_Update)
				{
					Install(api, update);
				}
				else if (choice == OfferChoice_Skip)
				{
					state.skippedReleaseTag = update.release.tagName;
					state.skippedReleaseVersion = update.manifest.version;
					store.Save(state);
					ForceLog("[BuildGuard] Player skipped %s.\n", update.release.tagName.c_str());
				}
				else
				{
					ForceLog("[BuildGuard] Player chose Later.\n");
				}
				closeLogger();
				return;
			}

			if (result.status == UpdateCheckStatus_UpdateAvailable)
			{
				ForceLog("[BuildGuard] Newest release %s does not support this game build either.\n",
					result.update.release.tagName.c_str());
			}
			else if (result.status != UpdateCheckStatus_NoUpdate)
			{
				ForceLog("[BuildGuard] Update check failed (status %d): %s\n", result.status, result.message.c_str());
			}
			reason = L("When a mod release that supports this game version comes out, the mod will offer to install it here the next time you start the game.");
		}

		ShowModOffNotice(api, gameBuildId, reason);
		closeLogger();
	}
}
