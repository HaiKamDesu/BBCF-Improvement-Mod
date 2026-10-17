#pragma once
#include "IWindow.h"
#include "Overlay/WindowContainer/WindowContainer.h"

#include <string>

// Asks once whether to run the game in 4 GB mode (LargeAddressSpace is -1 until answered) and
// explains exactly what accepting does before it does it (src/Core/LaaRedirect.h).
class LargeAddressPopupWindow : public IWindow
{
public:
	LargeAddressPopupWindow(const std::string& windowTitle, bool windowClosable,
		WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer) {}
	~LargeAddressPopupWindow() override = default;
	void Update() override;
protected:
	void Draw() override;
	WindowContainer* m_pWindowContainer = nullptr;
	std::string m_error;
};
