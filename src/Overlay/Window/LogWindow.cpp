#include "LogWindow.h"

#include "Core/Localization.h"
#include "Core/logger.h"

#include "imgui_internal.h"

#include <Windows.h>

#include <cstring>
#include <mutex>
#include <thread>

namespace
{
	const char* kDebugFilePath = "BBCF_IM\\DEBUG.txt";
	const wchar_t* kDebugFilePathW = L"BBCF_IM\\DEBUG.txt";

	// Memory cap for the in-window mirror of DEBUG.txt. When the buffer
	// exceeds the cap it is trimmed back down to the retain size on a line
	// boundary; the full log always remains on disk.
	const size_t kMaxBufferBytes = 2 * 1024 * 1024;
	const size_t kRetainBufferBytes = 1536 * 1024;

	// Save-dialog state, following the known-safe pattern from
	// UnlimitedPlaybackWindow: the Win32 dialog runs on its own thread with
	// OFN_NOCHANGEDIR and working-directory restore, and the render thread
	// only consumes the result.
	struct SaveDialogState
	{
		std::mutex mutex;
		bool active = false;
		bool completed = false;
		bool succeeded = false;
		std::string path;
	};

	SaveDialogState g_saveDialogState;

	void StartDebugLogSaveDialogAsync()
	{
		{
			std::lock_guard<std::mutex> lock(g_saveDialogState.mutex);
			if (g_saveDialogState.active)
				return;
			g_saveDialogState.active = true;
			g_saveDialogState.completed = false;
			g_saveDialogState.succeeded = false;
			g_saveDialogState.path.clear();
		}

		std::thread([]() {
			char selectedPath[MAX_PATH] = "DEBUG.txt";
			char originalWorkingDirectory[MAX_PATH] = {};
			OPENFILENAMEA ofn;
			std::memset(&ofn, 0, sizeof(ofn));
			GetCurrentDirectoryA(MAX_PATH, originalWorkingDirectory);

			ofn.lStructSize = sizeof(ofn);
			ofn.hwndOwner = nullptr;
			ofn.lpstrFile = selectedPath;
			ofn.nMaxFile = MAX_PATH;
			ofn.lpstrFilter = "Text File (*.txt)\0*.txt\0All Files\0*.*\0";
			ofn.lpstrDefExt = "txt";
			ofn.lpstrTitle = "Save debug log";
			ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

			const bool dialogConfirmed = GetSaveFileNameA(&ofn) == TRUE;

			if (originalWorkingDirectory[0] != '\0')
			{
				SetCurrentDirectoryA(originalWorkingDirectory);
			}

			bool copied = false;
			if (dialogConfirmed)
			{
				copied = CopyFileA(kDebugFilePath, selectedPath, FALSE) == TRUE;
			}

			std::lock_guard<std::mutex> lock(g_saveDialogState.mutex);
			g_saveDialogState.path = dialogConfirmed ? selectedPath : "";
			g_saveDialogState.succeeded = copied;
			g_saveDialogState.completed = dialogConfirmed; // silent on cancel
			g_saveDialogState.active = false;
		}).detach();
	}
}

void LogWindow::BeforeDraw()
{
	ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
}

void LogWindow::ResetView()
{
	m_fileBuffer.clear();
	m_lineOffsets.clear();
	m_trimmed = false;
}

void LogWindow::AppendChunk(const char* data, size_t len)
{
	m_fileBuffer.reserve(m_fileBuffer.size() + len);
	for (size_t i = 0; i < len; ++i)
	{
		if (data[i] == '\r')
			continue;
		if (data[i] == '\n')
			m_lineOffsets.push_back((int)m_fileBuffer.size());
		m_fileBuffer.push_back(data[i]);
	}
}

void LogWindow::TrimBufferIfNeeded()
{
	if (m_fileBuffer.size() <= kMaxBufferBytes)
		return;

	size_t cutPos = m_fileBuffer.size() - kRetainBufferBytes;
	const size_t nextNewline = m_fileBuffer.find('\n', cutPos);
	if (nextNewline != std::string::npos)
		cutPos = nextNewline + 1;

	m_fileBuffer.erase(0, cutPos);
	m_trimmed = true;

	m_lineOffsets.clear();
	for (size_t i = 0; i < m_fileBuffer.size(); ++i)
		if (m_fileBuffer[i] == '\n')
			m_lineOffsets.push_back((int)i);
}

void LogWindow::PollDebugFile()
{
	const double now = ImGui::GetTime();
	if (now < m_nextPollTime)
		return;
	m_nextPollTime = now + 0.25;

	HANDLE hFile = CreateFileW(kDebugFilePathW, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return;

	LARGE_INTEGER fileSize = {};
	if (!GetFileSizeEx(hFile, &fileSize))
	{
		CloseHandle(hFile);
		return;
	}

	// File shrank: a new session truncated it, start over.
	if (fileSize.QuadPart < m_fileReadOffset)
	{
		ResetView();
		m_fileReadOffset = 0;
	}

	if (fileSize.QuadPart == m_fileReadOffset)
	{
		CloseHandle(hFile);
		return;
	}

	LARGE_INTEGER readPos;
	readPos.QuadPart = m_fileReadOffset;

	// Way behind (e.g. window opened late into a long session): jump to the tail.
	if (fileSize.QuadPart - m_fileReadOffset > (long long)kMaxBufferBytes)
	{
		readPos.QuadPart = fileSize.QuadPart - (long long)kRetainBufferBytes;
		ResetView();
		m_trimmed = true;
	}

	if (!SetFilePointerEx(hFile, readPos, nullptr, FILE_BEGIN))
	{
		CloseHandle(hFile);
		return;
	}

	const DWORD toRead = (DWORD)(fileSize.QuadPart - readPos.QuadPart);
	std::vector<char> chunk(toRead);
	DWORD bytesRead = 0;
	if (ReadFile(hFile, chunk.data(), toRead, &bytesRead, nullptr) && bytesRead > 0)
	{
		AppendChunk(chunk.data(), bytesRead);
		m_fileReadOffset = readPos.QuadPart + bytesRead;
	}

	CloseHandle(hFile);

	TrimBufferIfNeeded();
}

void LogWindow::DrawSaveAsButton()
{
	{
		std::lock_guard<std::mutex> lock(g_saveDialogState.mutex);
		if (g_saveDialogState.active)
		{
			ImGui::BeginDisabled();
			ImGui::Button(Messages.Save_as());
			ImGui::EndDisabled();
			return;
		}

		if (g_saveDialogState.completed)
		{
			if (g_saveDialogState.succeeded)
				m_logger.Log("[system] Debug log saved to '%s'\n", g_saveDialogState.path.c_str());
			else
				m_logger.Log("[error] Failed to save debug log to '%s'\n", g_saveDialogState.path.c_str());
			g_saveDialogState.completed = false;
		}
	}

	if (ImGui::Button(Messages.Save_as()))
	{
		StartDebugLogSaveDialogAsync();
	}
}

void LogWindow::Draw()
{
	PollDebugFile();

	if (ImGui::Button(Messages.Clear()))
	{
		// Clears the view only; DEBUG.txt itself keeps everything.
		ResetView();
	}
	ImGui::SameLine();
	bool copyPressed = ImGui::Button(Messages.Copy_to_clipboard());
	ImGui::SameLine();
	DrawSaveAsButton();
	ImGui::SameLine();
	m_filter.Draw(Messages.Search(), -100.0f);

	if (!IsLoggingEnabled())
	{
		ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
			"Debug logging is disabled. Enable \"Generate debug logs\" in Settings to feed this window.");
	}
	if (m_trimmed)
	{
		ImGui::TextDisabled("Older lines were trimmed from this view. The full log is in BBCF_IM\\DEBUG.txt.");
	}

	ImGui::Separator();
	ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
	if (copyPressed)
	{
		ImGui::LogToClipboard();
		m_logger.Log(Messages.System_log_copied());
	}

	const char* bufBegin = m_fileBuffer.c_str();
	const char* bufEnd = bufBegin + m_fileBuffer.size();
	if (m_filter.IsActive() || copyPressed)
	{
		const char* line = bufBegin;
		for (int line_no = 0; line != NULL; line_no++)
		{
			const char* lineEnd = (line_no < (int)m_lineOffsets.size()) ? bufBegin + m_lineOffsets[line_no] : NULL;
			if (!m_filter.IsActive() || m_filter.PassFilter(line, lineEnd))
			{
				ImGui::TextUnformatted(line, lineEnd);
			}
			line = lineEnd && lineEnd + 1 < bufEnd ? lineEnd + 1 : NULL;
		}
	}
	else
	{
		// Clip to the visible lines so a large log stays cheap to draw.
		const int totalLines = (int)m_lineOffsets.size() + 1;
		ImGuiListClipper clipper;
		clipper.Begin(totalLines);
		while (clipper.Step())
		{
			for (int line_no = clipper.DisplayStart; line_no < clipper.DisplayEnd; line_no++)
			{
				const char* line = (line_no > 0) ? bufBegin + m_lineOffsets[line_no - 1] + 1 : bufBegin;
				const char* lineEnd = (line_no < (int)m_lineOffsets.size()) ? bufBegin + m_lineOffsets[line_no] : bufEnd;
				ImGui::TextUnformatted(line, lineEnd);
			}
		}
		clipper.End();
	}

	// Handle automatic scrolling
	if (m_prevScrollMaxY < ImGui::GetScrollMaxY())
	{
		// Scroll down automatically only if we didnt scroll up or we closed the window
		if (m_prevScrollMaxY - 5 <= ImGui::GetScrollY())
		{
			ImGui::SetScrollY(ImGui::GetScrollMaxY());
		}
		m_prevScrollMaxY = ImGui::GetScrollMaxY();
	}

	ImGui::EndChild();
}
