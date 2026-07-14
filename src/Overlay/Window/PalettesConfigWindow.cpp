#include "PalettesConfigWindow.h"

#include "Core/interfaces.h"
#include "Core/Settings.h"
#include "Core/utils.h"
#include "Game/characters.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Logger/ImGuiLogger.h"
#include "Palette/impl_templates.h"
#include "Palette/PaletteManager.h"

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

	// File dialogs follow the known-safe pattern from UnlimitedPlaybackWindow:
	// the Win32 dialog runs on its own thread with OFN_NOCHANGEDIR and
	// working-directory restore, and the render thread only consumes results.
	enum class PaletteFileDialogAction
	{
		None,
		ExportPalette,
		ImportPalette,
	};

	struct PaletteFileDialogState
	{
		std::mutex mutex;
		bool active = false;
		bool completed = false;
		PaletteFileDialogAction action = PaletteFileDialogAction::None;
		std::string path;
		bool exportSucceeded = false;
		std::string exportPalName;
	};

	PaletteFileDialogState g_paletteFileDialogState;

	void StartExportPaletteDialogAsync(const IMPL_t& paletteFile, const std::string& palName)
	{
		{
			std::lock_guard<std::mutex> lock(g_paletteFileDialogState.mutex);
			if (g_paletteFileDialogState.active)
				return;
			g_paletteFileDialogState.active = true;
			g_paletteFileDialogState.completed = false;
			g_paletteFileDialogState.action = PaletteFileDialogAction::ExportPalette;
			g_paletteFileDialogState.path.clear();
			g_paletteFileDialogState.exportSucceeded = false;
			g_paletteFileDialogState.exportPalName = palName;
		}

		std::thread([paletteFile, palName]() {
			char selectedPath[MAX_PATH] = {};
			char originalWorkingDirectory[MAX_PATH] = {};
			OPENFILENAMEA ofn;
			std::memset(&ofn, 0, sizeof(ofn));
			GetCurrentDirectoryA(MAX_PATH, originalWorkingDirectory);

			std::string defaultName = palName + IMPL_FILE_EXTENSION;
			std::strncpy(selectedPath, defaultName.c_str(), MAX_PATH - 1);

			ofn.lStructSize = sizeof(ofn);
			ofn.hwndOwner = nullptr;
			ofn.lpstrFile = selectedPath;
			ofn.nMaxFile = MAX_PATH;
			ofn.lpstrFilter = "BBCF Palette (*.cfpl)\0*.cfpl\0All Files\0*.*\0";
			ofn.lpstrDefExt = "cfpl";
			ofn.lpstrTitle = "Export palette";
			ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

			const bool dialogConfirmed = GetSaveFileNameA(&ofn) == TRUE;

			if (originalWorkingDirectory[0] != '\0')
			{
				SetCurrentDirectoryA(originalWorkingDirectory);
			}

			bool written = false;
			if (dialogConfirmed)
			{
				IMPL_t fileToWrite = paletteFile;
				written = utils_WriteFile(selectedPath, &fileToWrite, sizeof(IMPL_t), true);
			}

			std::lock_guard<std::mutex> lock(g_paletteFileDialogState.mutex);
			g_paletteFileDialogState.path = dialogConfirmed ? selectedPath : "";
			g_paletteFileDialogState.exportSucceeded = written;
			g_paletteFileDialogState.completed = dialogConfirmed; // silent on cancel
			g_paletteFileDialogState.active = false;
		}).detach();
	}

	void StartImportPaletteDialogAsync()
	{
		{
			std::lock_guard<std::mutex> lock(g_paletteFileDialogState.mutex);
			if (g_paletteFileDialogState.active)
				return;
			g_paletteFileDialogState.active = true;
			g_paletteFileDialogState.completed = false;
			g_paletteFileDialogState.action = PaletteFileDialogAction::ImportPalette;
			g_paletteFileDialogState.path.clear();
		}

		std::thread([]() {
			char selectedPath[MAX_PATH] = {};
			char originalWorkingDirectory[MAX_PATH] = {};
			OPENFILENAMEA ofn;
			std::memset(&ofn, 0, sizeof(ofn));
			GetCurrentDirectoryA(MAX_PATH, originalWorkingDirectory);

			ofn.lStructSize = sizeof(ofn);
			ofn.hwndOwner = nullptr;
			ofn.lpstrFile = selectedPath;
			ofn.nMaxFile = MAX_PATH;
			ofn.lpstrFilter = "BBCF Palettes (*.cfpl;*.hpl)\0*.cfpl;*.hpl\0All Files\0*.*\0";
			ofn.lpstrTitle = "Import palette";
			ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

			const bool dialogConfirmed = GetOpenFileNameA(&ofn) == TRUE;

			if (originalWorkingDirectory[0] != '\0')
			{
				SetCurrentDirectoryA(originalWorkingDirectory);
			}

			std::lock_guard<std::mutex> lock(g_paletteFileDialogState.mutex);
			g_paletteFileDialogState.path = dialogConfirmed ? selectedPath : "";
			g_paletteFileDialogState.completed = dialogConfirmed; // silent on cancel
			g_paletteFileDialogState.active = false;
		}).detach();
	}

	bool IsFileDialogActive()
	{
		std::lock_guard<std::mutex> lock(g_paletteFileDialogState.mutex);
		return g_paletteFileDialogState.active;
	}

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
	if (!ImGui::Button("Palettes"))
		return;

	BuildRows();
	ImGui::OpenPopup("Palettes##modal");
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

void PalettesConfigWindow::ImportPaletteFile(const std::string& sourcePath, int charIndex)
{
	const std::string fileName = FileNameFromPath(sourcePath);
	const std::string folder = std::string("BBCF_IM\\Palettes\\") + getCharacterNameByIndexA(charIndex) + "\\";

	std::string baseName = fileName;
	std::string extension;
	const size_t dot = fileName.rfind('.');
	if (dot != std::string::npos)
	{
		baseName = fileName.substr(0, dot);
		extension = fileName.substr(dot);
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

	if (CopyFileA(sourcePath.c_str(), destPath.c_str(), TRUE) != TRUE)
	{
		g_imGuiLogger->Log("[error] Failed to import palette '%s' into '%s'\n",
			fileName.c_str(), destPath.c_str());
		return;
	}

	g_imGuiLogger->Log("[system] Imported palette '%s' for %s\n",
		(finalName + extension).c_str(), getCharacterNameByIndexA(charIndex).c_str());

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
		ImGui::OpenPopup("Delete palette##confirm");
		m_openDeleteConfirm = false;
	}

	ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Always);
	if (!ImGui::BeginPopupModal("Delete palette##confirm", nullptr,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::TextWrapped("Delete \"%s\" (%s)?", m_pendingDeletePalName.c_str(),
		getCharacterNameByIndexA(m_pendingDeleteCharIndex).c_str());
	ImGui::Spacing();
	ImGui::TextWrapped("This removes the palette file (and any effect/bloom companion files) "
		"from BBCF_IM\\Palettes on disk. This cannot be undone.");
	ImGui::Spacing();

	const float buttonsWidth = 120.0f * 2.0f + ImGui::GetStyle().ItemSpacing.x;
	ImGui::SetCursorPosX((std::max)(ImGui::GetStyle().WindowPadding.x,
		(ImGui::GetWindowWidth() - buttonsWidth) * 0.5f));

	if (ImGui::Button("Cancel", ImVec2(120, 0)))
	{
		m_pendingDeletePalName.clear();
		ImGui::CloseCurrentPopup();
	}

	ImGui::SameLine();

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.18f, 0.18f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.12f, 0.12f, 1.0f));
	if (ImGui::Button("Delete", ImVec2(120, 0)))
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
	PaletteFileDialogAction action;
	std::string path;
	bool exportSucceeded;
	std::string exportPalName;

	{
		std::lock_guard<std::mutex> lock(g_paletteFileDialogState.mutex);
		if (!g_paletteFileDialogState.completed)
			return;

		action = g_paletteFileDialogState.action;
		path = g_paletteFileDialogState.path;
		exportSucceeded = g_paletteFileDialogState.exportSucceeded;
		exportPalName = g_paletteFileDialogState.exportPalName;
		g_paletteFileDialogState.completed = false;
	}

	if (action == PaletteFileDialogAction::ExportPalette)
	{
		if (exportSucceeded)
			g_imGuiLogger->Log("[system] Exported palette '%s' to '%s'\n", exportPalName.c_str(), path.c_str());
		else
			g_imGuiLogger->Log("[error] Failed to export palette '%s' to '%s'\n", exportPalName.c_str(), path.c_str());
		return;
	}

	if (action != PaletteFileDialogAction::ImportPalette || path.empty())
		return;

	const std::string fileName = FileNameFromPath(path);

	if (HasExtension(fileName, IMPL_FILE_EXTENSION))
	{
		// .cfpl headers carry the character index, so no character prompt is needed.
		IMPL_t fileContents;
		if (!utils_ReadFile(path.c_str(), &fileContents, sizeof(fileContents), true))
		{
			g_imGuiLogger->Log("[error] Unable to open '%s' : %s\n", fileName.c_str(), strerror(errno));
			return;
		}

		if (strncmp(fileContents.header.fileSig, IMPL_FILESIG, sizeof(fileContents.header.fileSig)) != 0 ||
			fileContents.header.dataLen != sizeof(IMPL_data_t))
		{
			g_imGuiLogger->Log("[error] '%s' unrecognized palette file format!\n", fileName.c_str());
			return;
		}

		if (isCharacterIndexOutOfBound(fileContents.header.charIndex))
		{
			g_imGuiLogger->Log("[error] '%s' has an invalid character index in the header\n", fileName.c_str());
			return;
		}

		ImportPaletteFile(path, fileContents.header.charIndex);
	}
	else if (HasExtension(fileName, LEGACY_HPL_FILE_EXTENSION))
	{
		// Legacy .hpl files carry no character info: ask the user.
		m_pendingImportPath = path;
		m_pendingImportCharIndex = 0;
		m_openImportCharSelect = true;
	}
	else
	{
		g_imGuiLogger->Log("[error] Unable to import '%s' : not a %s or %s file\n",
			fileName.c_str(), IMPL_FILE_EXTENSION, LEGACY_HPL_FILE_EXTENSION);
	}
}

void PalettesConfigWindow::DrawImportCharSelectModal()
{
	if (m_openImportCharSelect)
	{
		ImGui::OpenPopup("Import palette##charselect");
		m_openImportCharSelect = false;
	}

	ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Always);
	if (!ImGui::BeginPopupModal("Import palette##charselect", nullptr,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::TextWrapped("\"%s\" is a legacy palette file that doesn't say which character it belongs to.",
		FileNameFromPath(m_pendingImportPath).c_str());
	ImGui::Spacing();
	ImGui::TextUnformatted("Import it for:");

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

	if (ImGui::Button("Cancel", ImVec2(120, 0)))
	{
		m_pendingImportPath.clear();
		ImGui::CloseCurrentPopup();
	}

	ImGui::SameLine();

	if (ImGui::Button("Import", ImVec2(120, 0)))
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

	const bool dialogBusy = IsFileDialogActive();
	if (dialogBusy)
	{
		ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
	}

	if (ImGui::Button("Import palette...", ImVec2(buttonWidth, 0)) && !dialogBusy)
		StartImportPaletteDialogAsync();

	if (dialogBusy)
	{
		ImGui::PopStyleVar();
		ImGui::PopItemFlag();
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker("Copies a .cfpl or legacy .hpl palette file into the matching character's "
		"folder under BBCF_IM\\Palettes and loads it immediately.");
}

void PalettesConfigWindow::DrawGroup(CharacterGroup& group)
{
	const auto& customPalettes = g_interfaces.pPaletteManager->GetCustomPalettesVector();
	std::vector<std::string>& charSlots = m_draftSlots[group.charIndex];

	int assignedCount = 0;
	for (const PaletteRow& row : group.rows)
		if (row.assignedSlot > 0)
			++assignedCount;

	char header[128];
	snprintf(header, sizeof(header), "%s  (%d palette%s, %d assigned)###palcfg_%d",
		group.charName.c_str(), (int)group.rows.size(),
		group.rows.size() == 1 ? "" : "s", assignedCount, group.charIndex);

	if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
		return;

	ImGui::PushID(group.charIndex);
	ImGui::Columns(2, "##palcfg_cols", false);
	ImGui::SetColumnWidth(0, 330.0f);

	for (PaletteRow& row : group.rows)
	{
		if (!m_filter.PassFilter(row.name.c_str()) && !m_filter.PassFilter(group.charName.c_str()))
			continue;

		ImGui::PushID(row.palIndex);

		// Color strip preview taken straight from the palette's character colors
		ImGui::AlignTextToFramePadding();
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, 1.0f));
		for (int colorIdx = 1; colorIdx <= kSwatchColorCount; colorIdx++)
		{
			unsigned char* colorBytes =
				(unsigned char*)customPalettes[group.charIndex][row.palIndex].file0 + colorIdx * 4;
			char swatchId[32];
			snprintf(swatchId, sizeof(swatchId), "##swatch_%d", colorIdx);
			ImGui::ColorButtonOn32Bit(swatchId, colorIdx, colorBytes,
				ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip, ImVec2(10.0f, 17.0f));
			ImGui::SameLine();
		}
		ImGui::PopStyleVar();

		ImGui::TextUnformatted(row.name.c_str());
		ImGui::NextColumn();

		char preview[64];
		if (row.assignedSlot > 0)
			snprintf(preview, sizeof(preview), "Color %d", row.assignedSlot);
		else
			snprintf(preview, sizeof(preview), "Not assigned");

		if (row.assignedSlot > 0)
			ImGui::PushStyleColor(ImGuiCol_Text, kAssignedColor);

		ImGui::PushItemWidth(-114.0f);
		const bool comboOpen = ImGui::BeginCombo("##slot", preview);
		if (row.assignedSlot > 0)
			ImGui::PopStyleColor();

		if (comboOpen)
		{
			if (ImGui::Selectable("Not assigned", row.assignedSlot == 0))
				AssignSlot(group, row, 0);

			for (int slot = 1; slot <= kPaletteSlotCount; slot++)
			{
				const std::string& occupant = charSlots[slot - 1];
				char label[128];
				if (!occupant.empty() && !NamesEqual(occupant, row.name))
					snprintf(label, sizeof(label), "Color %d  (used by %s)", slot, occupant.c_str());
				else
					snprintf(label, sizeof(label), "Color %d", slot);

				if (ImGui::Selectable(label, row.assignedSlot == slot))
					AssignSlot(group, row, slot);
			}
			ImGui::EndCombo();
		}
		ImGui::PopItemWidth();

		ImGui::SameLine();
		if (ImGui::Button("Export") && !IsFileDialogActive())
		{
			IMPL_t paletteFile{};
			paletteFile.header.headerLen = sizeof(IMPL_header_t);
			paletteFile.header.dataLen = sizeof(IMPL_data_t);
			paletteFile.header.charIndex = (short)group.charIndex;
			paletteFile.palData = customPalettes[group.charIndex][row.palIndex];
			StartExportPaletteDialogAsync(paletteFile, row.name);
		}
		ImGui::HoverTooltip("Save a copy of this palette as a .cfpl file anywhere you like.");

		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
		if (ImGui::Button("X"))
		{
			m_pendingDeleteCharIndex = group.charIndex;
			m_pendingDeletePalName = row.name;
			m_openDeleteConfirm = true;
		}
		ImGui::PopStyleColor();
		ImGui::HoverTooltip("Delete this palette's file from disk (asks for confirmation).");

		ImGui::NextColumn();
		ImGui::PopID();
	}

	ImGui::Columns(1);

	// Entries written by hand into palettes.ini ("Random", comma lists, missing
	// files) have no matching palette row; keep them visible so saving is not a
	// surprise. Assigning a palette to that color replaces the manual entry.
	for (int slot = 1; slot <= kPaletteSlotCount; slot++)
	{
		const std::string& value = charSlots[slot - 1];
		if (value.empty())
			continue;

		bool matchesRow = false;
		for (const PaletteRow& row : group.rows)
			if (NamesEqual(value, row.name))
			{
				matchesRow = true;
				break;
			}

		if (!matchesRow)
			ImGui::TextDisabled("Color %d keeps its manual palettes.ini entry: \"%s\"", slot, value.c_str());
	}

	ImGui::Spacing();
	ImGui::PopID();
}

void PalettesConfigWindow::DrawModal()
{
	ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_Always);
	if (!ImGui::BeginPopupModal("Palettes##modal", nullptr, ImGuiWindowFlags_NoResize))
		return;

	if (!g_interfaces.pPaletteManager)
	{
		ImGui::TextUnformatted("Palettes are not loaded yet.");
		if (ImGui::Button("Close", ImVec2(120, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}

	ConsumeFinishedFileDialog();

	ImGui::TextUnformatted("Palette assignments");
	ImGui::SameLine();
	ImGui::TextDisabled("Picking that color in-game loads the assigned palette automatically.");
	m_filter.Draw("Search by palette or character name##palettes_filter", -1.0f);

	DrawImportButton();

	ImGui::BeginChild("##palettes_scroll", ImVec2(0, -64.0f), true);

	if (m_groups.empty())
	{
		ImGui::TextWrapped("No custom palettes were found.");
		ImGui::Spacing();
		ImGui::TextWrapped("Use \"Import palette...\" above, or place palette files in "
			"BBCF_IM\\Palettes\\<Character>\\ next to BBCF.exe and use \"Reload custom palettes\" "
			"in the Custom palettes section, then reopen this window.");
	}
	else
	{
		for (CharacterGroup& group : m_groups)
			DrawGroup(group);
	}

	ImGui::EndChild();

	ImGui::Checkbox("Allow opponents to download your palettes", &m_draftAllowDownloads);
	ImGui::SameLine();
	ImGui::ShowHelpMarker("Allows opponents to save your visible custom palette from the match UI. "
		"Applied when you hit Save.");

	const float footerWidth = 120.0f * 2.0f + ImGui::GetStyle().ItemSpacing.x;
	ImGui::SetCursorPosX((std::max)(ImGui::GetStyle().WindowPadding.x,
		(ImGui::GetWindowWidth() - footerWidth) * 0.5f));

	if (ImGui::Button("Cancel", ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();

	ImGui::SameLine();

	if (ImGui::Button("Save", ImVec2(120, 0)))
	{
		if (g_interfaces.pPaletteManager->SavePaletteSettingsFile(m_draftSlots))
			g_imGuiLogger->Log("[system] Palette assignments saved to 'palettes.ini'\n");
		else
			g_imGuiLogger->Log("[error] Failed to save palette assignments to 'palettes.ini'\n");

		if (m_draftAllowDownloads != Settings::settingsIni.allowPaletteDownloads)
		{
			Settings::settingsIni.allowPaletteDownloads = m_draftAllowDownloads;
			Settings::changeSetting("AllowPaletteDownloads", m_draftAllowDownloads ? "1" : "0");
		}

		ImGui::CloseCurrentPopup();
	}

	DrawImportCharSelectModal();
	DrawDeleteConfirmModal();

	ImGui::EndPopup();
}
