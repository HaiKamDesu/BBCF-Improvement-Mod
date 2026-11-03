#include "ModCleanup.h"

#include "Core/info.h"
#include "Core/logger.h"
#include "Core/SaveDataSafety.h"
#include "Core/utils.h"
#include "Network/ProfileSync.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
	struct CategoryFiles
	{
		std::vector<std::wstring> files;
		// Folders emptied by this category, deepest first; removed after their files.
		std::vector<std::wstring> folders;
		ModCleanup::CategoryTotals totals;
	};

	std::mutex g_mutex;
	std::atomic<bool> g_busy{ false };
	ModCleanup::ScanSummary g_scan;
	ModCleanup::ClearSummary g_lastClear;

	std::wstring Lower(std::wstring text)
	{
		for (wchar_t& c : text)
			c = (wchar_t)towlower(c);
		return text;
	}

	// Every path handed to DeleteFileW must sit under the game folder. The scan only builds
	// paths from GamePathW, so this is a last line of defence, not the filter.
	bool IsInsideGameFolder(const std::wstring& path)
	{
		std::wstring root = Lower(GetGameDirectoryW());
		if (root.empty())
			return false;
		if (root.back() != L'\\')
			root += L'\\';
		const std::wstring lowered = Lower(path);
		return lowered.size() > root.size() && lowered.compare(0, root.size(), root) == 0 &&
			lowered.find(L"\\..\\") == std::wstring::npos;
	}

	uint64_t FileSize(const WIN32_FIND_DATAW& data)
	{
		return ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
	}

	void AddFile(CategoryFiles& out, const std::wstring& path, uint64_t size)
	{
		out.files.push_back(path);
		out.totals.files++;
		out.totals.bytes += size;
	}

	// Files in one folder matching a wildcard; no recursion.
	void AddMatching(CategoryFiles& out, const std::wstring& folder, const wchar_t* pattern)
	{
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW((folder + L"\\" + pattern).c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			return;
		do
		{
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			AddFile(out, folder + L"\\" + data.cFileName, FileSize(data));
		} while (FindNextFileW(find, &data));
		FindClose(find);
	}

	// Everything inside a folder; the folder itself stays. Links (junctions, symlinks) are never
	// followed: they are removed as entries, which leaves whatever they point at alone.
	void AddContents(CategoryFiles& out, const std::wstring& folder)
	{
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			return;
		std::vector<std::wstring> subfolders;
		do
		{
			const std::wstring name = data.cFileName;
			if (name == L"." || name == L"..")
				continue;
			const std::wstring path = folder + L"\\" + name;
			if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
			{
				if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					out.folders.push_back(path);
				else
					AddFile(out, path, 0);
				continue;
			}
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				subfolders.push_back(path);
			else
				AddFile(out, path, FileSize(data));
		} while (FindNextFileW(find, &data));
		FindClose(find);

		for (const std::wstring& sub : subfolders)
		{
			AddContents(out, sub);
			out.folders.push_back(sub);
		}
	}

	// Leftovers of the write-to-.tmp-then-rename pattern several features use (settings,
	// caches, controllers, rank history, music conversion). The pending update's own folder is
	// skipped: a half-written file there belongs to a download in progress.
	void AddTempFiles(CategoryFiles& out, const std::wstring& folder, const std::wstring& skip)
	{
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			return;
		do
		{
			const std::wstring name = data.cFileName;
			if (name == L"." || name == L"..")
				continue;
			const std::wstring path = folder + L"\\" + name;
			if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
				continue;
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				if (Lower(path) != Lower(skip))
					AddTempFiles(out, path, skip);
				continue;
			}
			const std::wstring lowered = Lower(name);
			if (lowered.size() > 4 && lowered.compare(lowered.size() - 4, 4, L".tmp") == 0)
				AddFile(out, path, FileSize(data));
		} while (FindNextFileW(find, &data));
		FindClose(find);
	}

	void AddFileIfExists(CategoryFiles& out, const std::wstring& path)
	{
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) ||
			(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			return;
		AddFile(out, path, ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow);
	}

	std::wstring Widen(const std::string& text)
	{
		return std::wstring(text.begin(), text.end());
	}

	// Snapshots taken before older mod versions. The current version's is kept (it is the
	// one a broken update would need), and so is any snapshot a restore is scheduled from.
	void AddOldSaveSnapshots(CategoryFiles& out, const std::wstring& mod)
	{
		const std::wstring root = mod + L"\\ProfileBackup\\preupdate";
		const std::wstring keep[] = {
			Lower(Widen(MOD_VERSION)),
			Lower(Widen(SaveDataSafety::PendingLocalRestore())),
			Lower(Widen(ProfileSync::PendingSnapshotRestore())),
		};

		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &data);
		if (find != INVALID_HANDLE_VALUE)
		{
			do
			{
				const std::wstring name = data.cFileName;
				if (name == L"." || name == L".." || !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
					(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
					continue;
				bool kept = false;
				for (const std::wstring& k : keep)
					kept = kept || (!k.empty() && Lower(name) == k);
				if (kept)
					continue;
				const std::wstring path = root + L"\\" + name;
				AddContents(out, path);
				out.folders.push_back(path);
			} while (FindNextFileW(find, &data));
			FindClose(find);
		}

		// The save files a restore replaced, kept in case the restore was a mistake.
		AddContents(out, mod + L"\\ProfileBackup\\replaced");
	}

	void ScanCategory(ModCleanup::Category category, CategoryFiles& out)
	{
		const std::wstring game = GetGameDirectoryW();
		const std::wstring mod = GamePathW(L"BBCF_IM");
		switch (category)
		{
		case ModCleanup::Cat_DebugHistory:
			AddContents(out, mod + L"\\DebugHistory");
			break;
		case ModCleanup::Cat_DiagnosticLogs:
			AddMatching(out, mod, L"DEBUG_DCodeIncident_*.txt");
			AddMatching(out, mod, L"DCodeBlobFail_*.bin");
			AddFileIfExists(out, mod + L"\\DCodeIncidents.log");
			AddFileIfExists(out, mod + L"\\FrameStallIncidents.log");
			AddFileIfExists(out, mod + L"\\LargeAddressSpace.log");
			AddFileIfExists(out, mod + L"\\URT_RE_TRACE.log");
			break;
		case ModCleanup::Cat_CrashReports:
			AddContents(out, mod + L"\\CrashReports");
			// BBCF's own crash handler (SteamAPI_WriteMiniDump) and its exit-time messages.
			AddMatching(out, game, L"bbcf_*_crash_*.mdmp");
			AddFileIfExists(out, game + L"\\assert.txt");
			AddFileIfExists(out, game + L"\\leak_report.txt");
			break;
		case ModCleanup::Cat_ConfigBackups:
			// The updater's settings.ini/palettes.ini.bak-before-<version>, plus hand-made ones.
			AddMatching(out, game, L"*.bak-before-*");
			AddMatching(out, mod, L"*.bak-before-*");
			AddContents(out, mod + L"\\SettingsBackups");
			break;
		case ModCleanup::Cat_OldVersions:
			// What the updater keeps after an update; nothing reads it back. Updater\stage
			// (a pending update), state.ini, defaults\ and the updater itself are left alone.
			AddContents(out, mod + L"\\Updater\\backups");
			AddContents(out, mod + L"\\Updater\\downloads");
			AddContents(out, mod + L"\\Updater\\handoff");
			AddContents(out, mod + L"\\Updater\\logs");
			break;
		case ModCleanup::Cat_OldSaveSnapshots:
			AddOldSaveSnapshots(out, mod);
			break;
		case ModCleanup::Cat_TempFiles:
			AddMatching(out, game, L"*.tmp");
			AddTempFiles(out, mod, mod + L"\\Updater\\stage");
			break;
		case ModCleanup::Cat_Caches:
			AddFileIfExists(out, mod + L"\\HookAddrCache.bin");
			AddFileIfExists(out, mod + L"\\PaletteCache.bin");
			AddFileIfExists(out, mod + L"\\Updater\\releases-cache.txt");
			break;
		case ModCleanup::Cat_DeveloperData:
			AddContents(out, mod + L"\\URT_RE_BLOBS");
			AddContents(out, mod + L"\\URT_RE_HANG");
			AddMatching(out, mod, L"bbsave_backup_before_rollback_*.dat");
			AddFileIfExists(out, game + L"\\dinput8.pdb");
			break;
		default:
			break;
		}
	}

	void RunScan()
	{
		ModCleanup::ScanSummary summary;
		for (int i = 0; i < ModCleanup::Cat_Count; i++)
		{
			CategoryFiles files;
			ScanCategory((ModCleanup::Category)i, files);
			summary.totals[i] = files.totals;
		}
		summary.valid = true;

		std::lock_guard<std::mutex> lock(g_mutex);
		g_scan = summary;
	}

	void RunClear(std::vector<bool> selected)
	{
		ModCleanup::ClearSummary result;
		for (int i = 0; i < ModCleanup::Cat_Count; i++)
		{
			if (!selected[i])
				continue;
			CategoryFiles files;
			ScanCategory((ModCleanup::Category)i, files);
			int deleted = 0;
			uint64_t freed = 0;
			for (const std::wstring& path : files.files)
			{
				if (!IsInsideGameFolder(path))
				{
					result.failed++;
					LOG(0, "[Cleanup] refused a path outside the game folder: %ls\n", path.c_str());
					continue;
				}
				WIN32_FILE_ATTRIBUTE_DATA info = {};
				const bool haveInfo = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) != 0;
				if (haveInfo && (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
					SetFileAttributesW(path.c_str(), info.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
				if (DeleteFileW(path.c_str()))
				{
					deleted++;
					if (haveInfo)
						freed += ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
					continue;
				}
				const DWORD error = GetLastError();
				if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
					continue;
				if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_ACCESS_DENIED)
					result.skippedInUse++;
				else
					result.failed++;
				LOG(1, "[Cleanup] could not delete %ls (error %lu)\n", path.c_str(), error);
			}
			for (const std::wstring& folder : files.folders)
			{
				if (IsInsideGameFolder(folder))
					RemoveDirectoryW(folder.c_str()); // fails harmlessly if something was skipped
			}
			result.deletedFiles += deleted;
			result.freedBytes += freed;
			LOG(1, "[Cleanup] %s: deleted %d of %d files, %llu bytes\n", ModCleanup::CategoryLabel((ModCleanup::Category)i),
				deleted, files.totals.files, (unsigned long long)freed);
		}
		result.valid = true;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_lastClear = result;
		}
		RunScan();
	}

	void RunOnWorker(std::function<void()> work)
	{
		bool expected = false;
		if (!g_busy.compare_exchange_strong(expected, true))
			return;
		std::thread([work]()
		{
			work();
			g_busy = false;
		}).detach();
	}
}

namespace ModCleanup
{
	const char* CategoryLabel(Category category)
	{
		switch (category)
		{
		case Cat_DebugHistory: return "Old debug logs";
		case Cat_DiagnosticLogs: return "Diagnostic logs";
		case Cat_CrashReports: return "Crash reports";
		case Cat_ConfigBackups: return "Settings and palette backups";
		case Cat_OldVersions: return "Old mod versions and update downloads";
		case Cat_OldSaveSnapshots: return "Save backups from older mod versions";
		case Cat_TempFiles: return "Broken leftover files";
		case Cat_Caches: return "Caches";
		case Cat_DeveloperData: return "Developer test data";
		default: return "";
		}
	}

	const char* CategoryDescription(Category category)
	{
		switch (category)
		{
		case Cat_DebugHistory:
			return "Logs from previous sessions (BBCF_IM\\DebugHistory). The log of the session you are playing now is kept.";
		case Cat_DiagnosticLogs:
			return "Extra logs the mod writes when something goes wrong online or the game stutters (D-Code incidents, frame stalls, 4 GB mode). Only useful when sending a bug report.";
		case Cat_CrashReports:
			return "Crash dumps from the mod (BBCF_IM\\CrashReports) and from the game itself (bbcf_*_crash_*.mdmp, assert.txt in the game folder). Only useful when sending a bug report.";
		case Cat_ConfigBackups:
			return "The copies of settings.ini and palettes.ini the updater makes before each update (*.bak-before-*). Your current settings and palettes are not touched.";
		case Cat_OldVersions:
			return "Every previous mod version the updater backed up, and the update zips it downloaded. Nothing uses them after the update.";
		case Cat_OldSaveSnapshots:
			return "Copies of your save data taken before older mod versions first ran. The copy for this version, and any restore you have scheduled, are kept; older ones can no longer be restored from the Online page.";
		case Cat_TempFiles:
			return "Half-written .tmp files left behind when the game closed in the middle of saving something.";
		case Cat_Caches:
			return "Files the mod rebuilds by itself. The next launch takes a second or two longer while they are rebuilt.";
		case Cat_DeveloperData:
			return "Test dumps and debug symbols from mod development. Players normally have none of these.";
		default:
			return "";
		}
	}

	void StartScan()
	{
		RunOnWorker([]() { RunScan(); });
	}

	void StartClear(const bool selected[Cat_Count])
	{
		std::vector<bool> copy(selected, selected + Cat_Count);
		RunOnWorker([copy]() { RunClear(copy); });
	}

	bool IsBusy()
	{
		return g_busy;
	}

	ScanSummary GetScan()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_scan;
	}

	ClearSummary GetLastClear()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_lastClear;
	}

	std::string FormatBytes(uint64_t bytes)
	{
		char text[32];
		if (bytes >= 1024ull * 1024 * 1024)
			snprintf(text, sizeof(text), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
		else if (bytes >= 1024ull * 1024)
			snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024));
		else if (bytes >= 1024)
			snprintf(text, sizeof(text), "%.0f KB", bytes / 1024.0);
		else
			snprintf(text, sizeof(text), "%llu bytes", (unsigned long long)bytes);
		return text;
	}
}
