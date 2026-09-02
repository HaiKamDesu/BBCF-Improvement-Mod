#include "WindowManager.h"

#include "Branding.h"
#include "Palette/PaletteThumbnails.h"
#include "fonts.h"
#include "NotificationBar/NotificationBar.h"
#include "WindowContainer/WindowContainer.h"
#include "Window/LogWindow.h"
#include "Window/MainWindow.h"
#include "Window/MainMenu/MainMenuNav.h"
#include "Window/PaletteEditorWindow.h"
#include "Window/PalettesConfigWindow.h"
#include "Window/NetworkSquareColorWindow.h"
#include "Window/Ranked/RankedProgressWindow.h"
#include "Window/UnlimitedPlaybackWindow.h"
#include "Window/WinePopupWindow.h"

#include "Game/FrameStallDiagnostics.h"
#include "Game/FrameStallWatchdog.h"
#include "Network/LobbyLinkManager.h"
#include "Game/ReplayTakeover/ReplayTakeoverFeatureFlags.h"

#if BBCF_ENABLE_UNLIMITED_REPLAY_TAKEOVER
#include "Window/UnlimitedReplayTakeoverWindow.h"
#endif

#include "Core/HotkeyManager.h"
#include "Core/info.h"
#include "Core/InputDevices.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/logger.h"
#include "Core/RuntimePlatform.h"
#include "Core/Settings.h"
#include "Core/SystemSpecsLogger.h"
#include "Core/WineCheck.h"
#include "Core/utils.h"
#include "Web/update_check.h"
#include "Audio/MusicManager.h"
#include "Audio/BgmReplacementManager.h"
#include "Updater/UpdateCoordinator.h"

#include "imgui_utils.h"

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>
#include <cstdio>
#include <ctime>

#define DEFAULT_ALPHA 0.87f


// Fixed ImGui coordinate space requested by the 'viewport' setting, or (0,0) to follow the
// game window's client size. Applied every frame in ApplyViewportOverride().
static ImVec2 g_displaySizeOverride = ImVec2(0.0f, 0.0f);

static void SetDisplaySizeOverride(float width, float height)
{
	g_displaySizeOverride = ImVec2(width, height);
}

// Renders ImGui in a fixed coordinate space instead of the window client size, and rescales
// mouse input to match. Mouse events arrive from the Win32 backend in client pixels; without
// the matching rescale, hit-testing happens in a different space than rendering.
//
// The scale is applied AT THE SOURCE, inside the Win32 backend, and must be. Re-emitting a
// corrected position here instead - which is what this function used to do, on the stated
// assumption that "our position event is the last one queued and therefore wins" - does not
// work: ImGui's input-queue trickling stops applying queued MousePos events as soon as it has
// applied a mouse button change (UpdateInputEvents, "Trickling Rule: Stop processing queued
// events if we already handled a mouse button change"). The corrected position is queued
// after the button, so on every frame that carries a click it is discarded and the click is
// hit-tested at the raw client pixel instead. When the override is smaller than the window
// that lands outside the ImGui space entirely, so hovering highlights correctly and no click
// registers anywhere. See docs/ViewportMouseScaling.md.
static void ApplyViewportOverride()
{
	if (g_displaySizeOverride.x <= 0.0f || g_displaySizeOverride.y <= 0.0f)
	{
		ImGui_ImplWin32_SetMousePosScale(1.0f, 1.0f);
		return; // stock behaviour: backend-provided client size and unscaled mouse
	}

	ImGuiIO& io = ImGui::GetIO();

	RECT rect;
	if (!GetClientRect(g_gameProc.hWndGameWindow, &rect))
	{
		return;
	}

	const float clientWidth = (float)(rect.right - rect.left);
	const float clientHeight = (float)(rect.bottom - rect.top);
	if (clientWidth <= 0.0f || clientHeight <= 0.0f)
	{
		ImGui_ImplWin32_SetMousePosScale(1.0f, 1.0f);
		return;
	}

	io.DisplaySize = g_displaySizeOverride;

	const float scaleX = g_displaySizeOverride.x / clientWidth;
	const float scaleY = g_displaySizeOverride.y / clientHeight;

	// Applies from the next message pumped, which is what makes every position event in the
	// queue - not just the last one - already be in the overridden space.
	ImGui_ImplWin32_SetMousePosScale(scaleX, scaleY);

	// Seed the position for the first frame after the scale changes, and for the case where
	// no WM_MOUSEMOVE has arrived yet. Already in overridden space, so it agrees with the
	// events the backend queues rather than competing with them. Only while the cursor is
	// actually over the client area, so the backend's own mouse-leave event stays
	// authoritative when it isn't.
	POINT pos;
	if (GetCursorPos(&pos) && ScreenToClient(g_gameProc.hWndGameWindow, &pos))
	{
		if (pos.x >= 0 && pos.y >= 0 && (float)pos.x < clientWidth && (float)pos.y < clientHeight)
		{
			io.AddMousePosEvent((float)pos.x * scaleX, (float)pos.y * scaleY);
		}
	}
}

// Counted in hooks_bbcf.cpp by the wrapper around ImGui_ImplWin32_WndProcHandler.
extern unsigned int g_overlayMouseMoveMsgs;
extern unsigned int g_overlayMouseButtonMsgs;
extern unsigned int g_overlayKeyMsgs;

// Answers "mouse clicks don't register" from a log instead of from guesses. Two reports in
// a row could not be diagnosed because nothing about the overlay's input state was ever
// written down: opening the mod menu logs nothing, so a reporter's DEBUG.txt looks identical
// whether the overlay worked perfectly or swallowed every click.
//
// Change-driven, and deliberately does NOT key on the cursor position - that changes every
// frame and would flood the log. Positions are reported, the classification is what decides
// whether to log.
static void LogOverlayInputState(int openWindows)
{
	ImGuiIO& io = ImGui::GetIO();

	// Where the OS says the cursor is, in the same client space the backend reports.
	POINT osPos = {};
	bool osPosKnown = false;
	if (GetCursorPos(&osPos) && ScreenToClient(g_gameProc.hWndGameWindow, &osPos))
	{
		osPosKnown = true;
	}

	RECT client = {};
	GetClientRect(g_gameProc.hWndGameWindow, &client);

	// The single most useful number: how far ImGui's idea of the cursor is from the OS's.
	// Anything but ~0 while the cursor is over the window means hit-testing and rendering are
	// in different coordinate spaces, which is what makes a visible button unclickable.
	int delta = -1;
	if (osPosKnown)
	{
		const int dx = (int)io.MousePos.x - osPos.x;
		const int dy = (int)io.MousePos.y - osPos.y;
		delta = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
	}

	// A modal blocks every click outside itself, by design. An unanswered first-launch
	// prompt therefore makes the whole overlay unclickable, which looks exactly like broken
	// mouse input, so name it explicitly.
	ImGuiWindow* const modal = ImGui::GetTopMostPopupModal();
	const char* modalName = modal ? modal->Name : "";

	char key[512];
	snprintf(key, sizeof(key),
		"display=%.0fx%.0f client=%ldx%ld openWindows=%d capture=%d down=%d modal='%s' deltaClass=%s msgs=%s",
		io.DisplaySize.x, io.DisplaySize.y,
		client.right - client.left, client.bottom - client.top,
		openWindows, io.WantCaptureMouse ? 1 : 0, io.MouseDown[0] ? 1 : 0, modalName,
		delta < 0 ? "unknown" : (delta <= 2 ? "aligned" : (delta <= 40 ? "small" : "LARGE")),
		g_overlayMouseButtonMsgs == 0 ? "no-clicks-seen" : "clicks-seen");

	static std::string s_lastKey;
	if (s_lastKey == key)
	{
		return;
	}
	s_lastKey = key;

	LOG(1, "[OverlayInput] %s imguiPos=(%.0f,%.0f) osPos=(%ld,%ld) delta=%d "
	       "msgCounts move=%u button=%u key=%u\n",
		key, io.MousePos.x, io.MousePos.y,
		osPosKnown ? osPos.x : -1, osPosKnown ? osPos.y : -1, delta,
		g_overlayMouseMoveMsgs, g_overlayMouseButtonMsgs, g_overlayKeyMsgs);
}

WindowManager* WindowManager::m_instance = nullptr;

WindowManager& WindowManager::GetInstance()
{
	if (m_instance == nullptr)
	{
		m_instance = new WindowManager();
	}
	return *m_instance;
}

bool WindowManager::Initialize(void* hwnd, IDirect3DDevice9* device)
{
	if (m_initialized)
	{
		return true;
	}

	LOG(2, "WindowManager::Initialize\n");

	if (!hwnd)
	{
		LOG(2, "HWND not found!\n");
		return false;
	}
	if (!device)
	{
		LOG(2, "Direct3DDevice9 not found!\n");
		return false;
	}

	ImGui::CreateContext();

	// The overlay's window layout lives in menus.ini, NOT ImGui's default imgui.ini. This used to
	// be a one-line edit inside depends/imgui/imgui.cpp (IniFilename = "menus.ini"), which meant
	// updating ImGui silently reverted it and orphaned every saved window position. Set it here
	// instead so the vendored files stay pristine. Must be set before the first NewFrame(), which
	// is when ImGui loads the file. The pointer is not copied, so it needs static lifetime.
	ImGui::GetIO().IniFilename = "menus.ini";

	// Before the first frame: that file is parsed then, and a handler registered after it
	// never sees its own lines.
	PalettesConfigWindow::RegisterLayoutSettings();
	PaletteEditorWindow::RegisterLayoutSettings();
	MainMenu::RegisterLayoutSettings();

	m_initialized = ImGui_ImplWin32_Init(hwnd) && ImGui_ImplDX9_Init(device);
	if (!m_initialized)
	{
		LOG(2, "ImGui backend init failed!\n");
		return false;
	}

	// The 1.53 DX9-only backend had no cursor handling at all. The Win32 backend does, and would
	// start calling SetCursor() every frame over a game that manages its own cursor. Keep the OS
	// cursor untouched; the overlay's own software cursor (io.MouseDrawCursor, set in Render())
	// stays the only cursor logic, exactly as before.
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

	// After the ImGui DX9 backend, which is what proves the device is usable.
	Branding::Initialize(device);
	PaletteThumbnails::Initialize(device);

	m_pLogger = g_imGuiLogger;

	m_pLogger->Log("[system] Initialization starting...\n");

	// So a bug report's DEBUG.txt carries hardware/software context on its own,
	// without a separate round trip asking the reporter for their specs.
	LogSystemSpecs(device);

	// No-op unless LogFrameStalls is enabled in settings.ini.
	FrameStallWatchdog::Start();

        m_windowContainer = new WindowContainer();

        const bool wineLikely = WineCheck();
        if (wineLikely || !IsSafeToUseControllerHooks())
        {
                if (!Settings::settingsIni.ForceEnableControllerSettingHooks && Settings::settingsIni.EnableControllerHooks)
                {
                        LOG(1, "Wine/Proton detected; disabling hooks that break under Wine.\n");
                        Settings::changeSetting("EnableControllerHooks", "0");
                        Settings::settingsIni.EnableControllerHooks = 0;
                }

                if (!Settings::settingsIni.ForceEnableControllerSettingHooks)
                {
                        m_windowContainer->GetWindow<WinePopupWindow>(WindowType_WinePopup)->Open();
                }
        }

        ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowBorderSize = 1;
	style.FrameBorderSize = 1;
	style.ScrollbarSize = 14;
	style.Alpha = DEFAULT_ALPHA;

	// Add default font

	float unicodeFontSize = 18;

	if (Settings::settingsIni.menusize == 1)
	{
		ImFont* font = ImGui::GetIO().Fonts->AddFontFromMemoryCompressedBase85TTF(TinyFont_compressed_data_base85, 10);
		unicodeFontSize = 14;
	}
	else if (Settings::settingsIni.menusize == 3)
	{
		ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(DroidSans_compressed_data, DroidSans_compressed_size, 20);
		unicodeFontSize = 25;
	}
	else if (Settings::settingsIni.menusize == 4)
	{
		ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(DroidSans_compressed_data, DroidSans_compressed_size, 30);
		unicodeFontSize = 35;
	}
	else
	{
		ImGui::GetIO().Fonts->AddFontDefault();
	}

	// Add Unicode font

	ImFontConfig config;
	config.MergeMode = true;
	config.OversampleH = 1;
	config.OversampleV = 1;
	config.PixelSnapH = true;

	// No glyph range is supplied on purpose. Since 1.92 the font atlas is dynamic: glyphs are
	// rasterized on demand, and a merged source supplies any codepoint the earlier source lacks.
	// That covers the Japanese set plus the Western typography GitHub release notes rely on
	// (em-dash, curly quotes, ellipsis, bullet, arrows...) without hand-maintaining ranges, and
	// without paying for an atlas full of glyphs nobody displays.
	ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(mplusMedium_compressed_data, mplusMedium_compressed_size,
		unicodeFontSize, &config);


	//ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(DroidSans_compressed_data, DroidSans_compressed_size, 20);
	// Set up toggle buttons

	HotkeyManager::ReloadFromSettings();
	for (int i = 0; i < HotkeyManager::Hotkey_Count; ++i)
	{
		const HotkeyManager::Action action = (HotkeyManager::Action)i;
		m_pLogger->Log("[system] Hotkey '%s' bound to '%s'\n",
			HotkeyManager::DisplayName(action),
			HotkeyManager::DisplayString(HotkeyManager::GetBinding(action)).c_str());
	}

	// Load custom palettes

	g_interfaces.pPaletteManager->LoadAllPalettes();

	MusicManager::GetInstance().Initialize();
	BgmReplacementManager::GetInstance().Initialize();

	// Calling a frame to initialize beforehand to prevent a crash upon first call of Update() if the game window is not focused.
	// Simply calling ImGui_ImplDX9_CreateDeviceObjects() might be enough too
	ImGui_ImplWin32_NewFrame();
	ImGui_ImplDX9_NewFrame();
	ImGui::NewFrame();
	ImGui::EndFrame();
	///////

	srand(time(NULL));

	StartAsyncUpdateCheck();
	//StartAsyncReplayUpload();

	if (g_modVals.uploadReplayData == -1)
	{
		m_windowContainer->GetWindow(WindowType_ReplayDBPopup)->Open();
	}

	if (Settings::settingsIni.allowPaletteDownloads == -1)
	{
		m_windowContainer->GetWindow(WindowType_PaletteSharePopup)->Open();
	}




	std::string notificationText = MOD_WINDOW_TITLE;
	notificationText += " ";
	notificationText += MOD_VERSION_NUM;

#ifdef _DEBUG
	notificationText += " (DEBUG)";
#endif

        g_notificationBar->AddLocalizedNotification([notificationText]() {
                const char* format = Messages.Main_window_notification_format();
                const auto& toggleButton = Settings::settingsIni.togglebutton;

                const int size = std::snprintf(nullptr, 0, format, notificationText.c_str(), toggleButton.c_str()) + 1;
                std::string formatted(static_cast<size_t>(size), ' ');
                std::snprintf(&formatted[0], static_cast<size_t>(size), format,
                        notificationText.c_str(), toggleButton.c_str());

                return formatted;
        });

	m_pLogger->Log("[system] Finished initialization\n");
	m_pLogger->LogSeparator();
	LOG(2, "Initialize end\n");

	return true;
}

void WindowManager::Shutdown()
{
	if (!m_initialized)
	{
		return;
	}

	LOG(2, "WindowManager::Shutdown\n");

	FrameStallWatchdog::Stop();
	InputDevices::Shutdown();

	SAFE_DELETE(m_windowContainer);
	delete m_instance;

	ImGui_ImplDX9_Shutdown();
	PaletteThumbnails::Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

void WindowManager::InvalidateDeviceObjects()
{
	if (!m_initialized)
	{
		return;
	}

	LOG(2, "WindowManager::InvalidateDeviceObjects\n");
	ImGui_ImplDX9_InvalidateDeviceObjects();
	Branding::InvalidateDeviceObjects();
	// Palette thumbnails are D3DPOOL_DEFAULT, so a reset invalidates them too. They are
	// rebuilt lazily the next time the grid draws.
	PaletteThumbnails::ReleaseAll();
}

void WindowManager::CreateDeviceObjects()
{
	if (!m_initialized)
	{
		return;
	}

	LOG(2, "WindowManager::CreateDeviceObjects\n");
	ImGui_ImplDX9_CreateDeviceObjects();
	Branding::CreateDeviceObjects();
}

void WindowManager::Render()
{
	if (!m_initialized)
	{
		return;
	}

	if (!g_interfaces.pSteamApiHelper)
	{
		return;
	}

	// The "return to Character Select?" confirm dialog's message id only exists in the
	// render-phase UI buffer, so the Jukebox has to sample it from here.
	GetMusicManager().PollDialogRenderPhase();
	GetBgmReplacements().Update();

	if (g_interfaces.pSteamApiHelper->IsSteamOverlayActive())
	{
		return;
	}

	if (IsIconic(g_gameProc.hWndGameWindow))
	{
		return; // don't render when window is minimized, since this sometimes moves ui around
	}


	LOG(7, "WindowManager::Render\n");

	HandleButtons();

	// Overlay self-A/B (OverlayAbTestSeconds): drop the entire ImGui pass for
	// this frame. Hotkeys above still run, so the mod stays usable; only the
	// build/submit of the overlay's draw data is removed. NewFrame and Render
	// are skipped together, which is the one thing ImGui requires - never one
	// without the other. Inert unless the setting is set.
	if (FrameStallDiagnostics::OverlayAbSkipActive())
	{
		return;
	}

	// Must be set before NewFrame so hit-testing and rendering share the same
	// coordinate space; overriding DisplaySize after NewFrame offsets mouse hitboxes
	if (Settings::settingsIni.viewport == 2)
	{
		SetDisplaySizeOverride((float)Settings::settingsIni.renderwidth, (float)Settings::settingsIni.renderheight);
	}
	else if (Settings::settingsIni.viewport == 3)
	{
		SetDisplaySizeOverride(1280.0f, 768.0f);
	}
	else
	{
		SetDisplaySizeOverride(0.0f, 0.0f);
	}

	ImGui_ImplWin32_NewFrame();
	ImGui_ImplDX9_NewFrame();
	ApplyViewportOverride();
	ImGui::NewFrame();

	ImGui::GetIO().MouseDrawCursor = false;
	int openWindowCount = 0;
	for (auto p : m_windowContainer->GetWindows()) {
		if (p.first == WindowType_HitboxOverlay) continue; // ignore windows that don't need a mouse
		if (!p.second) continue;
		if (p.second->IsOpen()) {
			ImGui::GetIO().MouseDrawCursor = true;
			++openWindowCount;
		}
	}


	LogOverlayInputState(openWindowCount);

	DrawAllWindows();
	DrawRankedProgressOverlayStandalone();
	DrawNetworkSquareColorProgressStandalone();
	DrawUnlimitedPlaybackLoopSetupIndicatorStandalone();
#if BBCF_ENABLE_UNLIMITED_REPLAY_TAKEOVER
	DrawUnlimitedReplayTakeoverSetupDelayIndicatorStandalone();
#endif
	Updater::UpdateCoordinator::GetInstance().DrawSkippedLink();

	g_notificationBar->DrawNotifications();

	ImGui::Render();
	ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void WindowManager::HandleButtons()
{
	if (!m_initialized)
	{
		return;
	}

	// The single poll site for every hotkey in the mod. Everything else in this frame -
	// the windows drawn below, the playback manager, the lobby link shortcuts - reads the
	// press edges this call computes, so they all agree on what happened this frame.
	// Gating (window focus, typing into an overlay text field) lives inside it.
	HotkeyManager::Update();

	if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_ToggleMainWindow))
	{
		m_windowContainer->GetWindow(WindowType_Main)->ToggleOpen();
	}

	if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_ToggleOnlineWindow))
	{
		m_windowContainer->GetWindow(WindowType_Room)->ToggleOpen();
	}

	if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_ToggleHud) && g_gameVals.pIsHUDHidden)
	{
		*g_gameVals.pIsHUDHidden ^= 1;
	}

	if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_ToggleJukebox))
	{
		IWindow* jukebox = m_windowContainer->GetWindow(WindowType_Jukebox);
		const bool opening = !jukebox->IsOpen();
		jukebox->ToggleOpen();
		if (opening)
		{
			GetMusicManager().StartCustomMusicDiscovery();
		}
	}

	if (HotkeyManager::WasPressed(HotkeyManager::Hotkey_JukeboxNextTrack))
	{
		GetMusicManager().PlayNextTrack();
	}

	// Driven from the render loop rather than the GetFrameCounter hook: that hook only
	// fires while the match frame counter advances, so it ticks in training but is dead
	// on the online room screen -- exactly where copying a room link has to work.
	LobbyLinkManager::GetInstance().Tick();
}

void WindowManager::DrawAllWindows() const
{
	for (const auto& window : m_windowContainer->GetWindows())
	{
		if (!window.second)
		{
			continue;
		}
		window.second->Update();
	}
}
