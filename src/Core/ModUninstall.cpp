#include "ModUninstall.h"

#include "Core/EmbeddedResources.h"
#include "Core/logger.h"
#include "Core/NvidiaProfileMirror.h"
#include "Core/utils.h"

#include <Windows.h>

namespace
{
	bool WriteWholeFile(const std::wstring& path, const std::string& bytes)
	{
		HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		DWORD written = 0;
		const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
			written == bytes.size();
		CloseHandle(file);
		return ok;
	}
}

namespace ModUninstall
{
	bool UninstallAndExit(bool keepUserData, std::string* error)
	{
		std::string updater;
		if (!LoadEmbeddedResource(L"BBCFIMUpdater.exe", updater) || updater.size() < 2 || updater[0] != 'M' || updater[1] != 'Z')
		{
			*error = "The uninstaller is missing from this build of the mod.";
			return false;
		}

		wchar_t temp[MAX_PATH] = {};
		if (!GetTempPathW(MAX_PATH, temp))
		{
			*error = "Could not find the Windows temp folder.";
			return false;
		}
		const std::wstring folder = std::wstring(temp) + L"BBCF_IM_Uninstall_" + std::to_wstring(GetCurrentProcessId());
		CreateDirectoryW(folder.c_str(), nullptr);
		const std::wstring exe = folder + L"\\BBCFIMUpdater.exe";
		if (!WriteWholeFile(exe, updater))
		{
			*error = "Could not write the uninstaller to the temp folder.";
			return false;
		}

		std::wstring root = GetGameDirectoryW();
		while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
			root.pop_back();

		// The updater waits for this process and, in 4 GB mode, for the stock BBCF.exe that
		// launched it (it keeps dinput8.dll loaded until this copy exits).
		std::wstring command = L"\"" + exe + L"\" --uninstall \"" + root + L"\" --wait-pid " + std::to_wstring(GetCurrentProcessId());
		wchar_t parent[32] = {};
		if (GetEnvironmentVariableW(L"BBCF_IM_LAA_PARENT", parent, 32) > 0)
			command += std::wstring(L" --wait-pid ") + parent;
		if (!keepUserData)
			command += L" --wipe";

		// Before the game closes: the driver settings are edited through nvapi.dll, which the
		// updater does not load.
		LOG(0, "[Uninstall] NVIDIA: %s\n", NvidiaProfileMirror::Remove());

		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};
		if (!CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &si, &pi))
		{
			*error = "Could not start the uninstaller (Windows error " + std::to_string(GetLastError()) + ").";
			return false;
		}
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);

		LOG(0, "[Uninstall] uninstaller started (%s); closing the game\n", keepUserData ? "keeping the player's files" : "removing everything");
		ExitProcess(0);
		return true;
	}
}
