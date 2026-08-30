#pragma once

#include "Overlay/Window/IWindow.h"

#include <string>
#include <vector>

class WindowContainer;

class TasWindow : public IWindow {
public:
    TasWindow(const std::string& windowTitle, bool windowClosable,
        WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0)
        : IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer) {}

    ~TasWindow() override = default;

public:
    void Update() override;

protected:
    void Draw() override;

private:
    WindowContainer* m_pWindowContainer = nullptr;
    char m_p1Input[256] = "5";
    char m_p2Input[256] = "5";
    int m_frameCount = 1;
    char m_exportFileName[256] = "";
    std::string m_selectedImportFile;
    std::vector<std::string> m_importFiles;
    bool m_includeInitialConditions = true;
};
