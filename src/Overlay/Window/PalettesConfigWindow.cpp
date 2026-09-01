#include "PalettesConfigWindow.h"

#include "Core/NativeFileDialog.h"

#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/utils.h"
#include "Game/characters.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Logger/ImGuiLogger.h"
#include "Palette/impl_templates.h"
#include "Palette/PaletteManager.h"
#include "Overlay/NotificationBar/NotificationBar.h"
#include "Palette/PaletteSheet.h"
#include "Palette/PaletteThumbnails.h"
#include "Palette/PngPalette.h"

#include "imgui_internal.h"

#include <Windows.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>

namespace
{
	const int kPaletteSlotCount = 24;    // in-game color slots per character
	const int kSwatchColorCount = 10;    // preview colors shown per palette
	const ImVec4 kAssignedColor(0.30f, 0.85f, 0.39f, 1.0f);

	bool NamesEqual(const std::string& a, const std::string& b)
	{
		return _stricmp(a.c_str(), b.c_str()) == 0;
	}

	// palettes.ini values resolved by PaletteManager::ApplyDefaultCustomPalette
	// at color-select time; they have no palette file behind them.
	const char* kRandomIniValue = "Random";
	const char* kRandomExcludeDefaultIniValue = "Random_Exclude_Default";

	std::string DisplayNameForSlotValue(const std::string& value)
	{
		if (NamesEqual(value, kRandomIniValue))
			return Messages.Palette_random_label();
		if (NamesEqual(value, kRandomExcludeDefaultIniValue))
			return Messages.Palette_random_exclude_label();
		return value;
	}

	std::string FileNameFromPath(const std::string& path)
	{
		const size_t slash = path.find_last_of("\\/");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	bool HasExtension(const std::string& fileName, const char* extension)
	{
		const size_t dot = fileName.rfind('.');
		if (dot == std::string::npos)
			return false;
		return _stricmp(fileName.c_str() + dot, extension) == 0;
	}

	// PNG palette interchange (PLTE chunk), the format UNI2 Improvement Mod and unPAC use.
	constexpr const char* kPngFileExtension = ".png";

	constexpr const char* kFileDialogOwner = "palettes_window";
	constexpr int kFileDialogExportPalette = 0;
	constexpr int kFileDialogImportPalette = 1;

	// The picker thread only asks for a path; the palette waits here and is written on the
	// UI thread once the answer comes back. IMPL_t is not copy-assignable, so it is held as
	// raw bytes rather than a value.
	std::vector<unsigned char> g_pendingExportPaletteBytes;
	std::string g_pendingExportPalName;

	// Palettes can sit in subfolders and exist as .cfpl or legacy .hpl; legacy
	// ones may also have companion "<name>_effect0X.hpl" / "<name>_effectbloom.hpl"
	// files that would turn into load errors if left behind, so those go too.
	bool MatchesPaletteOrCompanion(const std::string& baseName, const std::string& palName)
	{
		if (NamesEqual(baseName, palName))
			return true;

		if (baseName.size() <= palName.size() ||
			_strnicmp(baseName.c_str(), palName.c_str(), palName.size()) != 0)
			return false;

		const char* suffix = baseName.c_str() + palName.size();
		return _strnicmp(suffix, "_effect0", 8) == 0 || _stricmp(suffix, "_effectbloom") == 0;
	}

	void DeletePaletteFilesRecursive(const std::string& folder, const std::string& palName, int& deletedCount)
	{
		WIN32_FIND_DATAA data;
		HANDLE hFind = FindFirstFileA((folder + "*").c_str(), &data);
		if (hFind == INVALID_HANDLE_VALUE)
			return;

		do
		{
			if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0)
				continue;

			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				DeletePaletteFilesRecursive(folder + data.cFileName + "\\", palName, deletedCount);
				continue;
			}

			const std::string fileName = data.cFileName;
			if (!HasExtension(fileName, IMPL_FILE_EXTENSION) &&
				!HasExtension(fileName, LEGACY_HPL_FILE_EXTENSION))
				continue;

			const std::string baseName = fileName.substr(0, fileName.rfind('.'));
			if (!MatchesPaletteOrCompanion(baseName, palName))
				continue;

			if (DeleteFileA((folder + fileName).c_str()))
			{
				++deletedCount;
				g_imGuiLogger->Log("[system] Deleted palette file '%s'\n", (folder + fileName).c_str());
			}
			else
			{
				g_imGuiLogger->Log("[error] Failed to delete palette file '%s'\n", (folder + fileName).c_str());
			}
		} while (FindNextFileA(hFind, &data));
		FindClose(hFind);
	}
}

void PalettesConfigWindow::DrawOpenButton()
{
	if (!ImGui::Button(Messages.Palettes()))
		return;

	BuildRows();
	// "###" keeps the popup ID stable regardless of the UI language.
	ImGui::OpenPopup("###palettes_modal");
}

void PalettesConfigWindow::BuildRows()
{
	m_draftSlots.clear();
	m_filter.Clear();
	m_draftAllowDownloads = Settings::settingsIni.allowPaletteDownloads;

	PaletteManager* paletteManager = g_interfaces.pPaletteManager;
	if (!paletteManager)
	{
		m_groups.clear();
		return;
	}

	m_draftSlots = paletteManager->GetPaletteSlots();
	RebuildGroupsFromDraft();
}

void PalettesConfigWindow::RebuildGroupsFromDraft()
{
	m_groups.clear();

	PaletteManager* paletteManager = g_interfaces.pPaletteManager;
	if (!paletteManager)
		return;

	m_draftSlots.resize(getCharactersCount());
	for (auto& charSlots : m_draftSlots)
		charSlots.resize(kPaletteSlotCount);

	const auto& customPalettes = paletteManager->GetCustomPalettesVector();
	for (int i = 0; i < getCharactersCount() && i < (int)customPalettes.size(); i++)
	{
		// Index 0 is the built-in "Default" entry; palettes past the online start
		// index were downloaded from opponents mid-session and have no local file.
		const int localPalCount = (std::min)((int)customPalettes[i].size(),
			paletteManager->GetOnlinePalsStartIndex((CharIndex)i));
		if (localPalCount <= 1)
			continue;

		CharacterGroup group;
		group.charIndex = i;
		group.charName = getCharacterNameByIndexA(i);

		// The two Random pseudo-entries lead the list so they can be assigned
		// to a color like any palette; the engine resolves them on selection.
		const char* specialValues[] = { kRandomIniValue, kRandomExcludeDefaultIniValue };
		for (int s = 0; s < 2; s++)
		{
			PaletteRow row;
			row.palIndex = -1 - s;
			row.isSpecial = true;
			row.name = specialValues[s];

			for (int slot = 1; slot <= kPaletteSlotCount; slot++)
			{
				if (NamesEqual(m_draftSlots[i][slot - 1], row.name))
				{
					row.assignedSlot = slot;
					break;
				}
			}

			group.rows.push_back(row);
		}

		for (int palIndex = 1; palIndex < localPalCount; palIndex++)
		{
			PaletteRow row;
			row.palIndex = palIndex;
			const char* palName = customPalettes[i][palIndex].palInfo.palName;
			row.name.assign(palName, strnlen(palName, IMPL_PALNAME_LENGTH));

			for (int slot = 1; slot <= kPaletteSlotCount; slot++)
			{
				if (NamesEqual(m_draftSlots[i][slot - 1], row.name))
				{
					row.assignedSlot = slot;
					break;
				}
			}

			group.rows.push_back(row);
		}

		m_groups.push_back(group);
	}

	// The grid was just rebuilt, so any selection points at a row that may no longer
	// exist. Drop it rather than risk indexing past the end.
	m_selectedRow = -1;
	if (m_selectedGroup < 0 || m_selectedGroup >= (int)m_groups.size())
		m_selectedGroup = 0;
}

void PalettesConfigWindow::AssignSlot(CharacterGroup& group, PaletteRow& row, int newSlot)
{
	if (newSlot == row.assignedSlot)
		return;

	std::vector<std::string>& charSlots = m_draftSlots[group.charIndex];

	if (row.assignedSlot > 0)
		charSlots[row.assignedSlot - 1].clear();

	if (newSlot > 0)
	{
		// A color can only hold one palette, so whoever occupied it gets unassigned.
		for (PaletteRow& other : group.rows)
			if (other.assignedSlot == newSlot)
				other.assignedSlot = 0;

		charSlots[newSlot - 1] = row.name;
	}

	row.assignedSlot = newSlot;
}

namespace
{
	// Import and export used to report only through g_imGuiLogger, which lives in the Log
	// window almost nobody has open - so a failed import looked exactly like the button
	// doing nothing at all. Every outcome now also goes to the on-screen notification bar,
	// which is visible whatever window has focus.
	void ReportPaletteOutcome(const char* logLine, const std::string& notification)
	{
		if (g_imGuiLogger)
			g_imGuiLogger->Log("%s", logLine);
		if (g_notificationBar)
			g_notificationBar->AddNotification(notification.c_str());
	}
}

void PalettesConfigWindow::ImportPaletteFile(const std::string& sourcePath, int charIndex)
{
	const std::string fileName = FileNameFromPath(sourcePath);
	const std::string folder = std::string("BBCF_IM\\Palettes\\") + getCharacterNameByIndexA(charIndex) + "\\";

	const bool isPng = HasExtension(fileName, kPngFileExtension);

	std::string baseName = fileName;
	std::string extension;
	const size_t dot = fileName.rfind('.');
	if (dot != std::string::npos)
	{
		baseName = fileName.substr(0, dot);
		extension = fileName.substr(dot);
	}

	if (isPng)
	{
		// A PNG is converted on the way in; what lands in the folder is a .cfpl whose
		// name has to survive the palInfo.palName field WritePaletteToFile builds its
		// path from.
		extension = IMPL_FILE_EXTENSION;
		baseName = baseName.substr(0, IMPL_PALNAME_LENGTH - 5);
	}

	std::string finalName = baseName;
	for (int suffix = 2; suffix < 1000; ++suffix)
	{
		const std::string candidate = folder + finalName + extension;
		if (GetFileAttributesA(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
			break;

		finalName = baseName + "_" + std::to_string(suffix);
	}

	const std::string destPath = folder + finalName + extension;

	if (isPng)
	{
		char characterFile[IMPL_PALETTE_DATALEN];
		std::string error;
		if (!PngPalette::ReadPaletteFile(sourcePath, characterFile, error))
		{
			ReportPaletteOutcome(("[error] Unable to import '" + fileName + "' : " + error + "\n").c_str(),
				"Could not import " + fileName + ": " + error);
			return;
		}

		// A PNG only carries the character colors; the effect files come from the
		// character's built-in template so they are never left blank.
		IMPL_data_t palData;
		if (!g_interfaces.pPaletteManager->CreatePaletteFromCharacterFile(
			(CharIndex)charIndex, finalName, characterFile, palData) ||
			!g_interfaces.pPaletteManager->WritePaletteToFile((CharIndex)charIndex, &palData))
		{
			ReportPaletteOutcome(("[error] Failed to import palette '" + fileName + "' into '" + destPath + "'\n").c_str(),
				"Could not save the imported palette to " + destPath);
			return;
		}
	}
	else if (CopyFileA(sourcePath.c_str(), destPath.c_str(), TRUE) != TRUE)
	{
		ReportPaletteOutcome(("[error] Failed to import palette '" + fileName + "' into '" + destPath + "'\n").c_str(),
			"Could not copy the palette to " + destPath);
		return;
	}

	ReportPaletteOutcome(("[system] Imported palette '" + finalName + extension + "' for " +
			getCharacterNameByIndexA(charIndex) + "\n").c_str(),
		"Imported " + finalName + extension + " for " + getCharacterNameByIndexA(charIndex));

	// Register the new file. This re-reads palette folders and palettes.ini, but
	// the modal keeps its own draft, so unsaved assignment edits survive.
	g_interfaces.pPaletteManager->ReloadAllPalettes();
	RebuildGroupsFromDraft();
}

void PalettesConfigWindow::DeletePalette(int charIndex, const std::string& palName)
{
	const std::string folder = std::string("BBCF_IM\\Palettes\\") + getCharacterNameByIndexA(charIndex) + "\\";

	int deletedCount = 0;
	DeletePaletteFilesRecursive(folder, palName, deletedCount);

	if (deletedCount == 0)
	{
		g_imGuiLogger->Log("[error] No file found for palette '%s' (%s); nothing was deleted\n",
			palName.c_str(), getCharacterNameByIndexA(charIndex).c_str());
		return;
	}

	// Drop any draft color assignment pointing at the deleted palette.
	if (charIndex < (int)m_draftSlots.size())
	{
		for (std::string& slotValue : m_draftSlots[charIndex])
			if (NamesEqual(slotValue, palName))
				slotValue.clear();
	}

	g_interfaces.pPaletteManager->ReloadAllPalettes();
	RebuildGroupsFromDraft();
}

void PalettesConfigWindow::DrawDeleteConfirmModal()
{
	if (m_openDeleteConfirm)
	{
		ImGui::OpenPopup("###palettes_delete_confirm");
		m_openDeleteConfirm = false;
	}

	const std::string deleteTitle = std::string(Messages.Delete_palette()) + "###palettes_delete_confirm";
	ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Always);
	if (!ImGui::BeginPopupModal(deleteTitle.c_str(), nullptr,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::TextWrapped(Messages.Palette_delete_confirm(), m_pendingDeletePalName.c_str(),
		getCharacterNameByIndexA(m_pendingDeleteCharIndex).c_str());
	ImGui::Spacing();
	ImGui::TextWrapped("%s", Messages.Palette_delete_details());
	ImGui::Spacing();

	const float buttonsWidth = 120.0f * 2.0f + ImGui::GetStyle().ItemSpacing.x;
	ImGui::SetCursorPosX((std::max)(ImGui::GetStyle().WindowPadding.x,
		(ImGui::GetWindowWidth() - buttonsWidth) * 0.5f));

	if (ImGui::Button(Messages.Cancel(), ImVec2(120, 0)))
	{
		m_pendingDeletePalName.clear();
		ImGui::CloseCurrentPopup();
	}

	ImGui::SameLine();

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.18f, 0.18f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.12f, 0.12f, 1.0f));
	if (ImGui::Button(Messages.Delete(), ImVec2(120, 0)))
	{
		DeletePalette(m_pendingDeleteCharIndex, m_pendingDeletePalName);
		m_pendingDeletePalName.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::PopStyleColor(3);

	ImGui::EndPopup();
}

void PalettesConfigWindow::ConsumeFinishedFileDialog()
{
	NativeFileDialog::Result result;
	if (!NativeFileDialog::Consume(kFileDialogOwner, &result) || !result.accepted)
		return;

	const std::string path = result.path;

	if (result.contextId == kFileDialogExportPalette)
	{
		bool written = false;
		std::string error;

		if (g_pendingExportPaletteBytes.size() == sizeof(IMPL_t))
		{
			if (HasExtension(FileNameFromPath(path), kPngFileExtension))
			{
				// A PNG palette only holds the character colors, so the effect files are
				// dropped; .cfpl stays the lossless format.
				const IMPL_t* paletteFile = (const IMPL_t*)g_pendingExportPaletteBytes.data();
				const char* characterColors = paletteFile->palData.file0;
				const int charIndex = paletteFile->header.charIndex;

				// Paint the palette onto the character's reference sheet: recolouring a
				// picture of the character beats recolouring 256 numbered squares. Falls
				// back to a plain swatch grid if the sheet is somehow unavailable, so an
				// export always produces something importable.
				written = PaletteSheet::Write(charIndex, characterColors, path, error);
				if (!written)
				{
					g_imGuiLogger->Log("[system] No reference sheet for this character (%s); exporting a swatch grid instead.\n",
						error.c_str());
					error.clear();
					written = PngPalette::WritePaletteFile(path, characterColors, error, charIndex);
				}
			}
			else
			{
				written = utils_WriteFile(path.c_str(), g_pendingExportPaletteBytes.data(), sizeof(IMPL_t), true);
			}
		}

		if (written)
		{
			ReportPaletteOutcome(("[system] Exported palette '" + g_pendingExportPalName + "' to '" + path + "'\n").c_str(),
				"Exported " + g_pendingExportPalName + " to " + FileNameFromPath(path));
		}
		else
		{
			const std::string why = error.empty() ? std::string("unable to write the file") : error;
			ReportPaletteOutcome(("[error] Failed to export palette '" + g_pendingExportPalName + "' to '" + path + "' : " + why + "\n").c_str(),
				"Could not export " + g_pendingExportPalName + ": " + why);
		}
		return;
	}

	if (result.contextId != kFileDialogImportPalette || path.empty())
		return;

	const std::string fileName = FileNameFromPath(path);

	if (HasExtension(fileName, IMPL_FILE_EXTENSION))
	{
		// .cfpl headers carry the character index, so no character prompt is needed.
		IMPL_t fileContents;
		if (!utils_ReadFile(path.c_str(), &fileContents, sizeof(fileContents), true))
		{
			ReportPaletteOutcome(("[error] Unable to open '" + fileName + "' : " + strerror(errno) + "\n").c_str(),
				"Could not open " + fileName);
			return;
		}

		if (strncmp(fileContents.header.fileSig, IMPL_FILESIG, sizeof(fileContents.header.fileSig)) != 0 ||
			fileContents.header.dataLen != sizeof(IMPL_data_t))
		{
			ReportPaletteOutcome(("[error] '" + fileName + "' unrecognized palette file format!\n").c_str(),
				fileName + " is not a palette file this build recognises");
			return;
		}

		if (isCharacterIndexOutOfBound(fileContents.header.charIndex))
		{
			ReportPaletteOutcome(("[error] '" + fileName + "' has an invalid character index in the header\n").c_str(),
				fileName + " names a character this build does not know");
			return;
		}

		ImportPaletteFile(path, fileContents.header.charIndex);
	}
	else if (HasExtension(fileName, LEGACY_HPL_FILE_EXTENSION) ||
		HasExtension(fileName, kPngFileExtension))
	{
		// Legacy .hpl files and PNG palettes carry no character info: ask the user.
		// A PNG we exported ourselves names its character, so a round trip needs no prompt
		// at all. Anything else - a PNG from another tool, or one an editor stripped the
		// marker from, or a legacy .hpl - still has to be asked about.
		if (HasExtension(fileName, kPngFileExtension))
		{
			char probe[IMPL_PALETTE_DATALEN];
			int stampedChar = -1;
			std::string probeError;
			if (PngPalette::ReadPaletteFileWithCharacter(path, probe, &stampedChar, probeError) &&
				stampedChar >= 0 && !isCharacterIndexOutOfBound(stampedChar))
			{
				ImportPaletteFile(path, stampedChar);
				return;
			}
			if (!probeError.empty())
			{
				ReportPaletteOutcome(("[error] Unable to import '" + fileName + "' : " + probeError + "\n").c_str(),
					"Could not import " + fileName + ": " + probeError);
				return;
			}
		}

		// No character on the file, so the import is finished in the character-select
		// popup. Say so: if that popup ever fails to appear, the user still sees that the
		// file arrived rather than nothing at all.
		m_pendingImportPath = path;
		m_pendingImportCharIndex = 0;
		m_openImportCharSelect = true;
		if (g_notificationBar)
			g_notificationBar->AddNotification(("Pick a character for " + fileName).c_str());
	}
	else
	{
		ReportPaletteOutcome(("[error] Unable to import '" + fileName + "' : not a " + IMPL_FILE_EXTENSION +
				", " + LEGACY_HPL_FILE_EXTENSION + " or " + kPngFileExtension + " file\n").c_str(),
			fileName + " is not a palette file (.cfpl, .hpl or .png)");
	}
}

void PalettesConfigWindow::DrawImportCharSelectModal()
{
	if (m_openImportCharSelect)
	{
		ImGui::OpenPopup("###palettes_import_charselect");
		m_openImportCharSelect = false;
	}

	const std::string importTitle = std::string(Messages.Import_palette_title()) + "###palettes_import_charselect";
	ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Always);
	if (!ImGui::BeginPopupModal(importTitle.c_str(), nullptr,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::TextWrapped(Messages.Palette_import_legacy_prompt(),
		FileNameFromPath(m_pendingImportPath).c_str());
	ImGui::Spacing();
	ImGui::TextUnformatted(Messages.Import_it_for());

	ImGui::PushItemWidth(-1);
	if (ImGui::BeginCombo("##import_char", getCharacterNameByIndexA(m_pendingImportCharIndex).c_str()))
	{
		for (int i = 0; i < getCharactersCount(); i++)
		{
			if (ImGui::Selectable(getCharacterNameByIndexA(i).c_str(), i == m_pendingImportCharIndex))
				m_pendingImportCharIndex = i;
		}
		ImGui::EndCombo();
	}
	ImGui::PopItemWidth();

	ImGui::Spacing();

	const float buttonsWidth = 120.0f * 2.0f + ImGui::GetStyle().ItemSpacing.x;
	ImGui::SetCursorPosX((std::max)(ImGui::GetStyle().WindowPadding.x,
		(ImGui::GetWindowWidth() - buttonsWidth) * 0.5f));

	if (ImGui::Button(Messages.Cancel(), ImVec2(120, 0)))
	{
		m_pendingImportPath.clear();
		ImGui::CloseCurrentPopup();
	}

	ImGui::SameLine();

	if (ImGui::Button(Messages.Import(), ImVec2(120, 0)))
	{
		ImportPaletteFile(m_pendingImportPath, m_pendingImportCharIndex);
		m_pendingImportPath.clear();
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}

void PalettesConfigWindow::DrawImportButton()
{
	const float buttonWidth = 170.0f;
	ImGui::SetCursorPosX((std::max)(ImGui::GetStyle().WindowPadding.x,
		(ImGui::GetWindowWidth() - buttonWidth) * 0.5f));

	const bool dialogBusy = NativeFileDialog::IsOpen();
	ImGui::BeginDisabled(dialogBusy);

	if (ImGui::Button(Messages.Import_palette_button(), ImVec2(buttonWidth, 0)))
	{
		NativeFileDialog::Request request;
		request.title = "Import palette";
		request.filters.push_back({ "BBCF Palettes (*.cfpl;*.hpl;*.png)", "*.cfpl;*.hpl;*.png" });
		request.contextId = kFileDialogImportPalette;
		NativeFileDialog::Open(kFileDialogOwner, request);
	}

	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::ShowHelpMarker(Messages.Palette_import_help());
}

// Export is reachable from the detail panel; kept separate so the panel stays readable.
void PalettesConfigWindow::ExportPalette(const CharacterGroup& group, const PaletteRow& row)
{
	if (NativeFileDialog::IsOpen())
		return;

	const auto& customPalettes = g_interfaces.pPaletteManager->GetCustomPalettesVector();

	IMPL_t paletteFile{};
	paletteFile.header.headerLen = sizeof(IMPL_header_t);
	paletteFile.header.dataLen = sizeof(IMPL_data_t);
	paletteFile.header.charIndex = (short)group.charIndex;
	paletteFile.palData = customPalettes[group.charIndex][row.palIndex];

	g_pendingExportPaletteBytes.resize(sizeof(IMPL_t));
	std::memcpy(g_pendingExportPaletteBytes.data(), &paletteFile, sizeof(IMPL_t));
	g_pendingExportPalName = row.name;

	NativeFileDialog::Request request;
	request.save = true;
	request.title = "Export palette";
	request.filters.push_back({ "BBCF Palette (*.cfpl)", "*.cfpl" });
	request.filters.push_back({ "PNG Palette (*.png)", "*.png" });
	request.defaultExtension = "cfpl";
	request.initialPath = row.name + IMPL_FILE_EXTENSION;
	request.contextId = kFileDialogExportPalette;
	NativeFileDialog::Open(kFileDialogOwner, request);
}

void PalettesConfigWindow::DrawSlotCombo(CharacterGroup& group, PaletteRow& row)
{
	std::vector<std::string>& charSlots = m_draftSlots[group.charIndex];

	char preview[64];
	if (row.assignedSlot > 0)
		snprintf(preview, sizeof(preview), Messages.Color_d(), row.assignedSlot);
	else
		snprintf(preview, sizeof(preview), "%s", Messages.Not_assigned());

	if (row.assignedSlot > 0)
		ImGui::PushStyleColor(ImGuiCol_Text, kAssignedColor);

	ImGui::PushItemWidth(-1.0f);
	const bool comboOpen = ImGui::BeginCombo("##slot", preview);
	if (row.assignedSlot > 0)
		ImGui::PopStyleColor();

	if (comboOpen)
	{
		if (ImGui::Selectable(Messages.Not_assigned(), row.assignedSlot == 0))
			AssignSlot(group, row, 0);

		for (int slot = 1; slot <= kPaletteSlotCount; slot++)
		{
			const std::string& occupant = charSlots[slot - 1];
			char label[128];
			if (!occupant.empty() && !NamesEqual(occupant, row.name))
				snprintf(label, sizeof(label), Messages.Color_used_by(),
					slot, DisplayNameForSlotValue(occupant).c_str());
			else
				snprintf(label, sizeof(label), Messages.Color_d(), slot);

			if (ImGui::Selectable(label, row.assignedSlot == slot))
				AssignSlot(group, row, slot);
		}
		ImGui::EndCombo();
	}
	ImGui::PopItemWidth();
}

void PalettesConfigWindow::DrawCharacterPicker()
{
	if (m_selectedGroup < 0 || m_selectedGroup >= (int)m_groups.size())
		m_selectedGroup = 0;

	ImGui::PushItemWidth(200.0f);
	if (ImGui::BeginCombo("##palettes_character", m_groups[m_selectedGroup].charName.c_str()))
	{
		for (int i = 0; i < (int)m_groups.size(); i++)
		{
			const CharacterGroup& group = m_groups[i];

			int paletteCount = 0;
			for (const PaletteRow& row : group.rows)
				if (!row.isSpecial)
					++paletteCount;

			char label[160];
			snprintf(label, sizeof(label), "%s (%d)###palchar_%d",
				group.charName.c_str(), paletteCount, group.charIndex);

			if (ImGui::Selectable(label, i == m_selectedGroup))
			{
				m_selectedGroup = i;
				m_selectedRow = -1;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::PopItemWidth();
	ImGui::SameLine();
	ImGui::TextDisabled("%s", Messages.Palette_assignments_hint());
}

void PalettesConfigWindow::DrawGrid(CharacterGroup& group)
{
	const auto& customPalettes = g_interfaces.pPaletteManager->GetCustomPalettesVector();

	// Cells are sized from the sprite's own aspect so nothing is stretched, and the
	// column count follows the window rather than being fixed, so the grid reflows.
	const float cellWidth = 96.0f;
	const float cellHeight = 120.0f;
	const float labelHeight = ImGui::GetTextLineHeightWithSpacing();
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float available = ImGui::GetContentRegionAvail().x;
	int columns = (int)((available + spacing) / (cellWidth + spacing));
	if (columns < 1)
		columns = 1;

	int drawn = 0;
	for (int rowIndex = 0; rowIndex < (int)group.rows.size(); rowIndex++)
	{
		PaletteRow& row = group.rows[rowIndex];
		const std::string displayName = row.isSpecial ? DisplayNameForSlotValue(row.name) : row.name;

		if (!m_filter.PassFilter(displayName.c_str()) && !m_filter.PassFilter(group.charName.c_str()))
			continue;

		if (drawn % columns != 0)
			ImGui::SameLine();
		drawn++;

		ImGui::PushID(rowIndex);
		ImGui::BeginGroup();

		const bool selected = (rowIndex == m_selectedRow);
		const ImVec2 cursor = ImGui::GetCursorScreenPos();

		// One invisible button covers the whole cell, so the sprite, the name and the
		// gap between them are all the same click target.
		const ImVec2 cellSize(cellWidth, cellHeight + labelHeight);
		if (ImGui::InvisibleButton("##cell", cellSize))
			m_selectedRow = rowIndex;
		const bool hovered = ImGui::IsItemHovered();

		ImDrawList* draw = ImGui::GetWindowDrawList();
		if (selected || hovered)
		{
			const ImU32 fill = ImGui::GetColorU32(selected ? ImGuiCol_Header : ImGuiCol_HeaderHovered);
			draw->AddRectFilled(cursor, ImVec2(cursor.x + cellSize.x, cursor.y + cellSize.y), fill, 3.0f);
		}

		if (row.isSpecial)
		{
			// No sprite behind "Random": the palette is not decided until the match starts.
			const char* mark = "?";
			const ImVec2 markSize = ImGui::CalcTextSize(mark);
			draw->AddText(ImVec2(cursor.x + (cellWidth - markSize.x) * 0.5f,
				cursor.y + (cellHeight - markSize.y) * 0.5f),
				ImGui::GetColorU32(ImGuiCol_TextDisabled), mark);
		}
		else
		{
			int texWidth = 0, texHeight = 0;
			const ImTextureID texture = PaletteThumbnails::Get(group.charIndex, row.name,
				customPalettes[group.charIndex][row.palIndex].file0, &texWidth, &texHeight);

			if (texture && texWidth > 0 && texHeight > 0)
			{
				const float scale = (std::min)(cellWidth / texWidth, cellHeight / texHeight);
				const float drawW = texWidth * scale;
				const float drawH = texHeight * scale;
				const ImVec2 topLeft(cursor.x + (cellWidth - drawW) * 0.5f,
					cursor.y + (cellHeight - drawH));
				draw->AddImage(ImTextureRef(texture), topLeft,
					ImVec2(topLeft.x + drawW, topLeft.y + drawH));
			}
			else
			{
				// No sprite for this character in this build, or the texture could not be
				// made: fall back to the colour strip rather than an empty cell.
				const float swatchHeight = cellHeight / 8.0f;
				for (int i = 0; i < 8; i++)
				{
					const unsigned char* bytes =
						(const unsigned char*)customPalettes[group.charIndex][row.palIndex].file0
						+ (1 + i * 12) * 4;
					const ImU32 colour = IM_COL32(bytes[2], bytes[1], bytes[0], 255);
					draw->AddRectFilled(ImVec2(cursor.x + 16.0f, cursor.y + i * swatchHeight),
						ImVec2(cursor.x + cellWidth - 16.0f, cursor.y + (i + 1) * swatchHeight),
						colour);
				}
			}
		}

		// Colour-slot badge, so an assignment is visible without selecting the cell.
		if (row.assignedSlot > 0)
		{
			char badge[16];
			snprintf(badge, sizeof(badge), "%d", row.assignedSlot);
			const ImVec2 badgeSize = ImGui::CalcTextSize(badge);
			const ImVec2 badgeMin(cursor.x + cellWidth - badgeSize.x - 8.0f, cursor.y + 2.0f);
			draw->AddRectFilled(badgeMin,
				ImVec2(badgeMin.x + badgeSize.x + 6.0f, badgeMin.y + badgeSize.y + 2.0f),
				IM_COL32(30, 30, 30, 200), 3.0f);
			draw->AddText(ImVec2(badgeMin.x + 3.0f, badgeMin.y + 1.0f),
				ImGui::GetColorU32(kAssignedColor), badge);
		}

		// Name, clipped to the cell so a long one cannot push the grid out of shape.
		const ImVec2 clipMin(cursor.x + 2.0f, cursor.y + cellHeight);
		const ImVec2 clipMax(cursor.x + cellWidth - 2.0f, cursor.y + cellHeight + labelHeight);
		draw->PushClipRect(clipMin, clipMax, true);
		draw->AddText(clipMin, ImGui::GetColorU32(ImGuiCol_Text), displayName.c_str());
		draw->PopClipRect();

		ImGui::EndGroup();

		if (hovered)
			ImGui::SetTooltip("%s", displayName.c_str());

		ImGui::PopID();
	}

	if (drawn == 0)
		ImGui::TextDisabled("%s", Messages.Palettes_none_found());
}

void PalettesConfigWindow::DrawDetailPanel(CharacterGroup& group)
{
	std::vector<std::string>& charSlots = m_draftSlots[group.charIndex];

	if (m_selectedRow < 0 || m_selectedRow >= (int)group.rows.size())
	{
		ImGui::TextDisabled("%s", Messages.Palette_select_hint());

		// Entries typed into palettes.ini by hand (comma lists, missing files) have no
		// cell in the grid. Keep them visible so saving is not a surprise.
		bool anyManual = false;
		for (int slot = 1; slot <= kPaletteSlotCount; slot++)
		{
			const std::string& value = charSlots[slot - 1];
			if (value.empty())
				continue;

			bool matchesRow = false;
			for (const PaletteRow& row : group.rows)
				if (NamesEqual(value, row.name) && row.assignedSlot == slot)
				{
					matchesRow = true;
					break;
				}

			if (!matchesRow)
			{
				if (!anyManual)
				{
					ImGui::Separator();
					anyManual = true;
				}
				ImGui::TextDisabled(Messages.Palette_manual_entry(), slot, value.c_str());
			}
		}
		return;
	}

	PaletteRow& row = group.rows[m_selectedRow];
	const std::string displayName = row.isSpecial ? DisplayNameForSlotValue(row.name) : row.name;

	ImGui::TextWrapped("%s", displayName.c_str());
	if (row.isSpecial)
	{
		ImGui::TextDisabled("%s", NamesEqual(row.name, kRandomIniValue)
			? Messages.Palette_random_tooltip()
			: Messages.Palette_random_exclude_tooltip());
	}
	ImGui::Separator();

	ImGui::TextUnformatted(Messages.Palette_colour_slot());
	DrawSlotCombo(group, row);

	if (!row.isSpecial)
	{
		const auto& customPalettes = g_interfaces.pPaletteManager->GetCustomPalettesVector();

		ImGui::Spacing();
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, 1.0f));
		for (int colorIdx = 1; colorIdx <= kSwatchColorCount; colorIdx++)
		{
			unsigned char* colorBytes =
				(unsigned char*)customPalettes[group.charIndex][row.palIndex].file0 + colorIdx * 4;
			char swatchId[32];
			snprintf(swatchId, sizeof(swatchId), "##swatch_%d", colorIdx);
			ImGui::ColorButtonOn32Bit(swatchId, colorIdx, colorBytes,
				ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip, ImVec2(10.0f, 17.0f));
			if (colorIdx % 16 != 0)
				ImGui::SameLine();
		}
		ImGui::PopStyleVar();

		ImGui::Spacing();
		ImGui::Separator();

		if (ImGui::Button(Messages.Export(), ImVec2(-1.0f, 0.0f)))
			ExportPalette(group, row);
		ImGui::HoverTooltip(Messages.Palette_export_tooltip());

		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
		if (ImGui::Button(Messages.Delete(), ImVec2(-1.0f, 0.0f)))
		{
			m_pendingDeleteCharIndex = group.charIndex;
			m_pendingDeletePalName = row.name;
			m_openDeleteConfirm = true;
		}
		ImGui::PopStyleColor();
		ImGui::HoverTooltip(Messages.Palette_delete_tooltip());
	}
}

void PalettesConfigWindow::DrawModal()
{
	const std::string modalTitle = std::string(Messages.Palettes()) + "###palettes_modal";
	ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_Always);
	if (!ImGui::BeginPopupModal(modalTitle.c_str(), nullptr, ImGuiWindowFlags_NoResize))
		return;

	if (!g_interfaces.pPaletteManager)
	{
		ImGui::TextUnformatted(Messages.Palettes_not_loaded());
		if (ImGui::Button(Messages.Close(), ImVec2(120, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}

	ConsumeFinishedFileDialog();

	ImGui::TextUnformatted(Messages.Palette_assignments());
	ImGui::SameLine();
	ImGui::TextDisabled("%s", Messages.Palette_assignments_hint());
	const std::string filterLabel = std::string(Messages.Palettes_search_hint()) + "###palettes_filter";
	m_filter.Draw(filterLabel.c_str(), -1.0f);

	DrawImportButton();

	if (m_groups.empty())
	{
		ImGui::BeginChild("##palettes_scroll", ImVec2(0, -64.0f), true);
		ImGui::TextWrapped("%s", Messages.Palettes_none_found());
		ImGui::Spacing();
		ImGui::TextWrapped("%s", Messages.Palettes_none_found_help());
		ImGui::EndChild();
	}
	else
	{
		DrawCharacterPicker();

		// Grid on the left, details for whatever is selected on the right. The grid is
		// what you scan; the panel is where the controls live, so cells stay clean.
		const float panelWidth = 250.0f;
		ImGui::BeginChild("##palettes_grid", ImVec2(-panelWidth, -64.0f), true);
		DrawGrid(m_groups[m_selectedGroup]);
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::BeginChild("##palettes_detail", ImVec2(0, -64.0f), true);
		DrawDetailPanel(m_groups[m_selectedGroup]);
		ImGui::EndChild();
	}

	bool allowDownloadsChecked = (m_draftAllowDownloads == 1);
	if (ImGui::Checkbox(Messages.Palette_allow_downloads(), &allowDownloadsChecked))
		m_draftAllowDownloads = allowDownloadsChecked ? 1 : 0;
	ImGui::SameLine();
	ImGui::ShowHelpMarker(Messages.Palette_allow_downloads_help());

	const float footerWidth = 120.0f * 2.0f + ImGui::GetStyle().ItemSpacing.x;
	ImGui::SetCursorPosX((std::max)(ImGui::GetStyle().WindowPadding.x,
		(ImGui::GetWindowWidth() - footerWidth) * 0.5f));

	if (ImGui::Button(Messages.Cancel(), ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();

	ImGui::SameLine();

	if (ImGui::Button(Messages.Save(), ImVec2(120, 0)))
	{
		if (g_interfaces.pPaletteManager->SavePaletteSettingsFile(m_draftSlots))
			g_imGuiLogger->Log("[system] Palette assignments saved to 'palettes.ini'\n");
		else
			g_imGuiLogger->Log("[error] Failed to save palette assignments to 'palettes.ini'\n");

		if (m_draftAllowDownloads != Settings::settingsIni.allowPaletteDownloads)
		{
			Settings::settingsIni.allowPaletteDownloads = m_draftAllowDownloads;
			Settings::changeSetting("AllowPaletteDownloads", std::to_string(m_draftAllowDownloads));
		}

		ImGui::CloseCurrentPopup();
	}

	DrawImportCharSelectModal();
	DrawDeleteConfirmModal();

	ImGui::EndPopup();
}
