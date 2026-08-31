#include "BgmReplacementManager.h"
#include "CustomMusicConverter.h"
#include "MusicManager.h"

#include "Core/logger.h"
#include "Overlay/Logger/ImGuiLogger.h"

#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>

namespace
{
	// VA 0x9DC650 with the exe's 0x400000 image base. Must be computed from the module
	// base at runtime - the game relocates (observed loading at 0x470000).
	const uintptr_t kTableRva = 0x005DC650;

	const int kEntryCount = 175;
	const int kBgmEntryCount = 160;

	// Replacements live in a subfolder of whichever directory holds the original, so no
	// shipped file is ever touched and Steam's file verification has nothing to restore.
	const char* kReplacementDir = "BBCFIM_Music";

	// Only two prefixes are used for BGM; a track's original tells us which one applies.
	const char* kBgmDir = "data/Sound/BGM";
	const char* kBgmEndDir = "data/sound/bgm_End";

	const char* kSettingsPath = "BBCF_IM/bgm_replacements.ini";

	void LogRepl(const char* fmt, ...)
	{
		char buf[512];
		va_list args;
		va_start(args, fmt);
		vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
		va_end(args);

		LOG(1, "[BgmReplace] %s", buf);
		if (g_imGuiLogger)
			g_imGuiLogger->Log("[BgmReplace] %s", buf);
	}

	bool FileExists(const std::string& path)
	{
		WIN32_FILE_ATTRIBUTE_DATA info = {};
		return GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info) != 0 &&
			!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	std::string FileNameOf(const std::string& path)
	{
		const size_t slash = path.find_last_of("/\\");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	// Kept free of anything needing unwinding so __try is legal here (C2712).
	bool ReadPointers(uintptr_t table, const char** out, int count)
	{
		__try
		{
			for (int i = 0; i < count; ++i)
				out[i] = *(const char**)(table + i * sizeof(char*));
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
}

BgmReplacementManager& BgmReplacementManager::GetInstance()
{
	static BgmReplacementManager instance;
	return instance;
}

// ---------------------------------------------------------------------------------------
// Table access
// ---------------------------------------------------------------------------------------

bool BgmReplacementManager::SnapshotTable()
{
	HMODULE hMod = GetModuleHandleA("BBCF.exe");
	if (!hMod)
	{
		LogRepl("BBCF.exe module not found - replacements unavailable\n");
		return false;
	}

	m_tableAddr = (uintptr_t)hMod + kTableRva;
	if (!ReadPointers(m_tableAddr, m_original, kEntryCount))
	{
		LogRepl("Could not read the filename table at 0x%p\n", (void*)m_tableAddr);
		m_tableAddr = 0;
		return false;
	}

	LogRepl("Filename table at 0x%p ([0]=\"%s\")\n", (void*)m_tableAddr,
		m_original[0] ? m_original[0] : "(null)");
	return true;
}

bool BgmReplacementManager::WritePointer(int tableIndex, const char* value)
{
	if (!m_tableAddr || tableIndex < 0 || tableIndex >= kEntryCount)
		return false;

	const uintptr_t slot = m_tableAddr + tableIndex * sizeof(char*);
	DWORD oldProtect = 0;
	if (!VirtualProtect((void*)slot, sizeof(char*), PAGE_READWRITE, &oldProtect))
	{
		LogRepl("VirtualProtect failed on entry %d (error %lu)\n", tableIndex, GetLastError());
		return false;
	}
	*(const char**)slot = value;
	DWORD ignored = 0;
	VirtualProtect((void*)slot, sizeof(char*), oldProtect, &ignored);
	return true;
}

bool BgmReplacementManager::ApplyPointer(int tableIndex)
{
	auto it = m_assignments.find(tableIndex);
	if (it == m_assignments.end())
		return false;

	// The one rule that must never be broken: a table entry pointing at a file that isn't
	// there hangs the game during match load rather than falling back to silence.
	if (!LooksLikeUsablePac(it->second.pacPath))
	{
		it->second.state = BgmReplacementState::Missing;
		it->second.error = "The converted file is missing - the original is being used instead";
		LogRepl("REFUSING to point entry %d at missing file \"%s\"\n",
			tableIndex, it->second.pacPath.c_str());
		RestorePointer(tableIndex);
		return false;
	}

	// Point the entry back at the shipped string before touching the buffer it may
	// currently reference - reassigning m_owned frees that memory.
	RestorePointer(tableIndex);
	m_owned[tableIndex] = it->second.relPath;
	if (!WritePointer(tableIndex, m_owned[tableIndex].c_str()))
	{
		it->second.state = BgmReplacementState::Failed;
		it->second.error = "Could not write to the game's filename table";
		return false;
	}

	it->second.state = BgmReplacementState::Active;
	it->second.error.clear();
	LogRepl("Entry %d: \"%s\" -> \"%s\"\n", tableIndex,
		m_original[tableIndex] ? m_original[tableIndex] : "(null)", it->second.relPath.c_str());
	return true;
}

bool BgmReplacementManager::RestorePointer(int tableIndex)
{
	if (tableIndex < 0 || tableIndex >= kEntryCount)
		return false;
	return WritePointer(tableIndex, m_original[tableIndex]);
}

// ---------------------------------------------------------------------------------------
// Catalogue
// ---------------------------------------------------------------------------------------

void BgmReplacementManager::BuildCatalogue()
{
	m_tracks.clear();

	// MusicManager already carries human-readable names and categories for the shipped
	// tracks; match them up by filename so the browser reads like a track list rather
	// than a list of .pac files.
	const auto& known = MusicManager::GetInstance().GetAllTracks();

	for (int i = 0; i < kBgmEntryCount; ++i)
	{
		const char* raw = m_original[i];
		if (!raw)
			continue;

		std::string fileName(raw);
		if (fileName.size() < 5)
			continue;
		// Voice entries are format strings, not real filenames; they never reach here
		// because they live past kBgmEntryCount, but guard anyway.
		if (fileName.find('%') != std::string::npos)
			continue;

		BgmReplaceableTrack track;
		track.tableIndex = i;
		track.fileName = fileName;
		track.baseName = fileName.substr(0, fileName.size() - 4);

		for (const auto& k : known)
		{
			const char* f = MusicManager::GetBgmFilename(k.id);
			if (f && _stricmp(f, track.baseName.c_str()) == 0)
			{
				track.displayName = k.name;
				track.category = k.category;
				break;
			}
		}
		if (track.displayName.empty())
			track.displayName = track.baseName;
		if (track.category.empty())
			track.category = "other";

		// Which folder ships this track decides where its replacement has to live, since
		// the game prefixes the table string differently for the two BGM directories.
		if (FileExists(std::string(kBgmDir) + "/" + fileName))
			track.originalDir = kBgmDir;
		else if (FileExists(std::string(kBgmEndDir) + "/" + fileName))
			track.originalDir = kBgmEndDir;

		m_tracks.push_back(track);
	}

	int installed = 0;
	for (const auto& t : m_tracks)
		if (!t.originalDir.empty()) ++installed;
	LogRepl("Catalogue: %d tracks, %d installed and replaceable\n",
		(int)m_tracks.size(), installed);
}

const BgmReplaceableTrack* BgmReplacementManager::FindTrack(int tableIndex) const
{
	for (const auto& t : m_tracks)
		if (t.tableIndex == tableIndex)
			return &t;
	return nullptr;
}

// ---------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------

void BgmReplacementManager::Initialize()
{
	if (m_initialized)
		return;
	if (!SnapshotTable())
		return;

	BuildCatalogue();
	m_initialized = true;

	Load();

	// Re-validate every saved assignment against disk before touching a single pointer.
	for (auto& entry : m_assignments)
	{
		if (LooksLikeUsablePac(entry.second.pacPath))
		{
			ApplyPointer(entry.first);
		}
		else
		{
			entry.second.state = BgmReplacementState::Missing;
			entry.second.error = FileExists(entry.second.sourcePath)
				? "The converted file is gone - use Rebuild to make it again"
				: "Both the converted file and the original MP3 are gone";
			LogRepl("Entry %d assignment is stale (\"%s\")\n",
				entry.first, entry.second.pacPath.c_str());
		}
	}

	LogRepl("Initialized: %d assignment(s), %d active\n",
		GetAssignedCount(), GetActiveCount());
}

bool BgmReplacementManager::LooksLikeUsablePac(const std::string& path)
{
	WIN32_FILE_ATTRIBUTE_DATA info = {};
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info))
		return false;
	if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return false;
	// A real BGM .pac is hundreds of KB; anything tiny is a truncated or bogus write.
	return info.nFileSizeHigh > 0 || info.nFileSizeLow >= 1024;
}

// ---------------------------------------------------------------------------------------
// Conversion queue
// ---------------------------------------------------------------------------------------

void BgmReplacementManager::StartNextJob()
{
	if (m_inFlight.valid() || m_pending.empty())
		return;

	const Job job = m_pending.front();
	m_pending.pop_front();
	m_convertingIndex = job.tableIndex;

	LogRepl("Converting \"%s\" for entry %d...\n",
		FileNameOf(job.mp3Path).c_str(), job.tableIndex);

	// Off the game thread: a full-length transcode takes seconds and would otherwise
	// stall the render loop.
	m_inFlight = std::async(std::launch::async, [job]() {
		JobResult result;
		result.tableIndex = job.tableIndex;
		std::string error;
		result.ok = ConvertMp3ToReplacementPac(job.mp3Path, job.originalPac, job.outPac, &error);
		result.error = error;
		return result;
	});
}

void BgmReplacementManager::Update()
{
	if (!m_initialized)
		return;

	if (m_inFlight.valid() &&
		m_inFlight.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
	{
		const JobResult result = m_inFlight.get();
		m_convertingIndex = -1;
		++m_batchDone;

		auto it = m_assignments.find(result.tableIndex);
		if (it == m_assignments.end())
		{
			// Unassigned while it was converting - clean up what the worker wrote.
			const BgmReplaceableTrack* track = FindTrack(result.tableIndex);
			if (result.ok && track && !track->originalDir.empty())
			{
				const std::string stray =
					track->originalDir + "/" + kReplacementDir + "/" + track->fileName;
				DeleteFileA(stray.c_str());
				LogRepl("Entry %d was undone mid-conversion; removed \"%s\"\n",
					result.tableIndex, stray.c_str());
			}
		}
		else
		{
			if (result.ok)
			{
				ApplyPointer(result.tableIndex);
			}
			else
			{
				it->second.state = BgmReplacementState::Failed;
				it->second.error = result.error.empty()
					? "Could not convert that file" : result.error;
				LogRepl("Entry %d conversion FAILED: %s\n",
					result.tableIndex, it->second.error.c_str());
			}
		}
		Save();
	}

	StartNextJob();

	if (!IsBusy() && m_batchTotal != 0)
	{
		m_batchTotal = 0;
		m_batchDone = 0;
	}
}

int BgmReplacementManager::GetQueueLength() const
{
	return (int)m_pending.size() + (m_inFlight.valid() ? 1 : 0);
}

float BgmReplacementManager::GetBatchProgress() const
{
	if (m_batchTotal <= 0)
		return 1.0f;
	const float p = (float)m_batchDone / (float)m_batchTotal;
	return p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
}

// ---------------------------------------------------------------------------------------
// Assignment
// ---------------------------------------------------------------------------------------

void BgmReplacementManager::Assign(int tableIndex, const std::string& mp3Path)
{
	if (!m_initialized)
		return;

	const BgmReplaceableTrack* track = FindTrack(tableIndex);
	if (!track)
		return;

	if (track->originalDir.empty())
	{
		Assignment bad;
		bad.sourcePath = mp3Path;
		bad.state = BgmReplacementState::Unavailable;
		bad.error = "This track isn't installed, so there's nothing to replace";
		m_assignments[tableIndex] = bad;
		return;
	}

	if (!FileExists(mp3Path))
	{
		Assignment bad;
		bad.sourcePath = mp3Path;
		bad.state = BgmReplacementState::Failed;
		bad.error = "That file no longer exists";
		m_assignments[tableIndex] = bad;
		return;
	}

	const std::string outDir = track->originalDir + "/" + kReplacementDir;
	CreateDirectoryA(outDir.c_str(), NULL);

	Assignment assignment;
	assignment.sourcePath = mp3Path;
	assignment.pacPath = outDir + "/" + track->fileName;
	assignment.relPath = std::string(kReplacementDir) + "/" + track->fileName;
	assignment.state = BgmReplacementState::Converting;
	m_assignments[tableIndex] = assignment;

	// Put the shipped filename back while the new file is being written, so the entry is
	// never pointing at a half-written .pac.
	RestorePointer(tableIndex);

	Job job;
	job.tableIndex = tableIndex;
	job.mp3Path = mp3Path;
	job.originalPac = track->originalDir + "/" + track->fileName;
	job.outPac = assignment.pacPath;
	m_pending.push_back(job);

	if (m_batchTotal == 0)
		m_batchDone = 0;
	++m_batchTotal;

	Save();
	StartNextJob();
}

void BgmReplacementManager::Retry(int tableIndex)
{
	auto it = m_assignments.find(tableIndex);
	if (it == m_assignments.end())
		return;
	const std::string source = it->second.sourcePath;
	if (!source.empty())
		Assign(tableIndex, source);
}

void BgmReplacementManager::Unassign(int tableIndex)
{
	auto it = m_assignments.find(tableIndex);
	if (it == m_assignments.end())
		return;

	RestorePointer(tableIndex);

	// Cancel any queued conversion for this track, or the worker would write the file
	// back out after we deleted it. A job already in flight can't be cancelled, but
	// Update() drops results whose assignment has gone.
	for (auto it2 = m_pending.begin(); it2 != m_pending.end(); )
		it2 = (it2->tableIndex == tableIndex) ? m_pending.erase(it2) : it2 + 1;

	// Drop the converted file too, so removing a replacement really does leave the game
	// folder as it was.
	if (!it->second.pacPath.empty())
		DeleteFileA(it->second.pacPath.c_str());

	LogRepl("Entry %d restored to \"%s\"\n", tableIndex,
		m_original[tableIndex] ? m_original[tableIndex] : "(null)");

	m_assignments.erase(it);
	Save();
}

void BgmReplacementManager::UnassignAll()
{
	std::vector<int> indices;
	for (const auto& entry : m_assignments)
		indices.push_back(entry.first);
	for (int index : indices)
		Unassign(index);
}

// ---------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------

BgmReplacementState BgmReplacementManager::GetState(int tableIndex) const
{
	const BgmReplaceableTrack* track = FindTrack(tableIndex);
	auto it = m_assignments.find(tableIndex);
	if (it == m_assignments.end())
	{
		if (track && track->originalDir.empty())
			return BgmReplacementState::Unavailable;
		return BgmReplacementState::Original;
	}
	return it->second.state;
}

std::string BgmReplacementManager::GetSourceName(int tableIndex) const
{
	auto it = m_assignments.find(tableIndex);
	return it == m_assignments.end() ? std::string() : FileNameOf(it->second.sourcePath);
}

std::string BgmReplacementManager::GetSourcePath(int tableIndex) const
{
	auto it = m_assignments.find(tableIndex);
	return it == m_assignments.end() ? std::string() : it->second.sourcePath;
}

std::string BgmReplacementManager::GetError(int tableIndex) const
{
	auto it = m_assignments.find(tableIndex);
	return it == m_assignments.end() ? std::string() : it->second.error;
}

int BgmReplacementManager::GetActiveCount() const
{
	int n = 0;
	for (const auto& entry : m_assignments)
		if (entry.second.state == BgmReplacementState::Active) ++n;
	return n;
}

int BgmReplacementManager::GetAssignedCount() const
{
	return (int)m_assignments.size();
}

std::string BgmReplacementManager::GetOriginalPreviewPath(int tableIndex) const
{
	const BgmReplaceableTrack* track = FindTrack(tableIndex);
	if (!track || track->originalDir.empty())
		return std::string();
	// MusicManager's preview builds "data/Sound/BGM/<rel>.pac", so bgm_End tracks would
	// need a different prefix than it supports; only offer BGM-folder originals.
	if (track->originalDir != kBgmDir)
		return std::string();
	return track->baseName;
}

std::string BgmReplacementManager::GetReplacementPreviewPath(int tableIndex) const
{
	auto it = m_assignments.find(tableIndex);
	const BgmReplaceableTrack* track = FindTrack(tableIndex);
	if (it == m_assignments.end() || !track || track->originalDir != kBgmDir)
		return std::string();
	if (!LooksLikeUsablePac(it->second.pacPath))
		return std::string();
	return std::string(kReplacementDir) + "/" + track->baseName;
}

// ---------------------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------------------

void BgmReplacementManager::Save()
{
	CreateDirectoryA("BBCF_IM", NULL); // absent on a fresh install

	std::ofstream file(kSettingsPath);
	if (!file.is_open())
	{
		LogRepl("Could not write %s\n", kSettingsPath);
		return;
	}

	file << "# BGM replacements. Each line is a shipped track and the song standing in\n";
	file << "# for it. Delete a line to put that track back to normal.\n";
	file << "[Replacements]\n";
	for (const auto& entry : m_assignments)
	{
		const BgmReplaceableTrack* track = FindTrack(entry.first);
		if (!track || entry.second.sourcePath.empty())
			continue;
		file << track->baseName << "=" << entry.second.sourcePath << "\n";
	}
}

void BgmReplacementManager::Load()
{
	std::ifstream file(kSettingsPath);
	if (!file.is_open())
		return;

	std::string line;
	std::string section;
	while (std::getline(file, line))
	{
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;
		if (line.front() == '[' && line.back() == ']')
		{
			section = line.substr(1, line.size() - 2);
			continue;
		}
		if (section != "Replacements")
			continue;

		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;

		const std::string baseName = line.substr(0, eq);
		const std::string source = line.substr(eq + 1);

		const BgmReplaceableTrack* track = nullptr;
		for (const auto& t : m_tracks)
		{
			if (_stricmp(t.baseName.c_str(), baseName.c_str()) == 0) { track = &t; break; }
		}
		if (!track || track->originalDir.empty())
		{
			LogRepl("Saved replacement for unknown or missing track \"%s\" - ignored\n",
				baseName.c_str());
			continue;
		}

		Assignment assignment;
		assignment.sourcePath = source;
		assignment.pacPath = track->originalDir + "/" + kReplacementDir + "/" + track->fileName;
		assignment.relPath = std::string(kReplacementDir) + "/" + track->fileName;
		assignment.state = BgmReplacementState::Missing;
		m_assignments[track->tableIndex] = assignment;
	}
}
