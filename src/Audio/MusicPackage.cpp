#include "MusicPackage.h"

#include "Core/logger.h"
#include "Core/utils.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <windows.h>
#include <algorithm>
#include <sstream>

namespace MusicPackage
{
	const char* const kReplacementExtension = "bbcfim-music";
	const char* const kPlaylistExtension = "bbcfim-playlist";

	namespace
	{
		const char* const kManifestName = "manifest.ini";
		const std::vector<std::pair<std::string, std::string>> kEmptySection;

		std::string Serialize(const Manifest& manifest, const std::string& kind)
		{
			std::ostringstream out;
			out << "; BBCF Improvement Mod " << kind << ". Open it from the mod's menu to use it.\n";
			// [Package] first, whatever map order says: it is what identifies the file.
			const auto writeSection = [&](const std::string& name) {
				out << "[" << name << "]\n";
				for (const auto& entry : manifest.Section(name))
					out << entry.first << "=" << entry.second << "\n";
				out << "\n";
			};
			writeSection("Package");
			for (const auto& section : manifest.sections)
			{
				if (section.first != "Package")
					writeSection(section.first);
			}
			return out.str();
		}

		Manifest Parse(const std::string& text)
		{
			Manifest manifest;
			std::istringstream in(text);
			std::string line;
			std::string section;
			while (std::getline(in, line))
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty() || line[0] == ';' || line[0] == '#')
					continue;
				if (line.front() == '[' && line.back() == ']')
				{
					section = line.substr(1, line.size() - 2);
					continue;
				}
				const size_t eq = line.find('=');
				if (eq == std::string::npos || section.empty())
					continue;
				manifest.Add(section, line.substr(0, eq), line.substr(eq + 1));
			}
			return manifest;
		}

		bool FileExistsW(const std::string& path)
		{
			const DWORD attributes = GetFileAttributesW(utf8_to_utf16(path).c_str());
			return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
		}

		long long FileSize(const std::string& path)
		{
			WIN32_FILE_ATTRIBUTE_DATA data = {};
			if (!GetFileAttributesExW(utf8_to_utf16(path).c_str(), GetFileExInfoStandard, &data))
				return -1;
			return (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
		}

		// Creates every missing folder of an absolute path.
		void CreateFolders(const std::string& dir)
		{
			std::wstring wide = utf8_to_utf16(dir);
			for (size_t i = 3; i <= wide.size(); ++i)
			{
				if (i == wide.size() || wide[i] == L'\\' || wide[i] == L'/')
					CreateDirectoryW(wide.substr(0, i).c_str(), NULL);
			}
		}

		// Where an archived song lands: the path asked for, unless it is taken by a
		// different file, then "name (2).ext" and so on.
		std::string ChooseDestination(const std::string& wanted, long long size)
		{
			const size_t slash = wanted.find_last_of("\\/");
			const std::string dir = slash == std::string::npos ? std::string() : wanted.substr(0, slash + 1);
			const std::string fileName = wanted.substr(dir.size());
			const size_t dot = fileName.find_last_of('.');
			const std::string stem = dot == std::string::npos ? fileName : fileName.substr(0, dot);
			const std::string ext = dot == std::string::npos ? "" : fileName.substr(dot);
			for (int n = 1; n < 1000; ++n)
			{
				const std::string candidate = dir + (n == 1 ? fileName : stem + " (" + std::to_string(n) + ")" + ext);
				if (!FileExistsW(candidate) || FileSize(candidate) == size)
					return candidate;
			}
			return wanted;
		}
	}

	void Manifest::Add(const std::string& section, const std::string& key, const std::string& value)
	{
		sections[section].emplace_back(key, value);
	}

	const std::vector<std::pair<std::string, std::string>>& Manifest::Section(const std::string& name) const
	{
		auto it = sections.find(name);
		return it == sections.end() ? kEmptySection : it->second;
	}

	std::string Manifest::Get(const std::string& section, const std::string& key, const std::string& fallback) const
	{
		for (const auto& entry : Section(section))
		{
			if (entry.first == key)
				return entry.second;
		}
		return fallback;
	}

	static WriteResult WriteNow(const std::string& path, const Manifest& manifest,
		const std::vector<SongFile>& songs, const std::string& kind, const std::shared_ptr<Progress>& progress)
	{
			WriteResult result;
			// Written beside the target and moved over it at the end, so a failed export
			// never leaves a half-written pack under the name the user chose.
			const std::string temp = path + ".partial";
			DeleteFileW(utf8_to_utf16(temp).c_str());

			mz_zip_archive zip = {};
			if (!mz_zip_writer_init_file_v2(&zip, temp.c_str(), 0, 0))
			{
				result.error = "Could not create the file (is the folder writable?)";
				return result;
			}
			// The kind is what an import checks first, so it is never left to the caller.
			Manifest stamped = manifest;
			if (stamped.Get("Package", "Kind").empty())
				stamped.Add("Package", "Kind", kind);
			const std::string text = Serialize(stamped, kind);
			if (progress)
			{
				long long total = 0;
				for (const SongFile& song : songs)
					total += (std::max)(0LL, FileSize(song.sourcePath));
				progress->totalBytes = total;
				progress->totalFiles = static_cast<int>(songs.size());
			}
			bool ok = mz_zip_writer_add_mem(&zip, kManifestName, text.data(), text.size(), MZ_DEFAULT_COMPRESSION) != 0;
			for (const SongFile& song : songs)
			{
				if (!ok)
					break;
				if (!FileExistsW(song.sourcePath))
				{
					result.missingSongs.push_back(song.sourcePath);
					if (progress) ++progress->doneFiles;
					continue;
				}
				ok = mz_zip_writer_add_file(&zip, song.archiveName.c_str(), song.sourcePath.c_str(),
					nullptr, 0, MZ_NO_COMPRESSION) != 0;
				if (progress)
				{
					progress->doneBytes += (std::max)(0LL, FileSize(song.sourcePath));
					++progress->doneFiles;
				}
				if (ok)
					++result.songsWritten;
				else
					result.error = "Could not add " + FileNameOf(song.sourcePath) + " (" +
						mz_zip_get_error_string(mz_zip_get_last_error(&zip)) + ")";
			}
			ok = ok && mz_zip_writer_finalize_archive(&zip);
			mz_zip_writer_end(&zip);
			if (!ok)
			{
				if (result.error.empty())
					result.error = "Could not finish writing the file (is the disk full?)";
				DeleteFileW(utf8_to_utf16(temp).c_str());
				return result;
			}
			if (!MoveFileExW(utf8_to_utf16(temp).c_str(), utf8_to_utf16(path).c_str(), MOVEFILE_REPLACE_EXISTING))
			{
				result.error = "Could not save over " + FileNameOf(path) + " (is it open somewhere?)";
				DeleteFileW(utf8_to_utf16(temp).c_str());
				return result;
			}
			result.ok = true;
			LOG(1, "[MusicPackage] Wrote %s \"%s\": %d song(s), %d missing\n", kind.c_str(), path.c_str(),
				result.songsWritten, static_cast<int>(result.missingSongs.size()));
			return result;
	}

	std::future<WriteResult> WriteAsync(const std::string& path, const Manifest& manifest,
		const std::vector<SongFile>& songs, const std::string& kind, std::shared_ptr<Progress> progress)
	{
		return std::async(std::launch::async, [path, manifest, songs, kind, progress]() {
			// Whatever this throws would come out of get() on the game's main thread.
			try
			{
				return WriteNow(path, manifest, songs, kind, progress);
			}
			catch (const std::exception& e)
			{
				WriteResult failed;
				failed.error = std::string("Could not write the file: ") + e.what();
				return failed;
			}
		});
	}

	static ReadResult ReadNow(const std::string& path, const std::string& kind,
		const DestinationsFor& destinationsFor, const std::shared_ptr<Progress>& progress)
	{
			ReadResult result;
			mz_zip_archive zip = {};
			if (!mz_zip_reader_init_file(&zip, path.c_str(), 0))
			{
				result.error = "This is not a file the mod can open";
				return result;
			}
			size_t size = 0;
			void* text = mz_zip_reader_extract_file_to_heap(&zip, kManifestName, &size, 0);
			if (!text)
			{
				mz_zip_reader_end(&zip);
				result.error = "This file has no manifest, so it is not a " + kind;
				return result;
			}
			result.manifest = Parse(std::string(static_cast<const char*>(text), size));
			mz_free(text);
			if (result.manifest.Get("Package", "Kind") != kind)
			{
				mz_zip_reader_end(&zip);
				result.error = "This file is not a " + kind;
				return result;
			}

			const std::map<std::string, std::string> destinations = destinationsFor(result.manifest);
			if (progress)
			{
				long long total = 0;
				for (const auto& entry : destinations)
				{
					const int index = mz_zip_reader_locate_file(&zip, entry.first.c_str(), nullptr, 0);
					mz_zip_archive_file_stat stat = {};
					if (index >= 0 && mz_zip_reader_file_stat(&zip, index, &stat))
						total += static_cast<long long>(stat.m_uncomp_size);
				}
				progress->totalBytes = total;
				progress->totalFiles = static_cast<int>(destinations.size());
			}
			for (const auto& entry : destinations)
			{
				const int index = mz_zip_reader_locate_file(&zip, entry.first.c_str(), nullptr, 0);
				mz_zip_archive_file_stat stat = {};
				if (index < 0 || !mz_zip_reader_file_stat(&zip, index, &stat))
				{
					LOG(1, "[MusicPackage] %s lists %s but does not contain it\n", path.c_str(), entry.first.c_str());
					continue;
				}
				const size_t slash = entry.second.find_last_of("\\/");
				if (slash != std::string::npos)
					CreateFolders(entry.second.substr(0, slash));
				const std::string dest = ChooseDestination(entry.second, static_cast<long long>(stat.m_uncomp_size));
				if (progress)
				{
					progress->doneBytes += static_cast<long long>(stat.m_uncomp_size);
					++progress->doneFiles;
				}
				if (FileExistsW(dest) && FileSize(dest) == static_cast<long long>(stat.m_uncomp_size))
				{
					result.extracted[entry.first] = dest; // already there from an earlier import
					continue;
				}
				if (!mz_zip_reader_extract_to_file(&zip, index, dest.c_str(), 0))
				{
					result.error = "Could not extract " + FileNameOf(entry.second) + " (is the disk full?)";
					mz_zip_reader_end(&zip);
					return result;
				}
				result.extracted[entry.first] = dest;
			}
			mz_zip_reader_end(&zip);
			result.ok = true;
			LOG(1, "[MusicPackage] Opened %s \"%s\": %d song(s) extracted\n", kind.c_str(), path.c_str(),
				static_cast<int>(result.extracted.size()));
			return result;
	}

	std::future<ReadResult> ReadAsync(const std::string& path, const std::string& kind,
		DestinationsFor destinationsFor, std::shared_ptr<Progress> progress)
	{
		return std::async(std::launch::async, [path, kind, destinationsFor, progress]() {
			// Whatever this throws would come out of get() on the game's main thread.
			try
			{
				return ReadNow(path, kind, destinationsFor, progress);
			}
			catch (const std::exception& e)
			{
				ReadResult failed;
				failed.error = std::string("Could not open the file: ") + e.what();
				return failed;
			}
		});
	}

	float Progress::Fraction() const
	{
		const long long total = totalBytes.load();
		if (total <= 0)
			return 0.0f;
		const float f = static_cast<float>(static_cast<double>(doneBytes.load()) / static_cast<double>(total));
		return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
	}

	std::string FileNameOf(const std::string& path)
	{
		const size_t slash = path.find_last_of("\\/");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	std::string StemOf(const std::string& path)
	{
		const std::string name = FileNameOf(path);
		const size_t dot = name.find_last_of('.');
		return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
	}

	std::string SafeFileName(const std::string& name)
	{
		std::string safe = FileNameOf(name);
		for (char& c : safe)
		{
			if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*' ||
				static_cast<unsigned char>(c) < 32)
				c = '_';
		}
		while (!safe.empty() && (safe.back() == '.' || safe.back() == ' '))
			safe.pop_back();
		return safe.empty() ? "song" : safe;
	}

	std::string ArchiveNameFor(int n, const std::string& sourcePath)
	{
		const std::string name = FileNameOf(sourcePath);
		const size_t dot = name.find_last_of('.');
		std::string ext = dot == std::string::npos ? "" : name.substr(dot);
		for (char& c : ext)
		{
			if (static_cast<unsigned char>(c) >= 128 || !(isalnum(static_cast<unsigned char>(c)) || c == '.'))
				c = '_';
		}
		return "songs/" + std::to_string(n) + ext;
	}
}
