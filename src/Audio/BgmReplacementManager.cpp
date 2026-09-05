#include "BgmReplacementManager.h"
#include <vector>
#include "CustomMusicConverter.h"
#include "PacFile.h"
#include "MusicManager.h"

#include "Core/logger.h"
#include "Core/utils.h"
#include "Overlay/Logger/ImGuiLogger.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
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

	// A .pac source means the audio is transplanted rather than transcoded. The extension
	// carries that distinction on its own, which is why nothing extra has to be written to
	// the saved assignments for it to survive a restart.
	bool IsPacPath(const std::string& path)
	{
		if (path.size() < 4)
			return false;
		std::string tail = path.substr(path.size() - 4);
		for (char& c : tail) c = (char)tolower((unsigned char)c);
		return tail == ".pac";
	}

	// The two entries the game loads during its own boot, verified in the disassembly:
	// both are read from the function that also carries InitParticle / InitFade /
	// InitSystemFont / InitGameSystem, and are registered into fixed audio slots (6 and 7)
	// for the rest of the process. A table pointer written after that is never consulted,
	// so a swap to either only takes effect on the next launch.
	const int kCharaSelectIndex  = 72;   // 201_charaselect.pac, read only at 0x00483B55
	const int kRandomSelectIndex = 85;   // 251_rannyu.pac,      read only at 0x00483ACF

	// Only two prefixes are used for BGM; a track's original tells us which one applies.
	// These are relative to the GAME FOLDER, never to the working directory - see
	// GetGameDirectory() for why that distinction matters.
	const char* kBgmDir = "data/Sound/BGM";
	const char* kBgmEndDir = "data/sound/bgm_End";

	const char* kSettingsRelPath = "BBCF_IM/bgm_replacements.ini";


	// Astral Heat music is the one place where "replace the file" is not the whole story,
	// so the browser spells out where each of these is actually heard.
	//
	// Established from the disassembly (docs/Research/BgmReplacementFeasibility.md):
	// the per-match audio loader FUN_00555A20 reads a saved option (global 0x16BB0B0,
	// the Astral Heat BGM setting, menu id MOGO_AstralHeatBGM) and loads ONE set of three
	// according to its value - 0 -> the 603/604/605 set, 1 -> 600/601/602, 2 -> 606/607/608.
	// All three of the chosen set are loaded together and one is picked when the Astral
	// actually fires, so replacing all three of a set is the only way to be certain.
	//
	// 609/610/611 are referenced NOWHERE in the executable and are byte-identical copies
	// of 600/601/602 - dead weight, replacing them can never do anything.
	struct AstralInfo { const char* base; const char* name; const char* note; bool used; };
	const AstralInfo kAstralInfo[] = {
		{ "603_cs_astral_a", "Astral Finish - set 1, variant A", "Astral Heat BGM: 1st option", true },
		{ "604_cs_astral_b", "Astral Finish - set 1, variant B", "Astral Heat BGM: 1st option", true },
		{ "605_cs_astral_c", "Astral Finish - set 1, variant C", "Astral Heat BGM: 1st option", true },
		{ "600_astral_a",    "Astral Finish - set 2, variant A", "Astral Heat BGM: 2nd option", true },
		{ "601_astral_b",    "Astral Finish - set 2, variant B", "Astral Heat BGM: 2nd option", true },
		{ "602_astral_c",    "Astral Finish - set 2, variant C", "Astral Heat BGM: 2nd option", true },
		{ "606_v2_astral_a", "Astral Finish - set 3, variant A", "Astral Heat BGM: 3rd option", true },
		{ "607_v2_astral_b", "Astral Finish - set 3, variant B", "Astral Heat BGM: 3rd option", true },
		{ "608_v2_astral_c", "Astral Finish - set 3, variant C", "Astral Heat BGM: 3rd option", true },
		{ "609_cs_astral_a", "Astral Finish - unused copy A",    "", false },
		{ "610_cs_astral_b", "Astral Finish - unused copy B",    "", false },
		{ "611_cs_astral_c", "Astral Finish - unused copy C",    "", false },
	};

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
		// Wide: a source path can be any filename the user picked, including one outside
		// the system codepage.
		return GetFileAttributesExW(utf8_to_utf16(path).c_str(), GetFileExInfoStandard, &info) != 0 &&
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

// Every table index carrying the same original filename as this one.
//
// The table is not one-entry-per-track: 205_abyss.pac occupies BOTH index 76 and index 109.
// Patching only the selected index means whichever one the game happens to read decides
// whether the replacement is heard, which is why Grim of Abyss reported as replaced and
// played the original. Nothing else in the table is duplicated, but keying off the filename
// rather than a hardcoded pair keeps this correct if that ever changes.
std::vector<int> BgmReplacementManager::IndicesSharingFile(int tableIndex) const
{
	std::vector<int> out;
	if (tableIndex < 0 || tableIndex >= kEntryCount || !m_original[tableIndex])
		return out;

	for (int i = 0; i < kEntryCount; ++i)
	{
		if (m_original[i] && _stricmp(m_original[i], m_original[tableIndex]) == 0)
			out.push_back(i);
	}
	return out;
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
	// Verify the cue before trusting the file. A mismatch here is the difference between
	// "replaced" and "replaced and audible", and it is invisible at every other layer.
	const BgmReplaceableTrack* const verifyTrack = FindTrack(tableIndex);
	if (verifyTrack != nullptr)
	{
		const std::string cue = ReadPacCueName(it->second.pacPath);
		if (cue.empty())
		{
			LogRepl("Entry %d: WARNING could not read a cue name out of \"%s\" - if this "
			        "track stays silent, that file is not a usable .pac\n",
				tableIndex, it->second.pacPath.c_str());
		}
		else if (_stricmp(cue.c_str(), verifyTrack->baseName.c_str()) != 0)
		{
			LogRepl("Entry %d: WARNING cue mismatch - the file carries cue \"%s\" but the "
			        "game will ask for \"%s\". It will load and play nothing. Rebuild this "
			        "replacement.\n",
				tableIndex, cue.c_str(), verifyTrack->baseName.c_str());
		}
		else
		{
			LogRepl("Entry %d: cue \"%s\" verified\n", tableIndex, cue.c_str());
		}
	}

	it->second.appliedBeforeBoot = m_applyingSavedAtStartup;

	RestorePointer(tableIndex);
	m_owned[tableIndex] = it->second.relPath;

	// Duplicate entries share the primary's buffer; m_owned[tableIndex] outlives them
	// because it is a fixed array slot that only RestorePointer above ever reassigns.
	const std::vector<int> shared = IndicesSharingFile(tableIndex);
	for (int other : shared)
	{
		if (other == tableIndex)
			continue;
		if (WritePointer(other, m_owned[tableIndex].c_str()))
			LogRepl("Entry %d also carries \"%s\"; pointed it at the same replacement\n",
				other, m_original[other] ? m_original[other] : "(null)");
	}

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

	// Restore the duplicates too, or undoing a replacement leaves the other entry pointing
	// at a buffer that is about to be reassigned.
	for (int other : IndicesSharingFile(tableIndex))
	{
		if (other != tableIndex)
			WritePointer(other, m_original[other]);
	}
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
		for (const auto& info : kAstralInfo)
		{
			if (_stricmp(info.base, track.baseName.c_str()) != 0)
				continue;
			track.displayName = info.name;
			track.everUsed = info.used;
			track.usageNote = info.used
				? std::string("Plays on an Astral Finish. Used only when your ") + info.note +
				  " is selected in the game's Sound settings, and only one of the three "
				  "variants plays - replace all three of a set to be sure."
				: "The game never loads this file, so replacing it does nothing. It is a "
				  "leftover duplicate of set 2.";
			break;
		}

		// Load timing. Only two entries are genuinely boot-loaded, and they are the reason
		// this field exists: 201_charaselect and 251_rannyu are both read from the game's
		// boot init (the function carrying InitParticle / InitFade / InitSystemFont /
		// InitGameSystem) into fixed audio slots, and stay there for the whole process.
		// Everything else is loaded when the game next needs it, so the only question is
		// whether that is a match or a screen.
		if (i == kCharaSelectIndex || i == kRandomSelectIndex)
		{
			track.timing = BgmLoadTiming::GameRestart;
		}
		else if (_stricmp(track.category.c_str(), "btl") == 0 ||
			_stricmp(track.category.c_str(), "boss") == 0 ||
			_stricmp(track.category.c_str(), "old") == 0 ||
			_stricmp(track.category.c_str(), "astral") == 0)
		{
			track.timing = BgmLoadTiming::NextMatch;
		}
		else
		{
			track.timing = BgmLoadTiming::NextScreen;
		}

		if (track.displayName.empty())
			track.displayName = track.baseName;
		if (track.category.empty())
			track.category = "other";

		// Which folder ships this track decides where its replacement has to live, since
		// the game prefixes the table string differently for the two BGM directories.
		if (FileExists(GamePath(std::string(kBgmDir) + "/" + fileName)))
			track.originalDir = kBgmDir;
		else if (FileExists(GamePath(std::string(kBgmEndDir) + "/" + fileName)))
			track.originalDir = kBgmEndDir;

		// A duplicated filename is one track to the user, not two rows with the same name
		// that behave differently depending on which the game reads. ApplyPointer patches
		// every index sharing the file, so the first one stands for all of them.
		bool alreadyListed = false;
		for (const auto& existing : m_tracks)
		{
			if (_stricmp(existing.fileName.c_str(), track.fileName.c_str()) == 0)
			{
				alreadyListed = true;
				break;
			}
		}
		if (alreadyListed)
		{
			LogRepl("Entry %d duplicates \"%s\" (already listed); it will be patched "
			        "together with the first entry\n", i, fileName.c_str());
			continue;
		}

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
	m_applyingSavedAtStartup = true;
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

	m_applyingSavedAtStartup = false;

	LogRepl("Initialized: %d assignment(s), %d active\n",
		GetAssignedCount(), GetActiveCount());
}

bool BgmReplacementManager::LooksLikeUsablePac(const std::string& path)
{
	WIN32_FILE_ATTRIBUTE_DATA info = {};
	if (!GetFileAttributesExW(utf8_to_utf16(path).c_str(), GetFileExInfoStandard, &info))
		return false;
	if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return false;
	// A real BGM .pac is hundreds of KB; anything tiny is a truncated or bogus write.
	return info.nFileSizeHigh > 0 || info.nFileSizeLow >= 1024;
}

// Reads the cue name out of a .pac's FPAC file table.
//
// This is the one property a replacement must get right and the only one nothing checked.
// The game asks XACT for a cue named exactly the original base filename, so a .pac whose
// internal name is anything else loads without error and then silently plays nothing - or
// leaves whatever was already registered playing, which reads as "the replacement did not
// apply" with no failure anywhere to point at.
//
// Layout (see BuildFpacContainer, verified against all 186 shipped files), after
// PacFile has taken off the DFASFPAC envelope a repacked .pac may be wearing:
//   +0x00 "FPAC"; +0x04 dataStart; +0x08 totalSize; +0x0C fileCount; +0x10 = 1;
//   +0x14 nameField; file table at +0x20, first entry's name is "<cue>.xsb".
std::string BgmReplacementManager::ReadPacCueName(const std::string& path)
{
	std::ifstream file(utf8_to_utf16(path).c_str(), std::ios::binary);
	if (!file)
		return std::string();

	char header[0x20] = {};
	file.read(header, sizeof(header));
	if (file.gcount() != (std::streamsize)sizeof(header))
		return std::string();

	// A compressed .pac tells us nothing until it is inflated, so that one goes the long
	// way round: read the whole file and unwrap it. Only files a music mod (or a repack
	// tool) produced land here; the mod's own output is always bare FPAC.
	std::vector<unsigned char> inflated;
	if (PacFile::IsCompressed(header, sizeof(header)))
	{
		file.close();
		if (!PacFile::Read(path, inflated, nullptr) || inflated.size() < sizeof(header))
			return std::string();
		memcpy(header, inflated.data(), sizeof(header));
	}
	if (memcmp(header, "FPAC", 4) != 0)
		return std::string();

	unsigned int nameField = 0;
	memcpy(&nameField, header + 0x14, sizeof(nameField));
	if (nameField == 0 || nameField > 256)
		return std::string();

	std::vector<char> name(nameField + 1, '\0');
	if (!inflated.empty())
	{
		if (inflated.size() < sizeof(header) + nameField)
			return std::string();
		memcpy(name.data(), inflated.data() + sizeof(header), nameField);
	}
	else
	{
		file.read(name.data(), nameField);
		if (file.gcount() <= 0)
			return std::string();
	}

	std::string sub(name.data());
	const size_t dot = sub.rfind('.');
	return (dot == std::string::npos) ? sub : sub.substr(0, dot);
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

		if (IsPacPath(job.mp3Path))
		{
			// A .pac source is transplanted whole: no decode, no gain, and so no headroom
			// or tag to report. It is also near-instant, but it still goes through the job
			// queue so it shares one state machine with everything else.
			result.ok = BuildReplacementPacFromPac(job.mp3Path, job.originalPac, job.outPac, &error);
		}
		else
		{
			float tagGain = 0.0f;
			result.ok = ConvertAudioToReplacementPac(job.mp3Path, job.gainDb, job.originalPac, job.outPac,
				&error, &result.headroomDb, &tagGain);
			result.tagGainDb = tagGain;
			result.hasTagGain = (tagGain != 0.0f);
		}

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
		if (it != m_assignments.end() && result.headroomDb > -1000.0f)
		{
			it->second.headroomDb = result.headroomDb;
			it->second.tagGainDb = result.tagGainDb;
			it->second.hasTagGain = result.hasTagGain;
		}

		if (it == m_assignments.end())
		{
			// Unassigned while it was converting - clean up what the worker wrote.
			const BgmReplaceableTrack* track = FindTrack(result.tableIndex);
			if (result.ok && track && !track->originalDir.empty())
			{
				const std::string stray =
					GamePath(track->originalDir + "/" + kReplacementDir + "/" + track->fileName);
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

	if (!track->everUsed)
	{
		Assignment bad;
		bad.sourcePath = mp3Path;
		bad.state = BgmReplacementState::Unavailable;
		bad.error = "The game never loads this track, so replacing it would do nothing";
		m_assignments[tableIndex] = bad;
		return;
	}

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

	const std::string outDir = GamePath(track->originalDir + "/" + kReplacementDir);
	CreateDirectoryA(outDir.c_str(), NULL);

	// Carry the gain over when this is a re-conversion of the same file - that is what
	// SetGain and Retry both go through. A genuinely new song starts on auto again.
	float carriedGain = 0.0f;
	{
		auto prior = m_assignments.find(tableIndex);
		if (prior != m_assignments.end() && prior->second.sourcePath == mp3Path)
			carriedGain = prior->second.gainDb;
	}

	Assignment assignment;
	assignment.sourcePath = mp3Path;
	assignment.gainDb = carriedGain;
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
	job.originalPac = GamePath(track->originalDir + "/" + track->fileName);
	job.outPac = assignment.pacPath;
	job.gainDb = assignment.gainDb;
	m_pending.push_back(job);

	if (m_batchTotal == 0)
		m_batchDone = 0;
	++m_batchTotal;

	Save();
	StartNextJob();
}

float BgmReplacementManager::GetHeadroomDb(int tableIndex) const
{
	auto it = m_assignments.find(tableIndex);
	return it == m_assignments.end() ? -1000.0f : it->second.headroomDb;
}

bool BgmReplacementManager::GetTagGainDb(int tableIndex, float& outGainDb) const
{
	auto it = m_assignments.find(tableIndex);
	if (it == m_assignments.end() || !it->second.hasTagGain)
		return false;
	outGainDb = it->second.tagGainDb;
	return true;
}

float BgmReplacementManager::GetGainDb(int tableIndex) const
{
	auto it = m_assignments.find(tableIndex);
	return it == m_assignments.end() ? 0.0f : it->second.gainDb;
}

void BgmReplacementManager::SetGainDb(int tableIndex, float gainDb)
{
	auto it = m_assignments.find(tableIndex);
	if (it == m_assignments.end())
		return;
	if (it->second.gainDb == gainDb)
		return; // nothing to re-encode for

	it->second.gainDb = gainDb;
	const std::string source = it->second.sourcePath;
	if (source.empty())
	{
		Save();
		return;
	}
	// Assign() carries the gain we just stored forward into the new job.
	Assign(tableIndex, source);
}

void BgmReplacementManager::AssignFromVanilla(int tableIndex, int sourceTableIndex)
{
	if (!m_initialized)
		return;

	const BgmReplaceableTrack* source = FindTrack(sourceTableIndex);
	const BgmReplaceableTrack* target = FindTrack(tableIndex);
	if (!source || !target)
		return;

	// Compared by filename rather than by index: a couple of entries share one file
	// (205_abyss.pac has two), and standing a track in for itself would build a copy of the
	// original and call it a replacement.
	if (source->fileName == target->fileName)
		return;

	if (source->originalDir.empty())
	{
		// Nothing on disk to take the audio from. Assign() reports the mirror of this for
		// the target, so say it about the source here rather than failing a job later.
		Assignment bad;
		bad.state = BgmReplacementState::Failed;
		bad.error = "\"" + source->displayName + "\" isn't installed, so its music can't be copied";
		m_assignments[tableIndex] = bad;
		return;
	}

	Assign(tableIndex, GamePath(source->originalDir + "/" + source->fileName));
}

bool BgmReplacementManager::IsPacSource(int tableIndex) const
{
	auto it = m_assignments.find(tableIndex);
	return it != m_assignments.end() && IsPacPath(it->second.sourcePath);
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
		if (track && (track->originalDir.empty() || !track->everUsed))
			return BgmReplacementState::Unavailable;
		return BgmReplacementState::Original;
	}
	return it->second.state;
}

std::string BgmReplacementManager::GetSourceName(int tableIndex) const
{
	auto it = m_assignments.find(tableIndex);
	if (it == m_assignments.end())
		return std::string();

	const std::string fileName = FileNameOf(it->second.sourcePath);

	// A shipped track standing in for another was chosen by its name in the picker, so
	// report it that way rather than as "012_btl_xx.pac", which is not what the user picked
	// and not something they can recognise. Foreign .pacs have no catalogue entry and keep
	// their filename, which is the best label available for them.
	if (IsPacPath(fileName))
	{
		for (const BgmReplaceableTrack& track : m_tracks)
		{
			if (track.fileName == fileName && !track.displayName.empty())
				return track.displayName;
		}
	}

	return fileName;
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

std::string BgmReplacementManager::GetActiveReplacementPlayPath(const std::string& baseName,
	bool mustBeWhatTheGameLoaded) const
{
	if (!m_initialized || baseName.empty())
		return std::string();

	for (const auto& track : m_tracks)
	{
		if (_stricmp(track.baseName.c_str(), baseName.c_str()) != 0)
			continue;

		// The path returned here is relative to kBgmDir, so a track shipped in the other
		// BGM folder cannot be described by it. Those are menu/ending tracks the jukebox
		// does not play, and answering with a path into the wrong folder would be worse
		// than answering with nothing.
		if (track.originalDir != kBgmDir)
			return std::string();

		const auto it = m_assignments.find(track.tableIndex);
		if (it == m_assignments.end() || it->second.state != BgmReplacementState::Active)
			return std::string();
		if (mustBeWhatTheGameLoaded && !IsLiveNow(track.tableIndex))
			return std::string();

		return std::string(kReplacementDir) + "/" + track.baseName;
	}
	return std::string();
}

bool BgmReplacementManager::HasActiveReplacement(const std::string& baseName,
	bool mustBeWhatTheGameLoaded) const
{
	return !GetActiveReplacementPlayPath(baseName, mustBeWhatTheGameLoaded).empty();
}

std::vector<BgmReplacementManager::ActiveReplacement>
BgmReplacementManager::GetActiveReplacements() const
{
	std::vector<ActiveReplacement> result;
	if (!m_initialized)
		return result;

	for (const auto& track : m_tracks)
	{
		// Same restriction as GetActiveReplacementPlayPath: the path is relative to kBgmDir,
		// so a track shipped in the other folder cannot be described by one.
		if (track.originalDir != kBgmDir)
			continue;
		const auto it = m_assignments.find(track.tableIndex);
		if (it == m_assignments.end())
			continue;
		// Converting counts as listable, on one condition. A gain change re-converts, and
		// dropping the row for those few seconds would take it out from under the volume
		// popup that started it - and out from under the player, if it was playing. The
		// converter writes a .tmp and moves it into place, so the file already there stays
		// whole and playable the entire time. Requiring the file is what keeps a
		// first-time conversion, which has nothing on disk yet, from being offered.
		const bool listable = it->second.state == BgmReplacementState::Active ||
			it->second.state == BgmReplacementState::Converting;
		if (!listable || !FileExists(it->second.pacPath))
			continue;

		ActiveReplacement entry;
		entry.tableIndex = track.tableIndex;
		entry.baseName = track.baseName;
		entry.relPathNoExt = std::string(kReplacementDir) + "/" + track.baseName;
		entry.trackName = track.displayName;
		entry.sourceName = GetSourceName(track.tableIndex);
		result.push_back(std::move(entry));
	}
	return result;
}

unsigned int BgmReplacementManager::GetActiveSignature() const
{
	if (!m_initialized)
		return 0;

	// Order-independent would be enough, but the walk is in table order anyway, so this
	// also catches a swap that keeps the count the same.
	unsigned int signature = 2166136261u;
	for (const auto& entry : m_assignments)
	{
		if (entry.second.state != BgmReplacementState::Active &&
			entry.second.state != BgmReplacementState::Converting)
			continue;
		// The state is mixed in as well as the index, so a first-time conversion finishing
		// is seen (its row could not be listed while it had no file yet). A gain change
		// therefore also re-publishes, which costs one rebuild and changes nothing about
		// the row - the point is only that it does not disappear.
		signature = (signature ^ static_cast<unsigned int>(entry.first)) * 16777619u;
		signature = (signature ^ static_cast<unsigned int>(entry.second.state)) * 16777619u;
	}
	return signature;
}

bool BgmReplacementManager::IsLiveNow(int tableIndex) const
{
	auto it = m_assignments.find(tableIndex);
	if (it == m_assignments.end() || it->second.state != BgmReplacementState::Active)
		return false;

	const BgmReplaceableTrack* const track = FindTrack(tableIndex);
	if (track == nullptr)
		return false;

	// Everything except the boot-loaded pair is read fresh each time the game needs it, so
	// the pointer being set is enough; the row's timing line says when that next happens.
	if (track->timing != BgmLoadTiming::GameRestart)
		return true;

	return it->second.appliedBeforeBoot;
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
	CreateDirectoryA(GamePath("BBCF_IM").c_str(), NULL); // absent on a fresh install

	std::ofstream file(GamePath(kSettingsRelPath));
	if (!file.is_open())
	{
		LogRepl("Could not write %s\n", GamePath(kSettingsRelPath).c_str());
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

	// Kept in its own section so that an ini written by an older build - which had no
	// concept of gain - still loads, and so a hand-edited file can drop the section
	// entirely to go back to automatic levelling.
	file << "\n";
	file << "# Playback gain per track. \"auto\" levels the song against the rest of the\n";
	file << "# game's music; a number is a manual offset in dB.\n";
	file << "[Gain]\n";
	for (const auto& entry : m_assignments)
	{
		const BgmReplaceableTrack* track = FindTrack(entry.first);
		if (!track || entry.second.sourcePath.empty())
			continue;
		file << track->baseName << "=" << entry.second.gainDb << "\n";
	}
}

void BgmReplacementManager::Load()
{
	std::ifstream file(GamePath(kSettingsRelPath));
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
		if (section != "Replacements" && section != "Gain")
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

		if (section == "Gain")
		{
			// [Gain] is written after [Replacements], so the assignment already exists.
			auto existing = m_assignments.find(track->tableIndex);
			if (existing == m_assignments.end())
				continue;
			// "auto" is what an older build wrote for the automatic levelling that no
			// longer exists; treat it as no adjustment rather than failing to load.
			existing->second.gainDb = (_stricmp(source.c_str(), "auto") == 0)
				? 0.0f : (float)atof(source.c_str());
			continue;
		}

		Assignment assignment;
		assignment.sourcePath = source;
		assignment.pacPath = GamePath(track->originalDir + "/" + kReplacementDir + "/" + track->fileName);
		assignment.relPath = std::string(kReplacementDir) + "/" + track->fileName;
		assignment.state = BgmReplacementState::Missing;
		m_assignments[track->tableIndex] = assignment;
	}
}
