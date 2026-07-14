#pragma once
#include "IWindow.h"
#include "Overlay/WindowContainer/WindowContainer.h"

// One-time popup asking the user whether opponents may download their custom
// palettes (AllowPaletteDownloads is -1 until they answer).
class PaletteSharePopupWindow : public IWindow
{
public:
	PaletteSharePopupWindow(const std::string& windowTitle, bool windowClosable,
		WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer) {}
	~PaletteSharePopupWindow() override = default;
	void Update() override;
protected:
	void Draw() override;
	WindowContainer* m_pWindowContainer = nullptr;
};
