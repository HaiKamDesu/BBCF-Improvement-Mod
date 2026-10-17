#include "LaaRedirect.h"

#include "FirewallMirror.h"
#include "LargeAddressAware.h"
#include "Settings.h"
#include "logger.h"
#include "utils.h"

#include <Windows.h>
#include <shellapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	constexpr const wchar_t* kCopyName = L"BBCF_IM_4GB.exe";
	constexpr const wchar_t* kParentEnv = L"BBCF_IM_LAA_PARENT";
	constexpr uintptr_t kWinMainCallRva = 0x003A5670;   // 0x007A5670: call WinMain, the CRT's only call
	constexpr uintptr_t kWinMainRva = 0x0004EB10;       // 0x0044EB10
	// The copy is treated as broken when it exits with a non-zero code before it ever showed a
	// window, within this long of starting: a boot crash. Not a player quitting, and not a
	// crash or a Task Manager kill once the game is up - those must not switch 4 GB mode off.
	// A boot that hangs for longer than this and is then killed is not blamed on the copy
	// either: the stock exe would most likely hang the same way.
	constexpr DWORD kBootWindowMs = 60000;

	bool g_redirecting = false;
	HANDLE g_child = nullptr;
	DWORD g_childStartTick = 0;
	HANDLE g_job = nullptr;
	DWORD g_parentPid = 0; // in the copy: the stock process that started it

	std::wstring GameFile(const wchar_t* name)
	{
		return GetGameDirectoryW() + L"\\" + name;
	}

	// The stock process cannot write DEBUG.txt (the game process owns it), so its decisions go
	// here. Small, appended, trimmed when it grows past 64 KB.
	void RedirectLog(const char* format, ...)
	{
		const std::wstring dir = GameFile(L"BBCF_IM");
		CreateDirectoryW(dir.c_str(), nullptr);
		const std::wstring path = dir + L"\\LargeAddressSpace.log";
		WIN32_FILE_ATTRIBUTE_DATA info = {};
		if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow > 64 * 1024)
			DeleteFileW(path.c_str());
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"ab") != 0 || !file)
			return;
		SYSTEMTIME now = {};
		GetLocalTime(&now);
		fprintf(file, "[%04d-%02d-%02d %02d:%02d:%02d] [pid %lu] ", now.wYear, now.wMonth, now.wDay,
			now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
		va_list args;
		va_start(args, format);
		vfprintf(file, format, args);
		va_end(args);
		fputs("\r\n", file);
		fclose(file);
	}

	// LargeAddressSpace from settings.ini, read directly: in DllMain the mod's settings are not
	// loaded yet. enum: -1 = ask, 0 = off, 1 = on (same order as SettingsIniWindow's options).
	int ReadSettingDirect()
	{
		return static_cast<int>(GetPrivateProfileIntW(L"Settings", L"LargeAddressSpace", -1, GameFile(L"settings.ini").c_str()));
	}

	void WriteSettingDirect(int value)
	{
		WritePrivateProfileStringW(L"Settings", L"LargeAddressSpace", std::to_wstring(value).c_str(), GameFile(L"settings.ini").c_str());
	}

	bool Is64BitWindows()
	{
		BOOL wow64 = FALSE;
		return IsWow64Process(GetCurrentProcess(), &wow64) && wow64;
	}

	const IMAGE_NT_HEADERS32* RunningHeaders()
	{
		const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
			return nullptr;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
		return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
	}

	std::wstring RunningExePath()
	{
		wchar_t path[MAX_PATH + 1] = {};
		const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
		return (length == 0 || length > MAX_PATH) ? std::wstring() : std::wstring(path, length);
	}

	bool ReadWholeFile(const std::wstring& path, std::vector<BYTE>& out)
	{
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		LARGE_INTEGER size = {};
		bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 256ll * 1024 * 1024;
		if (ok)
		{
			out.resize(static_cast<size_t>(size.QuadPart));
			DWORD read = 0;
			ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
		}
		CloseHandle(file);
		return ok;
	}

	// The file's NT headers, if it is a PE whose header names the same build as the running exe.
	IMAGE_NT_HEADERS32* SameBuildHeaders(std::vector<BYTE>& data)
	{
		const IMAGE_NT_HEADERS32* running = RunningHeaders();
		if (!running || data.size() < sizeof(IMAGE_DOS_HEADER))
			return nullptr;
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data.data());
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
			static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > data.size())
			return nullptr;
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(data.data() + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE ||
			nt->FileHeader.TimeDateStamp != running->FileHeader.TimeDateStamp ||
			nt->OptionalHeader.SizeOfImage != running->OptionalHeader.SizeOfImage)
			return nullptr;
		return nt;
	}

	// The copy exists, is the same build as the running stock exe, the same size, and carries
	// the flag. Anything else (a game update, a half-written file) means it is rewritten.
	bool CopyIsCurrent()
	{
		WIN32_FILE_ATTRIBUTE_DATA copyInfo = {};
		WIN32_FILE_ATTRIBUTE_DATA stockInfo = {};
		if (!GetFileAttributesExW(GameFile(kCopyName).c_str(), GetFileExInfoStandard, &copyInfo) ||
			!GetFileAttributesExW(RunningExePath().c_str(), GetFileExInfoStandard, &stockInfo) ||
			copyInfo.nFileSizeLow != stockInfo.nFileSizeLow || copyInfo.nFileSizeHigh != stockInfo.nFileSizeHigh)
			return false;

		HANDLE file = CreateFileW(GameFile(kCopyName).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		std::vector<BYTE> head(4096);
		DWORD read = 0;
		const bool ok = ReadFile(file, head.data(), static_cast<DWORD>(head.size()), &read, nullptr) && read == head.size();
		CloseHandle(file);
		if (!ok)
			return false;
		const IMAGE_NT_HEADERS32* nt = SameBuildHeaders(head);
		return nt && (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
	}

	// The stock exe's bytes with the one flag set, written next to it. Written to a temporary
	// name and moved into place, so a crash halfway never leaves a truncated copy behind.
	bool WriteCopy(std::string& error)
	{
		std::vector<BYTE> data;
		if (!ReadWholeFile(RunningExePath(), data))
		{
			error = "could not read BBCF.exe (Windows error " + std::to_string(GetLastError()) + ")";
			return false;
		}
		IMAGE_NT_HEADERS32* nt = SameBuildHeaders(data);
		if (!nt)
		{
			error = "BBCF.exe on disk is not the build that is running";
			return false;
		}
		nt->FileHeader.Characteristics |= IMAGE_FILE_LARGE_ADDRESS_AWARE;

		const std::wstring finalPath = GameFile(kCopyName);
		const std::wstring tempPath = finalPath + L".tmp";
		HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			error = "could not create " + std::string(tempPath.begin(), tempPath.end()) + " (Windows error " + std::to_string(GetLastError()) + ")";
			return false;
		}
		DWORD written = 0;
		const bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
		const DWORD writeError = GetLastError();
		CloseHandle(file);
		if (!ok || !MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			error = "could not write BBCF_IM_4GB.exe (Windows error " + std::to_string(ok ? GetLastError() : writeError) + ")";
			DeleteFileW(tempPath.c_str());
			return false;
		}
		return true;
	}

	// This process's command line with its program name replaced, so the copy gets exactly the
	// arguments Steam gave the stock exe.
	std::wstring CommandLineFor(const std::wstring& exePath)
	{
		const wchar_t* line = GetCommandLineW();
		const wchar_t* rest = line;
		if (*rest == L'"')
		{
			++rest;
			while (*rest && *rest != L'"')
				++rest;
			if (*rest == L'"')
				++rest;
		}
		else
		{
			while (*rest && *rest != L' ' && *rest != L'\t')
				++rest;
		}
		return L"\"" + exePath + L"\"" + rest;
	}

	// Starts exePath as the game, inside a job that dies with this process (Steam's Stop button
	// ends the stock process; the game must end with it). Returns the process handle or null.
	HANDLE StartGameProcess(const std::wstring& exePath)
	{
		if (!g_job)
		{
			g_job = CreateJobObjectW(nullptr, nullptr);
			if (g_job)
			{
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
				limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
			}
		}

		wchar_t pid[16] = {};
		swprintf_s(pid, L"%lu", GetCurrentProcessId());
		SetEnvironmentVariableW(kParentEnv, pid);

		std::wstring commandLine = CommandLineFor(exePath);
		wchar_t currentDir[MAX_PATH + 1] = {};
		GetCurrentDirectoryW(MAX_PATH, currentDir);
		STARTUPINFOW si = {};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi = {};
		if (!CreateProcessW(exePath.c_str(), &commandLine[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED,
				nullptr, currentDir, &si, &pi))
		{
			RedirectLog("Could not start %ls (Windows error %lu).", exePath.c_str(), GetLastError());
			SetEnvironmentVariableW(kParentEnv, nullptr);
			return nullptr;
		}
		SetEnvironmentVariableW(kParentEnv, nullptr);
		if (!g_job || !AssignProcessToJobObject(g_job, pi.hProcess))
			RedirectLog("Could not tie pid %lu to this process (Windows error %lu); Stop in Steam may leave it running.",
				pi.dwProcessId, GetLastError());
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);
		RedirectLog("Started %ls as pid %lu.", exePath.c_str(), pi.dwProcessId);
		return pi.hProcess;
	}

	struct WindowSearch
	{
		DWORD pid;
		bool found;
	};

	BOOL CALLBACK FindVisibleWindow(HWND hwnd, LPARAM param)
	{
		auto* search = reinterpret_cast<WindowSearch*>(param);
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid == search->pid && IsWindowVisible(hwnd))
		{
			search->found = true;
			return FALSE;
		}
		return TRUE;
	}

	// Waits for the process to exit. *showedWindow: whether it ever had a visible window.
	DWORD WaitForGameProcess(HANDLE process, bool* showedWindow = nullptr)
	{
		WindowSearch search = { GetProcessId(process), false };
		while (WaitForSingleObject(process, 250) == WAIT_TIMEOUT)
		{
			if (showedWindow && !search.found)
				EnumWindows(&FindVisibleWindow, reinterpret_cast<LPARAM>(&search));
		}
		if (showedWindow)
			*showedWindow = search.found;
		DWORD code = 0;
		GetExitCodeProcess(process, &code);
		return code;
	}

	// What the game's WinMain becomes in the stock process: wait for the copy and hand its exit
	// code to the CRT, which exits as the game normally would.
	int __stdcall RedirectedWinMain(HINSTANCE, HINSTANCE, LPSTR, int)
	{
		bool showedWindow = false;
		const DWORD code = WaitForGameProcess(g_child, &showedWindow);
		const DWORD lived = GetTickCount() - g_childStartTick;
		CloseHandle(g_child);
		g_child = nullptr;
		RedirectLog("BBCF_IM_4GB.exe exited with code 0x%08lX after %lu ms (%s a window).", code, lived,
			showedWindow ? "had shown" : "never showed");
		if (code == 0 || showedWindow || lived >= kBootWindowMs)
			return static_cast<int>(code);

		// It died while starting. Turn redirecting off and run the stock exe instead, so this
		// launch still ends up in the game with the mod. The setting tells the player why.
		WriteSettingDirect(0);
		RedirectLog("That was a boot failure: LargeAddressSpace set to 0 and the stock BBCF.exe started instead.");
		HANDLE stock = StartGameProcess(RunningExePath());
		if (!stock)
			return static_cast<int>(code);
		const DWORD stockCode = WaitForGameProcess(stock);
		CloseHandle(stock);
		return static_cast<int>(stockCode);
	}

	bool RetargetWinMainCall()
	{
		const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		const uintptr_t site = base + kWinMainCallRva;
		if (*reinterpret_cast<const BYTE*>(site) != 0xE8 ||
			site + 5 + *reinterpret_cast<const int32_t*>(site + 1) != base + kWinMainRva)
			return false;
		DWORD oldProtect = 0;
		if (!VirtualProtect(reinterpret_cast<void*>(site), 5, PAGE_EXECUTE_READWRITE, &oldProtect))
			return false;
		*reinterpret_cast<volatile int32_t*>(site + 1) =
			static_cast<int32_t>(reinterpret_cast<uintptr_t>(&RedirectedWinMain) - (site + 5));
		VirtualProtect(reinterpret_cast<void*>(site), 5, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);
		return true;
	}

	// ---- per-exe Windows settings ----
	//
	// Windows keeps some settings per exe path, so BBCF_IM_4GB.exe would not get the ones the
	// player gave BBCF.exe. Mirrored here, before every start of the copy, so a later change to
	// BBCF.exe's settings carries over too, and a removed one is removed:
	//   - Settings > Display > Graphics: which GPU the game runs on (laptops). The GPU the
	//     drivers pick by default needs nothing: BBCF.exe exports NvOptimusEnablement and
	//     AmdPowerXpressRequestHighPerformance, and the copy has the same exports.
	//   - Properties > Compatibility (compatibility mode, run as administrator, DPI, ...). The
	//     "for all users" ones live in HKLM, which needs admin to write, so they are copied
	//     into the current user's key; Windows applies both.
	struct MirroredKey
	{
		const wchar_t* subkey;
		bool alsoMachine;
		const char* what;
	};
	const MirroredKey kMirroredKeys[] = {
		{ L"Software\\Microsoft\\DirectX\\UserGpuPreferences", false, "GPU preference" },
		{ L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers", true, "compatibility settings" },
	};

	// The REG_SZ value whose name is `path` (compared case-insensitively, the way Windows does).
	bool FindPathValue(HKEY root, const wchar_t* subkey, const std::wstring& path, std::wstring& data)
	{
		HKEY key = nullptr;
		if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
			return false;
		bool found = false;
		for (DWORD i = 0; !found; ++i)
		{
			wchar_t name[MAX_PATH * 2] = {};
			wchar_t value[1024] = {};
			DWORD nameLength = MAX_PATH * 2, valueBytes = sizeof(value) - sizeof(wchar_t), type = 0;
			const LONG result = RegEnumValueW(key, i, name, &nameLength, nullptr, &type, reinterpret_cast<BYTE*>(value), &valueBytes);
			if (result == ERROR_NO_MORE_ITEMS)
				break;
			if (result == ERROR_SUCCESS && type == REG_SZ && _wcsicmp(name, path.c_str()) == 0)
			{
				data = value;
				found = true;
			}
		}
		RegCloseKey(key);
		return found;
	}

	void MirrorPerExeSettings()
	{
		const std::wstring stock = RunningExePath();
		const std::wstring copy = GameFile(kCopyName);
		for (const MirroredKey& entry : kMirroredKeys)
		{
			std::wstring wanted;
			const bool has = FindPathValue(HKEY_CURRENT_USER, entry.subkey, stock, wanted) ||
				(entry.alsoMachine && FindPathValue(HKEY_LOCAL_MACHINE, entry.subkey, stock, wanted));
			std::wstring current;
			const bool copyHas = FindPathValue(HKEY_CURRENT_USER, entry.subkey, copy, current);
			if (has == copyHas && (!has || wanted == current))
				continue;

			HKEY key = nullptr;
			if (RegCreateKeyExW(HKEY_CURRENT_USER, entry.subkey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS)
			{
				RedirectLog("Could not open the %s key to mirror BBCF.exe's %s.", "registry", entry.what);
				continue;
			}
			if (has)
			{
				const LONG result = RegSetValueExW(key, copy.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(wanted.c_str()),
					static_cast<DWORD>((wanted.size() + 1) * sizeof(wchar_t)));
				RedirectLog("Mirrored BBCF.exe's %s to BBCF_IM_4GB.exe: \"%ls\" (%s).", entry.what, wanted.c_str(),
					result == ERROR_SUCCESS ? "ok" : "FAILED");
			}
			else
			{
				RegDeleteValueW(key, copy.c_str());
				RedirectLog("BBCF.exe no longer has %s; removed them from BBCF_IM_4GB.exe.", entry.what);
			}
			RegCloseKey(key);
		}
	}

	// ---- firewall ----

	struct FirewallJob
	{
		std::wstring from;
		std::wstring to;
		int missing = -1;
		std::wstring report;
	};

	// On a thread of its own: COM must be initialised the way this needs it, which the game's
	// threads may already have done differently.
	DWORD WINAPI CountMissingFirewallRules(LPVOID param)
	{
		auto* job = static_cast<FirewallJob*>(param);
		if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
		{
			job->missing = FirewallMirror::Mirror(job->from, job->to, false, &job->report);
			CoUninitialize();
		}
		return 0;
	}

	BOOL CALLBACK MinimizeOwnWindow(HWND hwnd, LPARAM)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr)
			ShowWindow(hwnd, SW_MINIMIZE);
		return TRUE;
	}

	std::wstring NewestUpdater()
	{
		const std::wstring candidates[] = { GameFile(L"BBCF_IM\\Updater\\BBCFIMUpdater.exe"), GameFile(L"BBCFIMUpdater.exe") };
		std::wstring best;
		ULARGE_INTEGER bestTime = {};
		for (const std::wstring& path : candidates)
		{
			WIN32_FILE_ATTRIBUTE_DATA info = {};
			if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
				continue;
			ULARGE_INTEGER t;
			t.LowPart = info.ftLastWriteTime.dwLowDateTime;
			t.HighPart = info.ftLastWriteTime.dwHighDateTime;
			if (best.empty() || t.QuadPart > bestTime.QuadPart)
			{
				best = path;
				bestTime = t;
			}
		}
		return best;
	}
}

namespace LaaRedirect
{
	bool OnProcessAttach()
	{
		wchar_t parent[16] = {};
		if (GetEnvironmentVariableW(kParentEnv, parent, 16) > 0)
		{
			g_parentPid = wcstoul(parent, nullptr, 10);
			SetEnvironmentVariableW(kParentEnv, nullptr); // not for anything this process starts
		}

		// Test only: the copy dies while starting, to exercise the stock process's fallback.
		wchar_t failTest[4] = {};
		if (g_parentPid && LargeAddressAware::IsProcessLargeAddressAware() &&
			GetEnvironmentVariableW(L"BBCF_IM_LAA_TEST_BOOT_FAIL", failTest, 4) > 0 && failTest[0] == L'1')
			TerminateProcess(GetCurrentProcess(), 5);

		// The copy itself, or a BBCF.exe somebody flagged by hand: nothing to redirect.
		if (LargeAddressAware::IsProcessLargeAddressAware() || !Is64BitWindows())
			return false;
		if (ReadSettingDirect() != 1)
			return false;

		if (!CopyIsCurrent())
		{
			std::string error;
			if (!WriteCopy(error))
			{
				RedirectLog("BBCF_IM_4GB.exe is missing or out of date and could not be rewritten: %s. Running the stock 2 GB game.", error.c_str());
				return false;
			}
			RedirectLog("Wrote BBCF_IM_4GB.exe from this BBCF.exe.");
		}

		MirrorPerExeSettings();
		g_child = StartGameProcess(GameFile(kCopyName));
		if (!g_child)
			return false; // logged; this process carries on as the stock game
		g_childStartTick = GetTickCount();

		if (!RetargetWinMainCall())
		{
			// Without the retarget this process would run the game a second time. End the copy and
			// be the stock game instead.
			RedirectLog("WinMain's call site did not match; ending the copy and running the stock game.");
			TerminateProcess(g_child, 1);
			CloseHandle(g_child);
			g_child = nullptr;
			return false;
		}
		g_redirecting = true;
		return true;
	}

	bool IsRedirectingProcess()
	{
		return g_redirecting;
	}

	void LogSummary()
	{
		// What a compatibility mode makes this process see (RtlGetVersion, logged elsewhere,
		// gives the real version). Shows whether settings mirrored onto the copy took effect.
#pragma warning(suppress: 4996)
		OSVERSIONINFOW seen = { sizeof(seen) };
#pragma warning(suppress: 4996)
		if (GetVersionExW(&seen))
			ForceLog("[LAA] Windows version as this process sees it: %lu.%lu build %lu.\n",
				seen.dwMajorVersion, seen.dwMinorVersion, seen.dwBuildNumber);

		if (LargeAddressAware::IsProcessLargeAddressAware())
		{
			if (g_parentPid)
				ForceLog("[LAA] Running as BBCF_IM_4GB.exe, started by BBCF.exe (pid %lu).\n", g_parentPid);
			else
				ForceLog("[LAA] Running a large-address-aware exe that the mod did not start.\n");
			return;
		}
		ForceLog("[LAA] Running the stock 2 GB BBCF.exe (LargeAddressSpace=%d, 64-bit Windows=%d). "
			"See BBCF_IM\\LargeAddressSpace.log for redirect decisions.\n",
			Settings::settingsIni.largeAddressSpace, Is64BitWindows() ? 1 : 0);
	}

	bool ShouldAsk()
	{
		return Settings::settingsIni.largeAddressSpace == -1 && !LargeAddressAware::IsProcessLargeAddressAware() && Is64BitWindows();
	}

	bool AcceptAndRestart(std::string& error)
	{
		if (!WriteCopy(error))
			return false;
		Settings::changeSetting("LargeAddressSpace", "1");
		Settings::settingsIni.largeAddressSpace = 1;
		RedirectLog("Player accepted; wrote BBCF_IM_4GB.exe.");
		MirrorPerExeSettings();

		const std::wstring updater = NewestUpdater();

		// Firewall rules naming BBCF.exe (the player allowed it, or Steam's installer added them)
		// are copied for the copy now. That needs admin rights, so Windows asks once; declining
		// only means Windows asks about BBCF_IM_4GB.exe itself the first time it goes online.
		FirewallJob firewall;
		firewall.from = RunningExePath();
		firewall.to = GameFile(kCopyName);
		if (HANDLE thread = CreateThread(nullptr, 0, &CountMissingFirewallRules, &firewall, 0, nullptr))
		{
			WaitForSingleObject(thread, 15000);
			CloseHandle(thread);
		}
		RedirectLog("Firewall rules for BBCF.exe without a BBCF_IM_4GB.exe counterpart: %d.", firewall.missing);
		if (firewall.missing > 0 && !updater.empty())
		{
			const std::wstring logPath = GameFile(L"BBCF_IM\\LargeAddressSpace.log");
			std::wstring parameters = L"--mirror-firewall \"" + firewall.from + L"\" \"" + firewall.to + L"\" --log \"" + logPath + L"\"";
			// The game is closing anyway; out of the way so a fullscreen window cannot hide
			// Windows' permission prompt while this waits for it.
			EnumWindows(&MinimizeOwnWindow, 0);
			SHELLEXECUTEINFOW info = {};
			info.cbSize = sizeof(info);
			info.fMask = SEE_MASK_NOCLOSEPROCESS;
			info.lpVerb = L"runas";
			info.lpFile = updater.c_str();
			info.lpParameters = parameters.c_str();
			info.nShow = SW_HIDE;
			if (ShellExecuteExW(&info) && info.hProcess)
			{
				WaitForSingleObject(info.hProcess, 60000);
				DWORD added = 0;
				GetExitCodeProcess(info.hProcess, &added);
				CloseHandle(info.hProcess);
				RedirectLog("Firewall rules copied to BBCF_IM_4GB.exe: %ld of %d.", static_cast<long>(added), firewall.missing);
			}
			else
			{
				RedirectLog("Firewall rules not copied (Windows error %lu, 1223 = the player declined the admin prompt).", GetLastError());
			}
		}
		if (updater.empty())
		{
			error = "the copy is ready, but BBCFIMUpdater.exe was not found to restart the game. Restart it yourself and it will start in 4 GB mode.";
			return false;
		}
		std::wstring command = L"\"" + updater + L"\" --relaunch-after " + std::to_wstring(GetCurrentProcessId()) +
			L" --exe \"" + RunningExePath() + L"\"";
		STARTUPINFOW si = {};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi = {};
		if (!CreateProcessW(updater.c_str(), &command[0], nullptr, nullptr, FALSE, 0, nullptr,
				GetGameDirectoryW().c_str(), &si, &pi))
		{
			error = "the copy is ready, but the updater could not be started to restart the game (Windows error " +
				std::to_string(GetLastError()) + "). Restart it yourself and it will start in 4 GB mode.";
			return false;
		}
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		ForceLog("[LAA] Accepted 4 GB mode; closing so the updater can restart the game.\n");
		ExitProcess(0);
		return true;
	}
}
