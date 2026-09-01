#pragma once
#include "AudioDecode.h"
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
	void Assign(int tableIndex, const std::string& mp3Path);

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
	};

	struct Job
	{
		int         tableIndex = -1;
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

	void Save();
	void Load();

	bool        m_initialized = false;
	uintptr_t   m_tableAddr = 0;
	const char* m_original[175] = {};
	std::string m_owned[175];

	std::vector<BgmReplaceableTrack> m_tracks;
	std::map<int, Assignment>        m_assignments;

	std::deque<Job>        m_pending;
	std::future<JobResult> m_inFlight;
	int                    m_convertingIndex = -1;
	int                    m_batchTotal = 0;
	int                    m_batchDone = 0;
};

inline BgmReplacementManager& GetBgmReplacements() { return BgmReplacementManager::GetInstance(); }
