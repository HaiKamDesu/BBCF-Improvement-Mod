#pragma once
#include "IWindow.h"
#include "Overlay/Logger/ImGuiLogger.h"

#include <string>
#include <vector>

class LogWindow : public IWindow
{
public:
	LogWindow(const std::string& windowTitle, bool windowClosable, ImGuiLogger& logger,
		ImGuiWindowFlags windowFlags = 0)
		: IWindow(windowTitle, windowClosable, windowFlags), m_logger(logger) {}
	~LogWindow() override = default;
protected:
	void BeforeDraw() override;
	void Draw() override;
private:
	void PollDebugFile();
	void AppendChunk(const char* data, size_t len);
	void TrimBufferIfNeeded();
	void ResetView();
	void DrawSaveAsButton();

	ImGuiLogger&    m_logger;
	ImGuiTextFilter m_filter;
	float           m_prevScrollMaxY = 0;

	// Live tail of BBCF_IM\DEBUG.txt. Capped (oldest lines dropped) so a long
	// session cannot grow the overlay's memory unboundedly.
	std::string      m_fileBuffer;
	std::vector<int> m_lineOffsets;   // indices of '\n' in m_fileBuffer
	long long        m_fileReadOffset = 0;
	double           m_nextPollTime = 0.0;
	bool             m_trimmed = false;
};
