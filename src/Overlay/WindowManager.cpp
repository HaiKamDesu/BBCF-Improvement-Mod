#include "WindowManager.h"

#include "fonts.h"
#include "NotificationBar/NotificationBar.h"
#include "WindowContainer/WindowContainer.h"
#include "Window/LogWindow.h"
#include "Window/MainWindow.h"
#include "Window/NetworkSquareColorWindow.h"
#include "Window/Ranked/RankedProgressWindow.h"
#include "Window/WinePopupWindow.h"

#include "Core/info.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/logger.h"
#include "Core/RuntimePlatform.h"
#include "Core/Settings.h"
#include "Core/WineCheck.h"
#include "Core/utils.h"
#include "Web/update_check.h"
#include "Updater/UpdateCoordinator.h"

#include "imgui_utils.h"

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>
#include <cstdio>
#include <ctime>

#define DEFAULT_ALPHA 0.87f

int keyToggleMainWindow;
int keyToggleRoomWindow;
int keyToggleHud;

// Fixed ImGui coordinate space requested by the 'viewport' setting, or (0,0) to follow the
// game window's client size. Applied every frame in ApplyViewportOverride().
static ImVec2 g_displaySizeOverride = ImVec2(0.0f, 0.0f);

static void SetDisplaySizeOverride(float width, float height)
{
	g_displaySizeOverride = ImVec2(width, height);
}

// Renders ImGui in a fixed coordinate space instead of the window client size, and rescales
// mouse input to match. Mouse events arrive from the Win32 backend in client pixels; without
// the matching rescale, hitboxes drift away from the rendered widgets (worse further from the
// top-left). Must run after ImGui_ImplWin32_NewFrame() (so our position event is the last one
// queued and therefore wins) and before ImGui::NewFrame() (which consumes the queue and
// validates DisplaySize).
static void ApplyViewportOverride()
{
	if (g_displaySizeOverride.x <= 0.0f || g_displaySizeOverride.y <= 0.0f)
	{
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
		return;
	}

	io.DisplaySize = g_displaySizeOverride;

	const float scaleX = g_displaySizeOverride.x / clientWidth;
	const float scaleY = g_displaySizeOverride.y / clientHeight;

	// Re-emit the cursor position in the overridden space. Only while the cursor is actually
	// over the client area, so the backend's own mouse-leave event stays authoritative when
	// it isn't.
	POINT pos;
	if (GetCursorPos(&pos) && ScreenToClient(g_gameProc.hWndGameWindow, &pos))
	{
		if (pos.x >= 0 && pos.y >= 0 && (float)pos.x < clientWidth && (float)pos.y < clientHeight)
		{
			io.AddMousePosEvent((float)pos.x * scaleX, (float)pos.y * scaleY);
		}
	}
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

	m_pLogger = g_imGuiLogger;

	m_pLogger->Log("[system] Initialization starting...\n");

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

	keyToggleMainWindow = Settings::getButtonValue(Settings::settingsIni.togglebutton);
	m_pLogger->Log("[system] Toggling key set to '%s'\n", Settings::settingsIni.togglebutton.c_str());

	keyToggleRoomWindow = Settings::getButtonValue(Settings::settingsIni.toggleOnlineButton);
	m_pLogger->Log("[system] Online toggling key set to '%s'\n", Settings::settingsIni.toggleOnlineButton.c_str());

	keyToggleHud = Settings::getButtonValue(Settings::settingsIni.toggleHUDbutton);
	m_pLogger->Log("[system] HUD toggling key set to '%s'\n", Settings::settingsIni.toggleHUDbutton.c_str());

	// Load custom palettes

	g_interfaces.pPaletteManager->LoadAllPalettes();

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

	SAFE_DELETE(m_windowContainer);
	delete m_instance;

	ImGui_ImplDX9_Shutdown();
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
}

void WindowManager::CreateDeviceObjects()
{
	if (!m_initialized)
	{
		return;
	}

	LOG(2, "WindowManager::CreateDeviceObjects\n");
	ImGui_ImplDX9_CreateDeviceObjects();
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
	for (auto p : m_windowContainer->GetWindows()) {
		if (p.first == WindowType_HitboxOverlay) continue; // ignore windows that don't need a mouse
		if (!p.second) continue;
		if (p.second->IsOpen()) {
			ImGui::GetIO().MouseDrawCursor = true;
			break;
		}
	}


	DrawAllWindows();
	DrawRankedProgressOverlayStandalone();
	DrawNetworkSquareColorProgressStandalone();
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

	// Never let a rebound letter hotkey fire while the user is typing into a text field.
	if (IsTypingInImGuiTextField())
	{
		return;
	}

	if (ImGui::IsVirtualKeyPressed(keyToggleMainWindow))
	{
		m_windowContainer->GetWindow(WindowType_Main)->ToggleOpen();
	}

	if (ImGui::IsVirtualKeyPressed(keyToggleRoomWindow))
	{
		m_windowContainer->GetWindow(WindowType_Room)->ToggleOpen();
	}

	if (ImGui::IsVirtualKeyPressed(keyToggleHud) && g_gameVals.pIsHUDHidden)
	{
		*g_gameVals.pIsHUDHidden ^= 1;
	}
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
