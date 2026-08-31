#include "BgmTableProbe.h"
#include "MusicManager.h"
#include "CustomMusicConverter.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Overlay/Logger/ImGuiLogger.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <sstream>

namespace
{
	// VA 0x9DC650 with the exe's 0x400000 image base.
	const uintptr_t kTableRva = 0x005DC650;

	// audioMgr+0x1690, the same pair MusicManager reads.
	const uintptr_t kAudioMgrRva = 0x008903B0;
	const uintptr_t kBgmIdOffset = 0x1690;

	const char* kSubdir = "imtest";

	bool                      g_initialized = false;
	uintptr_t                 g_tableAddr = 0;
	const char*               g_original[BgmTableProbe::kEntryCount] = {};
	// Patched strings must outlive the write; the table only stores the pointer.
	std::string               g_owned[BgmTableProbe::kEntryCount];
	int                       g_lastIndex = -1;
	std::string               g_lastBaseName;
	std::vector<std::string>  g_bgmNames;
	std::vector<std::string>  g_customMp3s;

	void LogProbe(const char* fmt, ...)
	{
		char buf[512];
		va_list args;
		va_start(args, fmt);
		vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
		va_end(args);

		LOG(1, "[BgmProbe] %s", buf);
		if (g_imGuiLogger)
			g_imGuiLogger->Log("[BgmProbe] %s", buf);
	}

	// Kept free of anything needing unwinding so __try is legal here (C2712).
	bool SnapshotPointers(uintptr_t table, const char** out, int count)
	{
		__try
		{
			for (int i = 0; i < count; ++i)
				out[i] = *(const char**)(table + i * sizeof(char*));
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			LogProbe("Snapshot FAILED - exception 0x%08X reading the table\n", GetExceptionCode());
			return false;
		}
	}

	bool WritePointer(int index, const char* value)
	{
		uintptr_t slot = g_tableAddr + index * sizeof(char*);
		DWORD oldProtect = 0;
		if (!VirtualProtect((void*)slot, sizeof(char*), PAGE_READWRITE, &oldProtect))
		{
			LogProbe("VirtualProtect FAILED on slot %d (err %lu)\n", index, GetLastError());
			return false;
		}
		*(const char**)slot = value;
		DWORD ignored = 0;
		VirtualProtect((void*)slot, sizeof(char*), oldProtect, &ignored);
		return true;
	}
}

namespace BgmTableProbe
{
	void Initialize()
	{
		if (g_initialized)
			return;

		HMODULE hMod = GetModuleHandleA("BBCF.exe");
		if (!hMod)
		{
			LogProbe("Initialize FAILED - BBCF.exe module not found\n");
			return;
		}

		g_tableAddr = (uintptr_t)hMod + kTableRva;

		if (!SnapshotPointers(g_tableAddr, g_original, kEntryCount))
			return;

		g_bgmNames.clear();
		for (int i = 0; i < kBgmEntryCount; ++i)
			g_bgmNames.push_back(g_original[i] ? g_original[i] : "(null)");

		g_initialized = true;
		RefreshCustomMp3List();
		LogProbe("Initialized. table=0x%p  [0]=\"%s\"  [8]=\"%s\"  [97]=\"%s\"\n",
			(void*)g_tableAddr,
			g_original[0] ? g_original[0] : "(null)",
			g_original[8] ? g_original[8] : "(null)",
			g_original[97] ? g_original[97] : "(null)");
	}

	bool IsInitialized() { return g_initialized; }
	uintptr_t GetTableAddress() { return g_tableAddr; }
	int GetLastTouchedIndex() { return g_lastIndex; }
	const char* GetLastTouchedBaseName() { return g_lastBaseName.c_str(); }

	const char* GetEntry(int index)
	{
		if (!g_initialized || index < 0 || index >= kEntryCount)
			return nullptr;
		return *(const char**)(g_tableAddr + index * sizeof(char*));
	}

	const char* GetOriginalEntry(int index)
	{
		if (!g_initialized || index < 0 || index >= kEntryCount)
			return nullptr;
		return g_original[index];
	}

	int FindIndexByBaseName(const std::string& baseName)
	{
		if (!g_initialized)
			return -1;
		const std::string wanted = baseName + ".pac";
		for (int i = 0; i < kBgmEntryCount; ++i)
		{
			if (!g_original[i])
				continue;
			// The table's lobby entries differ in case from the files on disk, so
			// compare the way Windows resolves them.
			if (_stricmp(g_original[i], wanted.c_str()) == 0)
				return i;
		}
		return -1;
	}

	int GetLiveBgmId()
	{
		HMODULE hMod = GetModuleHandleA("BBCF.exe");
		if (!hMod)
			return -1;
		__try
		{
			return *(int*)((uintptr_t)hMod + kAudioMgrRva + kBgmIdOffset);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	int GetMusicSelectId()
	{
		return g_gameVals.musicSelect_X ? *g_gameVals.musicSelect_X : -1;
	}

	int GetManagerTrackId()
	{
		return GetMusicManager().GetCurrentTrackId();
	}

	const std::vector<std::string>& GetBgmEntryNames() { return g_bgmNames; }

	bool PatchEntry(int index, const std::string& relPath)
	{
		if (!g_initialized || index < 0 || index >= kEntryCount)
			return false;

		g_owned[index] = relPath;
		if (!WritePointer(index, g_owned[index].c_str()))
			return false;

		const char* readBack = GetEntry(index);
		LogProbe("PATCH [%d] \"%s\" -> \"%s\" (read back: \"%s\")\n",
			index,
			g_original[index] ? g_original[index] : "(null)",
			relPath.c_str(),
			readBack ? readBack : "(null)");
		return readBack && relPath == readBack;
	}

	bool RestoreEntry(int index)
	{
		if (!g_initialized || index < 0 || index >= kEntryCount)
			return false;
		if (!WritePointer(index, g_original[index]))
			return false;
		LogProbe("RESTORE [%d] -> \"%s\"\n", index,
			g_original[index] ? g_original[index] : "(null)");
		return true;
	}

	void RestoreAll()
	{
		if (!g_initialized)
			return;
		for (int i = 0; i < kEntryCount; ++i)
		{
			if (GetEntry(i) != g_original[i])
				WritePointer(i, g_original[i]);
		}
		g_lastIndex = -1;
		g_lastBaseName.clear();
		LogProbe("RESTORE ALL - every entry back to its original pointer\n");
	}

	void RefreshCustomMp3List()
	{
		g_customMp3s.clear();
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA("data/Sound/BGM/custom/*.mp3", &fd);
		if (h == INVALID_HANDLE_VALUE)
			return;
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				g_customMp3s.push_back(fd.cFileName);
		} while (FindNextFileA(h, &fd));
		FindClose(h);
		LogProbe("Found %d custom .mp3 file(s)\n", (int)g_customMp3s.size());
	}

	const std::vector<std::string>& GetCustomMp3s() { return g_customMp3s; }

	bool ConvertAndRedirect(int index, const std::string& mp3File, std::string* statusOut)
	{
		if (!g_initialized || index < 0 || index >= kBgmEntryCount)
		{
			if (statusOut) *statusOut = "Pick a track first";
			return false;
		}
		if (mp3File.empty())
		{
			if (statusOut) *statusOut = "Pick an .mp3 first";
			return false;
		}

		const std::string file = g_original[index] ? g_original[index] : "";
		if (file.size() < 5)
		{
			if (statusOut) *statusOut = "That entry has no usable filename";
			return false;
		}

		const std::string mp3Path = "data/Sound/BGM/custom/" + mp3File;
		const std::string origPath = "data/Sound/BGM/" + file;
		const std::string dstDir = std::string("data/Sound/BGM/") + kSubdir;
		const std::string dstPath = dstDir + "/" + file;
		CreateDirectoryA(dstDir.c_str(), NULL);

		LogProbe("Converting '%s' as a stand-in for \"%s\"...\n", mp3File.c_str(), file.c_str());

		std::string err;
		if (!ConvertMp3ToReplacementPac(mp3Path, origPath, dstPath, &err))
		{
			if (statusOut) *statusOut = "Conversion failed: " + err;
			LogProbe("Conversion FAILED: %s\n", err.c_str());
			return false;
		}

		// Never point an entry at something that isn't really there.
		WIN32_FILE_ATTRIBUTE_DATA info = {};
		if (!GetFileAttributesExA(dstPath.c_str(), GetFileExInfoStandard, &info) ||
			(info.nFileSizeHigh == 0 && info.nFileSizeLow < 1024))
		{
			if (statusOut) *statusOut = "Converted file is missing or implausibly small - not redirecting";
			LogProbe("Post-conversion check FAILED for %s\n", dstPath.c_str());
			return false;
		}
		LogProbe("Converted OK: %s (%lu bytes)\n", dstPath.c_str(), info.nFileSizeLow);

		const std::string relPath = std::string(kSubdir) + "/" + file;
		if (!PatchEntry(index, relPath))
		{
			if (statusOut) *statusOut = "Pointer write or read-back failed";
			return false;
		}

		g_lastIndex = index;
		g_lastBaseName = file.substr(0, file.size() - 4);

		char buf[400];
		sprintf_s(buf, "CHECK 3 armed: '%s' converted (%lu bytes) as a stand-in for %s. "
			"Play that song - you should hear the MP3 instead of the original.",
			mp3File.c_str(), info.nFileSizeLow, file.c_str());
		if (statusOut) *statusOut = buf;
		LogProbe("%s\n", buf);
		return true;
	}

	bool RedirectIndexToSubdir(int index, std::string* statusOut)
	{
		if (!g_initialized || index < 0 || index >= kBgmEntryCount)
		{
			if (statusOut) *statusOut = "Pick a track first";
			return false;
		}

		std::string file = g_original[index] ? g_original[index] : "";
		if (file.size() < 5)
		{
			if (statusOut) *statusOut = "That entry has no usable filename";
			return false;
		}
		const std::string base = file.substr(0, file.size() - 4); // strip ".pac"

		const std::string srcPath = "data/Sound/BGM/" + file;
		const std::string dstDir = std::string("data/Sound/BGM/") + kSubdir;
		const std::string dstPath = dstDir + "/" + file;

		CreateDirectoryA(dstDir.c_str(), NULL);
		if (!CopyFileA(srcPath.c_str(), dstPath.c_str(), FALSE))
		{
			char buf[320];
			sprintf_s(buf, "Copy failed: %s -> %s (err %lu). If the source is missing this "
				"track lives in another folder - pick a different one.",
				srcPath.c_str(), dstPath.c_str(), GetLastError());
			if (statusOut) *statusOut = buf;
			LogProbe("%s\n", buf);
			return false;
		}
		LogProbe("Copied %s -> %s\n", srcPath.c_str(), dstPath.c_str());

		const std::string relPath = std::string(kSubdir) + "/" + file;
		if (!PatchEntry(index, relPath))
		{
			if (statusOut) *statusOut = "Pointer write or read-back failed";
			return false;
		}

		g_lastIndex = index;
		g_lastBaseName = base;

		char buf[320];
		sprintf_s(buf, "CHECK 1 armed on [%d] %s -> %s. Start a match using this song. "
			"Music = subdirectories work; silence = they don't.",
			index, file.c_str(), relPath.c_str());
		if (statusOut) *statusOut = buf;
		LogProbe("%s\n", buf);
		return true;
	}

	bool RedirectIndexToMissing(int index, std::string* statusOut)
	{
		if (!g_initialized || index < 0 || index >= kBgmEntryCount)
		{
			if (statusOut) *statusOut = "Pick a track first";
			return false;
		}

		const std::string file = g_original[index] ? g_original[index] : "";
		const std::string relPath = std::string(kSubdir) + "/__does_not_exist__.pac";
		if (!PatchEntry(index, relPath))
		{
			if (statusOut) *statusOut = "Pointer write or read-back failed";
			return false;
		}

		g_lastIndex = index;
		g_lastBaseName = file.size() > 4 ? file.substr(0, file.size() - 4) : file;

		char buf[320];
		sprintf_s(buf, "CHECK 2 armed on [%d] %s -> a missing file. Music should keep playing "
			"NOW; after a reload, silence = table re-read, music = bank cached.",
			index, file.c_str());
		if (statusOut) *statusOut = buf;
		LogProbe("%s\n", buf);
		return true;
	}
}
