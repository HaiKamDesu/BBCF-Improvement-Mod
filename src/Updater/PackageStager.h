#pragma once

#include "UpdateModels.h"

#include <string>
#include <vector>

namespace Updater
{
	// One central-directory entry, carried from validation through to extraction so the
	// two can never disagree about what the archive contains.
	struct ZipEntryRecord
	{
		std::string name;                        // as stored, forward or back slashes
		bool isDirectory = false;
		unsigned short method = 0;               // 0 = stored, 8 = deflate; nothing else is accepted
		unsigned int compressedSize = 0;
		unsigned int uncompressedSize = 0;
		unsigned int crc32 = 0;
		unsigned int localHeaderOffset = 0;
	};

	struct ZipValidationResult
	{
		bool valid = false;
		std::string error;
		std::vector<std::string> entries;
		std::vector<ZipEntryRecord> records;
		unsigned long long totalUncompressedSize = 0;
	};

	bool DownloadFileWithProgress(
		const std::string& url,
		const std::wstring& tempPath,
		const std::wstring& finalPath,
		const std::string& userAgent,
		volatile long* progressPercent,
		std::string& error);

	bool ComputeFileSha256Hex(const std::wstring& path, std::string& outSha256, std::string& error);
	ZipValidationResult ValidateUpdateZip(const std::wstring& zipPath);
	// Extracts a zip that ValidateUpdateZip has already accepted. Taking the validated
	// result rather than re-parsing means every path, size and compression-method check
	// applies to exactly the bytes that get written.
	//
	// Deliberately does not use Shell.Application: Wine has no zip folder handler, so the
	// shell route fails outright under Proton and the updater could never run on Linux.
	bool ExtractUpdateZip(
		const std::wstring& zipPath,
		const ZipValidationResult& validated,
		const std::wstring& destination,
		std::string& error);
	bool WriteUpdaterHandoff(
		const AvailableUpdate& update,
		const std::wstring& stagedRoot,
		const std::wstring& packagePath,
		std::wstring& outHandoffPath,
		std::string& error);
	bool LaunchUpdaterAndExitGame(const std::wstring& handoffPath, std::string& error);
}
