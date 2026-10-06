#include "SaveDataSafety.h"

#include "Core/info.h"
#include "Core/logger.h"
#include "Core/utils.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace
{
	// DllMain runs before the log file is open, so its lines wait here; FlushAttachLog writes
	// them once logging is up.
	std::string g_attachLog;

	void AttachLog(const char* fmt, ...)
	{
		char line[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(line, sizeof(line), fmt, args);
		va_end(args);
		g_attachLog += line;
	}

	// The files the game keeps its local progress in. Replays are left alone: they are not
	// progress, and the folder can be large.
	const wchar_t* const kSaveFiles[] = { L"bbsave.dat", L"bbrd.dat", L"replay_list.dat" };

	std::wstring Root()
	{
		return GamePathW(L"BBCF_IM\\ProfileBackup\\preupdate");
	}

	std::wstring PendingMarker()
	{
		return Root() + L"\\restore_local.pending";
	}

	std::wstring Widen(const std::string& text)
	{
		return std::wstring(text.begin(), text.end());
	}

	bool Exists(const std::wstring& path)
	{
		return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	void EnsureFolder(const std::wstring& path)
	{
		// Creates every missing level; CreateDirectoryW alone only makes the last one.
		for (size_t i = path.find(L'\\', 3); i != std::wstring::npos; i = path.find(L'\\', i + 1))
		{
			CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
		}
		CreateDirectoryW(path.c_str(), nullptr);
	}

	std::wstring Stamp()
	{
		SYSTEMTIME now = {};
		GetLocalTime(&now);
		wchar_t text[32];
		swprintf_s(text, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
		return text;
	}

	bool ReadAll(const std::wstring& path, std::string* out)
	{
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		char buffer[128] = {};
		DWORD read = 0;
		const BOOL ok = ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
		CloseHandle(file);
		if (!ok)
		{
			return false;
		}
		out->assign(buffer, read);
		while (!out->empty() && (out->back() == '\r' || out->back() == '\n' || out->back() == ' '))
		{
			out->pop_back();
		}
		return true;
	}

	bool WriteAll(const std::wstring& path, const std::string& text)
	{
		HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		DWORD written = 0;
		const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
		CloseHandle(file);
		return ok && written == text.size();
	}

	void ApplyPendingLocalRestore()
	{
		std::string version;
		if (!ReadAll(PendingMarker(), &version) || version.empty())
		{
			return;
		}
		// Gone before anything is copied: a restore that fails half way must not repeat on
		// every launch.
		DeleteFileW(PendingMarker().c_str());

		const std::wstring source = SaveDataSafety::SnapshotFolder(version) + L"\\Save";
		const std::wstring save = GamePathW(L"Save");
		const std::wstring kept = GamePathW(L"BBCF_IM\\ProfileBackup\\replaced\\") + Stamp();
		EnsureFolder(kept);

		int restored = 0;
		for (const wchar_t* name : kSaveFiles)
		{
			const std::wstring from = source + L"\\" + name;
			const std::wstring to = save + L"\\" + name;
			if (!Exists(from))
			{
				continue;
			}
			if (Exists(to))
			{
				CopyFileW(to.c_str(), (kept + L"\\" + name).c_str(), FALSE);
			}
			if (CopyFileW(from.c_str(), to.c_str(), FALSE))
			{
				++restored;
			}
			else
			{
				AttachLog("[SaveSafety] could not restore %ls (error %lu)\n", name, GetLastError());
			}
		}
		AttachLog("[SaveSafety] restored %d local save file(s) from the snapshot taken before %s; the replaced files are in %ls\n",
			restored, version.c_str(), kept.c_str());
	}

	void SnapshotLocalSave()
	{
		const std::wstring folder = SaveDataSafety::SnapshotFolder(MOD_VERSION);
		const std::wstring target = folder + L"\\Save";
		if (Exists(target))
		{
			return; // this version already has its snapshot; never overwrite it
		}
		EnsureFolder(target);
		const std::wstring save = GamePathW(L"Save");
		int copied = 0;
		for (const wchar_t* name : kSaveFiles)
		{
			const std::wstring from = save + L"\\" + name;
			if (Exists(from) && CopyFileW(from.c_str(), (target + L"\\" + name).c_str(), TRUE))
			{
				++copied;
			}
		}
		std::string taken;
		{
			SYSTEMTIME now = {};
			GetLocalTime(&now);
			char text[64];
			snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u\n",
				now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
			taken = text;
		}
		WriteAll(folder + L"\\taken.txt", taken);
		AttachLog("[SaveSafety] first run of %s: copied %d local save file(s) to %ls\n", MOD_VERSION, copied, target.c_str());
	}
}

void SaveDataSafety::OnProcessAttach()
{
	ApplyPendingLocalRestore();
	SnapshotLocalSave();
}

void SaveDataSafety::FlushAttachLog()
{
	if (!g_attachLog.empty())
	{
		ForceLog("%s", g_attachLog.c_str());
		g_attachLog.clear();
	}
}

std::wstring SaveDataSafety::SnapshotFolder(const std::string& version)
{
	return Root() + L"\\" + Widen(version);
}

std::vector<SaveDataSafety::Snapshot> SaveDataSafety::List()
{
	std::vector<Snapshot> out;
	WIN32_FIND_DATAW found = {};
	HANDLE search = FindFirstFileW((Root() + L"\\*").c_str(), &found);
	if (search == INVALID_HANDLE_VALUE)
	{
		return out;
	}
	do
	{
		if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || found.cFileName[0] == L'.')
		{
			continue;
		}
		Snapshot snapshot;
		const std::wstring name = found.cFileName;
		snapshot.version.assign(name.begin(), name.end());
		snapshot.folder = Root() + L"\\" + name;
		snapshot.hasLocalSave = Exists(snapshot.folder + L"\\Save\\bbsave.dat");
		FILETIME local = {};
		FileTimeToLocalFileTime(&found.ftCreationTime, &local);
		FileTimeToSystemTime(&local, &snapshot.takenLocal);
		out.push_back(snapshot);
	} while (FindNextFileW(search, &found));
	FindClose(search);

	std::sort(out.begin(), out.end(), [](const Snapshot& a, const Snapshot& b)
	{
		FILETIME fa = {}, fb = {};
		SystemTimeToFileTime(&a.takenLocal, &fa);
		SystemTimeToFileTime(&b.takenLocal, &fb);
		return CompareFileTime(&fa, &fb) > 0;
	});
	return out;
}

bool SaveDataSafety::ScheduleLocalRestore(const std::string& version)
{
	if (!Exists(SnapshotFolder(version) + L"\\Save\\bbsave.dat"))
	{
		return false;
	}
	EnsureFolder(Root());
	return WriteAll(PendingMarker(), version + "\n");
}

void SaveDataSafety::CancelLocalRestore()
{
	DeleteFileW(PendingMarker().c_str());
}

std::string SaveDataSafety::PendingLocalRestore()
{
	std::string version;
	return ReadAll(PendingMarker(), &version) ? version : std::string();
}
