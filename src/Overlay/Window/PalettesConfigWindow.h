#pragma once

#include "imgui.h"

#include <string>
#include <vector>

class PalettesConfigWindow
{
public:
	void DrawOpenButton();
	void DrawModal();

private:
	struct PaletteRow
	{
		int palIndex = 0;     // index into the character's custom palette vector
		std::string name;
		int assignedSlot = 0; // 0 = not assigned, otherwise 1..24 (in-game color number)
	};

	struct CharacterGroup
	{
		int charIndex = 0;
		std::string charName;
		std::vector<PaletteRow> rows;
	};

	void BuildRows();
	void RebuildGroupsFromDraft();
	void AssignSlot(CharacterGroup& group, PaletteRow& row, int newSlot);
	void DrawGroup(CharacterGroup& group);
	void DrawImportButton();
	void ConsumeFinishedFileDialog();
	void ImportPaletteFile(const std::string& sourcePath, int charIndex);
	void DrawImportCharSelectModal();
	void DeletePalette(int charIndex, const std::string& palName);
	void DrawDeleteConfirmModal();

	std::vector<CharacterGroup> m_groups;
	// Draft copy of the palettes.ini slot table (per character, one string per color).
	// This is what gets written on save; Cancel throws it away.
	std::vector<std::vector<std::string>> m_draftSlots;
	bool m_draftAllowDownloads = true;
	ImGuiTextFilter m_filter;

	// Pending .hpl import waiting for the user to pick a character (the legacy
	// format does not store one; .cfpl files import straight from their header).
	std::string m_pendingImportPath;
	int m_pendingImportCharIndex = 0;
	bool m_openImportCharSelect = false;

	// Pending delete waiting for the user to confirm (files are removed from disk).
	std::string m_pendingDeletePalName;
	int m_pendingDeleteCharIndex = 0;
	bool m_openDeleteConfirm = false;
};
