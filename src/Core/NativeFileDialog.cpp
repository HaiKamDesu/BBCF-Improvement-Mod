#include "NativeFileDialog.h"

#include "utils.h"

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

	// The dialog is the wide one throughout, and paths come back as UTF-8.
	//
	// The ANSI dialog cannot represent a filename outside the system codepage: a song
	// named in Japanese on an English install comes back as question marks, and every
	// later attempt to open it reports that the file does not exist. That was a real
	// report, and it is why nothing here uses the A functions.
	//
	// OPENFILENAMEW wants description and pattern as NUL-separated pairs terminated by a
	// second NUL, which a plain string literal cannot express without embedded nulls going
	// wrong somewhere. Build the buffer explicitly instead.
	std::vector<wchar_t> BuildFilterBuffer(const std::vector<NativeFileDialog::Filter>& filters)
	{
		std::vector<wchar_t> buffer;
		const auto append = [&buffer](const std::string& text) {
			const std::wstring wide = utf8_to_utf16(text);
			buffer.insert(buffer.end(), wide.begin(), wide.end());
			buffer.push_back(L'\0');
		};

		for (const NativeFileDialog::Filter& filter : filters)
		{
			append(filter.description);
			append(filter.pattern);
		}
		append("All Files (*.*)");
		append("*.*");
		buffer.push_back(L'\0');
		return buffer;
	}

	void SeedPath(const std::string& initialPathUtf8, wchar_t* selectedPath, wchar_t* initialDir, OPENFILENAMEW& ofn)
	{
		if (initialPathUtf8.empty())
		{
			return;
		}

		const std::wstring initialPath = utf8_to_utf16(initialPathUtf8);
		const DWORD attributes = GetFileAttributesW(initialPath.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		{
			wcsncpy_s(initialDir, MAX_PATH, initialPath.c_str(), _TRUNCATE);
			ofn.lpstrInitialDir = initialDir;
			return;
		}

		wcsncpy_s(selectedPath, MAX_PATH, initialPath.c_str(), _TRUNCATE);

		wchar_t* const backslash = wcsrchr(selectedPath, L'\\');
		wchar_t* const slash = wcsrchr(selectedPath, L'/');
		wchar_t* const separator = (std::max)(backslash, slash);
		if (separator == nullptr)
		{
			return;
		}

		const size_t directoryLength = static_cast<size_t>(separator - selectedPath);
		if (directoryLength < MAX_PATH)
		{
			std::memcpy(initialDir, selectedPath, directoryLength * sizeof(wchar_t));
			initialDir[directoryLength] = L'\0';
			ofn.lpstrInitialDir = initialDir;
		}

		const wchar_t* const fileName = separator + 1;
		std::memmove(selectedPath, fileName, (wcslen(fileName) + 1) * sizeof(wchar_t));
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
		wchar_t selectedPath[MAX_PATH] = {};
		wchar_t initialDir[MAX_PATH] = {};
		wchar_t originalWorkingDirectory[MAX_PATH] = {};
		GetCurrentDirectoryW(MAX_PATH, originalWorkingDirectory);

		const std::vector<wchar_t> filterBuffer = BuildFilterBuffer(request.filters);
		const std::wstring title = utf8_to_utf16(request.title);
		const std::wstring defaultExtension = utf8_to_utf16(request.defaultExtension);

		OPENFILENAMEW ofn;
		std::memset(&ofn, 0, sizeof(ofn));
		ofn.lStructSize = sizeof(ofn);
		// Deliberately unowned: an owned modal would tie its message loop to a window the
		// render thread is driving.
		ofn.hwndOwner = nullptr;
		ofn.lpstrFile = selectedPath;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrFilter = filterBuffer.data();
		ofn.lpstrTitle = title.empty() ? nullptr : title.c_str();
		ofn.lpstrDefExt = defaultExtension.empty() ? nullptr : defaultExtension.c_str();

		SeedPath(request.initialPath, selectedPath, initialDir, ofn);

		bool accepted = false;
		if (request.save)
		{
			ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
			accepted = GetSaveFileNameW(&ofn) == TRUE;
		}
		else
		{
			ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
			accepted = GetOpenFileNameW(&ofn) == TRUE;
		}

		// Belt and braces: OFN_NOCHANGEDIR covers the documented path, but some shell
		// extensions loaded into the dialog change it anyway.
		if (originalWorkingDirectory[0] != L'\0')
		{
			SetCurrentDirectoryW(originalWorkingDirectory);
		}

		std::lock_guard<std::mutex> lock(g_state.mutex);
		g_state.result.accepted = accepted;
		// Back to UTF-8 for everyone else. Callers must open these with the wide file APIs
		// (see utils' utf8_to_utf16), or a non-ASCII name is lost again on the way in.
		g_state.result.path = accepted ? utf16_to_utf8(selectedPath) : std::string();
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
