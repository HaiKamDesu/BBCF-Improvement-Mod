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

#include <cfloat>
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

	// The width of the detail panel is window geometry, so it lives with the window
	// geometry - in ImGui's own menus.ini, next to the size and position of the window it
	// belongs to - rather than in settings.ini, where it would show up as a row asking the
	// user to type a pixel count.
	//
	// ImGui parses that file on the first frame, so the handler has to be registered
	// during init; a handler added later never sees the lines it was meant to read.
	float g_detailPanelWidth = 340.0f;

	void* PalettesLayout_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
	{
		return strcmp(name, "Layout") == 0 ? (void*)1 : nullptr;
	}

	void PalettesLayout_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
	{
		float width = 0.0f;
		if (sscanf_s(line, "DetailPanelWidth=%f", &width) == 1 && width > 0.0f)
			g_detailPanelWidth = width;
	}

	void PalettesLayout_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
	{
		buf->appendf("[%s][Layout]\n", handler->TypeName);
		buf->appendf("DetailPanelWidth=%.0f\n\n", g_detailPanelWidth);
	}

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

void PalettesConfigWindow::RegisterLayoutSettings()
{
	// AddSettingsHandler asserts if the type is already registered, and initialization can
	// run again after a device loss.
	if (ImGui::FindSettingsHandler("BBCFIMPalettes"))
		return;

	ImGuiSettingsHandler handler;
	handler.TypeName = "BBCFIMPalettes";
	handler.TypeHash = ImHashStr("BBCFIMPalettes");
	handler.ReadOpenFn = PalettesLayout_ReadOpen;
	handler.ReadLineFn = PalettesLayout_ReadLine;
	handler.WriteAllFn = PalettesLayout_WriteAll;
	ImGui::AddSettingsHandler(&handler);
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
		PngPalette::Imported imported;
		std::string error;
		if (!PngPalette::ReadPaletteFileEx(sourcePath, imported, error))
		{
			ReportPaletteOutcome(("[error] Unable to import '" + fileName + "' : " + error + "\n").c_str(),
				"Could not import " + fileName + ": " + error);
			return;
		}

		// A PNG carries only the 1024 bytes of character colors - not the seven effect
		// files, and not the creator/description/bloom metadata. Where that data comes
		// from decides how lossy an import is.
		//
		// The usual workflow is export a palette, recolor it, import it back, and in that
		// case the original is still installed under the same name: base on it, so
		// everything a PNG cannot carry survives and only the colors change. Falling back
		// to the character's built-in template is for a PNG that came from somewhere else.
		IMPL_data_t palData;
		bool built = false;

		// Best case: the PNG carries everything itself, so the import is lossless no
		// matter whose machine it is on or whether the original .cfpl was ever here.
		if (imported.hasExtras)
		{
			palData.palInfo = imported.info;
			memcpy(palData.file0, imported.characterFile, IMPL_PALETTE_DATALEN);
			char* files[IMPL_PALETTE_FILES_COUNT - 1] = {
				palData.file1, palData.file2, palData.file3,
				palData.file4, palData.file5, palData.file6, palData.file7
			};
			for (int i = 0; i < IMPL_PALETTE_FILES_COUNT - 1; i++)
				memcpy(files[i], imported.effects[i], IMPL_PALETTE_DATALEN);
			built = true;
		}

		const int existingIndex = built ? -1 :
			g_interfaces.pPaletteManager->FindCustomPalIndex((CharIndex)charIndex, baseName.c_str());
		if (existingIndex >= 0)
		{
			const IMPL_data_t* existing =
				g_interfaces.pPaletteManager->GetCustomPalData((CharIndex)charIndex, existingIndex);
			if (existing)
			{
				palData = *existing;
				memcpy(palData.file0, imported.characterFile, IMPL_PALETTE_DATALEN);
				memset(palData.palInfo.palName, 0, IMPL_PALNAME_LENGTH);
				strncpy(palData.palInfo.palName, finalName.c_str(), IMPL_PALNAME_LENGTH - 1);
				built = true;
			}
		}

		if (!built)
		{
			built = g_interfaces.pPaletteManager->CreatePaletteFromCharacterFile(
				(CharIndex)charIndex, finalName, imported.characterFile, palData);
		}

		// Whatever the source, the file on disk is named after where it landed.
		memset(palData.palInfo.palName, 0, IMPL_PALNAME_LENGTH);
		strncpy(palData.palInfo.palName, finalName.c_str(), IMPL_PALNAME_LENGTH - 1);

		if (!built ||
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
				const IMPL_data_t& palData = paletteFile->palData;
				const int charIndex = paletteFile->header.charIndex;

				// Paint the palette onto the character's reference sheet: recolouring a
				// picture of the character beats recolouring 256 numbered squares. Falls
				// back to a plain swatch grid if the sheet is somehow unavailable, so an
				// export always produces something importable.
				written = PaletteSheet::Write(charIndex, palData, path, error);
				if (!written)
				{
					g_imGuiLogger->Log("[system] No reference sheet for this character (%s); exporting a swatch grid instead.\n",
						error.c_str());
					error.clear();
					written = PngPalette::WritePaletteFile(path, palData.file0, error, charIndex, &palData);
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
			PngPalette::Imported probe;
			std::string probeError;
			if (PngPalette::ReadPaletteFileEx(path, probe, probeError) &&
				probe.charIndex >= 0 && !isCharacterIndexOutOfBound(probe.charIndex))
			{
				ImportPaletteFile(path, probe.charIndex);
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
void PalettesConfigWindow::ExportPalette(const CharacterGroup& group, const PaletteRow& row, bool asPng)
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
	// One button per format, so the format is chosen before the dialog rather than by
	// remembering to change the extension inside it.
	if (asPng)
	{
		request.filters.push_back({ "PNG Palette (*.png)", "*.png" });
		request.defaultExtension = "png";
		request.initialPath = row.name + ".png";
	}
	else
	{
		request.filters.push_back({ "BBCF Palette (*.cfpl)", "*.cfpl" });
		request.defaultExtension = "cfpl";
		request.initialPath = row.name + IMPL_FILE_EXTENSION;
	}
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

// One character: a header, the Random entries as plain rows (they have no sprite to
// show, so a grid cell would be a lie), then the palettes themselves as a grid.
void PalettesConfigWindow::DrawSection(int groupIndex)
{
	CharacterGroup& group = m_groups[groupIndex];
	const auto& customPalettes = g_interfaces.pPaletteManager->GetCustomPalettesVector();

	// Collect what passes the filter first, so a character with nothing matching is
	// skipped entirely rather than leaving an empty heading behind.
	std::vector<int> specials;
	std::vector<int> palettes;
	for (int i = 0; i < (int)group.rows.size(); i++)
	{
		const PaletteRow& row = group.rows[i];
		const std::string displayName = row.isSpecial ? DisplayNameForSlotValue(row.name) : row.name;
		if (!m_filter.PassFilter(displayName.c_str()) && !m_filter.PassFilter(group.charName.c_str()))
			continue;
		(row.isSpecial ? specials : palettes).push_back(i);
	}
	if (specials.empty() && palettes.empty())
		return;

	ImGui::PushID(groupIndex);

	int assignedCount = 0;
	for (const PaletteRow& row : group.rows)
		if (row.assignedSlot > 0)
			++assignedCount;

	char header[192];
	snprintf(header, sizeof(header), "%s  (%d, %d assigned)###palsec_%d",
		group.charName.c_str(), (int)palettes.size(), assignedCount, group.charIndex);

	if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
	{
		// Random and Random_Exclude_Default are controls, not palettes: there is nothing to
		// preview and nothing to export or delete, so they are a row with a combo box and
		// are deliberately not selectable.
		for (int index : specials)
		{
			PaletteRow& row = group.rows[index];
			ImGui::PushID(index);

			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(DisplayNameForSlotValue(row.name).c_str());
			ImGui::SameLine();
			ImGui::ShowHelpMarker(NamesEqual(row.name, kRandomIniValue)
				? Messages.Palette_random_tooltip()
				: Messages.Palette_random_exclude_tooltip());

			ImGui::SameLine(220.0f);
			ImGui::PushItemWidth(220.0f);
			DrawSlotCombo(group, row);
			ImGui::PopItemWidth();

			ImGui::PopID();
		}

		if (!specials.empty() && !palettes.empty())
			ImGui::Spacing();

		DrawPaletteCells(groupIndex, palettes, customPalettes);

		// Entries typed into palettes.ini by hand - comma lists, missing files, names that
		// match no palette - have no cell to appear in. Keep them visible under their own
		// character so saving does not quietly discard them.
		std::vector<std::string>& charSlots = m_draftSlots[group.charIndex];
		for (int slot = 1; slot <= kPaletteSlotCount; slot++)
		{
			const std::string& value = charSlots[slot - 1];
			if (value.empty())
				continue;

			bool matchesRow = false;
			for (const PaletteRow& existing : group.rows)
				if (NamesEqual(value, existing.name) && existing.assignedSlot == slot)
				{
					matchesRow = true;
					break;
				}

			if (!matchesRow)
			{
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextDisabled(Messages.Palette_manual_entry(), slot, value.c_str());
				ImGui::PopTextWrapPos();
			}
		}
	}

	ImGui::PopID();
	ImGui::Spacing();
}

void PalettesConfigWindow::DrawPaletteCells(int groupIndex, const std::vector<int>& rowIndices,
	const std::vector<std::vector<IMPL_data_t>>& customPalettes)
{
	CharacterGroup& group = m_groups[groupIndex];

	const float cellWidth = 108.0f;
	const float spriteHeight = 132.0f;
	// Two lines of name under the sprite, so a longer one wraps instead of vanishing.
	const float labelHeight = ImGui::GetTextLineHeight() * 2.0f + 2.0f;
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float available = ImGui::GetContentRegionAvail().x;
	int columns = (int)((available + spacing) / (cellWidth + spacing));
	if (columns < 1)
		columns = 1;

	int drawn = 0;
	for (int index : rowIndices)
	{
		PaletteRow& row = group.rows[index];

		if (drawn % columns != 0)
			ImGui::SameLine();
		drawn++;

		ImGui::PushID(index);
		ImGui::BeginGroup();

		const bool selected = (groupIndex == m_selectedGroup && index == m_selectedRow);
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		const ImVec2 cellSize(cellWidth, spriteHeight + labelHeight);

		if (ImGui::InvisibleButton("##cell", cellSize))
		{
			m_selectedGroup = groupIndex;
			m_selectedRow = index;
		}
		const bool hovered = ImGui::IsItemHovered();

		ImDrawList* draw = ImGui::GetWindowDrawList();
		if (selected || hovered)
		{
			draw->AddRectFilled(cursor, ImVec2(cursor.x + cellSize.x, cursor.y + cellSize.y),
				ImGui::GetColorU32(selected ? ImGuiCol_Header : ImGuiCol_HeaderHovered), 3.0f);
		}

		int texWidth = 0, texHeight = 0;
		const ImTextureID texture = PaletteThumbnails::Get(group.charIndex, row.name,
			customPalettes[group.charIndex][row.palIndex].file0, &texWidth, &texHeight);

		if (texture && texWidth > 0 && texHeight > 0)
		{
			const float scale = (std::min)(cellWidth / texWidth, spriteHeight / texHeight);
			const float drawW = texWidth * scale;
			const float drawH = texHeight * scale;
			const ImVec2 topLeft(cursor.x + (cellWidth - drawW) * 0.5f,
				cursor.y + (spriteHeight - drawH));
			draw->AddImage(ImTextureRef(texture), topLeft,
				ImVec2(topLeft.x + drawW, topLeft.y + drawH));
		}
		else
		{
			// No sprite in this build, or the texture could not be made: show the colours
			// themselves rather than an empty cell.
			const float swatchHeight = spriteHeight / 8.0f;
			for (int i = 0; i < 8; i++)
			{
				const unsigned char* bytes =
					(const unsigned char*)customPalettes[group.charIndex][row.palIndex].file0
					+ (1 + i * 12) * 4;
				draw->AddRectFilled(ImVec2(cursor.x + 20.0f, cursor.y + i * swatchHeight),
					ImVec2(cursor.x + cellWidth - 20.0f, cursor.y + (i + 1) * swatchHeight),
					IM_COL32(bytes[2], bytes[1], bytes[0], 255));
			}
		}

		// Assignment badge. Spelled out rather than a bare number, which reads as an
		// index into something rather than as the in-game colour it actually is.
		if (row.assignedSlot > 0)
		{
			char badge[32];
			snprintf(badge, sizeof(badge), Messages.Color_d(), row.assignedSlot);
			ImVec2 badgeSize = ImGui::CalcTextSize(badge);
			if (badgeSize.x > cellWidth - 8.0f)
			{
				snprintf(badge, sizeof(badge), "%d", row.assignedSlot);
				badgeSize = ImGui::CalcTextSize(badge);
			}
			const ImVec2 badgeMin(cursor.x + cellWidth - badgeSize.x - 8.0f, cursor.y + 2.0f);
			draw->AddRectFilled(badgeMin,
				ImVec2(badgeMin.x + badgeSize.x + 6.0f, badgeMin.y + badgeSize.y + 2.0f),
				IM_COL32(25, 25, 25, 210), 3.0f);
			draw->AddText(ImVec2(badgeMin.x + 3.0f, badgeMin.y + 1.0f),
				ImGui::GetColorU32(kAssignedColor), badge);
		}

		DrawCellLabel(row.name, cursor, cellWidth, spriteHeight, labelHeight);

		ImGui::EndGroup();

		if (hovered)
			ImGui::SetTooltip("%s", row.name.c_str());

		ImGui::PopID();
	}
}

// Centred, wrapped to at most two lines, with the second line ellipsised. A name too long
// for the cell is common, and clipping it mid-word leaves something unreadable.
void PalettesConfigWindow::DrawCellLabel(const std::string& name, const ImVec2& cursor,
	float cellWidth, float spriteHeight, float labelHeight)
{
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float maxWidth = cellWidth - 4.0f;
	const float lineHeight = ImGui::GetTextLineHeight();
	const ImU32 colour = ImGui::GetColorU32(ImGuiCol_Text);

	// How much of the name fits on one line.
	const char* begin = name.c_str();
	const char* end = begin + name.size();
	const char* firstEnd = ImGui::GetFont()->CalcWordWrapPosition(
		ImGui::GetFontSize(), begin, end, maxWidth);
	if (firstEnd == begin)
		firstEnd = end; // a single word wider than the cell: let the clip handle it

	auto drawCentred = [&](const char* from, const char* to, float y)
	{
		const ImVec2 size = ImGui::CalcTextSize(from, to);
		const float x = cursor.x + (cellWidth - size.x) * 0.5f;
		draw->AddText(NULL, 0.0f, ImVec2(x, y), colour, from, to);
	};

	const float firstY = cursor.y + spriteHeight + 1.0f;
	if (firstEnd >= end)
	{
		drawCentred(begin, end, firstY);
		return;
	}

	// Second line, ellipsised if the rest still does not fit.
	const char* secondBegin = firstEnd;
	while (secondBegin < end && *secondBegin == ' ')
		secondBegin++;

	const char* secondEnd = ImGui::GetFont()->CalcWordWrapPosition(
		ImGui::GetFontSize(), secondBegin, end, maxWidth);

	draw->PushClipRect(ImVec2(cursor.x, firstY),
		ImVec2(cursor.x + cellWidth, firstY + labelHeight), true);
	drawCentred(begin, firstEnd, firstY);

	if (secondEnd >= end)
	{
		drawCentred(secondBegin, end, firstY + lineHeight);
	}
	else
	{
		std::string tail(secondBegin, secondEnd);
		while (!tail.empty() &&
			ImGui::CalcTextSize((tail + "...").c_str()).x > maxWidth)
		{
			tail.erase(tail.size() - 1);
		}
		tail += "...";
		const ImVec2 size = ImGui::CalcTextSize(tail.c_str());
		draw->AddText(ImVec2(cursor.x + (cellWidth - size.x) * 0.5f, firstY + lineHeight),
			colour, tail.c_str());
	}
	draw->PopClipRect();
}

void PalettesConfigWindow::DrawDetailPanel()
{
	if (m_selectedGroup < 0 || m_selectedGroup >= (int)m_groups.size() ||
		m_selectedRow < 0 || m_selectedRow >= (int)m_groups[m_selectedGroup].rows.size())
	{
		ImGui::TextWrapped("%s", Messages.Palette_select_hint());
		return;
	}

	CharacterGroup& group = m_groups[m_selectedGroup];
	PaletteRow& row = group.rows[m_selectedRow];

	// Specials are drawn as controls in the grid and are not selectable, so nothing
	// should ever land here; if it somehow does, show the hint rather than a panel with
	// an export button for something that cannot be exported.
	if (row.isSpecial)
	{
		ImGui::TextWrapped("%s", Messages.Palette_select_hint());
		return;
	}

	// Character, then palette, then what it is bound to - most general to most specific.
	ImGui::TextDisabled("%s", group.charName.c_str());
	ImGui::PushFont(NULL, ImGui::GetFontSize() * 1.15f);
	ImGui::TextWrapped("%s", row.name.c_str());
	ImGui::PopFont();

	ImGui::Spacing();
	ImGui::TextUnformatted(Messages.Palette_ingame_color());
	DrawSlotCombo(group, row);

	const auto& customPalettes = g_interfaces.pPaletteManager->GetCustomPalettesVector();
	const char* paletteData = customPalettes[group.charIndex][row.palIndex].file0;

	// The same sheet the PNG export produces, so what you see here is what you get.
	ImGui::Spacing();
	int sheetWidth = 0, sheetHeight = 0;
	const ImTextureID sheet = PaletteThumbnails::GetSheet(group.charIndex, row.name,
		paletteData, &sheetWidth, &sheetHeight);

	// Everything below the sheet is pinned to the bottom, so the buttons do not move
	// around as the panel is resized.
	const float buttonHeight = ImGui::GetFrameHeightWithSpacing();
	const float reserved = buttonHeight * 3.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f;
	const float sheetArea = (std::max)(60.0f, ImGui::GetContentRegionAvail().y - reserved);

	ImGui::BeginChild("##sheet", ImVec2(0, sheetArea), false,
		ImGuiWindowFlags_HorizontalScrollbar);
	if (sheet && sheetWidth > 0 && sheetHeight > 0)
	{
		const float width = ImGui::GetContentRegionAvail().x;
		const float scale = width / sheetWidth;
		ImGui::Image(ImTextureRef(sheet), ImVec2(width, sheetHeight * scale));
	}
	else
	{
		ImGui::TextWrapped("%s", Messages.Palette_no_preview());
	}
	ImGui::EndChild();

	if (ImGui::Button(Messages.Palette_export_cfpl(), ImVec2(-1.0f, 0.0f)))
		ExportPalette(group, row, false);
	ImGui::HoverTooltip(Messages.Palette_export_tooltip());

	if (ImGui::Button(Messages.Palette_export_png(), ImVec2(-1.0f, 0.0f)))
		ExportPalette(group, row, true);
	ImGui::HoverTooltip(Messages.Palette_export_png_tooltip());

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

void PalettesConfigWindow::DrawModal()
{
	const std::string modalTitle = std::string(Messages.Palettes()) + "###palettes_modal";
	// Twice the old size, and resizable: the grid wants room, and how much is a matter of
	// how many palettes someone has. FirstUseEver so a resize sticks for the session.
	ImGui::SetNextWindowSize(ImVec2(1280, 1000), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(700, 480), ImVec2(FLT_MAX, FLT_MAX));
	if (!ImGui::BeginPopupModal(modalTitle.c_str(), nullptr, 0))
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

	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(Messages.Palette_assignments());
	ImGui::TextDisabled("%s", Messages.Palette_assignments_hint());
	ImGui::PopTextWrapPos();
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
		// Grid on the left, details for whatever is selected on the right, with a
		// draggable divider: how much room each wants depends on how long the palette
		// names are and how big the preview should be, which is the user's call.
		const float splitterWidth = 6.0f;
		const float totalWidth = ImGui::GetContentRegionAvail().x;
		const float minPanel = 220.0f;
		const float maxPanel = (std::max)(minPanel, totalWidth - 260.0f);
		g_detailPanelWidth = ImClamp(g_detailPanelWidth, minPanel, maxPanel);

		const float gridWidth = totalWidth - g_detailPanelWidth - splitterWidth;

		ImGui::BeginChild("##palettes_grid", ImVec2(gridWidth, -64.0f), true);
		for (int i = 0; i < (int)m_groups.size(); i++)
			DrawSection(i);
		ImGui::EndChild();

		ImGui::SameLine(0.0f, 0.0f);

		ImGui::InvisibleButton("##palettes_splitter", ImVec2(splitterWidth, ImGui::GetContentRegionAvail().y - 64.0f));
		if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0.0f)
		{
			g_detailPanelWidth -= ImGui::GetIO().MouseDelta.x;
			// Tell ImGui the ini is stale, or the new width is only written if something
			// else happens to dirty it.
			ImGui::MarkIniSettingsDirty();
		}
		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

		ImGui::SameLine(0.0f, 0.0f);

		ImGui::BeginChild("##palettes_detail", ImVec2(0, -64.0f), true);
		DrawDetailPanel();
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
