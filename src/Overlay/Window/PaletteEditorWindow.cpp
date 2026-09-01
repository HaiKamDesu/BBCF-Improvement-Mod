#include "PaletteEditorWindow.h"

#include <cfloat>

#include "Palette/PaletteThumbnails.h"

namespace
{
	// The in-match cell: header, name, sprite, button. Tall enough for a two-line name
	// without the button moving.
	const float kPaletteCellHeight = 190.0f;
	const float kPaletteSpriteHeight = 200.0f;
	// Cells in the picker grid. Big enough that the sprite is the thing you read, not a
	// stamp next to the name.
	const float kPalettePickCellWidth = 130.0f;
	const float kPalettePickSpriteHeight = 152.0f;
}

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/Localization.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Logger/ImGuiLogger.h"
#include "Palette/impl_format.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <Shlwapi.h>

#define NUMBER_OF_COLOR_BOXES (IMPL_PALETTE_DATALEN / sizeof(int)) // 256
#define COLUMNS 16

const int COLOR_BLACK = 0xFF000000;
const int COLOR_WHITE = 0xFFFFFFFF;
const ImVec4 COLOR_ONLINE(0.260f, 0.590f, 0.980f, 1.000f);

static char palNameBuf[IMPL_PALNAME_LENGTH] = "";
static char palDescBuf[IMPL_DESC_LENGTH] = "";
static char palCreatorBuf[IMPL_CREATOR_LENGTH] = "";
static bool palBoolEffect = false;

namespace
{
	// The picker's size, remembered across sessions. It is a context menu, and ImGui never
	// saves a popup's size itself (BeginPopup forces NoSavedSettings), so it is kept here
	// and written to the layout ini alongside the other palette UI sizes.
	float g_pickerWidth = 560.0f;
	float g_pickerHeight = 480.0f;

	void* PickerLayout_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
	{
		return strcmp(name, "Layout") == 0 ? (void*)1 : nullptr;
	}

	void PickerLayout_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
	{
		float value = 0.0f;
		if (sscanf_s(line, "PickerWidth=%f", &value) == 1 && value > 0.0f)
			g_pickerWidth = value;
		else if (sscanf_s(line, "PickerHeight=%f", &value) == 1 && value > 0.0f)
			g_pickerHeight = value;
	}

	void PickerLayout_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
	{
		buf->appendf("[%s][Layout]\n", handler->TypeName);
		buf->appendf("PickerWidth=%.0f\n", g_pickerWidth);
		buf->appendf("PickerHeight=%.0f\n\n", g_pickerHeight);
	}
}

void PaletteEditorWindow::RegisterLayoutSettings()
{
	// AddSettingsHandler asserts on a duplicate, and initialization can run again after a
	// device loss.
	if (ImGui::FindSettingsHandler("BBCFIMPalettePicker"))
		return;

	ImGuiSettingsHandler handler;
	handler.TypeName = "BBCFIMPalettePicker";
	handler.TypeHash = ImHashStr("BBCFIMPalettePicker");
	handler.ReadOpenFn = PickerLayout_ReadOpen;
	handler.ReadLineFn = PickerLayout_ReadLine;
	handler.WriteAllFn = PickerLayout_WriteAll;
	ImGui::AddSettingsHandler(&handler);
}


void PaletteEditorWindow::ClearUndoHistory()
{
	m_history.entries.clear();
	m_history.paletteChanges.clear();
	m_history.gradientChanges.clear();
	m_history.cursor = 0;
}

void PaletteEditorWindow::Undo()
{
	if (m_history.cursor > 0)
	{
		m_history.cursor -= 1;
	}

	if (m_history.cursor < m_history.entries.size())
	{
		HistoryEntry entry = m_history.entries[m_history.cursor];
		switch (entry.changeType)
		{
		case ChangeType::Palette:
		{
			PaletteChange change = m_history.paletteChanges[entry.changeIdx];
			memcpy(m_paletteEditorArray + change.offset, &change.oldValue, sizeof(Color));
			break;
		}

		case ChangeType::Gradient:
		{
			GradientChange& change = m_history.gradientChanges[entry.changeIdx];
			memcpy(m_paletteEditorArray, &change.oldColors[0], change.oldColors.size() * sizeof(Color));
			break;
		}

		default:
			break;
		}
	}
}

void PaletteEditorWindow::Redo()
{
	if (m_history.cursor < m_history.entries.size())
	{
		HistoryEntry entry = m_history.entries[m_history.cursor];
		switch (entry.changeType)
		{
		case ChangeType::Palette:
		{
			PaletteChange change = m_history.paletteChanges[entry.changeIdx];
			memcpy(m_paletteEditorArray + change.offset, &change.newValue, sizeof(Color));
			break;
		}

		case ChangeType::Gradient:
		{
			GradientChange& change = m_history.gradientChanges[entry.changeIdx];
			memcpy(m_paletteEditorArray, &change.newColors[0], change.newColors.size() * sizeof(Color));
			break;
		}

		default:
			break;
		}

		m_history.cursor += 1;
	}
}

void PaletteEditorWindow::ClearRedoEntries()
{
	// If the cursor is not at the end of the history, then erase everything after it
	while (m_history.cursor < m_history.entries.size())
	{
		HistoryEntry last = m_history.entries[m_history.entries.size() - 1];
		switch (last.changeType)
		{
		case ChangeType::Palette:
			m_history.paletteChanges.pop_back();
			break;

		case ChangeType::Gradient:
			m_history.gradientChanges.pop_back();
			break;

		default:
			break;
		}

		m_history.entries.pop_back();
	}
}

void PaletteEditorWindow::RecordPaletteChange(PaletteChange change)
{
	ClearRedoEntries();

	// If there's nothing currently in the list we can push this change and return early.
	if (m_history.entries.size() == 0)
	{
		HistoryEntry entry = { ChangeType::Palette, 0 };
		m_history.entries.push_back(entry);
		m_history.paletteChanges.push_back(change);
		m_history.cursor += 1;
		return;
	}

	// If the last entry isn't a palette change with the same offset we can also push and return
	//early.
	HistoryEntry lastEntry = m_history.entries[m_history.entries.size() - 1];
	if (lastEntry.changeType != ChangeType::Palette || m_history.paletteChanges[lastEntry.changeIdx].offset != change.offset)
	{
		HistoryEntry entry = { ChangeType::Palette, m_history.paletteChanges.size() };
		m_history.paletteChanges.push_back(change);
		m_history.entries.push_back(entry);
		m_history.cursor += 1;
		return;
	}

	// Check the timestamp of the last change; if it's <0.2 seconds. If it is, then update the old 
	// change, otherwise push a new one.
	PaletteChange& previous = m_history.paletteChanges[lastEntry.changeIdx];
	std::time_t now = std::time(nullptr);
	if (std::difftime(now, previous.timestamp) < 0.2)
	{
		previous.timestamp = now;
		previous.newValue = change.newValue;
	}
	else
	{
		HistoryEntry entry = { ChangeType::Palette, m_history.paletteChanges.size() };
		m_history.paletteChanges.push_back(change);
		m_history.entries.push_back(entry);
		m_history.cursor += 1;
	}
}

void PaletteEditorWindow::RecordGradientChange(GradientChange change)
{
	ClearRedoEntries();

	HistoryEntry entry = { ChangeType::Gradient, m_history.gradientChanges.size() };
	m_history.entries.push_back(entry);
	m_history.gradientChanges.push_back(change);
	m_history.cursor += 1;
}

void PaletteEditorWindow::ShowAllPaletteSelections(const std::string& windowID)
{
	if (HasNullPointer())
	{
		return;
	}

	// Localized now; these were hardcoded literals and the only untranslated strings left
	// in this section.
	const char* p1BtnText = Messages.Player_1();
	const char* p2BtnText = Messages.Player_2();
	const std::string p1PopupID = "select1-1" + windowID;
	const std::string p2PopupID = "select2-1" + windowID;

	if (g_interfaces.pRoomManager->IsRoomFunctional())
	{
		uint16_t thisPlayerMatchPlayerIndex = g_interfaces.pRoomManager->GetThisPlayerMatchPlayerIndex();

		ShowPaletteSelectionColumns(
			[&]() {
				if (thisPlayerMatchPlayerIndex == 0)
					ShowPaletteSelectButton(g_interfaces.player1, p1BtnText, p1PopupID.c_str());
				else
					ShowOnlinePaletteResetButton(g_interfaces.player1, 0, p1BtnText);
			},
			[&]() {
				if (thisPlayerMatchPlayerIndex == 1)
					ShowPaletteSelectButton(g_interfaces.player2, p2BtnText, p2PopupID.c_str());
				else
					ShowOnlinePaletteResetButton(g_interfaces.player2, 1, p2BtnText);
			});

		return;
	}

	// Offline: both players side by side, each in its own cell.
	ShowPaletteSelectionColumns(
		[&]() { ShowPaletteSelectButton(g_interfaces.player1, p1BtnText, p1PopupID.c_str()); },
		[&]() { ShowPaletteSelectButton(g_interfaces.player2, p2BtnText, p2PopupID.c_str()); });
}

// Two equal halves with a rule between them. Palettes are a per-player thing and reading
// them as one stacked list meant working out which row belonged to whom.
void PaletteEditorWindow::ShowPaletteSelectionColumns(
	const std::function<void()>& drawLeft, const std::function<void()>& drawRight)
{
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float total = ImGui::GetContentRegionAvail().x;
	const float columnWidth = (std::max)(120.0f, (total - spacing * 3.0f) * 0.5f);

	// AutoResizeY, so each half is exactly as tall as it needs to be. A fixed height meant
	// a scrollbar inside the section, which is the wrong place to scroll - the page around
	// it already does that.
	const ImGuiChildFlags childFlags = ImGuiChildFlags_AutoResizeY;
	const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

	ImGui::BeginGroup();

	ImGui::BeginChild("##pal_left", ImVec2(columnWidth, 0.0f), childFlags, windowFlags);
	drawLeft();
	ImGui::EndChild();

	ImGui::SameLine(0.0f, spacing * 2.0f);

	ImGui::BeginChild("##pal_right", ImVec2(columnWidth, 0.0f), childFlags, windowFlags);
	drawRight();
	ImGui::EndChild();

	ImGui::EndGroup();

	// The divider goes in afterwards, once both halves have been laid out and their
	// height is actually known.
	const ImVec2 groupMin = ImGui::GetItemRectMin();
	const ImVec2 groupMax = ImGui::GetItemRectMax();
	const float midX = groupMin.x + columnWidth + spacing;
	ImGui::GetWindowDrawList()->AddLine(
		ImVec2(midX, groupMin.y + 2.0f), ImVec2(midX, groupMax.y - 2.0f),
		ImGui::GetColorU32(ImGuiCol_Separator));
}

// Centred, wrapped to the cell, ellipsised if it still does not fit. Palette names are
// user-chosen and plenty are longer than a cell; clipping mid-word hides the only thing
// telling one apart from another.
void PaletteEditorWindow::DrawWrappedCellLabel(const char* text, const ImVec2& origin,
	float cellWidth, float labelTop, float labelHeight)
{
	ImDrawList* draw = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	const float fontSize = ImGui::GetFontSize();
	const float lineHeight = ImGui::GetTextLineHeight();
	const float maxWidth = cellWidth - 4.0f;
	const ImU32 colour = ImGui::GetColorU32(ImGuiCol_Text);
	const int maxLines = (int)(labelHeight / lineHeight);

	draw->PushClipRect(ImVec2(origin.x, origin.y + labelTop),
		ImVec2(origin.x + cellWidth, origin.y + labelTop + labelHeight), true);

	const char* cursor = text;
	const char* end = text + strlen(text);
	for (int line = 0; line < maxLines && cursor < end; line++)
	{
		const char* lineEnd = font->CalcWordWrapPosition(fontSize, cursor, end, maxWidth);
		if (lineEnd == cursor)
			lineEnd = cursor + 1; // a word wider than the cell still has to advance

		const bool lastLine = (line == maxLines - 1);
		std::string piece(cursor, lineEnd);
		if (lastLine && lineEnd < end)
		{
			while (!piece.empty() && ImGui::CalcTextSize((piece + "...").c_str()).x > maxWidth)
				piece.erase(piece.size() - 1);
			piece += "...";
		}

		const ImVec2 size = ImGui::CalcTextSize(piece.c_str());
		draw->AddText(ImVec2(origin.x + (cellWidth - size.x) * 0.5f,
			origin.y + labelTop + line * lineHeight), colour, piece.c_str());

		cursor = lineEnd;
		while (cursor < end && *cursor == ' ')
			cursor++;
	}

	draw->PopClipRect();
}

// Which bytes to draw a palette preview from.
//
// The "Default" entry is a placeholder with an empty colour table - it means "put the
// game's own palette back", not a palette of its own - so drawing it directly gives a
// black silhouette. The real colours are the ones captured when the match started, which
// is whichever in-game colour the player actually picked, so use those instead.
const char* PaletteEditorWindow::PaletteDataForPreview(CharPaletteHandle& charPalHandle,
	CharIndex charIndex, int palIndex)
{
	if (palIndex == 0)
		return g_interfaces.pPaletteManager->GetOrigPalFileAddr(PaletteFile_Character, charPalHandle);
	return m_customPaletteVector[charIndex][palIndex].file0;
}

// Centred single line, clipped rather than allowed to widen the cell.
void PaletteEditorWindow::CenteredText(const char* text)
{
	const float width = ImGui::GetContentRegionAvail().x;
	const ImVec2 size = ImGui::CalcTextSize(text);
	if (size.x < width)
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - size.x) * 0.5f);
	ImGui::TextUnformatted(text);
}

// Centred and wrapped. Palette names are user-chosen and some are long; wrapping keeps
// them readable where clipping would hide the end of the one thing identifying a palette.
void PaletteEditorWindow::WrappedCenteredText(const char* text)
{
	const float width = ImGui::GetContentRegionAvail().x;
	ImFont* font = ImGui::GetFont();
	const float fontSize = ImGui::GetFontSize();

	const char* cursor = text;
	const char* end = text + strlen(text);
	while (cursor < end)
	{
		const char* lineEnd = font->CalcWordWrapPosition(fontSize, cursor, end, width);
		if (lineEnd == cursor)
			lineEnd = cursor + 1; // a single word wider than the cell still has to advance

		const ImVec2 size = ImGui::CalcTextSize(cursor, lineEnd);
		const float indent = (size.x < width) ? (width - size.x) * 0.5f : 0.0f;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
		ImGui::TextUnformatted(cursor, lineEnd);

		cursor = lineEnd;
		while (cursor < end && *cursor == ' ')
			cursor++;
	}
}

// The character drawn in a palette, into a rectangle of the caller's choosing. Falls back
// to a strip of the palette's own colours when this build has no sprite for the character,
// so a cell is never blank.
// Cache key for a palette's thumbnail. The index alone is not enough for Default: its
// colours are whichever of the game's own palettes the player picked, so P1 and P2 would
// otherwise share one cached texture and the wrong one would win. Fold the bytes in.
void PaletteEditorWindow::PaletteThumbKey(char* out, size_t outSize, int palIndex,
	const char* paletteData)
{
	unsigned int hash = 2166136261u;
	if (palIndex == 0 && paletteData)
	{
		for (int i = 0; i < IMPL_PALETTE_DATALEN; i++)
			hash = (hash ^ (unsigned char)paletteData[i]) * 16777619u;
	}
	sprintf_s(out, outSize, "sel%d_%08x", palIndex, hash);
}

void PaletteEditorWindow::DrawPaletteSpriteAt(CharIndex charIndex, int palIndex,
	const char* paletteData, const ImVec2& origin, float width, float height)
{
	if (!paletteData)
	{
		// Nothing to draw from - out of a match the original palette has no address yet.
		DrawWrappedCellLabel(Messages.No_preview_available(), origin, width, 0.0f, height);
		return;
	}

	char key[64];
	PaletteThumbKey(key, sizeof(key), palIndex, paletteData);

	int texWidth = 0, texHeight = 0;
	const ImTextureID texture =
		PaletteThumbnails::Get((int)charIndex, key, paletteData, &texWidth, &texHeight);

	ImDrawList* draw = ImGui::GetWindowDrawList();

	if (texture && texWidth > 0 && texHeight > 0)
	{
		const float scale = (std::min)(width / texWidth, height / texHeight);
		const float drawnW = texWidth * scale;
		const float drawnH = texHeight * scale;
		const ImVec2 topLeft(origin.x + (width - drawnW) * 0.5f, origin.y + (height - drawnH));
		draw->AddImage(ImTextureRef(texture), topLeft,
			ImVec2(topLeft.x + drawnW, topLeft.y + drawnH));
		return;
	}

	const float swatch = height / 8.0f;
	for (int i = 0; i < 8; i++)
	{
		const unsigned char* bytes = (const unsigned char*)paletteData + (1 + i * 12) * 4;
		draw->AddRectFilled(ImVec2(origin.x + width * 0.25f, origin.y + i * swatch),
			ImVec2(origin.x + width * 0.75f, origin.y + (i + 1) * swatch),
			IM_COL32(bytes[2], bytes[1], bytes[0], 255));
	}
}

// Same thing at the cursor, advancing the layout.
void PaletteEditorWindow::DrawPaletteSprite(CharIndex charIndex, int palIndex,
	const char* paletteData, float height)
{
	// Fill the column rather than sitting in a fixed box with dead space under it: scale
	// to the available width and take whatever height the aspect ratio asks for, capped so
	// a wide sprite cannot push the button off the bottom.
	const float width = ImGui::GetContentRegionAvail().x;

	int texWidth = 0, texHeight = 0;
	if (paletteData)
	{
		char key[64];
		PaletteThumbKey(key, sizeof(key), palIndex, paletteData);
		PaletteThumbnails::Get((int)charIndex, key, paletteData, &texWidth, &texHeight);
	}

	float drawnHeight = height;
	if (texWidth > 0 && texHeight > 0)
		drawnHeight = (std::min)(height, width * (float)texHeight / (float)texWidth);

	DrawPaletteSpriteAt(charIndex, palIndex, paletteData, ImGui::GetCursorScreenPos(),
		width, drawnHeight);
	ImGui::Dummy(ImVec2(width, drawnHeight));
}


void PaletteEditorWindow::ShowReloadAllPalettesButton()
{
	if (ImGui::Button(Messages.Reload_custom_palettes()))
	{
		g_interfaces.pPaletteManager->ReloadAllPalettes();
	}
}

void PaletteEditorWindow::OnMatchInit()
{
	if (HasNullPointer())
	{
		return;
	}

	InitializeSelectedCharacters();

	m_selectedCharIndex = (CharIndex)m_playerHandles[0]->GetData()->charIndex;
	m_selectedCharName = m_allSelectedCharNames[0].c_str();
	m_selectedCharPalHandle = &m_playerHandles[0]->GetPalHandle();
	m_selectedPalIndex = g_interfaces.pPaletteManager->GetCurrentCustomPalIndex(*m_selectedCharPalHandle);
	CopyImplDataToEditorFields(*m_selectedCharPalHandle);
	m_selectedFile = PaletteFile_Character;

	m_colorEditFlags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha;
	m_highlightMode = false;
	m_showAlpha = false;

	ClearUndoHistory();

	CopyPalFileToEditorArray(m_selectedFile, *m_selectedCharPalHandle);
}

void PaletteEditorWindow::Draw()
{
	if (!isPaletteEditingEnabledInCurrentState() || HasNullPointer())
	{
		Close();
		return;
	}

	CheckSelectedPalOutOfBound();

	CharacterSelection();
	PaletteSelection();
	FileSelection();
	EditingModesSelection();
	ShowPaletteBoxes();
	ShowUndoAndRedo();
	SavePaletteToFile();
}

bool PaletteEditorWindow::HasNullPointer()
{
	return g_interfaces.player1.IsCharDataNullPtr() ||
		g_interfaces.player2.IsCharDataNullPtr();
}

void PaletteEditorWindow::InitializeSelectedCharacters()
{
	m_playerHandles[0] = &g_interfaces.player1;
	m_playerHandles[1] = &g_interfaces.player2;

	m_allSelectedCharNames[0] = getCharacterNameByIndexA(m_playerHandles[0]->GetData()->charIndex);
	m_allSelectedCharNames[1] = getCharacterNameByIndexA(m_playerHandles[1]->GetData()->charIndex);
}

void PaletteEditorWindow::CharacterSelection()
{
	LOG(7, "PaletteEditorWindow CharacterSelection\n");

	if (ImGui::Button(Messages.Select_character()))
	{
		ImGui::OpenPopup("select_char_pal");
	}

	ImGui::SameLine();
	ImGui::Text(m_selectedCharName);

	if (ImGui::BeginPopup("select_char_pal"))
	{
		const int NUMBER_OF_CHARS = 2;

		for (int i = 0; i < NUMBER_OF_CHARS; i++)
		{
			ImGui::PushID(i);

			if (ImGui::Selectable(m_allSelectedCharNames[i].c_str()))
			{
				DisableHighlightModes();

				m_selectedCharIndex = (CharIndex)m_playerHandles[i]->GetData()->charIndex;
				m_selectedCharName = m_allSelectedCharNames[i].c_str();
				m_selectedCharPalHandle = &m_playerHandles[i]->GetPalHandle();
				m_selectedPalIndex = g_interfaces.pPaletteManager->GetCurrentCustomPalIndex(*m_selectedCharPalHandle);
				CopyPalFileToEditorArray(m_selectedFile, *m_selectedCharPalHandle);
				CopyImplDataToEditorFields(*m_selectedCharPalHandle);
			}

			ImGui::PopID();
		}

		ImGui::EndPopup();
	}
}

void PaletteEditorWindow::PaletteSelection()
{
	LOG(7, "PaletteEditorWindow PaletteSelection\n");

	if (ImGui::Button(Messages.Select_palette()))
	{
		ImGui::OpenPopup("select_custom_pal");
	}

	ImGui::SameLine();
	ImGui::Text(m_customPaletteVector[m_selectedCharIndex][m_selectedPalIndex].palInfo.palName);

	ShowPaletteSelectPopup(*m_selectedCharPalHandle, m_selectedCharIndex, "select_custom_pal");
}

void PaletteEditorWindow::FileSelection()
{
	LOG(7, "PaletteEditorWindow FileSelection\n");

	if (ImGui::Button(Messages.Select_file()))
	{
		ImGui::OpenPopup("select_file_pal");
	}

	ImGui::SameLine();
	ImGui::Text(palFileNames[m_selectedFile]);

	if (ImGui::BeginPopup("select_file_pal"))
	{
		for (int i = 0; i < TOTAL_PALETTE_FILES; i++)
		{
			if (ImGui::Selectable(palFileNames[i]))
			{
				DisableHighlightModes();
				m_selectedFile = (PaletteFile)(i);
				CopyPalFileToEditorArray(m_selectedFile, *m_selectedCharPalHandle);
			}
		}

		ImGui::EndPopup();
	}
}

void PaletteEditorWindow::EditingModesSelection()
{
	LOG(7, "PaletteEditorWindow EditingModesSelection\n");

	ImGui::Separator();
	if (ImGui::Checkbox("Show transparency values", &m_showAlpha))
	{
		m_colorEditFlags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha;
		if (m_showAlpha)
		{
			m_colorEditFlags &= ~ImGuiColorEditFlags_NoAlpha;
			m_colorEditFlags |= ImGuiColorEditFlags_AlphaPreview | ImGuiColorEditFlags_AlphaBar;
		}
	}

	ImGui::SameLine();
	int nextLineColumnPosX = ImGui::GetCursorPosX();
	ImGui::Checkbox("Freeze frame", &g_gameVals.isFrameFrozen);

	if (ImGui::Checkbox("Highlight mode", &m_highlightMode))
	{
		if (m_highlightMode)
		{
			// Fill the array with black
			for (int i = 0; i < NUMBER_OF_COLOR_BOXES; i++)
			{
				((int*)m_highlightArray)[i] = COLOR_BLACK;
			}
			g_interfaces.pPaletteManager->ReplacePaletteFile(m_highlightArray, m_selectedFile, *m_selectedCharPalHandle);
		}
		else
		{
			DisableHighlightModes();
		}
	}

	if (ImGui::Button(Messages.Gradient_generator()))
	{
		ImGui::OpenPopup("gradient");
	}

	ShowGradientPopup();

	ImGui::Separator();
}

void PaletteEditorWindow::ShowPaletteBoxes()
{
	LOG(7, "PaletteEditorWindow ShowPaletteBoxes\n");

	ImGui::VerticalSpacing(10);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));

	ImGui::TextUnformatted("001 "); ImGui::SameLine();

	for (int i = 0, col = 1; i < NUMBER_OF_COLOR_BOXES; i++)
	{
		ImGui::PushID(i);

		bool pressed = false;
		int curColorBoxOffset = (i * sizeof(int));
		int idx = i + 1;

		PaletteChange potentialChange;
		memcpy(&potentialChange.oldValue, m_paletteEditorArray + curColorBoxOffset, sizeof(Color));
		potentialChange.offset = curColorBoxOffset;

		if (m_highlightMode)
		{
			ImGui::ColorButtonOn32Bit("##PalColorButton", idx, (unsigned char*)m_paletteEditorArray + curColorBoxOffset, m_colorEditFlags);

			if (ImGui::IsItemHovered())
				pressed = true;
		}
		else
		{
			pressed = ImGui::ColorEdit4On32Bit("##PalColorEdit", idx, (unsigned char*)m_paletteEditorArray + curColorBoxOffset, m_colorEditFlags);
		}

		if (pressed)
		{
			if (m_highlightMode)
			{
				UpdateHighlightArray(i);
			}
			else
			{
				memcpy(&potentialChange.newValue, m_paletteEditorArray + curColorBoxOffset, sizeof(Color));
				potentialChange.timestamp = std::time(nullptr);
				RecordPaletteChange(potentialChange);

				g_interfaces.pPaletteManager->ReplacePaletteFile(m_paletteEditorArray, m_selectedFile, *m_selectedCharPalHandle);
			}
		}

		if (col < COLUMNS)
		{
			// Continue the row
			ImGui::SameLine();
			col++;
		}
		else
		{
			// Start a new row
			col = 1;
			if (i < NUMBER_OF_COLOR_BOXES - 1)
			{
				ImGui::Text("%.3d ", i + 2);
				ImGui::SameLine();
			}
		}

		ImGui::PopID();
	}

	ImGui::PopStyleVar();
}

void PaletteEditorWindow::ShowUndoAndRedo()
{
	// Disable undo and redo when highlight mode is enabled
	if (m_highlightMode)
	{
		return;
	}

	if (m_history.cursor == 0)
	{
		ImGui::Text(Messages.Undo());
	}
	else if (ImGui::Button(Messages.Undo()))
	{
		Undo();
		g_interfaces.pPaletteManager->ReplacePaletteFile(m_paletteEditorArray, m_selectedFile, *m_selectedCharPalHandle);
	}

	ImGui::SameLine();

	if (m_history.cursor >= m_history.entries.size())
	{
		ImGui::Text(Messages.Redo());
	}
	else if (ImGui::Button(Messages.Redo()))
	{
		Redo();
		g_interfaces.pPaletteManager->ReplacePaletteFile(m_paletteEditorArray, m_selectedFile, *m_selectedCharPalHandle);
	}
}

void PaletteEditorWindow::DisableHighlightModes()
{
	m_highlightMode = false;
	g_interfaces.pPaletteManager->ReplacePaletteFile(m_paletteEditorArray, m_selectedFile, *m_selectedCharPalHandle);
}

void PaletteEditorWindow::SavePaletteToFile()
{
	static char message[200] = "";

	ImGui::VerticalSpacing(10);
	ImGui::Separator();

	if (m_highlightMode)
	{
		ImGui::TextDisabled(Messages.Cannot_save_with_Highlight_mode_on());
		return;
	}

	struct TextFilters
	{
		static int FilterAllowedChars(ImGuiInputTextCallbackData* data)
		{
			if (data->EventChar < 256 && strchr(" qwertzuiopasdfghjklyxcvbnmQWERTZUIOPASDFGHJKLYXCVBNM0123456789_.()[]!@&+-'^,;{}$=", (char)data->EventChar))
				return 0;
			return 1;
		}
	};


	ImGui::Checkbox(Messages.Save_with_bloom_effect(), &palBoolEffect);
	ImGui::HoverTooltip(Messages.Bloom_effects_cannot_be_changed_until_a_new_round_is_started());
	ImGui::Spacing();

	ImGui::Text(Messages.Palette_name());
	ImGui::PushItemWidth(250);
	ImGui::InputText("##palName", palNameBuf, IMPL_PALNAME_LENGTH, ImGuiInputTextFlags_CallbackCharFilter, TextFilters::FilterAllowedChars);
	ImGui::PopItemWidth();

	ImGui::Text(Messages.Creator_optional());
	ImGui::PushItemWidth(250);
	ImGui::InputText("##palcreator", palCreatorBuf, IMPL_CREATOR_LENGTH, ImGuiInputTextFlags_CallbackCharFilter, TextFilters::FilterAllowedChars);
	ImGui::PopItemWidth();

	ImGui::Text(Messages.Palette_description_optional());
	ImGui::PushItemWidth(250);
	ImGui::InputText("##palDesc", palDescBuf, IMPL_DESC_LENGTH, ImGuiInputTextFlags_CallbackCharFilter, TextFilters::FilterAllowedChars);
	ImGui::PopItemWidth();

	ImGui::Spacing();

	bool pressed = ImGui::Button(Messages.Save_palette(), ImVec2(125, 25));
	ImGui::Text(message);

	static bool show_overwrite_popup = false;

	if (!pressed && !show_overwrite_popup)
		return;

	if (strncmp(palNameBuf, "", IMPL_PALNAME_LENGTH) == 0)
	{
		std::string errorMsg = Messages.Error_no_filename_given();
		memcpy_s(message, sizeof(message), errorMsg.c_str(), errorMsg.length());
		g_imGuiLogger->Log("[error] Could not save custom palette, no filename was given\n");
		return;
	}

	if (strncmp(palNameBuf, "Default", IMPL_PALNAME_LENGTH) == 0 || strncmp(palNameBuf, "Random", IMPL_PALNAME_LENGTH) == 0)
	{
		std::string errorMsg = Messages.Error_not_a_valid_filename();
		memcpy_s(message, sizeof(message), errorMsg.c_str(), errorMsg.length());
		g_imGuiLogger->Log("[error] Could not save custom palette: not a valid filename\n");
		return;
	}

	TCHAR pathBuf[MAX_PATH];
	GetModuleFileName(NULL, pathBuf, MAX_PATH);
	std::wstring::size_type pos = std::wstring(pathBuf).find_last_of(L"\\");
	std::wstring wFullPath = std::wstring(pathBuf).substr(0, pos);

	wFullPath += L"\\BBCF_IM\\Palettes\\";
	wFullPath += getCharacterNameByIndexW(m_selectedCharIndex);
	wFullPath += L"\\";

	std::string filenameTemp(palNameBuf);
	std::wstring wFilename(filenameTemp.begin(), filenameTemp.end());
	wFullPath += wFilename;

	if (wFilename.find(IMPL_FILE_EXTENSION_W) == std::wstring::npos)
	{
		wFullPath += IMPL_FILE_EXTENSION_W;
		filenameTemp += IMPL_FILE_EXTENSION;
	}

	if (ShowOverwritePopup(&show_overwrite_popup, wFullPath.c_str(), filenameTemp.c_str()))
	{

		IMPL_data_t curPalData = g_interfaces.pPaletteManager->GetCurrentPalData(*m_selectedCharPalHandle);

		strncpy(curPalData.palInfo.creator, palCreatorBuf, IMPL_CREATOR_LENGTH);
		strncpy(curPalData.palInfo.palName, palNameBuf, IMPL_PALNAME_LENGTH);
		strncpy(curPalData.palInfo.desc, palDescBuf, IMPL_DESC_LENGTH);
		curPalData.palInfo.hasBloom = palBoolEffect;

		std::string messageText = FormatText(Messages.s_saved_successfully(), filenameTemp.c_str());

		if (g_interfaces.pPaletteManager->WritePaletteToFile(m_selectedCharIndex, &curPalData))
		{
			std::string fullPath(wFullPath.begin(), wFullPath.end());
			g_imGuiLogger->Log("[system] Custom palette '%s' successfully saved to:\n'%s'\n", filenameTemp.c_str(), fullPath.c_str());
			memcpy(message, messageText.c_str(), messageText.length() + 1);

			ReloadSavedPalette(palNameBuf);
		}
		else
		{
			g_imGuiLogger->Log("[error] Custom palette '%s' failed to be saved.\n", filenameTemp.c_str());
			std::string failureText = FormatText(Messages.s_save_failed(), filenameTemp.c_str());
			memcpy(message, failureText.c_str(), failureText.length() + 1);
		}
	}
}

void PaletteEditorWindow::ReloadSavedPalette(const char* palName)
{
	g_imGuiLogger->EnableLog(false);
	g_interfaces.pPaletteManager->ReloadAllPalettes();
	g_imGuiLogger->EnableLog(true);

	//find the newly loaded custom pal
	m_selectedPalIndex = g_interfaces.pPaletteManager->FindCustomPalIndex(m_selectedCharIndex, palName);

	if (m_selectedPalIndex < 0)
	{
		g_imGuiLogger->Log("[error] Saved custom palette couldn't be reloaded. Not found.\n");
		m_selectedPalIndex = 0;
	}

	g_interfaces.pPaletteManager->SwitchPalette(m_selectedCharIndex, *m_selectedCharPalHandle, m_selectedPalIndex);
	CopyPalFileToEditorArray(m_selectedFile, *m_selectedCharPalHandle);
}

bool PaletteEditorWindow::ShowOverwritePopup(bool* p_open, const wchar_t* wFullPath, const char* filename)
{
	bool isOverwriteAllowed = true;

	if (PathFileExists(wFullPath))
	{
		ImGui::OpenPopup("Overwrite?");
		*p_open = true;
	}

	if (ImGui::BeginPopupModal("Overwrite?", NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
ImGui::Text(Messages.Overwrite_confirmation_prompt(), filename);
		ImGui::Separator();

		if (ImGui::Button(Messages.OK(), ImVec2(120, 0)))
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			*p_open = false;
			isOverwriteAllowed = true;
			return isOverwriteAllowed;
		}

		ImGui::SameLine();
		if (ImGui::Button(Messages.Cancel(), ImVec2(120, 0)))
		{
			ImGui::CloseCurrentPopup();
			*p_open = false;
		}

		ImGui::EndPopup();
		isOverwriteAllowed = false;
	}

	return isOverwriteAllowed;
}

void PaletteEditorWindow::CheckSelectedPalOutOfBound()
{
	if (m_selectedPalIndex != 0 && m_selectedPalIndex >= m_customPaletteVector[m_selectedCharIndex].size())
	{
		// Reset back to default
		m_selectedPalIndex = 0;
		g_interfaces.pPaletteManager->SwitchPalette(m_selectedCharIndex, *m_selectedCharPalHandle, m_selectedPalIndex);
		CopyPalFileToEditorArray(m_selectedFile, *m_selectedCharPalHandle);
	}
}

void PaletteEditorWindow::ShowOnlinePaletteResetButton(Player& playerHandle, uint16_t matchPlayerIndex, const char* btnText)
{
	CharPaletteHandle& charPalHandle = playerHandle.GetPalHandle();
	CharIndex charIndex = (CharIndex)playerHandle.GetData()->charIndex;

	char buf[32];
	sprintf_s(buf, " X ##%s", btnText);

	if (ImGui::Button(buf))
	{
		g_interfaces.pPaletteManager->RestoreOrigPal(charPalHandle);
	}

	ImGui::HoverTooltip(Messages.Reset_palette());

	const OnlinePaletteManager::PaletteDownloadPermission downloadPermission =
		g_interfaces.pOnlinePaletteManager->GetDownloadPermission(matchPlayerIndex);
	char downloadButtonId[48];
	sprintf_s(downloadButtonId, " Download ##download%s", btnText);
	ImGui::SameLine();
	if (downloadPermission == OnlinePaletteManager::PaletteDownloadPermission::Granted)
	{
		if (ImGui::Button(downloadButtonId))
		{
			DownloadOnlinePalette(playerHandle, matchPlayerIndex);
		}
		ImGui::HoverTooltip(Messages.Download_palette());
	}
	else
	{
		// Greyed out but not item-disabled, so the explanatory tooltip still shows.
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
		ImGui::Button(downloadButtonId);
		ImGui::PopStyleVar();
		ImGui::HoverTooltip(downloadPermission == OnlinePaletteManager::PaletteDownloadPermission::Denied
			? Messages.Palette_download_denied_tooltip()
			: Messages.Palette_download_unsupported_tooltip());
	}

	// Dummy button
	ImGui::SameLine();
	ImGui::Button(btnText);

	ImGui::HoverTooltip(getCharacterNameByIndexA(charIndex).c_str());

	ImGui::SameLine();

	const IMPL_info_t& palInfo = g_interfaces.pPaletteManager->GetCurrentPalInfo(charPalHandle);
	ImGui::TextUnformatted(palInfo.palName);

	ShowHoveredPaletteInfoToolTip(palInfo, charIndex, 0);
}

void PaletteEditorWindow::DownloadOnlinePalette(Player& playerHandle, uint16_t matchPlayerIndex)
{
	if (!g_interfaces.pOnlinePaletteManager->CanDownloadPalette(matchPlayerIndex))
		return;

	CharIndex charIndex = (CharIndex)playerHandle.GetData()->charIndex;
	IMPL_data_t curPalData = g_interfaces.pPaletteManager->GetCurrentPalData(playerHandle.GetPalHandle());
	std::string savedPalName;

	if (g_interfaces.pPaletteManager->WriteDownloadedPaletteToFile(charIndex, &curPalData, &savedPalName))
	{
		g_imGuiLogger->Log("[system] Downloaded palette '%s%s' for %s.\n",
			savedPalName.c_str(),
			IMPL_FILE_EXTENSION,
			getCharacterNameByIndexA(charIndex).c_str());

		g_imGuiLogger->EnableLog(false);
		g_interfaces.pPaletteManager->ReloadAllPalettes();
		g_imGuiLogger->EnableLog(true);
	}
	else
	{
		g_imGuiLogger->Log("[error] Failed to download opponent palette.\n");
	}
}

void PaletteEditorWindow::ShowPaletteSelectButton(Player& playerHandle, const char* btnText, const char* popupID)
{
	CharPaletteHandle& charPalHandle = playerHandle.GetPalHandle();
	int selected_pal_index = g_interfaces.pPaletteManager->GetCurrentCustomPalIndex(charPalHandle);
	CharIndex charIndex = (CharIndex)playerHandle.GetData()->charIndex;

	if (charIndex >= getCharactersCount() || m_customPaletteVector[charIndex].size() <= selected_pal_index)
	{
		ImGui::TextUnformatted(Messages.Out_of_bounds());
		return;
	}

	const IMPL_info_t& palInfo = m_customPaletteVector[charIndex][selected_pal_index].palInfo;

	// Which player this is, then what they are wearing, then a picture of it. The name on
	// its own never told you what a palette actually looked like, which is the one thing
	// you want to know when picking one.
	//
	// The player label is small and grey on purpose: it is a heading, and it should not
	// compete with the thing it is labelling.
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::PushFont(NULL, ImGui::GetFontSize() * 0.85f);
	CenteredText(btnText);
	ImGui::PopFont();
	ImGui::PopStyleColor();
	ImGui::HoverTooltip(getCharacterNameByIndexA(playerHandle.GetData()->charIndex).c_str());

	// Name and sprite share one hover region, so the palette's details come up wherever on
	// the cell you happen to point at rather than only over the line of text.
	ImGui::BeginGroup();
	WrappedCenteredText(palInfo.palName);
	DrawPaletteSprite(charIndex, selected_pal_index,
		PaletteDataForPreview(charPalHandle, charIndex, selected_pal_index), kPaletteSpriteHeight);
	ImGui::EndGroup();
	ShowHoveredPaletteInfoToolTip(palInfo, charIndex, selected_pal_index);

	if (ImGui::Button(Messages.Assign_Palette(), ImVec2(-1.0f, 0.0f)))
	{
		ImGui::OpenPopup(popupID);
	}

	ShowPaletteSelectPopup(charPalHandle, charIndex, popupID);
}

void PaletteEditorWindow::ShowPaletteSelectPopup(CharPaletteHandle& charPalHandle, CharIndex charIndex, const char* popupID)
{
	static int hoveredPalIndex = 0;
	bool pressed = false;
	int onlinePalsStartIndex = g_interfaces.pPaletteManager->GetOnlinePalsStartIndex(charIndex);

	// A context menu, as before - it closes when you click away and it does not block the
	// rest of the UI. BeginPopup would force AlwaysAutoResize, which is what stops a popup
	// from being resizable, so this goes through BeginPopupEx and sets the flags itself.
	// The size is seeded on open from the remembered one and read back every frame, since
	// popups are also NoSavedSettings and ImGui will not store it for us.
	const ImGuiID popupIDHash = ImGui::GetCurrentWindow()->GetID(popupID);

	ImGui::SetNextWindowSize(ImVec2(g_pickerWidth, g_pickerHeight), ImGuiCond_Appearing);
	ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f, 200.0f), ImVec2(FLT_MAX, FLT_MAX));

	if (ImGui::BeginPopupEx(popupIDHash,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings))
	{
		const ImVec2 pickerSize = ImGui::GetWindowSize();
		if (pickerSize.x != g_pickerWidth || pickerSize.y != g_pickerHeight)
		{
			g_pickerWidth = pickerSize.x;
			g_pickerHeight = pickerSize.y;
			ImGui::MarkIniSettingsDirty();
		}

		ImGui::TextUnformatted(getCharacterNameByIndexA(charIndex).c_str());
		ImGui::Separator();

		const auto& palettes = m_customPaletteVector[charIndex];

		// A grid rather than a list of names: the whole point is being able to tell the
		// palettes apart at a glance, which a column of text cannot do. The column count
		// follows the window, so resizing it reflows rather than clipping.
		const float labelHeight = ImGui::GetTextLineHeight() * 2.0f + 2.0f;
		const float cellHeight = kPalettePickSpriteHeight + labelHeight;
		const float spacing = ImGui::GetStyle().ItemSpacing.x;

		ImGui::BeginChild("##palette_grid", ImVec2(0.0f, 0.0f), false);

		const float avail = ImGui::GetContentRegionAvail().x;
		int columns = (int)((avail + spacing) / (kPalettePickCellWidth + spacing));
		if (columns < 1)
			columns = 1;

		// Widen the cells to use the whole row rather than leaving a ragged strip on the
		// right; the sprite inside keeps its aspect ratio either way.
		const float cellWidth = (avail - spacing * (columns - 1)) / columns;

		int drawn = 0;
		const auto beginCell = [&]()
		{
			if (drawn % columns != 0)
				ImGui::SameLine();
			drawn++;
		};

		// Random first, as a tile with no sprite - there is nothing to show, because what
		// it lands on is not decided until it is pressed.
		beginCell();
		ImGui::PushID("random");
		ImGui::BeginGroup();
		{
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			if (ImGui::InvisibleButton("##cell", ImVec2(cellWidth, cellHeight)) && palettes.size() > 1)
			{
				const int current = g_interfaces.pPaletteManager->GetCurrentCustomPalIndex(charPalHandle);
				int next = current;
				while (current == next)
					next = rand() % palettes.size();

				pressed = true;
				ApplyPaletteSelection(charPalHandle, charIndex, next);
			}
			const bool hovered = ImGui::IsItemHovered();

			ImDrawList* draw = ImGui::GetWindowDrawList();
			if (hovered)
			{
				draw->AddRectFilled(origin, ImVec2(origin.x + cellWidth, origin.y + cellHeight),
					ImGui::GetColorU32(ImGuiCol_HeaderHovered), 3.0f);
			}

			const char* mark = "?";
			const ImVec2 markSize = ImGui::CalcTextSize(mark);
			draw->AddText(ImVec2(origin.x + (cellWidth - markSize.x) * 0.5f,
				origin.y + (kPalettePickSpriteHeight - markSize.y) * 0.5f),
				ImGui::GetColorU32(ImGuiCol_Text), mark);

			DrawWrappedCellLabel(Messages.Pick_Random(), origin, cellWidth,
				kPalettePickSpriteHeight, cellHeight - kPalettePickSpriteHeight);

			ImGui::EndGroup();
			if (hovered)
				ImGui::SetTooltip("%s", Messages.Random_selection());
		}
		ImGui::PopID();

		for (int i = 0; i < (int)palettes.size(); i++)
		{
			const IMPL_info_t& palInfo = palettes[i].palInfo;

			beginCell();
			ImGui::PushID(i);
			ImGui::BeginGroup();

			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const bool selected =
				(i == g_interfaces.pPaletteManager->GetCurrentCustomPalIndex(charPalHandle));

			if (ImGui::InvisibleButton("##cell", ImVec2(cellWidth, cellHeight)))
			{
				pressed = true;
				ApplyPaletteSelection(charPalHandle, charIndex, i);
			}
			const bool hovered = ImGui::IsItemHovered();
			if (hovered)
				hoveredPalIndex = i;

			ImDrawList* draw = ImGui::GetWindowDrawList();
			if (selected || hovered)
			{
				draw->AddRectFilled(origin, ImVec2(origin.x + cellWidth, origin.y + cellHeight),
					ImGui::GetColorU32(selected ? ImGuiCol_Header : ImGuiCol_HeaderHovered), 3.0f);
			}

			// Downloaded palettes get the same colour the list used to mark them with.
			if (i >= onlinePalsStartIndex)
			{
				draw->AddRect(origin, ImVec2(origin.x + cellWidth, origin.y + cellHeight),
					ImGui::GetColorU32(COLOR_ONLINE), 3.0f);
			}

			DrawPaletteSpriteAt(charIndex, i, PaletteDataForPreview(charPalHandle, charIndex, i),
				origin, cellWidth, kPalettePickSpriteHeight);

			DrawWrappedCellLabel(palInfo.palName, origin, cellWidth,
				kPalettePickSpriteHeight, labelHeight);

			ImGui::EndGroup();

			// The same tooltip the list has always shown: creator, description, bloom,
			// and whether it came from another player.
			ShowHoveredPaletteInfoToolTip(palInfo, charIndex, i);

			ImGui::PopID();
		}

		ImGui::EndChild();
		ImGui::EndPopup();
	}

	HandleHoveredPaletteSelection(&charPalHandle, charIndex, hoveredPalIndex, popupID, pressed);
}

// Everything that has to happen when a palette is chosen, wherever it was chosen from.
void PaletteEditorWindow::ApplyPaletteSelection(CharPaletteHandle& charPalHandle,
	CharIndex charIndex, int palIndex)
{
	g_interfaces.pPaletteManager->SwitchPalette(charIndex, charPalHandle, palIndex);

	// Keep the editor's own arrays in step if this is the character it has open.
	if (&charPalHandle == m_selectedCharPalHandle)
	{
		m_selectedPalIndex = palIndex;
		CopyPalFileToEditorArray(m_selectedFile, charPalHandle);
		DisableHighlightModes();
		CopyImplDataToEditorFields(charPalHandle);
	}

	if (g_interfaces.pRoomManager->IsRoomFunctional())
	{
		g_interfaces.pOnlinePaletteManager->SendPalettePackets();
	}
}

void PaletteEditorWindow::ShowHoveredPaletteInfoToolTip(const IMPL_info_t& palInfo, CharIndex charIndex, int palIndex)
{
	if (!ImGui::IsItemHovered())
	{
		return;
	}

	const char* creatorText = palInfo.creator;
	const char* descText = palInfo.desc;
	const int creatorLen = strnlen(creatorText, IMPL_CREATOR_LENGTH);
	const int descLen = strnlen(descText, IMPL_DESC_LENGTH);
	bool isOnlinePal = palIndex >= g_interfaces.pPaletteManager->GetOnlinePalsStartIndex(charIndex);
	bool hasBloom = palInfo.hasBloom;

	if (creatorLen || descLen || isOnlinePal || hasBloom)
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(300.0f);

		if (isOnlinePal)
			ImGui::TextColored(COLOR_ONLINE, Messages.ONLINE_PALETTE());

		if (creatorLen)
			ImGui::Text(Messages.Creator_s(), creatorText);

		if (descLen)
			ImGui::Text(Messages.Description_s(), descText);

		if (hasBloom)
			ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), Messages.Has_bloom_effect());

		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

void PaletteEditorWindow::HandleHoveredPaletteSelection(CharPaletteHandle* charPalHandle, CharIndex charIndex, int palIndex, const char* popupID, bool pressed)
{
	static CharPaletteHandle* prevCharHndl = 0;
	static int prevPalIndex = 0;
	static int origPalIndex = 0;
	static bool paletteSwitched = false;
	static char popupIDbkp[32];
	const char* palFileAddr = 0;

	if (pressed)
	{
		paletteSwitched = false;
	}
	else if (!ImGui::IsPopupOpen(popupID) && strcmp(popupIDbkp, popupID) == 0 &&
		paletteSwitched && prevCharHndl == charPalHandle && !pressed)
	{
		palFileAddr = g_interfaces.pPaletteManager->GetCustomPalFile(charIndex, origPalIndex, PaletteFile_Character, *charPalHandle);
		g_interfaces.pPaletteManager->ReplacePaletteFile(palFileAddr, PaletteFile_Character, *charPalHandle);
		paletteSwitched = false;
	}
	else if (ImGui::IsPopupOpen(popupID) && prevPalIndex != palIndex)
	{
		if (!paletteSwitched)
		{
			origPalIndex = g_interfaces.pPaletteManager->GetCurrentCustomPalIndex(*charPalHandle);
		}

		palFileAddr = g_interfaces.pPaletteManager->GetCustomPalFile(charIndex, palIndex, PaletteFile_Character, *charPalHandle);
		g_interfaces.pPaletteManager->ReplacePaletteFile(palFileAddr, PaletteFile_Character, *charPalHandle);
		prevPalIndex = palIndex;
		prevCharHndl = charPalHandle;
		paletteSwitched = true;
		strcpy(popupIDbkp, popupID);
	}
}

void PaletteEditorWindow::ShowPaletteRandomizerButton(const char* btnID, Player& playerHandle)
{
	int charIndex = playerHandle.GetData()->charIndex;
	char buf[32];
	sprintf_s(buf, " ? ##%s", btnID);

	if (ImGui::Button(buf) && m_customPaletteVector[charIndex].size() > 1)
	{
		CharPaletteHandle& charPalHandle = playerHandle.GetPalHandle();
		int curPalIndex = g_interfaces.pPaletteManager->GetCurrentCustomPalIndex(charPalHandle);
		int newPalIndex = curPalIndex;

		while (curPalIndex == newPalIndex)
		{
			newPalIndex = rand() % m_customPaletteVector[charIndex].size();
		}

		g_interfaces.pPaletteManager->SwitchPalette((CharIndex)charIndex, charPalHandle, newPalIndex);

		if (g_interfaces.pRoomManager->IsRoomFunctional())
		{
			g_interfaces.pOnlinePaletteManager->SendPalettePackets();
		}
	}

	ImGui::HoverTooltip(Messages.Random_selection());
}

void PaletteEditorWindow::CopyToEditorArray(const char* pSrc)
{
	ClearUndoHistory();
	memcpy(m_paletteEditorArray, pSrc, IMPL_PALETTE_DATALEN);
}

void PaletteEditorWindow::CopyPalFileToEditorArray(PaletteFile palFile, CharPaletteHandle& charPalHandle)
{
	const char* fileAddr = g_interfaces.pPaletteManager->GetCurPalFileAddr(palFile, charPalHandle);
	if (fileAddr == nullptr)
	{
		LOG(1, "PaletteEditorWindow::CopyPalFileToEditorArray skipped because palette file %d is not readable\n", (int)palFile);
		return;
	}
	CopyToEditorArray(fileAddr);
}

void PaletteEditorWindow::UpdateHighlightArray(int selectedBoxIndex)
{
	static int previousSelectedBoxIndex = 0;

	if (previousSelectedBoxIndex == selectedBoxIndex)
		return;

	// Set previously pressed box back to black
	((int*)m_highlightArray)[previousSelectedBoxIndex] = COLOR_BLACK;

	// Set currently pressed box to white
	((int*)m_highlightArray)[selectedBoxIndex] = COLOR_WHITE;

	g_interfaces.pPaletteManager->ReplacePaletteFile(m_highlightArray, m_selectedFile, *m_selectedCharPalHandle);

	previousSelectedBoxIndex = selectedBoxIndex;
}

void PaletteEditorWindow::CopyImplDataToEditorFields(CharPaletteHandle& charPalHandle)
{
	const IMPL_info_t& palInfo = g_interfaces.pPaletteManager->GetCurrentPalInfo(charPalHandle);

	std::string newPalName = strncmp(palInfo.palName, "Default", IMPL_PALNAME_LENGTH) == 0
		? ""
		: palInfo.palName;

	strncpy(palNameBuf, newPalName.c_str(), IMPL_PALNAME_LENGTH);
	strncpy(palDescBuf, palInfo.desc, IMPL_DESC_LENGTH);
	strncpy(palCreatorBuf, palInfo.creator, IMPL_CREATOR_LENGTH);
	palBoolEffect = palInfo.hasBloom;
}

void PaletteEditorWindow::ShowGradientPopup()
{
	if (ImGui::BeginPopup("gradient"))
	{
		ImGui::TextUnformatted(Messages.Gradient_generator());

		static int idx1 = 1;
		static int idx2 = 2;
		int minVal_idx2 = idx1 + 1;

		if (idx2 <= idx1)
		{
			idx2 = minVal_idx2;
		}

		ImGui::SliderInt(Messages.Start_index(), &idx1, 1, NUMBER_OF_COLOR_BOXES - 1);
		ImGui::SliderInt(Messages.End_index(), &idx2, minVal_idx2, NUMBER_OF_COLOR_BOXES);

		static int color1 = 0xFFFFFFFF;
		static int color2 = 0xFFFFFFFF;
		int alpha_flag = m_colorEditFlags & ImGuiColorEditFlags_NoAlpha;

		ImGui::ColorEdit4On32Bit(Messages.Start_color(), NULL, (unsigned char*)&color1, alpha_flag);
		ImGui::ColorEdit4On32Bit(Messages.End_color(), NULL, (unsigned char*)&color2, alpha_flag);

		if (ImGui::Button(Messages.Swap_colors()))
		{
			int temp = color2;
			color2 = color1;
			color1 = temp;
		}

		if (ImGui::Button(Messages.Generate_gradient()))
		{
			DisableHighlightModes();
			GenerateGradient(idx1, idx2, color1, color2);
		}

		ImGui::EndPopup();
	}
}

void PaletteEditorWindow::GenerateGradient(int idx1, int idx2, int color1, int color2)
{
	idx1 -= 1;
	idx2 -= 1;



	int steps = idx2 - idx1;
	if (steps < 1)
	{
		return;
	}

	size_t size = steps + 1;
	GradientChange change;
	change.start = idx1;
	change.oldColors.resize(size + 1);
	change.newColors.resize(size);
	memcpy(change.oldColors.data(), m_paletteEditorArray + idx1, size * sizeof(Color));

	float frac = 1.0 / (float)(idx2 - idx1);

	unsigned char a1 = (color1 & 0xFF000000) >> 24;
	unsigned char a2 = (color2 & 0xFF000000) >> 24;
	unsigned char r1 = (color1 & 0xFF0000) >> 16;
	unsigned char r2 = (color2 & 0xFF0000) >> 16;
	unsigned char g1 = (color1 & 0xFF00) >> 8;
	unsigned char g2 = (color2 & 0xFF00) >> 8;
	unsigned char b1 = color1 & 0xFF;
	unsigned char b2 = color2 & 0xFF;

	((int*)m_paletteEditorArray)[idx1] = color1;

	for (int i = 1; i <= steps; i++)
	{
		int a = ((int)((a2 - a1) * i * frac + a1) & 0xFF) << 24;
		int r = ((int)((r2 - r1) * i * frac + r1) & 0xFF) << 16;
		int g = ((int)((g2 - g1) * i * frac + g1) & 0xFF) << 8;
		int b = (int)((b2 - b1) * i * frac + b1) & 0xFF;
		int color = r | g | b;

		((int*)m_paletteEditorArray)[idx1 + i] = color ^ ((int*)m_paletteEditorArray)[idx1 + i] & a;
	}

	memcpy(change.newColors.data(), m_paletteEditorArray + idx1, size * sizeof(Color));
	RecordGradientChange(change);

	g_interfaces.pPaletteManager->ReplacePaletteFile(m_paletteEditorArray, m_selectedFile, *m_selectedCharPalHandle);
}
