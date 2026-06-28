#pragma once
#include "IWindow.h"
#include "Updater/UpdateModels.h"

#include <string>
#include <vector>
#include <Windows.h>

class ReleaseCheckerWindow : public IWindow
{
public:
	ReleaseCheckerWindow(const std::string& windowTitle, bool windowClosable,
		ImGuiWindowFlags windowFlags);
	~ReleaseCheckerWindow() override;
	void Update() override;

protected:
	void BeforeDraw() override;
	void Draw() override;

private:
	enum class FetchState { Idle, Fetching, Loaded, Error };

	void StartFetch();
	void DrawRelease(const Updater::GitHubRelease& release, size_t idx,
		bool isCurrent, int versionCmp, bool coordinatorBusy);

	static DWORD WINAPI FetchThreadProc(LPVOID param);
	void FetchThread();

	CRITICAL_SECTION m_lock;
	FetchState m_fetchState = FetchState::Idle;
	std::vector<Updater::GitHubRelease> m_releases;
	std::string m_fetchError;
	bool m_fetchStarted = false;

	bool m_openManualPopup = false;
	std::string m_manualPopupUrl;
	std::string m_manualPopupTag;
};
