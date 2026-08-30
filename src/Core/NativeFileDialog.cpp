#include "NativeFileDialog.h"

#include "Core/logger.h"

#include <Windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>

namespace
{
	struct DialogState
	{
		std::mutex mutex;
		bool active = false;
		bool completed = false;
		std::string owner;
		NativeFileDialog::Result result;
	};

	DialogState g_state;

	// OPENFILENAMEA wants description and pattern as NUL-separated pairs terminated by a
	// second NUL, which a plain std::string literal cannot express without embedded nulls
	// going wrong somewhere. Build the buffer explicitly instead.
	std::vector<char> BuildFilterBuffer(const std::vector<NativeFileDialog::Filter>& filters)
	{
		std::vector<char> buffer;
		const auto append = [&buffer](const std::string& text) {
			buffer.insert(buffer.end(), text.begin(), text.end());
			buffer.push_back('\0');
		};

		for (const NativeFileDialog::Filter& filter : filters)
		{
			append(filter.description);
			append(filter.pattern);
		}
		append("All Files (*.*)");
		append("*.*");
		buffer.push_back('\0');
		return buffer;
	}

	void SeedPath(const std::string& initialPath, char* selectedPath, char* initialDir, OPENFILENAMEA& ofn)
	{
		if (initialPath.empty())
		{
			return;
		}

		const DWORD attributes = GetFileAttributesA(initialPath.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		{
			std::strncpy(initialDir, initialPath.c_str(), MAX_PATH - 1);
			initialDir[MAX_PATH - 1] = '\0';
			ofn.lpstrInitialDir = initialDir;
			return;
		}

		std::strncpy(selectedPath, initialPath.c_str(), MAX_PATH - 1);
		selectedPath[MAX_PATH - 1] = '\0';

		char* const backslash = std::strrchr(selectedPath, '\\');
		char* const slash = std::strrchr(selectedPath, '/');
		char* const separator = (std::max)(backslash, slash);
		if (separator == nullptr)
		{
			return;
		}

		const size_t directoryLength = static_cast<size_t>(separator - selectedPath);
		if (directoryLength < MAX_PATH)
		{
			std::memcpy(initialDir, selectedPath, directoryLength);
			initialDir[directoryLength] = '\0';
			ofn.lpstrInitialDir = initialDir;
		}

		const char* const fileName = separator + 1;
		std::memmove(selectedPath, fileName, std::strlen(fileName) + 1);
	}
}

bool NativeFileDialog::Open(const char* ownerToken, const Request& request)
{
	{
		std::lock_guard<std::mutex> lock(g_state.mutex);
		if (g_state.active)
		{
			return false;
		}
		// Drop anything an earlier caller never picked up, so one window closing mid-dialog
		// cannot leave a result sitting here forever.
		g_state.active = true;
		g_state.completed = false;
		g_state.owner = ownerToken ? ownerToken : "";
		g_state.result = Result{};
	}

	std::thread([request]() {
		char selectedPath[MAX_PATH] = {};
		char initialDir[MAX_PATH] = {};
		char originalWorkingDirectory[MAX_PATH] = {};
		GetCurrentDirectoryA(MAX_PATH, originalWorkingDirectory);

		const std::vector<char> filterBuffer = BuildFilterBuffer(request.filters);

		OPENFILENAMEA ofn;
		std::memset(&ofn, 0, sizeof(ofn));
		ofn.lStructSize = sizeof(ofn);
		// Deliberately unowned: an owned modal would tie its message loop to a window the
		// render thread is driving.
		ofn.hwndOwner = nullptr;
		ofn.lpstrFile = selectedPath;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrFilter = filterBuffer.data();
		ofn.lpstrTitle = request.title.empty() ? nullptr : request.title.c_str();
		ofn.lpstrDefExt = request.defaultExtension.empty() ? nullptr : request.defaultExtension.c_str();

		SeedPath(request.initialPath, selectedPath, initialDir, ofn);

		bool accepted = false;
		if (request.save)
		{
			ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
			accepted = GetSaveFileNameA(&ofn) == TRUE;
		}
		else
		{
			ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
			accepted = GetOpenFileNameA(&ofn) == TRUE;
		}

		// Belt and braces: OFN_NOCHANGEDIR covers the documented path, but some shell
		// extensions loaded into the dialog change it anyway.
		if (originalWorkingDirectory[0] != '\0')
		{
			SetCurrentDirectoryA(originalWorkingDirectory);
		}

		std::lock_guard<std::mutex> lock(g_state.mutex);
		g_state.result.accepted = accepted;
		g_state.result.path = accepted ? selectedPath : "";
		g_state.result.contextId = request.contextId;
		g_state.completed = true;
		g_state.active = false;
	}).detach();

	return true;
}

bool NativeFileDialog::IsOpen()
{
	std::lock_guard<std::mutex> lock(g_state.mutex);
	return g_state.active;
}

bool NativeFileDialog::Consume(const char* ownerToken, Result* outResult)
{
	std::lock_guard<std::mutex> lock(g_state.mutex);
	if (!g_state.completed)
	{
		return false;
	}
	if (g_state.owner != (ownerToken ? ownerToken : ""))
	{
		return false;
	}

	if (outResult)
	{
		*outResult = g_state.result;
	}

	g_state.completed = false;
	g_state.owner.clear();
	g_state.result = Result{};
	return true;
}
