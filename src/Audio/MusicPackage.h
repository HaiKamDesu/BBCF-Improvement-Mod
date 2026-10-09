#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <future>
#include <map>
#include <string>
#include <vector>

// The self-contained files the music features share with other people: a music
// replacement pack (*.bbcfim-music) and a Jukebox playlist (*.bbcfim-playlist).
//
// Both are ordinary zip archives: a "manifest.ini" saying what goes where, and the songs
// themselves under "songs/". Songs are stored, not compressed - they are already
// compressed audio, and deflating them again costs time for nothing. Archive names are
// plain ASCII ("songs/3.mp3"); a song's real file name, which can be anything, travels in
// the manifest instead, so no zip tool ever has to agree with us about Unicode names.
//
// Writing and extracting run on a worker thread: a pack of thirty songs is a few hundred
// megabytes, and the game must not freeze while it is copied.
namespace MusicPackage
{
	extern const char* const kReplacementExtension;   // "bbcfim-music"
	extern const char* const kPlaylistExtension;      // "bbcfim-playlist"

	// A manifest is a list of [Section] key=value lines, in order.
	struct Manifest
	{
		// section -> its key/value pairs, in the order written
		std::map<std::string, std::vector<std::pair<std::string, std::string>>> sections;

		void Add(const std::string& section, const std::string& key, const std::string& value);
		const std::vector<std::pair<std::string, std::string>>& Section(const std::string& name) const;
		// The value of a key, or `fallback` when the section or key is absent.
		std::string Get(const std::string& section, const std::string& key, const std::string& fallback = "") const;
	};

	// How far a background write or read has got, for a progress bar. Filled by the worker,
	// read by the UI; totals are 0 until the worker has worked them out.
	struct Progress
	{
		std::atomic<long long> doneBytes{ 0 };
		std::atomic<long long> totalBytes{ 0 };
		std::atomic<int> doneFiles{ 0 };
		std::atomic<int> totalFiles{ 0 };
		float Fraction() const;   // 0..1 by bytes
	};

	struct SongFile
	{
		std::string sourcePath;    // where the song is on disk now
		std::string archiveName;   // "songs/3.mp3"
	};

	struct WriteResult
	{
		bool ok = false;
		std::string error;
		int songsWritten = 0;
		std::vector<std::string> missingSongs;   // songs that were not on disk any more
	};

	// Writes the archive. A song whose file is gone is left out and listed in the result,
	// so one deleted download does not cost the whole export.
	std::future<WriteResult> WriteAsync(const std::string& path, const Manifest& manifest,
		const std::vector<SongFile>& songs, const std::string& kind,
		std::shared_ptr<Progress> progress = nullptr);

	struct ReadResult
	{
		bool ok = false;
		std::string error;
		Manifest manifest;
		// archive name -> where it was extracted to
		std::map<std::string, std::string> extracted;
	};

	// archive name -> the full path to extract it to, worked out from the parsed manifest
	using DestinationsFor = std::function<std::map<std::string, std::string>(const Manifest&)>;

	// Reads the manifest and extracts every song to the path `destinationsFor` gives it
	// (folders are created as needed). A path already taken by a different file gets a
	// " (2)" style suffix; a file already there byte-for-byte the same size is reused
	// rather than copied again. One file can send songs to different folders - a playlist
	// puts its Jukebox songs in the custom folder and its replacement songs elsewhere.
	std::future<ReadResult> ReadAsync(const std::string& path, const std::string& kind,
		DestinationsFor destinationsFor, std::shared_ptr<Progress> progress = nullptr);

	// "My pack.bbcfim-music" -> "My pack"
	std::string StemOf(const std::string& path);
	std::string FileNameOf(const std::string& path);
	// A file name with anything Windows refuses in a file name replaced.
	std::string SafeFileName(const std::string& name);
	// "songs/<n><ext of sourcePath>"
	std::string ArchiveNameFor(int n, const std::string& sourcePath);
}
