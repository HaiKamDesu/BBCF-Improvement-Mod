#pragma once

#include "imgui.h"

#include "PaletteEditorModal.h"
#include "Palette/impl_format.h"

#include <string>
#include <vector>

class PalettesConfigWindow
{
public:
	// Registers the handler that keeps the detail panel's width in ImGui's own ini.
	// Must run before the first frame, which is when that file is parsed.
	static void RegisterLayoutSettings();

	void DrawOpenButton();
	void DrawModal();

private:
	struct PaletteRow
	{
		int palIndex = 0;     // index into the character's custom palette vector; negative for special rows
		std::string name;     // palette name, or the raw palettes.ini value for special rows
		int assignedSlot = 0; // 0 = not assigned, otherwise 1..24 (in-game color number)
		bool isSpecial = false; // "Random" / "Random_Exclude_Default" pseudo-entries (no file behind them)
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
	void DrawSection(int groupIndex);
	void DrawPaletteCells(int groupIndex, const std::vector<int>& rowIndices,
		const std::vector<std::vector<IMPL_data_t>>& customPalettes);
	void DrawCellLabel(const std::string& name, const ImVec2& cursor,
		float cellWidth, float spriteHeight, float labelHeight);
	void DrawDetailPanel();
	void DrawSlotCombo(CharacterGroup& group, PaletteRow& row);
	void ExportPalette(const CharacterGroup& group, const PaletteRow& row, bool asPng);
	void DrawImportButton();
	void ConsumeFinishedFileDialog();
	void ImportPaletteFile(const std::string& sourcePath, int charIndex);
	// One file out of whatever the picker returned: imports it straight away when the file
	// names its own character, or queues it for the character-select prompt when it cannot.
	void ImportPickedFile(const std::string& path);
	void DrawImportCharSelectModal();
	void DeletePalette(int charIndex, const std::string& palName);
	void DrawDeleteConfirmModal();

	std::vector<CharacterGroup> m_groups;
	// Which character's grid is showing, and which cell in it is selected. Indices into
	// m_groups / its rows; the selection is cleared whenever the grid is rebuilt.
	int m_selectedGroup = -1;
	int m_selectedRow = -1;
	// Draft copy of the palettes.ini slot table (per character, one string per color).
	// This is what gets written on save; Cancel throws it away.
	std::vector<std::vector<std::string>> m_draftSlots;
	// Mirrors AllowPaletteDownloads: -1 unset, 0 deny, 1 allow. Stays -1 until
	// the user actually touches the checkbox so an untouched Save keeps "unset".
	int m_draftAllowDownloads = -1;
	ImGuiTextFilter m_filter;

	// Pending .hpl / character-less PNG imports waiting for the user to pick a character
	// (the legacy format does not store one; .cfpl files import straight from their header).
	//
	// A list, not one path: the picker takes several files at once, and any number of them
	// can need the prompt. They are asked about one at a time, front first, so a batch of
	// ten legacy files is ten answers rather than one guess applied to all ten.
	std::vector<std::string> m_pendingImportPaths;
	int m_pendingImportCharIndex = 0;
	bool m_openImportCharSelect = false;

	// Pending delete waiting for the user to confirm (files are removed from disk).
	std::string m_pendingDeletePalName;
	int m_pendingDeleteCharIndex = 0;
	bool m_openDeleteConfirm = false;

	PaletteEditorModal m_editor;

	// The overlay hotkey hides this with the rest of the overlay, and ImGui closes the
	// popups of a window that goes away. DrawModal notices and puts it back, drafts intact.
	bool m_modalOpenLastDraw = false;
	int m_modalLastDrawFrame = -1;

	// The block list manager.
	void DrawBlockedModal();
	bool m_openBlocked = false;
	char m_blockSteamIdInput[32] = {};
};
