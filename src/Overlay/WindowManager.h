#pragma once
#include "Logger/Logger.h"
#include "WindowContainer/WindowContainer.h"

#include <d3d9.h>

#include <string>

class WindowManager
{
public:
	static WindowManager& GetInstance();
	WindowContainer* GetWindowContainer() const { return m_windowContainer; }
	bool Initialize(void *hwnd, IDirect3DDevice9 *device);
	void Shutdown();
	void Render();
	void InvalidateDeviceObjects();
	void CreateDeviceObjects();
	bool IsInitialized() const { return m_initialized; }

private:
	WindowManager() = default;
	void HandleButtons();
	void DrawAllWindows() const;

	static WindowManager* m_instance;
	bool m_initialized = false;
	WindowContainer* m_windowContainer = nullptr;
	Logger* m_pLogger = nullptr;

	// Last set of window types that asked for the mouse cursor, so the log records the
	// change rather than one line per frame.
	std::string m_lastCursorClaimants;
};
