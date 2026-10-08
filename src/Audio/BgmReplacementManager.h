#pragma once
#include "AudioDecode.h"
#include "MusicPackage.h"
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <future>

// Replaces any shipped BGM track with a user-supplied song.
//
// The game resolves every audio file through a pointer table in .rdata (see
// docs/Research/BgmReplacementFeasibility.md). Pointing an entry at a string of our own
// redirects that track's load, with no code hook at all - and because the pointer lives in
// process memory, nothing on disk changes and the swap is undone by restoring the pointer.
//
// Two rules the research settled, both load-bearing:
//   - A replacement keeps the ORIGINAL base filename and varies only the directory, because
//     the game asks its sound bank for a cue by name and a .pac's cue is always its own base
//     filename.
//   - A pointer is NEVER written unless the target file is confirmed on disk. A missing
//     target does not degrade to silence, it hangs the game during match load.
enum class BgmReplacementState
{
	Original,     // no replacement assigned
	Converting,   // queued or mid-conversion
	Active,       // converted, on disk, pointer patched
	Missing,      // assigned, but the converted file is gone - pointer left alone
	Failed,       // conversion failed
	Unavailable,  // the shipped track isn't installed, so there's nothing to stand in for
	Off,          // converted and on disk, but turned off by the user - the original plays
};

// When a swap to this track actually starts being heard. The game does not load every BGM
// the same way, and a replacement that has been written correctly still does nothing until
// the game next loads that file - which for a couple of tracks means not until it restarts.
// Saying so on the row is the difference between "applied" and the user believing it worked.
// Derived in BuildCatalogue; see docs/BgmReplacementTiming.md for how each was established.
enum class BgmLoadTiming
{
	NextMatch,     // loaded with the match: battle, boss, astral
	NextScreen,    // loaded when its screen comes up: menus, lobby, versus, results
	GameRestart,   // loaded once during the game's own boot and kept for the whole process
};

struct BgmReplaceableTrack
{
	int         tableIndex = -1;   // index into the game's filename table
	std::string fileName;          // "008_btl_bn.pac"
	std::string baseName;          // "008_btl_bn" - also the cue name
	std::string displayName;       // "REPPUU II (Bang)", else the base name
	std::string category;          // "btl", "astral", ...
	std::string originalDir;       // folder holding the shipped file; empty if not installed
	std::string usageNote;         // where this track is actually heard; empty if obvious
	bool        everUsed = true;   // false for entries the game never reads at all
	BgmLoadTiming timing = BgmLoadTiming::NextScreen;
};

class BgmReplacementManager
{
public:
	static BgmReplacementManager& GetInstance();

	// Snapshots the table, builds the track catalogue, then loads and re-applies saved
	// assignments (re-validating each against disk first).
	void Initialize();
	bool IsInitialized() const { return m_initialized; }

	// Main thread, every frame: retires finished conversions and applies their pointers.
	void Update();

	const std::vector<BgmReplaceableTrack>& GetTracks() const { return m_tracks; }
	const BgmReplaceableTrack* FindTrack(int tableIndex) const;

	BgmReplacementState GetState(int tableIndex) const;
	// Display name of the song standing in for this track ("mysong.mp3"), else empty.
	std::string GetSourceName(int tableIndex) const;
	// Full path of the .mp3 this replacement came from, for re-conversion and the UI.
	std::string GetSourcePath(int tableIndex) const;
	// Why a track is Failed or Missing; empty otherwise.
	std::string GetError(int tableIndex) const;

	// Queues a conversion. The pointer is patched only once it succeeds.
	//
	// A source path ending in .pac is transplanted rather than transcoded (see
	// BuildReplacementPacFromPac), which is what lets one shipped track stand in for
	// another and what makes a .pac from a music mod usable directly. Everything else about
	// the assignment - persistence, Retry, the pointer dance - is identical.
	//
	// `gainDb`, when given, is the volume to build it at (a pack carries one per track);
	// otherwise a re-conversion of the same song keeps its volume and a new song starts at 0.
	void Assign(int tableIndex, const std::string& mp3Path, const float* gainDb = nullptr);

	// Assign the shipped file of `sourceTableIndex` as the replacement for `tableIndex`.
	// Both must be installed tracks; a no-op otherwise.
	void AssignFromVanilla(int tableIndex, int sourceTableIndex);

	// True when this assignment came from a .pac rather than a user audio file, so its
	// audio was transplanted and there is no gain stage to offer.
	bool IsPacSource(int tableIndex) const;

	// Gain is baked into the converted .pac, because the game plays it through its own
	// XACT engine where the mod has no volume control. Changing it therefore re-runs the
	// conversion, so the UI only applies a change when the user asks for it.
	float GetGainDb(int tableIndex) const;
	void SetGainDb(int tableIndex, float gainDb);
	// How much gain this song can take before anything has to be limited, in dB.
	// -1000 when it has not been measured yet.
	float GetHeadroomDb(int tableIndex) const;
	// The song's own ReplayGain if it had one; false when it did not.
	bool GetTagGainDb(int tableIndex, float& outGainDb) const;
	// A replacement can be switched off without being forgotten: the original plays, and
	// switching it back on is instant because the converted file is kept.
	bool IsEnabled(int tableIndex) const;
	void SetEnabled(int tableIndex, bool enabled);

	// Restores the pointer, forgets the assignment and deletes the converted file.
	void Unassign(int tableIndex);
	// Drops every assignment.
	void UnassignAll();
	// Re-runs a conversion for a Missing/Failed entry using its remembered source.
	void Retry(int tableIndex);

	int  GetActiveCount() const;
	int  GetAssignedCount() const;
	bool IsBusy() const { return m_inFlight.valid() || !m_pending.empty(); }
	int  GetQueueLength() const;
	// 0..1 across the current batch; 1 when idle.
	float GetBatchProgress() const;
	// Track currently being converted, or -1.
	int  GetConvertingIndex() const { return m_convertingIndex; }

	// Relative path (no extension) usable with MusicManager::PreviewPac, for whichever of
	// the original / replacement is asked for. Empty if that file isn't available.
	std::string GetOriginalPreviewPath(int tableIndex) const;
	std::string GetReplacementPreviewPath(int tableIndex) const;

	// The active replacement for this track, as a path relative to data/Sound/BGM with no
	// extension ("BBCFIM_Music/008_btl_bn") - the same shape MusicManager already takes for
	// a preview, so it can be loaded and its length read with no new plumbing. Keyed by base
	// filename because that is all MusicManager knows a track by. Empty when nothing stands
	// in for the track, in which case the caller's path to the shipped file is still right.
	//
	// Note the cue name does NOT change with the path: a replacement .pac keeps the original
	// base filename as its cue, which is what makes the pointer swap work at all, so callers
	// keep playing the cue they already were.
	//
	//   mustBeWhatTheGameLoaded - answer only when the pointer was already in place when the
	//   game read the entry, i.e. when the replacement is what the game itself is playing.
	//   Pass false when the caller opens the file itself, where that history is irrelevant.
	std::string GetActiveReplacementPlayPath(const std::string& baseName,
		bool mustBeWhatTheGameLoaded) const;

	// True when a replacement is standing in for this track. Callers use it to know that a
	// precomputed length for the SHIPPED file no longer describes what is playing.
	bool HasActiveReplacement(const std::string& baseName, bool mustBeWhatTheGameLoaded) const;

	// One replacement, in the shape the Jukebox needs to offer it as a track of its own.
	struct ActiveReplacement
	{
		int         tableIndex = -1;
		std::string baseName;       // "008_btl_bn" - also the .pac's cue name
		std::string relPathNoExt;   // "BBCFIM_Music/008_btl_bn", relative to data/Sound/BGM
		std::string trackName;      // the shipped track it stands in for, e.g. "REPPUU II (Bang)"
		std::string sourceName;     // the song the user picked, e.g. "flowerman.mp3"
	};

	// Every replacement that is converted and on disk, so it can be listed and played
	// directly rather than only being heard when the game happens to load its track.
	std::vector<ActiveReplacement> GetActiveReplacements() const;

	// Cheap "has the set of active replacements changed" probe, so a per-frame caller can
	// skip rebuilding its own list. Allocates nothing; only equality is meaningful.
	unsigned int GetActiveSignature() const;

	// ---- Song library ---------------------------------------------------------------
	// The songs the user has brought in, whether or not each one replaces anything yet.
	// Adding songs and then choosing what each replaces is what lets a whole set be put
	// together in one go instead of one track at a time. A song stays in the library after
	// its last replacement is undone, until it is removed. Every song an older build had
	// assigned is part of it from the first load, so nothing is lost on the upgrade.
	// One of the game's own tracks can be a library song too, to play it in place of others.
	struct LibrarySong
	{
		std::string      path;       // the audio file, or the shipped .pac for a game track
		std::string      name;       // its file name, or the game track's name
		bool             exists = true;
		int              vanillaTrack = -1;   // the game track it is, or -1 for a user file
		std::vector<int> replaces;   // table indices it stands in for, in table order
	};
	std::vector<LibrarySong> GetLibrary() const;
	// Adds files to the library; returns how many were new.
	int AddToLibrary(const std::vector<std::string>& paths);
	// Adds one of the game's own tracks as a song; returns its library path, empty if it
	// is not installed.
	std::string AddVanillaToLibrary(int sourceTableIndex);
	// Removes a song, undoing every replacement it was used for.
	void RemoveFromLibrary(const std::string& path);
	// Makes `path` the song for this track, whichever kind of song it is.
	void AssignSong(int tableIndex, const std::string& path);

	// ---- Packs (*.bbcfim-music) -----------------------------------------------------
	// A pack is the whole setup in one file: every library song and what each replaces,
	// with its volume. Export and import run in the background; Update() finishes them.
	bool ExportPack(const std::string& path);
	// `replaceEverything` undoes the current setup first; otherwise the pack's
	// replacements are added on top, taking over only the tracks the pack sets.
	bool ImportPack(const std::string& path, bool replaceEverything);
	bool IsPackBusy() const { return m_exportJob.valid() || m_importJob.valid(); }
	// The outcome of the last export/import, once: true when there is one to show.
	bool ConsumePackMessage(std::string& message, bool& ok);

	// ---- The setup inside another file ----------------------------------------------
	// A playlist carries the whole replacement setup too, so the pieces a pack is made of
	// are open to it. Every section is named `prefix` + its name ("" for a pack itself);
	// songs are numbered on from the ones already in `songs`.
	void WriteSetup(MusicPackage::Manifest& manifest, std::vector<MusicPackage::SongFile>& songs,
		const std::string& prefix) const;
	// Where each of the setup's songs goes when the file is opened: a folder per file, so
	// two files with a song of the same name keep both.
	static void AddSetupDestinations(const MusicPackage::Manifest& manifest, const std::string& prefix,
		const std::string& fileStem, std::map<std::string, std::string>& destinations);
	struct SetupResult
	{
		int songs = 0;
		int applied = 0;
		int skipped = 0;   // tracks this copy of the game does not have
	};
	// Puts the file's setup in place. `replaceEverything` makes it exactly the file's;
	// otherwise it is added on top, taking over only the tracks the file sets. A track
	// already playing the same song at the same volume is left alone, not converted again.
	SetupResult ApplySetup(const MusicPackage::ReadResult& result, const std::string& prefix, bool replaceEverything);

	// True when this replacement's pointer was already in the table before the game loaded
	// the track, so it is what you are hearing now. Only ever false for a GameRestart track
	// that was swapped mid-session: those are read once during the game's boot, so the swap
	// is correct but silent until the next launch.
	bool IsLiveNow(int tableIndex) const;

private:
	BgmReplacementManager() = default;
	~BgmReplacementManager() = default;

	struct Assignment
	{
		std::string          sourcePath;   // the audio file the user picked
		std::string          pacPath;      // converted file on disk
		std::string          relPath;      // what goes in the table, e.g. "BBCFIM_Music/x.pac"
		BgmReplacementState  state = BgmReplacementState::Original;
		std::string          error;
		float gainDb = 0.0f;           // baked into pacPath; changing it forces a rebuild
		// Peak level of the source in dBFS, measured when it was decoded. Gain up to
		// -headroom is completely transparent; past that the limiter has to work.
		float headroomDb = -1000.0f;
		// The song's own ReplayGain, already applied. gainDb is the user's offset on top.
		float tagGainDb = 0.0f;
		bool  hasTagGain = false;
		// Whether the pointer was in place before the game's boot read this entry.
		bool  appliedBeforeBoot = false;
		// Switched off by the user: kept and converted, but the pointer stays original.
		bool  enabled = true;
	};

	struct Job
	{
		int         tableIndex = -1;
		// The source. A .pac here means transplant, anything else means transcode; the
		// extension is the whole distinction, which keeps it true across a save/load with
		// no extra field in the file.
		std::string mp3Path;
		std::string originalPac;
		std::string outPac;
		float gainDb = 0.0f;
	};

	struct JobResult
	{
		int         tableIndex = -1;
		bool        ok = false;
		std::string error;
		float       headroomDb = -1000.0f;
		float       tagGainDb = 0.0f;
		bool        hasTagGain = false;
	};

	void BuildCatalogue();
	bool SnapshotTable();
	bool WritePointer(int tableIndex, const char* value);
	bool ApplyPointer(int tableIndex);
	bool RestorePointer(int tableIndex);
	void StartNextJob();
	// True when the file exists and is big enough to be a real .pac.
	static bool LooksLikeUsablePac(const std::string& path);
	// Cue name carried inside a .pac's FPAC file table, or empty if it is not readable.
	// The game asks XACT for a cue named exactly the original base filename, so this has to
	// match or the replacement loads and plays nothing.
	static std::string ReadPacCueName(const std::string& path);
	// Every table index carrying the same original filename (205_abyss.pac has two).
	std::vector<int> IndicesSharingFile(int tableIndex) const;

	void Save();
	void Load();

	// The shipped track whose own file `path` is (a vanilla stand-in), or -1.
	int VanillaTrackForSource(const std::string& path) const;
	void RememberInLibrary(const std::string& path);
	void FinishPackJobs();
	void SetPackMessage(const std::string& message, bool ok);

	bool        m_initialized = false;
	// Set only while Initialize() re-applies saved assignments, which happens before the
	// game's own boot audio init; distinguishes those from mid-session swaps.
	bool        m_applyingSavedAtStartup = false;
	uintptr_t   m_tableAddr = 0;
	const char* m_original[175] = {};
	std::string m_owned[175];

	std::vector<BgmReplaceableTrack> m_tracks;
	std::map<int, Assignment>        m_assignments;
	std::vector<std::string>         m_library;   // song paths, in the order added

	std::future<MusicPackage::WriteResult> m_exportJob;
	std::future<MusicPackage::ReadResult>  m_importJob;
	std::string m_packPath;
	bool        m_importReplace = false;
	std::string m_packMessage;
	bool        m_packMessageOk = true;
	bool        m_hasPackMessage = false;

	std::deque<Job>        m_pending;
	std::future<JobResult> m_inFlight;
	int                    m_convertingIndex = -1;
	int                    m_batchTotal = 0;
	int                    m_batchDone = 0;
};

inline BgmReplacementManager& GetBgmReplacements() { return BgmReplacementManager::GetInstance(); }
