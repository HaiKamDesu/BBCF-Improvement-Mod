#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../Core/FirewallMirror.h"

namespace
{
	struct Handoff
	{
		std::wstring installRoot;
		std::wstring stagedRoot;
		std::wstring backupRoot;
		std::wstring logPath;
		std::wstring bbcfExePath;
		DWORD parentPid = 0;
		std::string tag;
		std::string version;
		bool relaunch = true;
	};

	std::wstring Utf8ToWide(const std::string& value)
	{
		if (value.empty()) return L"";
		const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
		if (len <= 0) return std::wstring(value.begin(), value.end());
		std::wstring result(static_cast<size_t>(len - 1), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &result[0], len);
		return result;
	}

	std::string WideToUtf8(const std::wstring& value)
	{
		if (value.empty()) return "";
		const int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (len <= 0) return "";
		std::string result(static_cast<size_t>(len - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], len, nullptr, nullptr);
		return result;
	}

	std::wstring Combine(const std::wstring& a, const std::wstring& b)
	{
		if (a.empty()) return b;
		if (b.empty()) return a;
		if (a[a.size() - 1] == L'\\' || a[a.size() - 1] == L'/') return a + b;
		return a + L"\\" + b;
	}

	bool Exists(const std::wstring& path, bool dir = false)
	{
		const DWORD attrs = GetFileAttributesW(path.c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES) return false;
		return dir ? ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) : ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
	}

	bool EnsureDir(const std::wstring& path)
	{
		if (path.empty() || Exists(path, true)) return true;
		const size_t pos = path.find_last_of(L"\\/");
		if (pos != std::wstring::npos && !EnsureDir(path.substr(0, pos))) return false;
		return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
	}

	std::wstring Parent(const std::wstring& path)
	{
		const size_t pos = path.find_last_of(L"\\/");
		return pos == std::wstring::npos ? L"" : path.substr(0, pos);
	}

	std::string Stamp()
	{
		std::time_t now = std::time(nullptr);
		std::tm tmValue = {};
		gmtime_s(&tmValue, &now);
		char buffer[32] = {};
		std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d",
			tmValue.tm_year + 1900, tmValue.tm_mon + 1, tmValue.tm_mday,
			tmValue.tm_hour, tmValue.tm_min, tmValue.tm_sec);
		return buffer;
	}

	void Log(const Handoff& h, const char* format, ...)
	{
		EnsureDir(Parent(h.logPath));
		FILE* file = nullptr;
		_wfopen_s(&file, h.logPath.c_str(), L"ab");
		if (!file) return;
		std::string prefix = "[" + Stamp() + "] ";
		fwrite(prefix.data(), 1, prefix.size(), file);
		va_list args;
		va_start(args, format);
		vfprintf(file, format, args);
		va_end(args);
		fwrite("\r\n", 1, 2, file);
		fclose(file);
	}

	std::wstring ReadIniString(const std::wstring& path, const wchar_t* key)
	{
		wchar_t buffer[2048] = {};
		GetPrivateProfileStringW(L"Update", key, L"", buffer, 2048, path.c_str());
		return buffer;
	}

	bool LoadHandoff(const std::wstring& path, Handoff& h)
	{
		h.installRoot = ReadIniString(path, L"InstallRoot");
		h.stagedRoot = ReadIniString(path, L"StagedRoot");
		h.backupRoot = ReadIniString(path, L"BackupRoot");
		h.logPath = ReadIniString(path, L"LogPath");
		h.bbcfExePath = ReadIniString(path, L"BbcfExePath");
		h.tag = WideToUtf8(ReadIniString(path, L"Tag"));
		h.version = WideToUtf8(ReadIniString(path, L"Version"));
		h.parentPid = GetPrivateProfileIntW(L"Update", L"ParentPid", 0, path.c_str());
		h.relaunch = GetPrivateProfileIntW(L"Update", L"Relaunch", 1, path.c_str()) != 0;
		return !h.installRoot.empty() && !h.stagedRoot.empty() && !h.backupRoot.empty() && !h.logPath.empty() && !h.tag.empty();
	}

	bool CopyFileWithDirs(const std::wstring& from, const std::wstring& to)
	{
		EnsureDir(Parent(to));
		return CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE;
	}

	bool BackupIfExists(const Handoff& h, const std::wstring& installRelative, const std::wstring& backupDir, std::vector<std::wstring>& touched)
	{
		const std::wstring src = Combine(h.installRoot, installRelative);
		if (!Exists(src))
			return true;
		const std::wstring dst = Combine(backupDir, installRelative);
		if (!CopyFileWithDirs(src, dst))
		{
			Log(h, "Backup failed: %s -> %s (Windows error %lu)", WideToUtf8(src).c_str(), WideToUtf8(dst).c_str(), GetLastError());
			return false;
		}
		touched.push_back(installRelative);
		return true;
	}

	void Rollback(const Handoff& h, const std::wstring& backupDir, const std::vector<std::wstring>& touched)
	{
		for (size_t i = 0; i < touched.size(); ++i)
		{
			const std::wstring src = Combine(backupDir, touched[i]);
			const std::wstring dst = Combine(h.installRoot, touched[i]);
			if (Exists(src))
				CopyFileWithDirs(src, dst);
		}
	}

	bool WaitForGame(const Handoff& h)
	{
		if (h.parentPid)
		{
			HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, h.parentPid);
			if (process)
			{
				WaitForSingleObject(process, 30000);
				CloseHandle(process);
			}
		}

		const std::wstring dllPath = Combine(h.installRoot, L"dinput8.dll");
		for (int i = 0; i < 120; ++i)
		{
			HANDLE file = CreateFileW(dllPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file != INVALID_HANDLE_VALUE)
			{
				CloseHandle(file);
				return true;
			}
			Sleep(500);
		}
		return false;
	}

	struct IniLine
	{
		std::string raw;
		std::string section;
		std::string key;
		std::string value;
		bool keyValue = false;
	};

	std::string Trim(const std::string& s)
	{
		size_t a = 0;
		while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
		size_t b = s.size();
		while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
		return s.substr(a, b - a);
	}

	std::string KeyId(const std::string& section, const std::string& key)
	{
		return section + "\n" + key;
	}

	bool ReadText(const std::wstring& path, std::string& out)
	{
		out.clear();
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		LARGE_INTEGER size = {};
		if (!GetFileSizeEx(file, &size) || size.QuadPart > 8 * 1024 * 1024) { CloseHandle(file); return false; }
		out.resize(static_cast<size_t>(size.QuadPart));
		DWORD read = 0;
		BOOL ok = out.empty() || ReadFile(file, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
		CloseHandle(file);
		return ok != FALSE;
	}

	bool WriteText(const std::wstring& path, const std::string& text)
	{
		EnsureDir(Parent(path));
		HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		DWORD written = 0;
		BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
		CloseHandle(file);
		return ok && written == text.size();
	}

	std::vector<IniLine> ParseIni(const std::string& text, std::map<std::string, std::string>& values)
	{
		std::vector<IniLine> lines;
		std::string section;
		size_t pos = 0;
		while (pos <= text.size())
		{
			size_t end = text.find('\n', pos);
			std::string raw = end == std::string::npos ? text.substr(pos) : text.substr(pos, end - pos);
			pos = end == std::string::npos ? text.size() + 1 : end + 1;
			IniLine line;
			line.raw = raw;
			std::string t = Trim(raw);
			if (t.size() > 2 && t[0] == '[' && t[t.size() - 1] == ']')
			{
				section = t.substr(1, t.size() - 2);
				line.section = section;
			}
			else if (!t.empty() && t[0] != ';' && t[0] != '#')
			{
				size_t eq = t.find('=');
				if (eq != std::string::npos)
				{
					line.section = section;
					line.key = Trim(t.substr(0, eq));
					line.value = Trim(t.substr(eq + 1));
					line.keyValue = true;
					values[KeyId(section, line.key)] = line.value;
				}
			}
			lines.push_back(line);
		}
		return lines;
	}

	bool MergeIni(const std::wstring& currentPath, const std::wstring& oldDefaultPath, const std::wstring& newDefaultPath)
	{
		std::string currentText;
		std::string oldText;
		std::string newText;
		if (!ReadText(newDefaultPath, newText)) return true;
		ReadText(currentPath, currentText);
		ReadText(oldDefaultPath, oldText);

		std::map<std::string, std::string> currentValues;
		std::map<std::string, std::string> oldValues;
		std::map<std::string, std::string> newValues;
		std::vector<IniLine> currentLines = ParseIni(currentText, currentValues);
		ParseIni(oldText, oldValues);
		std::vector<IniLine> newLines = ParseIni(newText, newValues);

		std::set<std::string> emitted;
		std::string output;
		for (size_t i = 0; i < currentLines.size(); ++i)
		{
			IniLine line = currentLines[i];
			if (!line.keyValue)
			{
				output += line.raw + "\n";
				continue;
			}
			const std::string id = KeyId(line.section, line.key);
			emitted.insert(id);
			std::map<std::string, std::string>::const_iterator newIt = newValues.find(id);
			std::map<std::string, std::string>::const_iterator oldIt = oldValues.find(id);
			if (newIt != newValues.end() && oldIt != oldValues.end() && line.value == oldIt->second)
				line.value = newIt->second;
			output += line.key + "=" + line.value + "\n";
		}

		std::string lastSection;
		for (size_t i = 0; i < newLines.size(); ++i)
		{
			const IniLine& line = newLines[i];
			if (!line.section.empty() && !line.keyValue && line.raw.find('[') != std::string::npos)
				lastSection = line.section;
			if (!line.keyValue)
				continue;
			const std::string id = KeyId(line.section, line.key);
			if (emitted.find(id) != emitted.end())
				continue;
			output += "\n[" + line.section + "]\n";
			output += line.key + "=" + line.value + "\n";
			emitted.insert(id);
		}

		return WriteText(currentPath, output);
	}

	bool Apply(const Handoff& h)
	{
		if (!Exists(Combine(h.installRoot, L"BBCF.exe")))
		{
			Log(h, "Install root rejected: BBCF.exe missing.");
			return false;
		}

		if (!WaitForGame(h))
		{
			Log(h, "Timed out waiting for dinput8.dll to unlock.");
			return false;
		}

		const std::wstring backupDir = Combine(h.backupRoot, Utf8ToWide(h.tag + "-" + Stamp()));
		if (!EnsureDir(backupDir))
		{
			Log(h, "Backup directory creation failed: %s (Windows error %lu)", WideToUtf8(backupDir).c_str(), GetLastError());
			return false;
		}
		std::vector<std::wstring> touched;
		// Everything the update can overwrite, so a failure can be rolled back. The
		// ".default" files are not shipped any more, but the DLL writes them into the install
		// and the merge below reads the backed-up copies as its "old default".
		const std::vector<std::wstring> files = {
			L"dinput8.dll",
			L"BBCF_IM\\Updater\\BBCFIMUpdater.exe",
			L"USER_README.txt",
			L"BBCF_IM\\Updater\\defaults\\settings.ini.default",
			L"BBCF_IM\\Updater\\defaults\\palettes.ini.default"
		};

		for (size_t i = 0; i < files.size(); ++i)
			if (!BackupIfExists(h, files[i], backupDir, touched))
				return false;

		BackupIfExists(h, L"settings.ini", backupDir, touched);
		BackupIfExists(h, L"palettes.ini", backupDir, touched);
		if (!CopyFileWithDirs(Combine(h.installRoot, L"settings.ini"), Combine(h.installRoot, L"settings.ini.bak-before-" + Utf8ToWide(h.tag))))
			Log(h, "settings.ini pre-update backup copy skipped or failed (Windows error %lu)", GetLastError());
		if (!CopyFileWithDirs(Combine(h.installRoot, L"palettes.ini"), Combine(h.installRoot, L"palettes.ini.bak-before-" + Utf8ToWide(h.tag))))
			Log(h, "palettes.ini pre-update backup copy skipped or failed (Windows error %lu)", GetLastError());

		struct CopyOp { const wchar_t* src; const wchar_t* dst; };
		// The package only carries these three; the ini templates and their ".default"
		// copies are written by the DLL itself on the next launch.
		const CopyOp ops[] = {
			{ L"dinput8.dll", L"dinput8.dll" },
			{ L"BBCFIMUpdater.exe", L"BBCF_IM\\Updater\\BBCFIMUpdater.exe" },
			{ L"USER_README.txt", L"USER_README.txt" }
		};

		for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
		{
			const std::wstring src = Combine(h.stagedRoot, ops[i].src);
			if (Exists(src) && !CopyFileWithDirs(src, Combine(h.installRoot, ops[i].dst)))
			{
				Log(h, "Copy failed: %s (Windows error %lu)", WideToUtf8(src).c_str(), GetLastError());
				Rollback(h, backupDir, touched);
				return false;
			}
		}

		// Three-way merge against the ".default" files the previous DLL wrote. A package no
		// longer carries a new default, so MergeIni finds no newDefaultPath and leaves the
		// user's ini untouched; the updated DLL refreshes the defaults on its next launch and
		// fills in any key it does not find. Kept wired up so a package that does carry one
		// (or a future in-DLL merge) still works.
		if (!MergeIni(Combine(h.installRoot, L"settings.ini"),
			Combine(backupDir, L"BBCF_IM\\Updater\\defaults\\settings.ini.default"),
			Combine(h.stagedRoot, L"BBCF_IM\\Updater\\defaults\\settings.ini.default")))
		{
			Log(h, "settings.ini merge failed.");
			Rollback(h, backupDir, touched);
			return false;
		}

		if (!MergeIni(Combine(h.installRoot, L"palettes.ini"),
			Combine(backupDir, L"BBCF_IM\\Updater\\defaults\\palettes.ini.default"),
			Combine(h.stagedRoot, L"BBCF_IM\\Updater\\defaults\\palettes.ini.default")))
		{
			Log(h, "palettes.ini merge failed.");
			Rollback(h, backupDir, touched);
			return false;
		}

		Log(h, "Update applied successfully for %s.", h.tag.c_str());
		return true;
	}

	void Relaunch(const Handoff& h)
	{
		if (!h.relaunch) return;
		// Test only: a copy of the game outside Steam's install (the archived run copy) must
		// not be relaunched through steam://, which always starts the Steam install.
		wchar_t direct[4] = {};
		if (GetEnvironmentVariableW(L"BBCF_IM_RELAUNCH_DIRECT", direct, 4) > 0 && direct[0] == L'1')
		{
			ShellExecuteW(nullptr, L"open", h.bbcfExePath.c_str(), nullptr, h.installRoot.c_str(), SW_SHOWNORMAL);
			return;
		}
		HINSTANCE result = ShellExecuteW(nullptr, L"open", L"steam://rungameid/586140", nullptr, nullptr, SW_SHOWNORMAL);
		if (reinterpret_cast<intptr_t>(result) > 32)
			return;

		ShellExecuteW(nullptr, L"open", h.bbcfExePath.c_str(), nullptr, h.installRoot.c_str(), SW_SHOWNORMAL);
	}

	// ---- Uninstall (--uninstall, started by the mod's Settings > Uninstall) ----
	//
	// Runs from a copy in %TEMP% (the mod extracts it from the DLL), so the version always
	// matches the mod and nothing it deletes is in use by itself. Waits for the game, then
	// removes the mod's files, the registry values 4 GB mode mirrored, and (elevated, once) the
	// firewall rules it copied for BBCF_IM_4GB.exe. BBCF.exe, data\ and Save\ are never
	// touched, apart from the files the mod itself put there.

	struct UninstallStats
	{
		int deleted = 0;
		unsigned long long bytes = 0;
		std::vector<std::wstring> failed;
	};

	bool IsUnder(const std::wstring& root, const std::wstring& path)
	{
		std::wstring prefix = root;
		if (!prefix.empty() && prefix.back() != L'\\')
			prefix += L'\\';
		return path.size() > prefix.size() && _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0 &&
			path.find(L"\\..") == std::wstring::npos;
	}

	void DeleteOne(const Handoff& h, const std::wstring& path, UninstallStats& stats)
	{
		WIN32_FILE_ATTRIBUTE_DATA info = {};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
			return;
		if (!IsUnder(h.installRoot, path))
			return;
		if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
			SetFileAttributesW(path.c_str(), info.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
		if (DeleteFileW(path.c_str()))
		{
			++stats.deleted;
			stats.bytes += (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
			return;
		}
		Log(h, "Could not delete %s (Windows error %lu)", WideToUtf8(path).c_str(), GetLastError());
		stats.failed.push_back(path);
	}

	// A folder and everything in it. Links are removed, never followed.
	void DeleteTree(const Handoff& h, const std::wstring& folder, UninstallStats& stats)
	{
		if (!IsUnder(h.installRoot, folder) || !Exists(folder, true))
			return;
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW(Combine(folder, L"*").c_str(), &data);
		if (find != INVALID_HANDLE_VALUE)
		{
			do
			{
				const std::wstring name = data.cFileName;
				if (name == L"." || name == L"..")
					continue;
				const std::wstring path = Combine(folder, name);
				const bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
				if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
				{
					if (isDir)
						RemoveDirectoryW(path.c_str());
					else
						DeleteOne(h, path, stats);
				}
				else if (isDir)
					DeleteTree(h, path, stats);
				else
					DeleteOne(h, path, stats);
			} while (FindNextFileW(find, &data));
			FindClose(find);
		}
		RemoveDirectoryW(folder.c_str());
	}

	void DeleteMatching(const Handoff& h, const std::wstring& folder, const wchar_t* pattern, UninstallStats& stats,
		bool (*accept)(const std::wstring& name) = nullptr)
	{
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW(Combine(folder, pattern).c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			return;
		do
		{
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			if (accept && !accept(data.cFileName))
				continue;
			DeleteOne(h, Combine(folder, data.cFileName), stats);
		} while (FindNextFileW(find, &data));
		FindClose(find);
	}

	// Converted custom songs: "c" + five digits + "_" + name + ".pac" (CustomMusicConverter),
	// and the ".pac.gain" volume file next to each. The stock tracks are "000_btl_rg.pac" and
	// the like, so this never matches one.
	bool IsCustomSongPac(const std::wstring& name)
	{
		if (name.size() < 12 || (name[0] != L'c' && name[0] != L'C') || name[6] != L'_')
			return false;
		for (int i = 1; i <= 5; ++i)
			if (name[i] < L'0' || name[i] > L'9')
				return false;
		const auto endsWith = [&name](const wchar_t* suffix)
		{
			const size_t n = wcslen(suffix);
			return name.size() > n && _wcsicmp(name.c_str() + name.size() - n, suffix) == 0;
		};
		return endsWith(L".pac") || endsWith(L".pac.gain");
	}

	void DeleteRegistryValue(const Handoff& h, const wchar_t* key, const std::wstring& value, REGSAM extra)
	{
		HKEY handle = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE | extra, &handle) != ERROR_SUCCESS)
			return;
		const LSTATUS status = RegDeleteValueW(handle, value.c_str());
		if (status == ERROR_SUCCESS)
			Log(h, "Removed registry value HKCU\\%s\\%s", WideToUtf8(key).c_str(), WideToUtf8(value).c_str());
		RegCloseKey(handle);
	}

	// Returns how many of the copy's firewall rules are left (0 = none, or all removed).
	int RemoveFirewallCopies(const Handoff& h, const std::wstring& copyPath)
	{
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		std::wstring report;
		const int found = FirewallMirror::RemoveFor(copyPath, false, &report);
		CoUninitialize();
		if (found <= 0)
			return 0;
		Log(h, "%d firewall rule(s) name %s; asking for admin rights to remove them.", found, WideToUtf8(copyPath).c_str());

		wchar_t self[MAX_PATH * 2] = {};
		GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
		const std::wstring parameters = L"--remove-firewall \"" + copyPath + L"\" --log \"" + h.logPath + L"\"";
		SHELLEXECUTEINFOW info = { sizeof(info) };
		info.fMask = SEE_MASK_NOCLOSEPROCESS;
		info.lpVerb = L"runas";
		info.lpFile = self;
		info.lpParameters = parameters.c_str();
		info.nShow = SW_HIDE;
		if (!ShellExecuteExW(&info) || !info.hProcess)
		{
			Log(h, "Firewall cleanup was not allowed (Windows error %lu).", GetLastError());
			return found;
		}
		WaitForSingleObject(info.hProcess, 60000);
		DWORD code = 0;
		GetExitCodeProcess(info.hProcess, &code);
		CloseHandle(info.hProcess);
		const int removed = static_cast<int>(code);
		return removed < 0 ? found : (std::max)(0, found - removed);
	}

	int RunUninstall(const std::wstring& installRoot, const std::vector<DWORD>& waitPids, bool wipe)
	{
		wchar_t temp[MAX_PATH] = {};
		GetTempPathW(MAX_PATH, temp);

		Handoff h;
		h.installRoot = installRoot;
		h.logPath = Combine(temp, L"BBCF_IM_uninstall.log");
		Log(h, "Uninstall started for %s (%s).", WideToUtf8(installRoot).c_str(), wipe ? "everything" : "keeping the player's files");

		for (DWORD pid : waitPids)
		{
			HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
			if (process)
			{
				WaitForSingleObject(process, 60000);
				CloseHandle(process);
			}
		}
		if (!WaitForGame(h))
		{
			Log(h, "dinput8.dll is still in use; giving up.");
			MessageBoxW(nullptr, L"BBCF Improvement Mod could not be uninstalled because the game is still running.\n\n"
				L"Close BlazBlue Centralfiction and use Uninstall again.", L"BBCF IM Uninstall", MB_ICONERROR);
			return 1;
		}

		const std::wstring copyPath = Combine(installRoot, L"BBCF_IM_4GB.exe");
		const int firewallLeft = RemoveFirewallCopies(h, copyPath);

		// 4 GB mode's mirrors of BBCF.exe's GPU preference and compatibility settings, and the
		// profile backup markers. The values are named after the copy's path, so BBCF.exe's own
		// settings are untouched.
		DeleteRegistryValue(h, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", copyPath, KEY_WOW64_64KEY);
		DeleteRegistryValue(h, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers", copyPath, KEY_WOW64_64KEY);
		if (RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\BBCF_IM") == ERROR_SUCCESS)
		{
			RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\BBCF_IM");
			Log(h, "Removed HKCU\\Software\\BBCF_IM");
		}

		UninstallStats stats;
		const std::wstring mod = Combine(installRoot, L"BBCF_IM");
		for (const wchar_t* name : { L"dinput8.dll", L"BBCFIMUpdater.exe", L"USER_README.txt", L"BBCF_IM_4GB.exe",
			L"BBCF_IM_4GB.exe.tmp", L"dinput8.pdb" })
			DeleteOne(h, Combine(installRoot, name), stats);
		DeleteMatching(h, installRoot, L"*.bak-before-*", stats);
		// The mod's temporary copies of the visible replays (ReplayFileManager); the replays
		// themselves, and the archive of older ones, stay.
		DeleteTree(h, Combine(installRoot, L"Save\\Replay\\tmp"), stats);

		if (wipe)
		{
			for (const wchar_t* name : { L"settings.ini", L"palettes.ini", L"menus.ini" })
				DeleteOne(h, Combine(installRoot, name), stats);
			DeleteTree(h, Combine(installRoot, L"slots"), stats);
			DeleteTree(h, mod, stats);
			DeleteTree(h, Combine(installRoot, L"data\\Sound\\BGM\\custom"), stats);
			DeleteTree(h, Combine(installRoot, L"data\\Sound\\BGM\\BBCFIM_Music"), stats);
			DeleteTree(h, Combine(installRoot, L"data\\sound\\bgm_End\\BBCFIM_Music"), stats);
			DeleteMatching(h, Combine(installRoot, L"data\\Sound\\BGM"), L"c*_*.pac*", stats, IsCustomSongPac);
		}
		else
		{
			// Everything that is not the player's: the updater, logs, crash dumps, caches,
			// test dumps. Palettes, music, recordings, settings and save backups stay, so
			// installing the mod again picks up where it left off.
			for (const wchar_t* folder : { L"Updater", L"DebugHistory", L"CrashReports", L"URT_RE_BLOBS", L"URT_RE_HANG" })
				DeleteTree(h, Combine(mod, folder), stats);
			for (const wchar_t* pattern : { L"DEBUG*.txt", L"*.log", L"DCodeBlobFail_*.bin", L"*.tmp", L"*.bak-before-*",
				L"bbsave_backup_before_rollback_*.dat", L"HookAddrCache.bin", L"PaletteCache.bin" })
				DeleteMatching(h, mod, pattern, stats);
		}

		Log(h, "Uninstall finished: %d file(s), %llu bytes removed, %zu could not be removed, %d firewall rule(s) left.",
			stats.deleted, stats.bytes, stats.failed.size(), firewallLeft);

		wchar_t size[64] = {};
		swprintf_s(size, L"%.1f MB", stats.bytes / (1024.0 * 1024.0));
		std::wstring message = L"BBCF Improvement Mod has been uninstalled (" + std::to_wstring(stats.deleted) +
			L" files, " + size + L").\n\nPress OK to start the game again, now without the mod.";
		if (!wipe)
			message += L"\n\nYour settings, palettes, music and recordings were kept in the game folder (settings.ini, palettes.ini, BBCF_IM), so reinstalling picks them up again.";
		if (!stats.failed.empty())
			message += L"\n\n" + std::to_wstring(stats.failed.size()) + L" file(s) could not be removed, starting with:\n" + stats.failed[0];
		if (firewallLeft > 0)
			message += L"\n\n" + std::to_wstring(firewallLeft) + L" Windows Firewall rule(s) for BBCF_IM_4GB.exe are still there (administrator permission was not given). They are harmless, and can be removed in Windows Defender Firewall > Advanced settings.";
		message += L"\n\nDetails: " + h.logPath;
		MessageBoxW(nullptr, message.c_str(), L"BBCF IM Uninstall", stats.failed.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);

		// Start the game again, the way an update does, but only once the mod is really gone:
		// with dinput8.dll still there it would start with the mod.
		Handoff relaunch;
		relaunch.installRoot = installRoot;
		relaunch.bbcfExePath = Combine(installRoot, L"BBCF.exe");
		if (GetFileAttributesW(Combine(installRoot, L"dinput8.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			Log(h, "Starting the game again.");
			Relaunch(relaunch);
		}
		else
			Log(h, "dinput8.dll is still there; not starting the game.");
		return stats.failed.empty() ? 0 : 1;
	}

	// The mod runs this updater from its own %TEMP% folder; remove that folder once this
	// process has exited.
	void DeleteOwnTempFolder()
	{
		wchar_t self[MAX_PATH * 2] = {};
		GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
		const std::wstring folder = Parent(self);
		wchar_t temp[MAX_PATH] = {};
		GetTempPathW(MAX_PATH, temp);
		std::wstring tempRoot = temp;
		if (!IsUnder(tempRoot, folder) || folder.find(L"BBCF_IM_Uninstall") == std::wstring::npos)
			return;
		std::wstring command = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + folder + L"\"";
		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};
		if (CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, tempRoot.c_str(), &si, &pi))
		{
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
	}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::wstring handoffPath;
	DWORD relaunchAfter = 0;
	std::wstring relaunchExe;
	std::wstring firewallFrom, firewallTo, firewallLog;
	bool firewallDryRun = false;
	for (int i = 1; i + 1 < argc; ++i)
	{
		if (std::wstring(argv[i]) == L"--handoff")
			handoffPath = argv[i + 1];
		else if (std::wstring(argv[i]) == L"--relaunch-after")
			relaunchAfter = wcstoul(argv[i + 1], nullptr, 10);
		else if (std::wstring(argv[i]) == L"--exe")
			relaunchExe = argv[i + 1];
		else if (std::wstring(argv[i]) == L"--mirror-firewall" && i + 2 < argc)
		{
			firewallFrom = argv[i + 1];
			firewallTo = argv[i + 2];
		}
		else if (std::wstring(argv[i]) == L"--log")
			firewallLog = argv[i + 1];
	}
	std::wstring uninstallRoot, removeFirewallFor;
	std::vector<DWORD> waitPids;
	bool wipe = false;
	for (int i = 1; i < argc; ++i)
	{
		const std::wstring arg = argv[i];
		if (arg == L"--dry-run")
			firewallDryRun = true;
		else if (arg == L"--wipe")
			wipe = true;
		else if (arg == L"--uninstall" && i + 1 < argc)
			uninstallRoot = argv[i + 1];
		else if (arg == L"--wait-pid" && i + 1 < argc)
			waitPids.push_back(wcstoul(argv[i + 1], nullptr, 10));
		else if (arg == L"--remove-firewall" && i + 1 < argc)
			removeFirewallFor = argv[i + 1];
	}
	if (argv)
		LocalFree(argv);

	if (!removeFirewallFor.empty())
	{
		// Uninstall, started elevated: remove the rules 4 GB mode copied for BBCF_IM_4GB.exe.
		// Exit code: rules removed, or -1 if the firewall could not be read.
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		std::wstring report;
		int removed = 0;
		const int found = FirewallMirror::RemoveFor(removeFirewallFor, true, &report, &removed);
		CoUninitialize();
		if (!firewallLog.empty())
		{
			FILE* file = nullptr;
			if (_wfopen_s(&file, firewallLog.c_str(), L"ab") == 0 && file)
			{
				const std::string text = "[" + Stamp() + "] remove-firewall " + WideToUtf8(removeFirewallFor) + ": found=" +
					std::to_string(found) + " removed=" + std::to_string(removed) + "\r\n" + WideToUtf8(report);
				fwrite(text.data(), 1, text.size(), file);
				fclose(file);
			}
		}
		return found < 0 ? -1 : removed;
	}

	if (!uninstallRoot.empty())
	{
		const int result = RunUninstall(uninstallRoot, waitPids, wipe);
		DeleteOwnTempFolder();
		return result;
	}

	if (!firewallFrom.empty())
	{
		// 4 GB mode (the mod's LaaRedirect.cpp) started elevated: give BBCF_IM_4GB.exe the
		// firewall rules BBCF.exe has. Exit code: rules added, or -1 if the firewall could not
		// be read. --dry-run only lists what is missing.
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		std::wstring report;
		int added = 0;
		const int missing = FirewallMirror::Mirror(firewallFrom, firewallTo, !firewallDryRun, &report, &added);
		CoUninitialize();
		if (!firewallLog.empty())
		{
			FILE* file = nullptr;
			if (_wfopen_s(&file, firewallLog.c_str(), L"ab") == 0 && file)
			{
				const std::string text = "[" + Stamp() + "] mirror-firewall " + (firewallDryRun ? std::string("(dry run) ") : std::string()) +
					WideToUtf8(firewallFrom) + " -> " + WideToUtf8(firewallTo) + ": missing=" + std::to_string(missing) +
					" added=" + std::to_string(added) + "\r\n" + WideToUtf8(report);
				fwrite(text.data(), 1, text.size(), file);
				fclose(file);
			}
		}
		return missing < 0 ? -1 : (firewallDryRun ? missing : added);
	}

	if (relaunchAfter != 0)
	{
		// The mod's 4 GB mode was just accepted (LaaRedirect.cpp): once the game has closed,
		// start it again the way the player did, through Steam, so the mod's redirect runs.
		HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, relaunchAfter);
		if (game)
		{
			WaitForSingleObject(game, 60000);
			CloseHandle(game);
		}
		Sleep(1000); // let Steam notice the game stopped before asking it to start again
		Handoff relaunch;
		relaunch.bbcfExePath = relaunchExe;
		relaunch.installRoot = Parent(relaunchExe);
		Relaunch(relaunch);
		return 0;
	}

	Handoff h;
	if (handoffPath.empty() || !LoadHandoff(handoffPath, h))
		return 2;

	Log(h, "Updater started for %s.", h.tag.c_str());
	const bool ok = Apply(h);
	if (ok)
		Relaunch(h);
	else
		MessageBoxW(nullptr, L"BBCF Improvement Mod update failed. See BBCF_IM\\Updater\\logs\\updater.log.", L"BBCF IM Updater", MB_ICONERROR);
	return ok ? 0 : 1;
}
