#include "PaletteEditorModal.h"

#include "Core/HotkeyManager.h"
#include "Core/interfaces.h"
#include "Core/Localization.h"
#include "Core/utils.h"
#include "Game/characters.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Logger/ImGuiLogger.h"
#include "Core/NativeFileDialog.h"
#include "Core/ScreenColorPicker.h"
#include "Core/Settings.h"
#include "Palette/EffectSheets.h"
#include "Palette/PaletteSheet.h"
#include "Palette/PngPalette.h"
#include "Palette/NativePalettes.h"
#include "Palette/PaletteManager.h"
#include "Palette/PaletteThumbnails.h"

#include "imgui_internal.h"

#include <Windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

extern const char* implTemplates[];

namespace
{
	const char* kEditorPopupId = "###palette_editor";
	const char* kApplyId = "###palette_editor_apply";
	const char* kFadeId = "###palette_editor_fade";
	const char* kRebaseId = "###palette_editor_rebase";
	const char* kDiscardId = "###palette_editor_discard";
	const char* kOverwriteId = "###palette_editor_overwrite";
	const char* kSavedId = "###palette_editor_saved";

	// How far either side of the open colour the shade ramp strip reaches.
	const int kRampReach = 12;

	const char* kFileDialogOwner = "palette_editor";
	const int kDialogExportPage = 0;
	const int kDialogImportPage = 1;

	const size_t kMaxUndo = 200;
	const float kMaxZoom = 32.0f;
	const float kPickerWidth = 340.0f;

	// Thumbnail cache keys for pictures that are not installed palettes. The cache is
	// keyed by character + string, and a palette file name can never start with \x01.
	const char* kTemplateKey = "\x01template";

	// Fade settings, kept in ImGui's ini next to the window geometry, like the Palettes
	// window's panel width: they are a viewing preference, not a mod setting.
	const PaletteThumbnails::SheetFade kDefaultFade;
	const ImVec4 kDefaultCanvasBackground(0.11f, 0.11f, 0.125f, 1.0f);
	PaletteThumbnails::SheetFade g_fade;
	ImVec4 g_canvasBackground = kDefaultCanvasBackground;
	bool g_showColourGrid = false; // the 256-colour grid beside the picture

	void* EditorLayout_ReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
	{
		return strcmp(name, "Fade") == 0 ? (void*)1 : nullptr;
	}

	void EditorLayout_ReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
	{
		int r = 0, g = 0, b = 0;
		float f = 0.0f;
		float bg[3] = {};
		int flag = 0;
		if (sscanf_s(line, "Colour=%d,%d,%d", &r, &g, &b) == 3)
		{
			g_fade.red = (unsigned char)ImClamp(r, 0, 255);
			g_fade.green = (unsigned char)ImClamp(g, 0, 255);
			g_fade.blue = (unsigned char)ImClamp(b, 0, 255);
		}
		else if (sscanf_s(line, "Strength=%f", &f) == 1)
			g_fade.strength = ImClamp(f, 0.0f, 1.0f);
		else if (sscanf_s(line, "Shading=%f", &f) == 1)
			g_fade.shading = ImClamp(f, 0.0f, 1.0f);
		else if (sscanf_s(line, "Opacity=%f", &f) == 1)
			g_fade.opacity = ImClamp(f, 0.0f, 1.0f);
		else if (sscanf_s(line, "Background=%f,%f,%f", &bg[0], &bg[1], &bg[2]) == 3)
			g_canvasBackground = ImVec4(ImClamp(bg[0], 0.0f, 1.0f), ImClamp(bg[1], 0.0f, 1.0f), ImClamp(bg[2], 0.0f, 1.0f), 1.0f);
		else if (sscanf_s(line, "ColorGrid=%d", &flag) == 1)
			g_showColourGrid = flag != 0;
	}

	void EditorLayout_WriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
	{
		buf->appendf("[%s][Fade]\n", handler->TypeName);
		buf->appendf("Colour=%d,%d,%d\n", g_fade.red, g_fade.green, g_fade.blue);
		buf->appendf("Strength=%.3f\n", g_fade.strength);
		buf->appendf("Shading=%.3f\n", g_fade.shading);
		buf->appendf("Opacity=%.3f\n", g_fade.opacity);
		buf->appendf("Background=%.3f,%.3f,%.3f\n", g_canvasBackground.x, g_canvasBackground.y, g_canvasBackground.z);
		buf->appendf("ColorGrid=%d\n\n", g_showColourGrid ? 1 : 0);
	}

	// Same characters the in-match editor accepts, since both end up as a file name.
	int FilterPaletteNameChars(ImGuiInputTextCallbackData* data)
	{
		if (data->EventChar < 256 && strchr(" qwertzuiopasdfghjklyxcvbnmQWERTZUIOPASDFGHJKLYXCVBNM0123456789_.()[]!@&+-'^,;{}$=", (char)data->EventChar))
			return 0;
		return 1;
	}

	// Tooltip for any control, disabled ones included - a greyed-out button is exactly the
	// one whose reason you want to know - wrapped so long explanations stay readable.
	void Tip(const std::string& text)
	{
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay |
			ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
			ImGui::TextUnformatted(text.c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	// Modifier keys for clicks, read from the keyboard itself as well as from ImGui: BBCF
	// never hands its window the WM_SYSKEYDOWN that Alt arrives as, so ImGui's KeyAlt stays
	// false while it is held. The mod's own hotkeys read the keys the same way.
	bool KeyHeld(bool imguiState, int virtualKey)
	{
		return imguiState || (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
	}
	bool ShiftHeld() { return KeyHeld(ImGui::GetIO().KeyShift, VK_SHIFT); }
	bool CtrlHeld() { return KeyHeld(ImGui::GetIO().KeyCtrl, VK_CONTROL); }
	bool AltHeld() { return KeyHeld(ImGui::GetIO().KeyAlt, VK_MENU); }

	std::string ColorLabel(int colorIndex)
	{
		char label[32];
		sprintf_s(label, "Color %02d", colorIndex + 1);
		return label;
	}

	std::string NativeKey(int colorIndex)
	{
		char key[32];
		sprintf_s(key, "\x01native_%02d", colorIndex);
		return key;
	}

	// A cache key that changes whenever the colours do, for the palette being edited: the
	// thumbnail cache only ever compares keys.
	std::string EditingKey(const char* paletteData)
	{
		unsigned int hash = 2166136261u;
		for (int i = 0; i < IMPL_PALETTE_DATALEN; i++)
			hash = (hash ^ (unsigned char)paletteData[i]) * 16777619u;
		char key[32];
		sprintf_s(key, "\x01" "editing_%08x", hash);
		return key;
	}

	// The character colours of the built-in template (Color 01), straight out of the
	// embedded bytes: the character grid wants one per character per frame, and copying
	// 36 whole palettes a frame for it would be silly.
	const char* TemplateCharacterFile(int charIndex)
	{
		return implTemplates[charIndex] + offsetof(IMPL_t, palData) + offsetof(IMPL_data_t, file0);
	}

	IMPL_data_t TemplatePalette(int charIndex)
	{
		IMPL_t implTemplate;
		memcpy_s(&implTemplate, sizeof(IMPL_t), implTemplates[charIndex], sizeof(IMPL_t));
		IMPL_data_t data = implTemplate.palData;
		data.palInfo = IMPL_info_t();
		return data;
	}

	void CopyColourFiles(IMPL_data_t& dst, const IMPL_data_t& src)
	{
		const IMPL_info_t info = dst.palInfo;
		dst = src;
		dst.palInfo = info;
	}

	ImVec4 EntryColourVec(const unsigned char* bgra)
	{
		return ImVec4(bgra[2] / 255.0f, bgra[1] / 255.0f, bgra[0] / 255.0f, bgra[3] / 255.0f);
	}

	bool IsReservedName(const char* name)
	{
		return _stricmp(name, "Default") == 0 || _stricmp(name, "Random") == 0 ||
			_stricmp(name, "Random_Exclude_Default") == 0;
	}

	std::string Trimmed(const char* text)
	{
		std::string s(text);
		while (!s.empty() && (s.back() == ' ' || s.back() == '.'))
			s.pop_back();
		while (!s.empty() && s.front() == ' ')
			s.erase(s.begin());
		return s;
	}

	std::string PalettePath(int charIndex, const std::string& name)
	{
		return std::string("BBCF_IM\\Palettes\\") + getCharacterNameByIndexA(charIndex) + "\\" +
			name + IMPL_FILE_EXTENSION;
	}

	const std::vector<IMPL_data_t>* InstalledPalettes(int charIndex)
	{
		if (!g_interfaces.pPaletteManager)
			return nullptr;
		const auto& custom = g_interfaces.pPaletteManager->GetCustomPalettesVector();
		if (charIndex < 0 || charIndex >= (int)custom.size())
			return nullptr;
		return &custom[charIndex];
	}

	// Lays cells out in rows that fill the available width.
	struct CellFlow
	{
		int columns = 1;
		int drawn = 0;

		CellFlow(float cellWidth)
		{
			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			columns = (std::max)(1, (int)((ImGui::GetContentRegionAvail().x + spacing) / (cellWidth + spacing)));
		}

		void Next()
		{
			if (drawn % columns != 0)
				ImGui::SameLine();
			drawn++;
		}
	};

	void VerticalSeparator()
	{
		ImGui::SameLine(0.0f, 12.0f);
		ImGui::TextDisabled("|");
		ImGui::SameLine(0.0f, 12.0f);
	}

	std::string CanvasHelpText()
	{
		return L("How the editor works:") + "\n\n" +
			L("Click a spot on the character to edit the color that paints it. Everything else fades out, so you can see exactly what that one color covers.") + "\n" +
			L("Recolor it with the picker. Clicking another color, empty space or anything else keeps the change; Cancel throws it away.") + "\n\n" +
			L("Ctrl+click adds more colors to the open one: they all take the picker's color once you change it. Alt+click moves on to another color and gives it the color you had, for walking along a ramp a step at a time.") + "\n\n" +
			L("Drag with any mouse button to move around; scroll to zoom.") + "\n" +
			L("The swatch blocks in the corner hold the colors that do not appear on the sprites, mostly effects.") + "\n" +
			L("\"Show color grid\" at the top right lists all 256 colors of the file on screen, including the ones on no sprite.") + "\n\n" +
			L("Ctrl+Z / Ctrl+Y undo and redo your changes. Escape cancels an open color.") + "\n" +
			L("Ctrl+C copies the open color; Ctrl+V pastes it into whichever color is open.");
	}
}

void PaletteEditorModal::RegisterLayoutSettings()
{
	if (ImGui::FindSettingsHandler("BBCFIMPaletteEditor"))
		return;

	ImGuiSettingsHandler handler;
	handler.TypeName = "BBCFIMPaletteEditor";
	handler.TypeHash = ImHashStr("BBCFIMPaletteEditor");
	handler.ReadOpenFn = EditorLayout_ReadOpen;
	handler.ReadLineFn = EditorLayout_ReadLine;
	handler.WriteAllFn = EditorLayout_WriteAll;
	ImGui::AddSettingsHandler(&handler);
}

// --- Opening -----------------------------------------------------------------------------

void PaletteEditorModal::OpenNew(int defaultCharIndex)
{
	m_charIndex = (defaultCharIndex >= 0 && defaultCharIndex < getCharactersCount()) ? defaultCharIndex : 0;
	m_choice = BaseChoice();
	m_choice.valid = true; // Color 01, so Next is one click away
	m_loadedChoice = BaseChoice();
	m_loadedChar = -1;
	m_originalName.clear();
	m_dirty = false;
	m_selected = -1;
	memset(m_name, 0, sizeof(m_name));
	memset(m_creator, 0, sizeof(m_creator));
	memset(m_desc, 0, sizeof(m_desc));
	m_bloom = false;
	m_status.clear();
	m_baseFilter.Clear();
	m_step = Step_Character;
	m_file = 0;
	m_keepLiveOnClose = false;
	m_editingExisting = false;
	m_requestOpen = true;
}

void PaletteEditorModal::OpenExisting(int charIndex, const IMPL_data_t& palette)
{
	m_charIndex = charIndex;
	m_choice = BaseChoice();
	m_choice.valid = true;
	m_choice.isNative = false;
	m_choice.name = palette.palInfo.palName;
	m_originalName = palette.palInfo.palName;

	memset(m_name, 0, sizeof(m_name));
	memset(m_creator, 0, sizeof(m_creator));
	memset(m_desc, 0, sizeof(m_desc));
	strncpy(m_name, palette.palInfo.palName, sizeof(m_name) - 1);
	strncpy(m_creator, palette.palInfo.creator, sizeof(m_creator) - 1);
	strncpy(m_desc, palette.palInfo.desc, sizeof(m_desc) - 1);
	m_bloom = palette.palInfo.hasBloom;
	m_status.clear();
	m_baseFilter.Clear();

	LoadBase(palette);
	m_loadedChoice = m_choice;
	m_loadedChar = charIndex;
	m_step = Step_Edit;
	m_file = 0;
	m_keepLiveOnClose = false;
	// An installed palette already has its character and colours: editing it is just
	// recolouring and saving, with no way back into picking something else.
	m_editingExisting = true;
	m_requestOpen = true;
}

// --- Palette state -----------------------------------------------------------------------

const IMPL_data_t* PaletteEditorModal::NativeColour(int charIndex, int colorIndex)
{
	if (m_nativeChar != charIndex)
	{
		m_nativeChar = charIndex;
		m_nativeColours.assign(NativePalettes::kColorCount, IMPL_data_t());
		m_nativeLoaded.assign(NativePalettes::kColorCount, false);
		m_nativeError.clear();

		for (int i = 0; i < NativePalettes::kColorCount; i++)
		{
			std::string error;
			if (NativePalettes::Load(charIndex, i, m_nativeColours[i], error))
			{
				m_nativeLoaded[i] = true;
			}
			else if (m_nativeError.empty())
			{
				m_nativeError = error;
				g_imGuiLogger->Log("[error] Palette editor: %s\n", error.c_str());
			}
		}

		// Color 01 is always there, even when the archive is not: it is embedded.
		if (!m_nativeLoaded[0])
		{
			m_nativeColours[0] = TemplatePalette(charIndex);
			m_nativeLoaded[0] = true;
		}
	}

	if (colorIndex < 0 || colorIndex >= (int)m_nativeLoaded.size() || !m_nativeLoaded[colorIndex])
		return nullptr;
	return &m_nativeColours[colorIndex];
}

bool PaletteEditorModal::ResolveBase(const BaseChoice& choice, IMPL_data_t& out, std::string& error)
{
	if (!choice.valid)
	{
		error = L("Pick something to start from first.");
		return false;
	}

	if (choice.isNative)
	{
		const IMPL_data_t* native = NativeColour(m_charIndex, choice.nativeIndex);
		if (!native)
		{
			error = m_nativeError.empty() ? L("That color could not be read from the game files.") : m_nativeError;
			return false;
		}
		out = *native;
		return true;
	}

	const std::vector<IMPL_data_t>* installed = InstalledPalettes(m_charIndex);
	if (installed)
	{
		// Index 0 is the "Default" placeholder, not a file.
		for (size_t i = 1; i < installed->size(); i++)
		{
			if (choice.name == (*installed)[i].palInfo.palName)
			{
				out = (*installed)[i];
				return true;
			}
		}
	}
	error = L("That palette is no longer installed.");
	return false;
}

void PaletteEditorModal::LoadBase(const IMPL_data_t& palette)
{
	if (m_file != 0 && EffectSheets::ImageCount(m_charIndex, m_file) == 0)
		m_file = 0;
	CopyColourFiles(m_palette, palette);
	CopyColourFiles(m_base, palette);
	m_dirty = false;
	m_selected = -1;
	m_undo.clear();
	m_redo.clear();
	m_fitPending = true;
	RecountUsage();
}

void PaletteEditorModal::GoToEditStep()
{
	// Coming back from step 2 with the same pick keeps the edits; a different pick starts
	// over, which is worth asking about once there is something to lose.
	if (m_loadedChar == m_charIndex && m_loadedChoice == m_choice)
	{
		m_step = Step_Edit;
		return;
	}
	if (m_dirty)
	{
		m_requestRebase = true;
		return;
	}

	IMPL_data_t data;
	std::string error;
	if (!ResolveBase(m_choice, data, error))
	{
		m_status = error;
		m_statusIsError = true;
		return;
	}
	LoadBase(data);
	m_loadedChoice = m_choice;
	m_loadedChar = m_charIndex;
	m_status.clear();
	m_step = Step_Edit;
}

void PaletteEditorModal::RecountUsage()
{
	memset(m_usage, 0, sizeof(m_usage));
	const unsigned char* indices = nullptr;
	int width = 0, height = 0;
	if (!PaletteThumbnails::GetEditorSheetIndices(m_charIndex, &indices, &width, &height))
		return;
	const size_t count = (size_t)width * height;
	for (size_t i = 0; i < count; i++)
		m_usage[indices[i]]++;
	m_usageKey = m_charIndex;
}

void PaletteEditorModal::Select(int index)
{
	if (index == m_selected && m_extraCount == 0)
		return;
	// Moving on to another colour keeps what this one became: click, adjust, click, adjust.
	AcceptPick();
	m_selected = index;
	if (index >= 0)
	{
		memcpy(m_current, Entry(index), 4);
		memcpy(m_pending, m_current, 4);
		AutoDetectRamp();
		// A click is "change this colour"; working on a range is something you switch to.
		m_forceSingleTab = true;
		m_rampOn = false;
	}
}


void PaletteEditorModal::ClickEntry(int index)
{
	if (m_selected < 0)
	{
		Select(index);
		return;
	}

	// Shift+click moves the end of the range being edited.
	if (ShiftHeld() && m_rampOn)
	{
		m_rampEnd = (std::max)(1, index);
		return;
	}

	// Ctrl+click adds a colour to the open one, or takes it back out. It keeps its own
	// colour until the picker changes; then it takes the picker's, like the one clicked first.
	if (CtrlHeld())
	{
		if (index != m_selected)
		{
			m_extra[index] = !m_extra[index];
			m_extraCount += m_extra[index] ? 1 : -1;
		}
		return;
	}

	// Alt+click moves on to `index` and gives it the colour the picker had: pick the hair,
	// Alt+click the next strand, nudge it, Alt+click the next...
	if (AltHeld() && index != m_selected)
	{
		unsigned char carried[4];
		memcpy(carried, m_pending, 4);
		Select(index);
		memcpy(m_pending, carried, 4);
		return;
	}

	Select(index);
}

void PaletteEditorModal::AcceptPick()
{
	if (m_selected < 0)
		return;
	if (m_eyedropTarget)
		ScreenColorPicker::Cancel();
	BuildPendingFile();
	char* file = FileData(m_palette, m_file);
	if (memcmp(file, m_pendingFile, IMPL_PALETTE_DATALEN) != 0)
	{
		PushUndo(m_palette);
		memcpy(file, m_pendingFile, IMPL_PALETTE_DATALEN);
		m_dirty = true;
	}
	m_selected = -1;
	memset(m_extra, 0, sizeof(m_extra));
	m_extraCount = 0;
}


unsigned char* PaletteEditorModal::PickerColour()
{
	if (m_selected < 0)
		return nullptr;
	// In gradient mode the picker edits one of the two ends, not the colour clicked.
	if (m_rampOn && m_rampMode == 1)
		return m_rampEditEnd == 0 ? m_rampFrom : m_rampTo;
	return m_pending;
}

void PaletteEditorModal::CopyColour()
{
	const unsigned char* colour = PickerColour();
	if (!colour)
		return;
	memcpy(m_clipboard, colour, 4);
	m_haveClipboard = true;
	char hex[16];
	sprintf_s(hex, "#%02X%02X%02X", colour[2], colour[1], colour[0]);
	m_status = FormatText(L("Copied %s. Open another color and press Ctrl+V to paste it.").c_str(), hex);
	m_statusIsError = false;
}

void PaletteEditorModal::PasteColour()
{
	unsigned char* colour = PickerColour();
	if (!colour || !m_haveClipboard)
		return;
	// Transparency comes along only while it is being edited, as with the eyedropper.
	memcpy(colour, m_clipboard, m_editAlpha ? 4 : 3);
	if (colour != m_pending)
		m_rampEndsCustom = true;
}

bool PaletteEditorModal::PickChanged()
{
	if (m_selected < 0)
		return false;
	BuildPendingFile();
	return memcmp(FileData(m_palette, m_file), m_pendingFile, IMPL_PALETTE_DATALEN) != 0;
}

void PaletteEditorModal::CancelPick()
{
	if (m_eyedropTarget)
		ScreenColorPicker::Cancel();
	m_selected = -1;
	memset(m_extra, 0, sizeof(m_extra));
	m_extraCount = 0;
}

void PaletteEditorModal::PushUndo(const IMPL_data_t& before)
{
	m_undo.push_back(before);
	if (m_undo.size() > kMaxUndo)
		m_undo.erase(m_undo.begin());
	m_redo.clear();
}

void PaletteEditorModal::Undo()
{
	// An open colour is kept first, so Undo takes back that change when there is one.
	AcceptPick();
	if (m_undo.empty())
		return;
	m_redo.push_back(m_palette);
	CopyColourFiles(m_palette, m_undo.back());
	m_undo.pop_back();
	m_dirty = true;
}

void PaletteEditorModal::Redo()
{
	// An open change is a new change, and a new change ends what there was to redo.
	if (m_redo.empty() || PickChanged())
		return;
	CancelPick();
	m_undo.push_back(m_palette);
	CopyColourFiles(m_palette, m_redo.back());
	m_redo.pop_back();
	m_dirty = true;
}

void PaletteEditorModal::RequestClose()
{
	if (m_dirty || PickChanged())
		m_requestDiscard = true;
	else
		ImGui::CloseCurrentPopup();
}

// --- Frame -------------------------------------------------------------------------------

void PaletteEditorModal::Draw(const std::function<void()>& onSaved)
{
	// Not drawn last frame while it was open: the overlay was hidden, and ImGui has closed
	// the popup since. Everything it was showing is still here, so just put it back.
	const int frame = ImGui::GetFrameCount();
	if (m_openLastDraw && m_lastDrawFrame != frame - 1 && !ImGui::IsPopupOpen(kEditorPopupId))
		m_requestOpen = true;
	m_lastDrawFrame = frame;

	if (m_requestOpen)
	{
		m_requestOpen = false;
		ImGui::OpenPopup(kEditorPopupId);
	}
	const bool open = ImGui::IsPopupOpen(kEditorPopupId);
	DrawEditor(onSaved);
	const bool stillOpen = open && ImGui::IsPopupOpen(kEditorPopupId);
	m_openLastDraw = stillOpen;
	UpdateLive(stillOpen);
	// The effect sprites are only worth their memory while the editor is up.
	if (!stillOpen)
		EffectSheets::Release();
}


void PaletteEditorModal::DrawEditor(const std::function<void()>& onSaved)
{
	const std::string title = L("Palette Editor") + (m_dirty ? " *" : "") + kEditorPopupId;
	// The whole screen: the picture is the editor, and it wants every pixel it can get.
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse))
		return;

	DrawStepHeader();

	const float navHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 4.0f;
	ImGui::BeginChild("##pe_step", ImVec2(0, -navHeight), ImGuiChildFlags_None,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	switch (m_step)
	{
	case Step_Character: DrawCharacterStep(); break;
	case Step_Base:      DrawBaseStep(); break;
	case Step_Edit:      DrawEditStep(); break;
	case Step_Details:   DrawDetailsStep(); break;
	}
	ImGui::EndChild();

	DrawNavigation(onSaved);

	DrawApplyPalettePopup();
	DrawFadeSettingsPopup();
	DrawRebaseConfirm();
	DrawDiscardConfirm();
	DrawOverwriteConfirm(onSaved);
	DrawSavedPopup();

	// Set by a nested popup that wants the editor itself gone (discard, saved). It has
	// to happen here, where the editor is the current popup again.
	if (m_closeAfterPopup)
	{
		m_closeAfterPopup = false;
		m_selected = -1;
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}

void PaletteEditorModal::DrawStepHeader()
{
	// "1. Character > 2. Starting palette > 3. Edit colors > 4. Name and save", current lit.
	const std::string names[4] = {
		L("Character"), L("Starting palette"), L("Edit colors"), L("Name and save")
	};
	// Editing an installed palette skips the first two.
	const int firstStep = m_editingExisting ? Step_Edit : Step_Character;
	for (int i = firstStep; i < 4; i++)
	{
		if (i > firstStep)
		{
			ImGui::SameLine();
			ImGui::TextDisabled(">");
			ImGui::SameLine();
		}
		char label[128];
		sprintf_s(label, "%d. %s", i - firstStep + 1, names[i].c_str());
		if (i == (int)m_step)
			ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", label);
		else
			ImGui::TextDisabled("%s", label);
	}

	if (m_step != Step_Character)
	{
		ImGui::SameLine(0.0f, 32.0f);
		ImGui::TextUnformatted(getCharacterNameByIndexA(m_charIndex).c_str());
		if (m_step >= Step_Edit && m_loadedChoice.valid)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(%s %s)", L("from").c_str(),
				m_loadedChoice.isNative ? ColorLabel(m_loadedChoice.nativeIndex).c_str() : m_loadedChoice.name.c_str());
		}
	}
	ImGui::Separator();
}

void PaletteEditorModal::DrawNavigation(const std::function<void()>& onSaved)
{
	ImGui::Separator();

	const float buttonWidth = 120.0f;
	const ImGuiStyle& style = ImGui::GetStyle();

	ImGui::AlignTextToFramePadding();
	if (!m_status.empty() && m_step != Step_Details)
	{
		ImGui::TextColored(m_statusIsError ? ImVec4(1.0f, 0.42f, 0.42f, 1.0f) : ImVec4(0.30f, 0.85f, 0.39f, 1.0f),
			"%s", m_status.c_str());
		ImGui::SameLine();
	}

	ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
		ImGui::GetWindowWidth() - buttonWidth * 3.0f - style.ItemSpacing.x * 2.0f - style.WindowPadding.x));

	if (ImGui::Button(Messages.Cancel(), ImVec2(buttonWidth, 0)))
		RequestClose();
	Tip(L("Close the editor without saving. Asks first if you changed anything."));

	ImGui::SameLine();
	const bool atFirstStep = m_step == Step_Character || (m_editingExisting && m_step == Step_Edit);
	ImGui::BeginDisabled(atFirstStep);
	if (ImGui::Button(L("< Back").c_str(), ImVec2(buttonWidth, 0)))
	{
		AcceptPick();
		m_status.clear();
		m_step = (Step)(m_step - 1);
	}
	ImGui::EndDisabled();
	Tip(L("Go back a step. Your edits are kept unless you pick a different character or starting palette."));

	ImGui::SameLine();
	if (m_step == Step_Details)
	{
		const std::string problem = NameProblem();
		ImGui::BeginDisabled(!problem.empty());
		if (ImGui::Button(Messages.Save(), ImVec2(buttonWidth, 0)))
			Save(onSaved, false);
		ImGui::EndDisabled();
		Tip(!problem.empty() ? problem
			: L("Write the palette to your palettes folder. It shows up in the Palettes window straight away."));
		return;
	}

	const bool canContinue = !(m_step == Step_Base && !m_choice.valid);
	ImGui::BeginDisabled(!canContinue);
	if (ImGui::Button(L("Next >").c_str(), ImVec2(buttonWidth, 0)))
	{
		AcceptPick();
		m_status.clear();
		if (m_step == Step_Character)
			m_step = Step_Base;
		else if (m_step == Step_Base)
			GoToEditStep();
		else if (m_step == Step_Edit)
			m_step = Step_Details;
	}
	ImGui::EndDisabled();
	Tip(m_step == Step_Edit ? L("Done recoloring: go on to naming and saving the palette.")
		: L("Go on to the next step. Double-clicking a picture does the same."));
}

// --- Development: thumbnail framing tuner ----------------------------------------------------
//
// Only with EnableInDevelopmentFeatures on. Right-click a character in step 1, Adjust, and
// move and scale their thumbnail live. The values land in BBCF_IM\ThumbnailAdjust.txt; copy
// them into FRAMING_OVERRIDES in tools/build_palette_thumbnails.py, which re-renders the
// thumbnails from the full-size sprites, then bump kThumbnailGeneration below.
//
// Adjustments are relative to the thumbnails they were made on. Once baked in they would
// apply twice, so the file records the thumbnail generation it was tuned against and a
// file from any other generation is ignored.

namespace
{
	struct ThumbnailAdjust
	{
		float scale = 1.0f;
		float dx = 0.0f; // thumbnail pixels (the thumbnails are 160x200)
		float dy = 0.0f;
	};

	// Bump whenever resource/palette_thumbnails.bin is rebuilt with new framing.
	const int kThumbnailGeneration = 2;

	ThumbnailAdjust g_thumbAdjust[64];
	bool g_thumbAdjustLoaded = false;

	bool ThumbAdjustEnabled()
	{
		return Settings::settingsIni.enableInDevelopmentFeatures;
	}

	std::string ThumbAdjustPath()
	{
		return GamePath("BBCF_IM\\ThumbnailAdjust.txt");
	}

	void LoadThumbAdjust()
	{
		if (g_thumbAdjustLoaded)
			return;
		g_thumbAdjustLoaded = true;
		FILE* file = nullptr;
		if (fopen_s(&file, ThumbAdjustPath().c_str(), "r") != 0 || !file)
			return;
		char line[256];
		bool current = false;
		while (fgets(line, sizeof(line), file))
		{
			int generation = 0;
			if (sscanf_s(line, "# thumbnails %d", &generation) == 1)
			{
				current = generation == kThumbnailGeneration;
				continue;
			}
			if (!current)
				continue;
			int index = -1;
			char tag[16] = {};
			float scale = 1.0f, dx = 0.0f, dy = 0.0f;
			if (sscanf_s(line, "%d %15s %f %f %f", &index, tag, (unsigned)sizeof(tag), &scale, &dx, &dy) == 5 &&
				index >= 0 && index < 64)
			{
				g_thumbAdjust[index].scale = scale;
				g_thumbAdjust[index].dx = dx;
				g_thumbAdjust[index].dy = dy;
			}
		}
		fclose(file);
	}

	void SaveThumbAdjust()
	{
		FILE* file = nullptr;
		if (fopen_s(&file, ThumbAdjustPath().c_str(), "w") != 0 || !file)
			return;
		fprintf(file, "# thumbnails %d\n", kThumbnailGeneration);
		fprintf(file, "# index tag scale dx dy  (thumbnail pixels, 160x200)\n");
		for (int i = 0; i < getCharactersCount() && i < 64; i++)
		{
			const ThumbnailAdjust& a = g_thumbAdjust[i];
			const char* tag = NativePalettes::CharTag(i);
			fprintf(file, "%d %s %.3f %.2f %.2f\n", i, tag ? tag : "?", a.scale, a.dx, a.dy);
		}
		fclose(file);
	}
}

void PaletteEditorModal::DrawAdjustedThumbnail(ImDrawList* draw, ImTextureID texture, int charIndex,
	const ImVec2& boxMin, const ImVec2& boxSize, float pixelScale)
{
	LoadThumbAdjust();
	const ThumbnailAdjust a = (ThumbAdjustEnabled() && charIndex >= 0 && charIndex < 64)
		? g_thumbAdjust[charIndex] : ThumbnailAdjust();
	// Scaled about the centre of the thumbnail, then moved; clipped to the thumbnail's own
	// box, which is exactly what baking it into the 160x200 image will do.
	const ImVec2 centre(boxMin.x + boxSize.x * 0.5f, boxMin.y + boxSize.y * 0.5f);
	const ImVec2 size(boxSize.x * a.scale, boxSize.y * a.scale);
	const ImVec2 p0(centre.x - size.x * 0.5f + a.dx * pixelScale, centre.y - size.y * 0.5f + a.dy * pixelScale);
	draw->PushClipRect(boxMin, ImVec2(boxMin.x + boxSize.x, boxMin.y + boxSize.y), true);
	draw->AddImage(ImTextureRef(texture), p0, ImVec2(p0.x + size.x, p0.y + size.y));
	draw->PopClipRect();
}

void PaletteEditorModal::DrawThumbnailAdjust()
{
	if (m_openAdjustMenu)
	{
		m_openAdjustMenu = false;
		ImGui::OpenPopup("##pe_adjust_menu");
	}
	if (ImGui::BeginPopup("##pe_adjust_menu"))
	{
		if (ImGui::MenuItem(L("Adjust").c_str()))
			m_openAdjust = true;
		ImGui::EndPopup();
	}
	if (m_openAdjust)
	{
		m_openAdjust = false;
		ImGui::OpenPopup("##pe_adjust");
	}

	// A plain popup, so the grid behind it keeps showing the change live.
	if (!ImGui::BeginPopup("##pe_adjust"))
		return;
	if (m_adjustChar < 0 || m_adjustChar >= 64)
	{
		ImGui::EndPopup();
		return;
	}

	LoadThumbAdjust();
	ThumbnailAdjust& a = g_thumbAdjust[m_adjustChar];
	ImGui::Text("%s - %s", L("Adjust").c_str(), getCharacterNameByIndexA(m_adjustChar).c_str());
	ImGui::TextDisabled("%s", L("Development tool for tuning the character thumbnails.").c_str());
	ImGui::Separator();

	// Large preview, drawn the same way as the grid cells.
	int texWidth = 0, texHeight = 0;
	const ImTextureID texture = PaletteThumbnails::Get(m_adjustChar, kTemplateKey,
		TemplateCharacterFile(m_adjustChar), &texWidth, &texHeight);
	if (texture && texWidth > 0 && texHeight > 0)
	{
		const float pixelScale = 1.6f;
		const ImVec2 boxSize(texWidth * pixelScale, texHeight * pixelScale);
		const ImVec2 boxMin = ImGui::GetCursorScreenPos();
		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(boxMin, ImVec2(boxMin.x + boxSize.x, boxMin.y + boxSize.y), ImGui::GetColorU32(ImGuiCol_FrameBg));
		DrawAdjustedThumbnail(draw, texture, m_adjustChar, boxMin, boxSize, pixelScale);
		draw->AddRect(boxMin, ImVec2(boxMin.x + boxSize.x, boxMin.y + boxSize.y), ImGui::GetColorU32(ImGuiCol_Border));
		ImGui::Dummy(boxSize);
	}

	bool changed = false;
	ImGui::SetNextItemWidth(260.0f);
	ImGui::SliderFloat(L("Scale").c_str(), &a.scale, 0.3f, 3.0f, "%.2f");
	changed |= ImGui::IsItemDeactivatedAfterEdit();
	Tip(L("Ctrl+click to type an exact value."));
	ImGui::SetNextItemWidth(260.0f);
	ImGui::SliderFloat(L("Horizontal").c_str(), &a.dx, -160.0f, 160.0f, "%.1f");
	changed |= ImGui::IsItemDeactivatedAfterEdit();
	ImGui::SetNextItemWidth(260.0f);
	ImGui::SliderFloat(L("Vertical").c_str(), &a.dy, -200.0f, 200.0f, "%.1f");
	changed |= ImGui::IsItemDeactivatedAfterEdit();

	if (ImGui::Button(L("Reset").c_str()))
	{
		a = ThumbnailAdjust();
		changed = true;
	}
	ImGui::SameLine();
	if (ImGui::Button(Messages.Close()))
		ImGui::CloseCurrentPopup();
	ImGui::TextDisabled("%s", L("Saved to BBCF_IM\\ThumbnailAdjust.txt").c_str());

	if (changed)
		SaveThumbAdjust();
	ImGui::EndPopup();
}

// --- Visual pickers ----------------------------------------------------------------------

bool PaletteEditorModal::DrawCell(int charIndex, const std::string& key, const char* paletteData,
	const std::string& label, bool selected, float width, float spriteHeight, bool* doubleClicked)
{
	const float labelHeight = ImGui::GetTextLineHeight() + 4.0f;
	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	const ImVec2 size(width, spriteHeight + labelHeight);
	const ImVec2 cellMax(cursor.x + size.x, cursor.y + size.y);

	const bool clicked = ImGui::InvisibleButton("##cell", size);
	const bool hovered = ImGui::IsItemHovered();
	if (doubleClicked)
		*doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

	// Only what is on screen asks for a texture; the cache is sized for a screenful.
	if (!ImGui::IsRectVisible(cursor, cellMax))
		return clicked;

	ImDrawList* draw = ImGui::GetWindowDrawList();
	if (selected || hovered)
		draw->AddRectFilled(cursor, cellMax, ImGui::GetColorU32(selected ? ImGuiCol_Header : ImGuiCol_HeaderHovered), 4.0f);

	int texWidth = 0, texHeight = 0;
	const ImTextureID texture = paletteData ?
		PaletteThumbnails::Get(charIndex, key, paletteData, &texWidth, &texHeight) : 0;
	if (texture && texWidth > 0 && texHeight > 0)
	{
		const float scale = (std::min)((width - 8.0f) / texWidth, (spriteHeight - 4.0f) / texHeight);
		const float drawW = texWidth * scale;
		const float drawH = texHeight * scale;
		const ImVec2 topLeft(cursor.x + (width - drawW) * 0.5f, cursor.y + (spriteHeight - drawH));
		DrawAdjustedThumbnail(draw, texture, charIndex, topLeft, ImVec2(drawW, drawH), scale);
	}
	else if (paletteData)
	{
		// No sprite in this build: show the colours themselves rather than an empty cell.
		const float swatchHeight = spriteHeight / 8.0f;
		for (int i = 0; i < 8; i++)
		{
			const unsigned char* bytes = (const unsigned char*)paletteData + (1 + i * 12) * 4;
			draw->AddRectFilled(ImVec2(cursor.x + 20.0f, cursor.y + i * swatchHeight),
				ImVec2(cursor.x + width - 20.0f, cursor.y + (i + 1) * swatchHeight),
				IM_COL32(bytes[2], bytes[1], bytes[0], 255));
		}
	}

	// Centred name, clipped to the cell when it is too long; the tooltip has all of it.
	const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
	const float textX = textSize.x < width - 4.0f ? cursor.x + (width - textSize.x) * 0.5f : cursor.x + 2.0f;
	const float textY = cursor.y + spriteHeight + 2.0f;
	draw->PushClipRect(ImVec2(cursor.x + 2.0f, textY), ImVec2(cellMax.x - 2.0f, cellMax.y), true);
	draw->AddText(ImVec2(textX, textY), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
	draw->PopClipRect();

	if (selected)
		draw->AddRect(cursor, cellMax, ImGui::GetColorU32(ImGuiCol_CheckMark), 4.0f, 0, 2.0f);

	return clicked;
}

void PaletteEditorModal::DrawCharacterStep()
{
	DrawThumbnailAdjust();

	ImGui::TextUnformatted(L("Which character is this palette for?").c_str());
	ImGui::TextDisabled("%s", L("Click to choose, double-click to go straight to the next step.").c_str());
	ImGui::Spacing();

	ImGui::BeginChild("##pe_chars", ImVec2(0, 0), ImGuiChildFlags_Borders);
	const float cellWidth = 120.0f;
	CellFlow flow(cellWidth);
	for (int i = 0; i < getCharactersCount(); i++)
	{
		flow.Next();
		ImGui::PushID(i);
		bool doubleClicked = false;
		const std::string name = getCharacterNameByIndexA(i);
		if (DrawCell(i, kTemplateKey, TemplateCharacterFile(i), name, i == m_charIndex, cellWidth, 150.0f, &doubleClicked))
		{
			if (i != m_charIndex)
			{
				m_charIndex = i;
				// A different character's palettes are a different list; start them on
				// Color 01 again so Next is still one click away.
				m_choice = BaseChoice();
				m_choice.valid = true;
				m_baseFilter.Clear();
			}
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", name.c_str());
		if (doubleClicked)
			m_step = Step_Base;
		// Development thumbnail framing tuner, see ThumbnailAdjust.
		if (ThumbAdjustEnabled() && ImGui::IsItemClicked(ImGuiMouseButton_Right))
		{
			// Opened from DrawThumbnailAdjust(), in the ID scope the popup is drawn in.
			m_adjustChar = i;
			m_openAdjustMenu = true;
		}
		ImGui::PopID();
	}
	ImGui::EndChild();
}

void PaletteEditorModal::DrawBaseStep()
{
	ImGui::TextUnformatted(L("What should the palette start from?").c_str());
	ImGui::TextDisabled("%s", L("One of the game's own colors, or a palette you already have. You recolor it in the next step.").c_str());
	ImGui::Spacing();

	if (DrawBaseChooser("##pe_base_grid", ImVec2(0, 0), m_choice, m_baseFilter))
		GoToEditStep();
}

bool PaletteEditorModal::DrawBaseChooser(const char* id, const ImVec2& size, BaseChoice& choice, ImGuiTextFilter& filter)
{
	bool activated = false;
	const float cellWidth = 108.0f;
	const float spriteHeight = 132.0f;

	ImGui::BeginChild(id, size, ImGuiChildFlags_Borders);

	ImGui::SeparatorText(L("Game colors").c_str());
	NativeColour(m_charIndex, 0); // loads the character's colours if they are not yet
	if (!m_nativeError.empty())
		ImGui::TextColoredWrapped(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "%s", m_nativeError.c_str());

	{
		CellFlow flow(cellWidth);
		for (int i = 0; i < NativePalettes::kColorCount; i++)
		{
			const IMPL_data_t* native = NativeColour(m_charIndex, i);
			if (!native)
				continue;
			flow.Next();
			ImGui::PushID(i);
			const bool selected = choice.valid && choice.isNative && choice.nativeIndex == i;
			bool doubleClicked = false;
			if (DrawCell(m_charIndex, NativeKey(i), native->file0, ColorLabel(i), selected, cellWidth, spriteHeight, &doubleClicked))
			{
				choice = BaseChoice();
				choice.valid = true;
				choice.nativeIndex = i;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", (ColorLabel(i) + " - " + L("as it comes with the game")).c_str());
			activated |= doubleClicked;
			ImGui::PopID();
		}
	}

	// Index 0 of the installed list is the "Default" placeholder, not a file: a character
	// with no palettes of their own gets no section at all.
	const std::vector<IMPL_data_t>* installed = InstalledPalettes(m_charIndex);
	if (!installed || installed->size() <= 1)
	{
		ImGui::EndChild();
		return activated;
	}

	ImGui::Spacing();
	ImGui::SeparatorText(L("Your palettes").c_str());
	const std::string filterLabel = L("Search") + "###pe_filter";
	filter.Draw(filterLabel.c_str(), 260.0f);
	Tip(L("Only show your palettes whose name contains this."));

	int shown = 0;
	{
		CellFlow flow(cellWidth);
		// Index 0 is the "Default" placeholder, not a file.
		for (size_t i = 1; i < installed->size(); i++)
		{
			const IMPL_data_t& pal = (*installed)[i];
			if (!filter.PassFilter(pal.palInfo.palName))
				continue;
			shown++;
			flow.Next();
			ImGui::PushID((int)i + 1000);
			const bool selected = choice.valid && !choice.isNative && choice.name == pal.palInfo.palName;
			bool doubleClicked = false;
			// Keyed by name, like the Palettes window, so the two share thumbnails.
			if (DrawCell(m_charIndex, pal.palInfo.palName, pal.file0, pal.palInfo.palName, selected, cellWidth, spriteHeight, &doubleClicked))
			{
				choice = BaseChoice();
				choice.valid = true;
				choice.isNative = false;
				choice.name = pal.palInfo.palName;
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(pal.palInfo.palName);
				if (pal.palInfo.creator[0])
					ImGui::TextDisabled("%s %s", L("by").c_str(), pal.palInfo.creator);
				if (pal.palInfo.desc[0])
					ImGui::TextDisabled("%s", pal.palInfo.desc);
				ImGui::EndTooltip();
			}
			activated |= doubleClicked;
			ImGui::PopID();
		}
	}
	if (shown == 0)
		ImGui::TextDisabled("%s", L("No palettes match the search.").c_str());

	ImGui::EndChild();
	return activated;
}

// --- Step 3: editing ---------------------------------------------------------------------

void PaletteEditorModal::DrawEditStep()
{
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))
		Undo();
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
		ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))
		Redo();

	// Ctrl+C / Ctrl+V copy the open colour and paste it into another. The same keys join and
	// copy lobby links by default, which would be a strange thing to do from in here.
	HotkeyManager::Claim(HotkeyManager::Hotkey_CopyLobbyLink);
	HotkeyManager::Claim(HotkeyManager::Hotkey_JoinLobbyLink);
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, ImGuiInputFlags_RouteGlobal))
		CopyColour();
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, ImGuiInputFlags_RouteGlobal))
		PasteColour();

	ConsumePageDialog();
	PollEyedropper();
	DrawToolbar();
	BuildPendingFile();
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	DrawCanvas(avail.x, avail.y);
}


void PaletteEditorModal::DrawToolbar()
{
	// Which of the palette's eight files is on screen.
	auto fileLabel = [](int file) {
		return file == 0 ? L("Character colors") : FormatText(L("Effect %d").c_str(), file);
	};
	ImGui::SetNextItemWidth(170.0f);
	if (ImGui::BeginCombo("##pe_file", fileLabel(m_file).c_str()))
	{
		for (int f = 0; f < IMPL_PALETTE_FILES_COUNT; f++)
		{
			// A file the game never draws anything with has nothing to edit it on, and
			// nothing it changes: offered, but not selectable.
			const bool unused = f > 0 && EffectSheets::ImageCount(m_charIndex, f) == 0;
			const std::string label = unused ? fileLabel(f) + "  " + L("(unused)") : fileLabel(f);
			if (ImGui::Selectable(label.c_str(), f == m_file, unused ? ImGuiSelectableFlags_Disabled : 0))
			{
				AcceptPick(); // into the file it was opened on, before that changes
				m_file = f;
				m_effectNote.clear();
				m_fitPending = true;
			}
			if (unused)
				Tip(FormatText(L("%s never draws anything with this file, so there is nothing to see it on and nothing it would change.").c_str(),
					getCharacterNameByIndexA(m_charIndex).c_str()));
			if (f == 0)
				ImGui::Separator();
		}
		ImGui::EndCombo();
	}
	Tip(L("A palette is eight sets of 256 colors: the character, and seven the game uses for that character's effects (slashes, projectiles, auras...). Choose which one to edit."));

	VerticalSeparator();

	ImGui::BeginDisabled(m_undo.empty() && !PickChanged());
	if (ImGui::Button(Messages.Undo()))
		Undo();
	ImGui::EndDisabled();
	Tip(L("Take back the last change (Ctrl+Z)."));
	ImGui::SameLine();
	ImGui::BeginDisabled(m_redo.empty() || PickChanged());
	if (ImGui::Button(Messages.Redo()))
		Redo();
	ImGui::EndDisabled();
	Tip(L("Put back what Undo took (Ctrl+Y)."));

	VerticalSeparator();

	// Zooming from the toolbar keeps the middle of the view where it is.
	auto zoomAboutCentre = [this](float factor) {
		const float old = m_zoom;
		m_zoom = ImClamp(m_zoom * factor, 0.05f, kMaxZoom);
		const ImVec2 centre(m_canvasSize.x * 0.5f, m_canvasSize.y * 0.5f);
		m_pan.x = centre.x - (centre.x - m_pan.x) * (m_zoom / old);
		m_pan.y = centre.y - (centre.y - m_pan.y) * (m_zoom / old);
	};

	if (ImGui::Button("-"))
		zoomAboutCentre(1.0f / 1.25f);
	Tip(L("Zoom out. The mouse wheel over the picture zooms too."));
	ImGui::SameLine();
	if (ImGui::Button("+"))
		zoomAboutCentre(1.25f);
	Tip(L("Zoom in. The mouse wheel over the picture zooms too."));
	ImGui::SameLine();
	if (ImGui::Button(L("Fit").c_str()))
		m_fitPending = true;
	Tip(L("Zoom so the whole sheet fits in the view."));
	ImGui::SameLine();
	if (ImGui::Button("1:1"))
		zoomAboutCentre(1.0f / m_zoom);
	Tip(L("Actual size: one sheet pixel per screen pixel."));
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::Text("%d%%", (int)(m_zoom * 100.0f + 0.5f));

	VerticalSeparator();

	if (ImGui::Button(L("Apply palette...").c_str()))
	{
		AcceptPick();
		m_applyChoice = m_loadedChoice;
		m_applyFilter.Clear();
		m_requestApply = true;
	}
	Tip(L("Replace every color with another palette's - one of the game's or one of yours. Undo brings your colors back."));
	ImGui::SameLine();
	if (ImGui::Button(L("Fade out settings...").c_str()))
		m_requestFadeSettings = true;
	Tip(L("How the colors you are not editing are faded out while a color is open."));

	VerticalSeparator();
	const bool dialogBusy = NativeFileDialog::IsOpen();
	ImGui::BeginDisabled(dialogBusy);
	if (ImGui::Button(L("Export page...").c_str()))
	{
		AcceptPick();
		BeginExportPage();
	}
	Tip(L("Save the colors on screen - the character colors, or this effect file - as a PNG. Import it on any palette's page to copy these exact colors over."));
	ImGui::SameLine();
	if (ImGui::Button(L("Import page...").c_str()))
	{
		AcceptPick();
		NativeFileDialog::Request request;
		request.title = "Import page";
		request.filters.push_back({ "PNG palette (*.png)", "*.png" });
		request.contextId = kDialogImportPage;
		NativeFileDialog::Open(kFileDialogOwner, request);
	}
	Tip(L("Replace the colors on screen with a PNG's - a page exported from here, or any palette PNG. Undo brings yours back."));
	ImGui::EndDisabled();

	VerticalSeparator();
	if (LiveActive())
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.39f, 1.0f), "%s", L("Live in training").c_str());
		Tip(L("The character in your training match is wearing this palette right now, open color included. Closing the editor puts their palette back, unless you saved.") + "\n\n" +
			FormatText(L("Press %s to hide the overlay and look at them; press it again and the editor is back where you left it.").c_str(),
				Settings::settingsIni.togglebutton.c_str()));
	}
	else
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", L("Live preview off").c_str());
		Tip(L("Open the editor during a training match with this character and they wear the palette live while you edit - effects included."));
	}
}

void PaletteEditorModal::DrawCanvas(float width, float height)
{
	ImGui::BeginChild("##pe_canvas", ImVec2(width, height), ImGuiChildFlags_Borders,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
	m_entryGridOnScreen = false; // DrawEntryGrid sets it, before the picker is drawn

	// What to draw: the reference sheet for the character colours; for an effect file,
	// the effect sprites the game draws with it, read from the game files. The colour grid
	// fills the canvas instead when there is no picture for a file.
	const unsigned char* indices = nullptr;
	int sheetWidth = 0, sheetHeight = 0;
	int imageKey = m_charIndex;
	bool haveSheet = false;
	if (m_file == 0)
	{
		haveSheet = PaletteThumbnails::GetEditorSheetIndices(m_charIndex, &indices, &sheetWidth, &sheetHeight);
	}
	else if (EffectSheets::ImageCount(m_charIndex, m_file) > 0)
	{
		std::string error;
		const EffectSheets::Status status = EffectSheets::Request(m_charIndex, &error);
		if (status == EffectSheets::Status_Ready)
		{
			haveSheet = EffectSheets::GetSheet(m_charIndex, m_file, &indices, &sheetWidth, &sheetHeight);
			imageKey = 1000 + m_charIndex * 8 + m_file;
		}
		else if (status == EffectSheets::Status_Loading)
		{
			const ImVec2 avail = ImGui::GetContentRegionAvail();
			const std::string text = L("Reading the effect sprites from the game files...");
			const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
			ImGui::SetCursorPos(ImVec2((avail.x - textSize.x) * 0.5f, avail.y * 0.45f));
			ImGui::TextDisabled("%s", text.c_str());
			ImGui::EndChild();
			return;
		}
		else if (status == EffectSheets::Status_Failed)
		{
			m_effectNote = error;
		}
	}

	if (m_file != 0 && !haveSheet)
	{
		DrawEffectGrid();
		ImGui::EndChild();
		return;
	}

	// Pixel counts on the tooltip and picker follow whichever picture is up.
	if (haveSheet && imageKey != m_usageKey)
	{
		memset(m_usage, 0, sizeof(m_usage));
		const size_t count = (size_t)sheetWidth * sheetHeight;
		for (size_t i = 0; i < count; i++)
			m_usage[indices[i]]++;
		m_usageKey = imageKey;
		m_fitPending = true;
	}

	// The open pick is shown live, but lives outside m_palette until accepted.
	const char* preview = m_pendingFile;

	// Fading is only for while a colour is open: it is what shows what that colour (and
	// the ramp it is part of) covers.
	unsigned char mask[256] = {};
	if (m_selected >= 0)
	{
		mask[m_selected] = 1;
		for (int i = 0; i < 256; i++)
			if (m_extra[i])
				mask[i] = 1;
		if (m_rampOn)
			for (int i = RampLow(); i <= RampHigh(); i++)
				mask[i] = 1;
	}
	ImTextureID texture = 0;
	if (haveSheet && m_file == 0)
	{
		texture = PaletteThumbnails::GetEditorImage(imageKey, indices, sheetWidth, sheetHeight, preview,
			m_selected >= 0 ? mask : nullptr, g_fade);
	}
	else if (haveSheet)
	{
		// Effects are drawn the way the game draws them - additive, subtractive, colour
		// recipes - so they are composited over the canvas colour rather than coloured
		// one index at a time.
		const ImU32 bg = ImGui::ColorConvertFloat4ToU32(g_canvasBackground); // ABGR
		const unsigned int background = ((bg & 0xFF) << 16) | (bg & 0xFF00) | ((bg >> 16) & 0xFF);
		bool changed = false;
		const unsigned int* pixels = EffectSheets::Render(preview, m_selected >= 0 ? mask : nullptr, g_fade,
			background, &changed);
		texture = PaletteThumbnails::GetEditorImageRGBA(imageKey, pixels, sheetWidth, sheetHeight, changed);
	}

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 size = ImGui::GetContentRegionAvail();
	m_canvasSize = size;

	if (!texture || sheetWidth <= 0 || sheetHeight <= 0 || size.x < 1.0f || size.y < 1.0f)
	{
		ImGui::TextWrapped("%s", Messages.Palette_no_preview());
		ImGui::EndChild();
		return;
	}

	const float fitZoom = (std::min)(size.x / sheetWidth, size.y / sheetHeight);
	if (m_fitPending)
	{
		m_fitPending = false;
		m_zoom = fitZoom;
		m_pan = ImVec2((size.x - sheetWidth * m_zoom) * 0.5f, (size.y - sheetHeight * m_zoom) * 0.5f);
	}

	ImGui::InvisibleButton("##pe_canvas_input", size,
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();
	ImGuiIO& io = ImGui::GetIO();

	if (hovered)
	{
		// Keep the wheel for zooming rather than letting it scroll the modal behind.
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
		if (io.MouseWheel != 0.0f)
		{
			const float old = m_zoom;
			m_zoom = ImClamp(m_zoom * powf(1.25f, io.MouseWheel), fitZoom * 0.5f, kMaxZoom);
			// Zoom about the cursor: the image point under it stays under it.
			const ImVec2 mouse(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
			m_pan.x = mouse.x - (mouse.x - m_pan.x) * (m_zoom / old);
			m_pan.y = mouse.y - (mouse.y - m_pan.y) * (m_zoom / old);
		}
	}

	// A left press is a click unless it moves past the drag threshold, at which point it
	// becomes a pan and selects nothing. Right and middle always pan.
	if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		m_leftPressed = true;
		m_dragged = false;
	}
	if (active)
	{
		const bool leftDrag = m_leftPressed && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
		if (leftDrag)
			m_dragged = true;
		if (leftDrag || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) ||
			ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
		{
			m_pan.x += io.MouseDelta.x;
			m_pan.y += io.MouseDelta.y;
		}
	}
	bool clickReleased = false;
	if (m_leftPressed && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		m_leftPressed = false;
		clickReleased = !m_dragged;
	}
	const bool panning = active && (m_dragged || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
		ImGui::IsMouseDown(ImGuiMouseButton_Middle));

	// Never let the picture be dragged entirely out of view.
	const float imageW = sheetWidth * m_zoom;
	const float imageH = sheetHeight * m_zoom;
	const float keep = 40.0f;
	m_pan.x = ImClamp(m_pan.x, keep - imageW, size.x - keep);
	m_pan.y = ImClamp(m_pan.y, keep - imageH, size.y - keep);

	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 canvasMax(origin.x + size.x, origin.y + size.y);
	draw->PushClipRect(origin, canvasMax, true);
	draw->AddRectFilled(origin, canvasMax, ImGui::GetColorU32(g_canvasBackground));

	const ImVec2 p0(origin.x + m_pan.x, origin.y + m_pan.y);
	const ImVec2 p1(p0.x + imageW, p0.y + imageH);
	// Hard pixels once zoomed in, which is when you are aiming at one.
	const bool pointSample = m_zoom >= 1.5f;
	if (pointSample)
		PaletteThumbnails::BeginPointSampling(draw);
	draw->AddImage(ImTextureRef(texture), p0, p1);
	if (pointSample)
		PaletteThumbnails::EndPointSampling(draw);

	// The palette entry under the mouse: -1 off the picture, 0 on its transparent background.
	int underMouse = -1;
	int px = 0, py = 0;
	const bool mouseInCanvas = io.MousePos.x >= origin.x && io.MousePos.y >= origin.y &&
		io.MousePos.x < canvasMax.x && io.MousePos.y < canvasMax.y;
	if (mouseInCanvas)
	{
		px = (int)floorf((io.MousePos.x - p0.x) / m_zoom);
		py = (int)floorf((io.MousePos.y - p0.y) / m_zoom);
		if (px >= 0 && py >= 0 && px < sheetWidth && py < sheetHeight)
			underMouse = indices[(size_t)py * sheetWidth + px];
	}

	// Always-there help, top left of the canvas, deliberately quiet.
	const ImVec2 helpSize = ImGui::CalcTextSize("(?)");
	const ImVec2 helpPos(origin.x + 6.0f, origin.y + 4.0f);
	const bool helpHovered = ImGui::IsWindowHovered() &&
		io.MousePos.x >= helpPos.x - 3.0f && io.MousePos.y >= helpPos.y - 3.0f &&
		io.MousePos.x < helpPos.x + helpSize.x + 3.0f && io.MousePos.y < helpPos.y + helpSize.y + 3.0f;
	DrawCanvasHelp(origin);
	const bool toggleHovered = DrawGridToggle(origin, canvasMax, clickReleased);

	if (!helpHovered && !toggleHovered && hovered && !panning && underMouse > 0)
	{
		if (m_zoom >= 4.0f)
		{
			const ImVec2 c0(p0.x + px * m_zoom, p0.y + py * m_zoom);
			draw->AddRect(c0, ImVec2(c0.x + m_zoom, c0.y + m_zoom), IM_COL32(255, 255, 255, 220));
		}
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

		ImGui::BeginTooltip();
		const unsigned char* entry = (const unsigned char*)preview + underMouse * 4;
		ImGui::ColorButton("##pe_tip", EntryColourVec(entry),
			ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(28, 28));
		ImGui::SameLine();
		ImGui::Text("#%03d\n%02X%02X%02X", underMouse + 1, entry[2], entry[1], entry[0]);
		ImGui::TextDisabled("%s", L("Click to edit this color").c_str());
		ImGui::EndTooltip();
	}
	if (panning)
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

	if (clickReleased && !helpHovered && !toggleHovered && mouseInCanvas)
	{
		if (underMouse > 0)
			ClickEntry(underMouse);
		// Clicking the background or off the picture is "done with this one" - unless it
		// was a modifier click that just missed.
		else if (!ShiftHeld() && !CtrlHeld() && !AltHeld())
			AcceptPick();
	}

	draw->PopClipRect();

	DrawSidePanel(origin, canvasMax);

	ImGui::EndChild();
}

bool PaletteEditorModal::DrawGridToggle(const ImVec2& canvasMin, const ImVec2& canvasMax, bool clicked)
{
	// Quiet, like the (?) opposite it: text on the canvas, not a button.
	const std::string label = g_showColourGrid ? L("Hide color grid") : L("Show color grid");
	const ImVec2 size = ImGui::CalcTextSize(label.c_str());
	const ImVec2 pos(canvasMax.x - size.x - 10.0f, canvasMin.y + 4.0f);
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool hovered = ImGui::IsWindowHovered() &&
		mouse.x >= pos.x - 3.0f && mouse.y >= pos.y - 3.0f &&
		mouse.x < pos.x + size.x + 3.0f && mouse.y < pos.y + size.y + 3.0f;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImU32 colour = ImGui::GetColorU32(ImGuiCol_TextDisabled, hovered ? 1.0f : 0.7f);
	draw->AddText(pos, colour, label.c_str());
	if (hovered)
	{
		draw->AddLine(ImVec2(pos.x, pos.y + size.y), ImVec2(pos.x + size.x, pos.y + size.y), colour);
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		ImGui::SetTooltip("%s", L("All 256 colors of this file as a grid, next to the picture. Some colors are on no sprite and can only be reached from here.").c_str());
		if (clicked)
		{
			g_showColourGrid = !g_showColourGrid;
			ImGui::MarkIniSettingsDirty();
		}
	}
	return hovered;
}

void PaletteEditorModal::DrawSidePanel(const ImVec2& canvasMin, const ImVec2& canvasMax)
{
	// Panels over the canvas' top-right corner rather than windows: they belong to the
	// picture, and a window could be dragged off somewhere it hides what it is editing.
	// Both start under the grid toggle.
	const float margin = 10.0f;
	float top = canvasMin.y + ImGui::GetTextLineHeight() + 10.0f;
	if (g_showColourGrid)
	{
		const float width = (std::min)(kPickerWidth, canvasMax.x - canvasMin.x - margin * 2.0f);
		ImGui::SetCursorScreenPos(ImVec2(canvasMax.x - width - margin, top));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
		ImGui::BeginChild("##pe_grid_panel", ImVec2(width, 0),
			ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleColor();
		DrawEntryGrid("##pe_side_grid", floorf(ImGui::GetContentRegionAvail().x / 16.0f), 1.0f);
		ImGui::EndChild();
		top = ImGui::GetItemRectMax().y + 6.0f;
	}
	DrawPickerPanel(canvasMin, canvasMax, top);
}

bool PaletteEditorModal::DrawEntryGrid(const char* id, float cell, float gap)
{
	// While it is on screen, the Range / gradient tab uses this grid rather than its own.
	m_entryGridOnScreen = true;
	const ImVec2 gridOrigin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(id, ImVec2(cell * 16.0f, cell * 16.0f));
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	int underMouse = -1;
	if (hovered || active)
	{
		// Clamped while held, so a drag that leaves the grid keeps the nearest cell.
		const int cx = (int)floorf((mouse.x - gridOrigin.x) / cell);
		const int cy = (int)floorf((mouse.y - gridOrigin.y) / cell);
		if (active)
			underMouse = ImClamp(cy, 0, 15) * 16 + ImClamp(cx, 0, 15);
		else if (cx >= 0 && cy >= 0 && cx < 16 && cy < 16)
			underMouse = cy * 16 + cx;
	}

	// A click opens a colour. With a range being edited, dragging from one cell to another
	// chooses the range instead, and Shift+click moves its end.
	const bool rangeMode = m_rampOn && m_selected >= 0;
	if (ImGui::IsItemActivated() && underMouse >= 0)
	{
		m_gridPressed = underMouse;
		m_gridDragged = false;
	}
	if (active && rangeMode && m_gridPressed >= 0 && (m_gridDragged || underMouse != m_gridPressed))
	{
		m_gridDragged = true;
		const int low = RampLow(), high = RampHigh();
		// Entry 0 is the transparent slot of every file, never part of a range.
		m_rampStart = (std::max)(1, m_gridPressed);
		m_rampEnd = (std::max)(1, underMouse);
		if ((RampLow() != low || RampHigh() != high) && !m_rampEndsCustom)
			ResetRampEnds();
	}
	if (ImGui::IsItemDeactivated())
	{
		if (!m_gridDragged && m_gridPressed >= 0 && hovered)
		{
			ClickEntry(m_gridPressed);
		}
		m_gridPressed = -1;
		m_gridDragged = false;
	}

	ImDrawList* draw = ImGui::GetWindowDrawList();
	const float rounding = gap > 1.0f ? 2.0f : 0.0f;
	const unsigned char* shown = (const unsigned char*)m_pendingFile;
	for (int i = 0; i < 256; i++)
	{
		const ImVec2 c0(gridOrigin.x + (i % 16) * cell, gridOrigin.y + (i / 16) * cell);
		const ImVec2 c1(c0.x + cell - gap, c0.y + cell - gap);
		const unsigned char* e = shown + i * 4;
		draw->AddRectFilled(c0, c1, IM_COL32(e[2], e[1], e[0], 255), rounding);
		// Outside the range is dimmed, so the run reads at a glance.
		if (rangeMode && (i < RampLow() || i > RampHigh()))
			draw->AddRectFilled(c0, c1, IM_COL32(20, 20, 24, 150), rounding);
	}
	auto outline = [&](int i, ImU32 colour, float thickness) {
		const ImVec2 c0(gridOrigin.x + (i % 16) * cell - 1.0f, gridOrigin.y + (i / 16) * cell - 1.0f);
		draw->AddRect(c0, ImVec2(c0.x + cell, c0.y + cell), colour, rounding, 0, thickness);
	};
	if (m_selected >= 0 && m_rampOn)
		for (int i = RampLow(); i <= RampHigh(); i++)
			outline(i, ImGui::GetColorU32(ImGuiCol_CheckMark), 1.5f);
	if (underMouse >= 0)
		outline(underMouse, IM_COL32(200, 200, 200, 255), 1.5f);
	if (m_selected >= 0)
	{
		for (int i = 0; i < 256; i++)
			if (m_extra[i])
				outline(i, IM_COL32(255, 255, 255, 200), 1.5f);
		outline(m_selected, IM_COL32(255, 255, 255, 255), 2.5f);
	}

	if (underMouse >= 0)
	{
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		const unsigned char* e = shown + underMouse * 4;
		ImGui::SetTooltip("#%03d  %02X%02X%02X\n%s", underMouse + 1, e[2], e[1], e[0], L("Click to edit this color").c_str());
	}
	return hovered;
}

void PaletteEditorModal::DrawPickerPanel(const ImVec2& canvasMin, const ImVec2& canvasMax, float top)
{
	if (m_selected < 0)
		return;

	// It grows with its contents but never past the canvas; beyond that it scrolls.
	const float margin = 10.0f;
	const float width = (std::min)(kPickerWidth, canvasMax.x - canvasMin.x - margin * 2.0f);
	const float maxHeight = (std::max)(120.0f, canvasMax.y - top - margin);
	ImGui::SetCursorScreenPos(ImVec2(canvasMax.x - width - margin, top));
	ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, maxHeight));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
	ImGui::BeginChild("##pe_picker", ImVec2(width, 0),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleColor();

	ImGui::Text("#%03d", m_selected + 1);
	ImGui::SameLine();
	if (m_extraCount > 0)
	{
		ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s",
			FormatText(L("+%d more").c_str(), m_extraCount).c_str());
		Tip(L("Colors added with Ctrl+click. They keep their own color until you change the picker, then all take the picker's. Ctrl+click one again to take it back out."));
		ImGui::SameLine();
	}
	if (m_file != 0)
		ImGui::TextDisabled("%s", FormatText(L("Effect %d").c_str(), m_file).c_str());
	else if (m_usage[m_selected] > 0)
		ImGui::TextDisabled("%s", FormatText(L("%d pixels on the sheet").c_str(), m_usage[m_selected]).c_str());
	else
		ImGui::TextDisabled("%s", L("Not shown on the sheet").c_str());

	// Two ways to change things: this one entry, or a run of them at once.
	bool rangeTab = false;
	if (ImGui::BeginTabBar("##pe_pick_tabs"))
	{
		if (ImGui::BeginTabItem(L("One color").c_str(), nullptr,
			m_forceSingleTab ? ImGuiTabItemFlags_SetSelected : 0))
		{
			DrawSingleColour();
			ImGui::EndTabItem();
		}
		Tip(L("Change just the color you clicked."));
		if (ImGui::BeginTabItem(L("Range / gradient").c_str()))
		{
			rangeTab = true;
			DrawShadeRamp();
			ImGui::EndTabItem();
		}
		Tip(L("Change a whole run of neighboring colors at once: recolor it keeping its shading, or fill it with a gradient between two colors."));
		ImGui::EndTabBar();
	}
	m_forceSingleTab = false;
	m_rampOn = rangeTab;

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Checkbox(L("Edit transparency").c_str(), &m_editAlpha);
	Tip(L("Show an alpha slider too. Most palettes never touch it; the game draws some effects with it."));

	// No Accept: clicking another color, empty space or anything else keeps the change.
	ImGui::Spacing();
	if (ImGui::Button(Messages.Cancel(), ImVec2(-FLT_MIN, 0)))
		CancelPick();
	Tip(L("Throw this edit away, as if you never touched it (Escape). Anything else you do next keeps it."));

	// Enter and Escape, but not while typing into the picker's own fields, which use them.
	if (!ImGui::IsAnyItemActive())
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
			AcceptPick();
		else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			CancelPick();
	}

	ImGui::EndChild();
}

ImGuiColorEditFlags PaletteEditorModal::PickerFlags() const
{
	ImGuiColorEditFlags flags = ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_DisplayRGB |
		ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_NoSidePreview;
	flags |= m_editAlpha ? (ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)
		: ImGuiColorEditFlags_NoAlpha;
	return flags;
}

void PaletteEditorModal::DrawSingleColour()
{
	EyedropperButton("##pe_eyedrop_single", m_pending, false);
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	ImGui::ColorPicker4On32Bit("##pe_picker_ctl", m_pending, PickerFlags(), nullptr);

	// Current / Modified / Original, side by side. Current and Original are also buttons,
	// for "go back to that".
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float swatchWidth = (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f;
	const ImGuiColorEditFlags swatchFlags = ImGuiColorEditFlags_NoTooltip |
		(m_editAlpha ? ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha);

	struct Swatch
	{
		const char* id;
		std::string label;
		const unsigned char* bytes;
		std::string tip;
		bool clickable;
	};
	const Swatch swatches[3] = {
		{ "##pe_current", L("Current"), m_current,
			L("The color as it is now, before this edit. Click to go back to it."), true },
		{ "##pe_modified", L("Modified"), m_pending,
			L("What the color becomes. It is kept as soon as you move on to anything else."), false },
		{ "##pe_original", L("Original"), BaseEntry(m_selected),
			L("The color in the palette you started from, or last applied. Click to use it."), true },
	};
	for (int i = 0; i < 3; i++)
	{
		if (i > 0)
			ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::TextDisabled("%s", swatches[i].label.c_str());
		if (ImGui::ColorButton(swatches[i].id, EntryColourVec(swatches[i].bytes), swatchFlags, ImVec2(swatchWidth, 28.0f)) &&
			swatches[i].clickable)
		{
			memcpy(m_pending, swatches[i].bytes, 4);
		}
		ImGui::EndGroup();
		Tip(swatches[i].tip);
	}
}


// --- Shade ramp ----------------------------------------------------------------------------

namespace
{
	void ToHsv(const unsigned char* bgra, float& h, float& sat, float& v)
	{
		ImGui::ColorConvertRGBtoHSV(bgra[2] / 255.0f, bgra[1] / 255.0f, bgra[0] / 255.0f, h, sat, v);
	}

	void FromHsv(float h, float sat, float v, unsigned char* bgra)
	{
		float r, g, b;
		ImGui::ColorConvertHSVtoRGB(h, ImClamp(sat, 0.0f, 1.0f), ImClamp(v, 0.0f, 1.0f), r, g, b);
		bgra[2] = (unsigned char)(r * 255.0f + 0.5f);
		bgra[1] = (unsigned char)(g * 255.0f + 0.5f);
		bgra[0] = (unsigned char)(b * 255.0f + 0.5f);
	}

	// Would these two neighbouring entries be steps of the same shading ramp? Same hue
	// (greys count as one family) and no big jump in brightness.
	bool SameRamp(const unsigned char* a, const unsigned char* b)
	{
		float ha, sa, va, hb, sb, vb;
		ToHsv(a, ha, sa, va);
		ToHsv(b, hb, sb, vb);
		if (fabsf(va - vb) > 0.35f)
			return false;
		const bool greyA = sa < 0.15f || va < 0.08f;
		const bool greyB = sb < 0.15f || vb < 0.08f;
		if (greyA || greyB)
			return greyA && greyB;
		float dh = fabsf(ha - hb);
		if (dh > 0.5f)
			dh = 1.0f - dh;
		return dh < 0.085f;
	}

	// Scales a channel so `reference` lands exactly on `target`, which keeps the steps of
	// a ramp in proportion. Near zero a ratio means nothing, so it shifts instead.
	float Rescale(float value, float reference, float target)
	{
		if (reference > 0.05f)
			return value * (target / reference);
		return value + (target - reference);
	}
}

void PaletteEditorModal::AutoDetectRamp()
{
	if (m_selected < 0)
		return;
	const unsigned char* file = (const unsigned char*)FileData(m_palette, m_file);
	int low = m_selected;
	int high = m_selected;
	// Entry 0 is the transparent slot of every file, never part of a ramp.
	while (low - 1 >= 1 && m_selected - (low - 1) <= kRampReach && SameRamp(file + (low - 1) * 4, file + low * 4))
		low--;
	while (high + 1 <= 255 && (high + 1) - m_selected <= kRampReach && SameRamp(file + (high + 1) * 4, file + high * 4))
		high++;
	m_rampStart = low;
	m_rampEnd = high;
	ResetRampEnds();
}

void PaletteEditorModal::ResetRampEnds()
{
	memcpy(m_rampFrom, Entry(RampLow()), 4);
	memcpy(m_rampTo, Entry(RampHigh()), 4);
	m_rampEndsCustom = false;
}


void PaletteEditorModal::ApplyRamp(char* fileData) const
{
	unsigned char* file = (unsigned char*)fileData;
	const unsigned char* committed = (const unsigned char*)FileData(m_palette, m_file);
	const int low = RampLow();
	const int high = RampHigh();
	if (low < 0 || high > 255)
		return;

	if (m_rampMode == 1)
	{
		// Even blend from one end to the other: the old editor's gradient generator.
		for (int i = low; i <= high; i++)
		{
			const float t = high == low ? 0.0f : (float)(i - low) / (float)(high - low);
			unsigned char* out = file + i * 4;
			for (int c = 0; c < 3; c++)
				out[c] = (unsigned char)(m_rampFrom[c] + (m_rampTo[c] - m_rampFrom[c]) * t + 0.5f);
			if (m_editAlpha)
				out[3] = (unsigned char)(m_rampFrom[3] + (m_rampTo[3] - m_rampFrom[3]) * t + 0.5f);
		}
		return;
	}

	// Recolour, keeping shading: every step takes the picked hue, and has its saturation
	// and brightness moved by as much as the open colour's were. The open colour lands
	// exactly on what was picked; the rest keep their light and shadow relative to it.
	float rh, rs, rv, th, ts, tv;
	ToHsv(m_current, rh, rs, rv);
	ToHsv(m_pending, th, ts, tv);
	for (int i = low; i <= high; i++)
	{
		float h, sat, v;
		ToHsv(committed + i * 4, h, sat, v);
		unsigned char* out = file + i * 4;
		FromHsv(th, Rescale(sat, rs, ts), Rescale(v, rv, tv), out);
		out[3] = m_editAlpha ? m_pending[3] : committed[i * 4 + 3];
	}
}

void PaletteEditorModal::BuildPendingFile()
{
	memcpy(m_pendingFile, FileData(m_palette, m_file), IMPL_PALETTE_DATALEN);
	if (m_selected < 0)
		return;
	memcpy(m_pendingFile + m_selected * 4, m_pending, 4);
	// The Ctrl+clicked colours follow the picker once it has moved off the colour it opened on.
	if (m_extraCount > 0 && memcmp(m_pending, m_current, 4) != 0)
	{
		for (int i = 0; i < 256; i++)
			if (m_extra[i])
				memcpy(m_pendingFile + i * 4, m_pending, 4);
	}
	if (m_rampOn)
		ApplyRamp(m_pendingFile);
}

void PaletteEditorModal::DrawShadeRamp()
{
	const int before[2] = { RampLow(), RampHigh() };

	// --- 1. Which colours ----------------------------------------------------------------
	ImGui::SeparatorText(L("1. Choose the range").c_str());

	// The color grid on screen already does this (DrawEntryGrid); two grids is one too many.
	ImDrawList* draw = ImGui::GetWindowDrawList();
	if (!m_entryGridOnScreen)
	{
		// All 256 entries of this file, in order, so a range can reach anywhere. Drag across
		// it to select; the run is outlined and the colour you clicked is marked with a dot.
		const int columns = 16;
		const float cell = floorf(ImGui::GetContentRegionAvail().x / columns);
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##pe_range_grid", ImVec2(cell * columns, cell * columns));
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const int cx = ImClamp((int)((mouse.x - origin.x) / cell), 0, columns - 1);
		const int cy = ImClamp((int)((mouse.y - origin.y) / cell), 0, columns - 1);
		// Entry 0 is the transparent slot of every file, never part of a range.
		const int underMouse = (std::max)(1, cy * columns + cx);
		if (ImGui::IsItemActivated())
		{
			m_rampStart = underMouse;
			m_rampEnd = underMouse;
		}
		else if (ImGui::IsItemActive())
		{
			m_rampEnd = underMouse;
		}
		if (hovered)
		{
			const unsigned char* e = (const unsigned char*)m_pendingFile + underMouse * 4;
			ImGui::SetTooltip("#%03d  %02X%02X%02X", underMouse + 1, e[2], e[1], e[0]);
		}

		const unsigned char* shown = (const unsigned char*)m_pendingFile;
		const int low = RampLow(), high = RampHigh();
		for (int i = 0; i < 256; i++)
		{
			const ImVec2 c0(origin.x + (i % columns) * cell, origin.y + (i / columns) * cell);
			const ImVec2 c1(c0.x + cell - 1.0f, c0.y + cell - 1.0f);
			const unsigned char* e = shown + i * 4;
			const bool inRange = i >= low && i <= high;
			draw->AddRectFilled(c0, c1, IM_COL32(e[2], e[1], e[0], 255));
			// Outside the range is dimmed, so the run reads at a glance.
			if (!inRange)
				draw->AddRectFilled(c0, c1, IM_COL32(20, 20, 24, 150));
			if (i == m_selected)
				draw->AddCircleFilled(ImVec2((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f), cell * 0.18f + 1.0f,
					IM_COL32(255, 255, 255, 255));
		}
		// Outline the range, row by row, since it can wrap across several.
		for (int row = low / columns; row <= high / columns; row++)
		{
			const int a = (std::max)(low, row * columns);
			const int b = (std::min)(high, row * columns + columns - 1);
			const ImVec2 r0(origin.x + (a % columns) * cell - 1.0f, origin.y + row * cell - 1.0f);
			const ImVec2 r1(origin.x + (b % columns + 1) * cell, origin.y + (row + 1) * cell);
			draw->AddRect(r0, r1, ImGui::GetColorU32(ImGuiCol_CheckMark), 0.0f, 0, 2.0f);
		}
	}

	// Exact numbers, for when the run is known.
	const int low = RampLow(), high = RampHigh();
	int first = low + 1, last = high + 1;
	const float fieldWidth = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(L("From").c_str()).x -
		ImGui::CalcTextSize(L("to").c_str()).x - ImGui::GetStyle().ItemSpacing.x * 4.0f) * 0.5f;
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(L("From").c_str());
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fieldWidth);
	if (ImGui::InputInt("##pe_range_from", &first, 1, 8))
		m_rampStart = ImClamp(first, 2, 256) - 1, m_rampEnd = high;
	Tip(L("First entry of the range."));
	ImGui::SameLine();
	ImGui::TextUnformatted(L("to").c_str());
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fieldWidth);
	if (ImGui::InputInt("##pe_range_to", &last, 1, 8))
		m_rampStart = low, m_rampEnd = ImClamp(last, 2, 256) - 1;
	Tip(L("Last entry of the range."));

	if (ImGui::Button(L("Detect the run this color is in").c_str(), ImVec2(-FLT_MIN, 0)))
		AutoDetectRamp();
	Tip(L("Guess the range from the neighbors that look like lighter and darker steps of the color you clicked."));
	ImGui::TextDisabledWrapped("%s", L("Drag across the grid to choose, or Shift+click the character to move the end of the range.").c_str());

	// --- 2. How --------------------------------------------------------------------------
	ImGui::SeparatorText(L("2. Choose how").c_str());
	if (ImGui::RadioButton(L("Recolor, keep the shading").c_str(), m_rampMode == 0))
		m_rampMode = 0;
	Tip(L("Every color in the range takes the new color's hue, and keeps how much lighter or darker it was than the one you clicked. The usual way to recolor a whole material."));
	if (ImGui::RadioButton(L("Gradient between two colors").c_str(), m_rampMode == 1))
		m_rampMode = 1;
	Tip(L("Fill the range with an even blend from a start color to an end color - what the old editor's gradient generator did."));

	// The ends follow the range until you pick them yourself.
	if ((RampLow() != before[0] || RampHigh() != before[1]) && !m_rampEndsCustom)
		ResetRampEnds();

	// --- 3. Colours ----------------------------------------------------------------------
	ImGui::SeparatorText(m_rampMode == 0 ? L("3. Choose the new color").c_str() : L("3. Choose the two colors").c_str());
	if (m_rampMode == 0)
	{
		ImGui::TextDisabledWrapped("%s", FormatText(L("The color for #%03d. The rest of the range follows it.").c_str(), m_selected + 1).c_str());
		EyedropperButton("##pe_eyedrop_ramp", m_pending, false);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::ColorPicker4On32Bit("##pe_ramp_target", m_pending, PickerFlags(), nullptr);
		return;
	}

	// Two big swatches: click one, and the picker under them edits it.
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float swatchWidth = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
	for (int end = 0; end < 2; end++)
	{
		if (end > 0)
			ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::TextDisabled("%s", FormatText(end == 0 ? L("Start (#%03d)").c_str() : L("End (#%03d)").c_str(),
			(end == 0 ? RampLow() : RampHigh()) + 1).c_str());
		const unsigned char* bytes = end == 0 ? m_rampFrom : m_rampTo;
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		if (ImGui::ColorButton(end == 0 ? "##pe_ramp_start" : "##pe_ramp_end", EntryColourVec(bytes),
			ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(swatchWidth, 34.0f)))
		{
			m_rampEditEnd = end;
		}
		if (m_rampEditEnd == end)
			draw->AddRect(ImVec2(pos.x - 2.0f, pos.y - 2.0f), ImVec2(pos.x + swatchWidth + 2.0f, pos.y + 36.0f),
				ImGui::GetColorU32(ImGuiCol_CheckMark), 3.0f, 0, 2.0f);
		ImGui::EndGroup();
		Tip(end == 0 ? L("The color the gradient starts from. Click to edit it with the picker below.")
			: L("The color the gradient ends on. Click to edit it with the picker below."));
	}

	unsigned char* editing = m_rampEditEnd == 0 ? m_rampFrom : m_rampTo;
	EyedropperButton("##pe_eyedrop_end", editing, true);
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	if (ImGui::ColorPicker4On32Bit("##pe_ramp_end_picker", editing, PickerFlags(), nullptr))
		m_rampEndsCustom = true;

	if (ImGui::Button(L("Swap").c_str(), ImVec2(swatchWidth, 0)))
	{
		unsigned char tmp[4];
		memcpy(tmp, m_rampFrom, 4);
		memcpy(m_rampFrom, m_rampTo, 4);
		memcpy(m_rampTo, tmp, 4);
		m_rampEndsCustom = true;
	}
	Tip(L("Swap the start and end colors."));
	ImGui::SameLine();
	if (ImGui::Button(L("Reset ends").c_str(), ImVec2(swatchWidth, 0)))
		ResetRampEnds();
	Tip(L("Set the start and end back to the colors currently at the two ends of the range."));
}


// --- Effect files ------------------------------------------------------------------------

void PaletteEditorModal::DrawCanvasHelp(const ImVec2& origin)
{
	const char* label = "(?)";
	const ImVec2 pos(origin.x + 6.0f, origin.y + 4.0f);
	const ImVec2 size = ImGui::CalcTextSize(label);
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool hovered = ImGui::IsWindowHovered() &&
		mouse.x >= pos.x - 3.0f && mouse.y >= pos.y - 3.0f &&
		mouse.x < pos.x + size.x + 3.0f && mouse.y < pos.y + size.y + 3.0f;
	ImGui::GetWindowDrawList()->AddText(pos, ImGui::GetColorU32(ImGuiCol_TextDisabled, hovered ? 1.0f : 0.55f), label);

	if (hovered)
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
		ImGui::TextUnformatted(CanvasHelpText().c_str());
		if (m_file != 0)
		{
			ImGui::Spacing();
			ImGui::TextUnformatted(L("Effect files are shown on the effect sprites the game draws with them, read from your game files. In a training match the character also shows them live.").c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

void PaletteEditorModal::DrawEffectGrid()
{
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 size = ImGui::GetContentRegionAvail();
	const ImVec2 canvasMax(origin.x + size.x, origin.y + size.y);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(origin, canvasMax, ImGui::GetColorU32(g_canvasBackground));

	// Room on the right for the picker, so it never sits on top of the swatches.
	const float pickerRoom = m_selected >= 0 ? kPickerWidth + 20.0f : 0.0f;
	const float top = 28.0f;
	const float cell = floorf((std::min)((size.x - pickerRoom - 20.0f) / 16.0f, (size.y - top - 12.0f) / 16.0f));
	if (cell < 4.0f)
	{
		DrawPickerPanel(origin, canvasMax, origin.y + 10.0f);
		return;
	}

	const ImVec2 gridOrigin(origin.x + (size.x - pickerRoom - cell * 16.0f) * 0.5f, origin.y + top);
	std::string note;
	if (EffectSheets::ImageCount(m_charIndex, m_file) == 0)
		note = FormatText(L("%s never draws anything with this file. Its colors are still saved with the palette.").c_str(),
			getCharacterNameByIndexA(m_charIndex).c_str());
	else if (!m_effectNote.empty())
		note = m_effectNote + " " + L("Showing the colors as a grid instead.");
	else
		note = L("No effect sprite could be read for this file; showing the colors as a grid instead.");
	draw->AddText(ImVec2(origin.x + 30.0f, origin.y + 5.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), note.c_str());

	ImGui::SetCursorScreenPos(gridOrigin);
	const bool hovered = DrawEntryGrid("##pe_effect_grid", cell, 2.0f);
	// A click on the empty canvas around the grid is "done with this one", as on the sheet.
	if (!hovered && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		AcceptPick();

	DrawCanvasHelp(origin);
	DrawPickerPanel(origin, canvasMax, origin.y + 10.0f);
}

// --- Live preview in training ----------------------------------------------------------------

bool PaletteEditorModal::InTrainingMatch()
{
	return g_gameVals.pGameMode && g_gameVals.pGameState &&
		*g_gameVals.pGameMode == GameMode_Training && *g_gameVals.pGameState == GameState_InMatch;
}

void PaletteEditorModal::UpdateLive(bool editorOpen)
{
	// Out of the match, the game has reloaded palettes itself; there is nothing to put back.
	if (!InTrainingMatch() || !g_interfaces.pPaletteManager)
	{
		m_live[0].active = false;
		m_live[1].active = false;
		return;
	}

	// Only once there is a palette being edited, and only on characters it was made for.
	const bool wanted = editorOpen && m_step >= Step_Edit && m_loadedChar == m_charIndex;

	Player* players[2] = { &g_interfaces.player1, &g_interfaces.player2 };
	for (int p = 0; p < 2; p++)
	{
		LiveSlot& slot = m_live[p];
		Player& player = *players[p];
		const bool matches = wanted && !player.IsCharDataNullPtr() &&
			(int)player.GetData()->charIndex == m_charIndex;
		CharPaletteHandle& handle = player.GetPalHandle();

		if (!matches)
		{
			if (slot.active)
			{
				if (!m_keepLiveOnClose && !player.IsCharDataNullPtr())
				{
					for (int f = 0; f < IMPL_PALETTE_FILES_COUNT; f++)
					{
						if (memcmp(FileData(slot.backup, f), FileData(slot.pushed, f), IMPL_PALETTE_DATALEN) != 0)
							g_interfaces.pPaletteManager->ReplacePaletteFile(FileData(slot.backup, f), (PaletteFile)f, handle);
					}
				}
				slot.active = false;
			}
			continue;
		}

		if (!slot.active)
		{
			bool ok = true;
			for (int f = 0; f < IMPL_PALETTE_FILES_COUNT && ok; f++)
			{
				const char* current = g_interfaces.pPaletteManager->GetCurPalFileAddr((PaletteFile)f, handle);
				if (current)
					memcpy(FileData(slot.backup, f), current, IMPL_PALETTE_DATALEN);
				else
					ok = false;
			}
			if (!ok)
				continue;
			slot.pushed = slot.backup;
			slot.active = true;
		}

		// Everything the editor shows, the open colour and ramp included; only the files
		// that actually changed since the last push are written, since each write makes the
		// game refresh the character's palette.
		for (int f = 0; f < IMPL_PALETTE_FILES_COUNT; f++)
		{
			const char* wantedData = (f == m_file && m_selected >= 0) ? m_pendingFile : FileData(m_palette, f);
			if (memcmp(FileData(slot.pushed, f), wantedData, IMPL_PALETTE_DATALEN) == 0)
				continue;
			g_interfaces.pPaletteManager->ReplacePaletteFile(wantedData, (PaletteFile)f, handle);
			memcpy(FileData(slot.pushed, f), wantedData, IMPL_PALETTE_DATALEN);
		}
	}
}

// --- Eyedropper ----------------------------------------------------------------------------

void PaletteEditorModal::EyedropperButton(const char* id, unsigned char* target, bool isRampEnd)
{
	const bool picking = m_eyedropTarget == target;
	const float size = ImGui::GetFrameHeight();
	ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(picking ? ImGuiCol_ButtonActive : ImGuiCol_Button));
	ImGui::BeginDisabled(m_eyedropTarget != nullptr && !picking);
	const bool clicked = ImGui::Button(id, ImVec2(size, size));
	ImGui::EndDisabled();
	ImGui::PopStyleColor();

	// A pipette, drawn: the bulb at the top right, the glass running down to the tip.
	const ImVec2 min = ImGui::GetItemRectMin();
	const ImVec2 max = ImGui::GetItemRectMax();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImU32 colour = ImGui::GetColorU32(ImGuiCol_Text);
	const float s = max.x - min.x;
	const ImVec2 tip(min.x + s * 0.22f, max.y - s * 0.22f);
	const ImVec2 neck(min.x + s * 0.62f, min.y + s * 0.38f);
	draw->AddLine(tip, neck, colour, s * 0.13f);
	draw->AddCircleFilled(ImVec2(min.x + s * 0.70f, min.y + s * 0.30f), s * 0.14f, colour);
	draw->AddLine(ImVec2(min.x + s * 0.50f, min.y + s * 0.30f), ImVec2(min.x + s * 0.70f, min.y + s * 0.50f), colour, s * 0.08f);

	if (clicked)
	{
		if (picking)
		{
			ScreenColorPicker::Cancel();
		}
		else if (ScreenColorPicker::Start(g_gameProc.hWndGameWindow))
		{
			m_eyedropTarget = target;
			m_eyedropIsRampEnd = isRampEnd;
			memcpy(m_eyedropBefore, target, 4);
		}
	}
	Tip(picking ? L("Picking: click anywhere on the screen - in the game or outside it - to take that color. Right-click or Escape cancels.")
		: L("Eyedropper: pick a color from anywhere on your screen, even outside the game."));

	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	if (picking)
		ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", L("Click anywhere to take its color...").c_str());
	else
		ImGui::TextDisabled("%s", L("Pick from screen").c_str());
}

void PaletteEditorModal::PollEyedropper()
{
	if (!m_eyedropTarget)
		return;

	unsigned int rgb = 0;
	const ScreenColorPicker::State state = ScreenColorPicker::Poll(&rgb);
	unsigned char* target = m_eyedropTarget;
	// Alpha is kept: a screen pixel has none to give.
	auto apply = [&]() {
		target[0] = (unsigned char)(rgb & 0xFF);
		target[1] = (unsigned char)((rgb >> 8) & 0xFF);
		target[2] = (unsigned char)((rgb >> 16) & 0xFF);
		if (m_eyedropIsRampEnd)
			m_rampEndsCustom = true;
	};

	switch (state)
	{
	case ScreenColorPicker::State_Picking:
		apply(); // live, so the picture shows the colour under the cursor as you move
		break;
	case ScreenColorPicker::State_Picked:
		apply();
		m_eyedropTarget = nullptr;
		break;
	case ScreenColorPicker::State_Cancelled:
	case ScreenColorPicker::State_Idle:
		memcpy(target, m_eyedropBefore, 4);
		m_eyedropTarget = nullptr;
		break;
	}
}

// --- Page export / import ----------------------------------------------------------------

std::string PaletteEditorModal::PageName(int file) const
{
	return file == 0 ? L("Character colors") : FormatText(L("Effect %d").c_str(), file);
}

void PaletteEditorModal::BeginExportPage()
{
	const std::string palette = m_name[0] ? Trimmed(m_name)
		: (m_loadedChoice.isNative ? ColorLabel(m_loadedChoice.nativeIndex) : m_loadedChoice.name);
	char fileName[160];
	if (m_file == 0)
		sprintf_s(fileName, "%s %s.png", getCharacterNameByIndexA(m_charIndex).c_str(), palette.c_str());
	else
		sprintf_s(fileName, "%s %s - Effect %d.png", getCharacterNameByIndexA(m_charIndex).c_str(), palette.c_str(), m_file);

	NativeFileDialog::Request request;
	request.save = true;
	request.title = "Export page";
	request.filters.push_back({ "PNG palette (*.png)", "*.png" });
	request.defaultExtension = "png";
	request.initialPath = fileName;
	request.contextId = kDialogExportPage;
	NativeFileDialog::Open(kFileDialogOwner, request);
}

void PaletteEditorModal::ConsumePageDialog()
{
	NativeFileDialog::Result result;
	if (!NativeFileDialog::Consume(kFileDialogOwner, &result) || !result.accepted || result.path.empty())
		return;

	std::string error;
	if (result.contextId == kDialogExportPage)
	{
		bool written = false;
		if (m_file == 0)
		{
			// The character colours go out as the reference sheet, the same file the
			// Palettes window's PNG export writes - and like it, with the whole palette
			// embedded, so it imports there as a complete palette too.
			IMPL_data_t out = m_palette;
			out.palInfo = IMPL_info_t();
			strncpy(out.palInfo.palName, m_name, IMPL_PALNAME_LENGTH - 1);
			strncpy(out.palInfo.creator, m_creator, IMPL_CREATOR_LENGTH - 1);
			strncpy(out.palInfo.desc, m_desc, IMPL_DESC_LENGTH - 1);
			out.palInfo.hasBloom = m_bloom;
			written = PaletteSheet::Write(m_charIndex, out, result.path, error);
		}
		else
		{
			// An effect file goes out on its effect sprites, as plain indices recoloured by
			// the PNG's palette - so an image editor shows what the colours are on. With no
			// sprites to show it on, it is the 256 colours as a 16x16 grid of swatches.
			const char* fileData = FileData(m_palette, m_file);
			const unsigned char* raw = nullptr;
			int width = 0, height = 0;
			if (EffectSheets::ImageCount(m_charIndex, m_file) > 0 &&
				EffectSheets::Request(m_charIndex) == EffectSheets::Status_Ready &&
				EffectSheets::GetRawSheet(m_charIndex, m_file, &raw, &width, &height))
			{
				written = PngPalette::WriteIndexedPng(result.path, width, height, fileData, raw, error,
					m_charIndex, nullptr, m_file);
			}
			else
			{
				const int swatch = 16;
				std::vector<unsigned char> grid((size_t)256 * swatch * swatch);
				for (int y = 0; y < 16 * swatch; y++)
					for (int x = 0; x < 16 * swatch; x++)
						grid[(size_t)y * 16 * swatch + x] = (unsigned char)((y / swatch) * 16 + x / swatch);
				written = PngPalette::WriteIndexedPng(result.path, 16 * swatch, 16 * swatch, fileData, grid.data(),
					error, m_charIndex, nullptr, m_file);
			}
		}

		if (written)
		{
			m_status = FormatText(L("Exported %s to %s.").c_str(), PageName(m_file).c_str(), result.path.c_str());
			m_statusIsError = false;
			g_imGuiLogger->Log("[system] Palette editor: exported %s to '%s'\n", PageName(m_file).c_str(), result.path.c_str());
		}
		else
		{
			m_status = FormatText(L("Could not export: %s").c_str(), error.c_str());
			m_statusIsError = true;
		}
		return;
	}

	if (result.contextId == kDialogImportPage)
	{
		PngPalette::Imported imported;
		if (!PngPalette::ReadPaletteFileEx(result.path, imported, error))
		{
			m_status = FormatText(L("Could not import: %s").c_str(), error.c_str());
			m_statusIsError = true;
			return;
		}

		// Entry 0 is the transparent slot and a PNG does not carry it; the rest replaces
		// this page, as one undo step.
		PushUndo(m_palette);
		memcpy(FileData(m_palette, m_file) + 4, imported.characterFile + 4, IMPL_PALETTE_DATALEN - 4);
		m_dirty = true;

		std::string message = FormatText(L("Imported %s into %s.").c_str(),
			result.path.substr(result.path.find_last_of("\\/") + 1).c_str(), PageName(m_file).c_str());
		// Allowed either way - copying one character's effects onto another is a fair thing
		// to want - but worth saying, since it is usually a slip.
		const int fromFile = imported.paletteFile >= 0 ? imported.paletteFile : (imported.hasExtras ? 0 : -1);
		if (fromFile >= 0 && fromFile != m_file)
			message += " " + FormatText(L("It was exported from %s.").c_str(), PageName(fromFile).c_str());
		if (imported.charIndex >= 0 && imported.charIndex != m_charIndex)
			message += " " + FormatText(L("It was made for %s.").c_str(), getCharacterNameByIndexA(imported.charIndex).c_str());
		m_status = message;
		m_statusIsError = false;
	}
}

// --- Step 4: details ---------------------------------------------------------------------

std::string PaletteEditorModal::NameProblem() const
{
	const std::string name = Trimmed(m_name);
	if (name.empty())
		return L("Give the palette a name to save it.");
	if (IsReservedName(name.c_str()))
		return Messages.Error_not_a_valid_filename();
	return std::string();
}

void PaletteEditorModal::DrawDetailsStep()
{
	// The finished palette on the left, big, and what it will be saved as on the right.
	const float previewWidth = (std::max)(320.0f, ImGui::GetContentRegionAvail().x * 0.55f);
	ImGui::BeginChild("##pe_details_preview", ImVec2(previewWidth, 0), ImGuiChildFlags_Borders,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	DrawDetailsPreview();
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("##pe_details_form", ImVec2(0, 0),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
	DrawDetailsForm();
	ImGui::EndChild();
}

void PaletteEditorModal::DrawDetailsPreview()
{
	// The whole reference sheet: every pose and every colour, the picture the palette
	// was edited on.
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 area = ImGui::GetContentRegionAvail();
	const float captionHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 8.0f;
	if (area.x < 8.0f || area.y < captionHeight + 8.0f)
		return;

	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImVec2 areaMax(origin.x + area.x, origin.y + area.y);
	draw->AddRectFilled(origin, areaMax, ImGui::GetColorU32(g_canvasBackground), 4.0f);

	const ImVec2 imageArea(area.x - 16.0f, area.y - captionHeight - 16.0f);
	int texWidth = 0, texHeight = 0;
	const ImTextureID texture = PaletteThumbnails::GetEditorSheet(m_charIndex, m_palette.file0, nullptr, g_fade,
		&texWidth, &texHeight);
	if (texture && texWidth > 0 && texHeight > 0 && imageArea.x > 0.0f && imageArea.y > 0.0f)
	{
		const float scale = (std::min)(imageArea.x / texWidth, imageArea.y / texHeight);
		const ImVec2 size(texWidth * scale, texHeight * scale);
		const ImVec2 p0(origin.x + (area.x - size.x) * 0.5f, origin.y + 8.0f + (imageArea.y - size.y) * 0.5f);
		draw->AddImage(ImTextureRef(texture), p0, ImVec2(p0.x + size.x, p0.y + size.y));
	}

	// Caption: the name as it will be saved, and what it is for.
	const std::string name = Trimmed(m_name);
	const std::string title = name.empty() ? L("(unnamed)") : name;
	std::string subtitle = getCharacterNameByIndexA(m_charIndex);
	if (m_creator[0])
		subtitle += "  -  " + L("by") + " " + m_creator;

	const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
	const float captionTop = areaMax.y - captionHeight;
	ImGui::PushFont(NULL, ImGui::GetFontSize() * 1.3f);
	const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
	draw->AddText(ImVec2(origin.x + (area.x - titleSize.x) * 0.5f, captionTop),
		ImGui::GetColorU32(name.empty() ? ImGuiCol_TextDisabled : ImGuiCol_Text), title.c_str());
	ImGui::PopFont();
	const ImVec2 subtitleSize = ImGui::CalcTextSize(subtitle.c_str());
	draw->AddText(ImVec2(origin.x + (area.x - subtitleSize.x) * 0.5f, captionTop + lineHeight * 1.2f),
		ImGui::GetColorU32(ImGuiCol_TextDisabled), subtitle.c_str());

	ImGui::Dummy(area);
}

void PaletteEditorModal::DrawDetailsForm()
{
	ImGui::PushFont(NULL, ImGui::GetFontSize() * 1.3f);
	ImGui::TextUnformatted(L("Name and save").c_str());
	ImGui::PopFont();

	int changed = 0;
	for (int i = 0; i < 256; i++)
	{
		if (memcmp(Entry(i), BaseEntry(i), 4) != 0)
			changed++;
	}
	const std::string from = m_loadedChoice.isNative ? ColorLabel(m_loadedChoice.nativeIndex) : m_loadedChoice.name;
	ImGui::TextDisabledWrapped("%s", FormatText(L("%d of 256 colors changed from %s.").c_str(), changed, from.c_str()).c_str());
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// One labelled field: label and a length counter on one line, the box full width,
	// a hint underneath.
	auto field = [](const char* id, const std::string& label, bool required, char* buffer, size_t size,
		const std::string& hint) {
		ImGui::TextUnformatted(label.c_str());
		if (required)
		{
			ImGui::SameLine(0.0f, 2.0f);
			ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "*");
		}
		char counter[16];
		sprintf_s(counter, "%d/%d", (int)strlen(buffer), (int)size - 1);
		ImGui::SameLine();
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(counter).x);
		ImGui::TextDisabled("%s", counter);
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText(id, buffer, size, ImGuiInputTextFlags_CallbackCharFilter, FilterPaletteNameChars);
		Tip(hint);
		ImGui::TextDisabledWrapped("%s", hint.c_str());
		ImGui::Spacing();
		ImGui::Spacing();
	};

	field("##pe_name", Messages.Palette_name(), true, m_name, sizeof(m_name),
		L("Also the file name. Letters, numbers, spaces and simple punctuation."));
	field("##pe_creator", Messages.Creator_optional(), false, m_creator, sizeof(m_creator),
		L("Who made the palette. Stored in the palette file."));
	field("##pe_desc", Messages.Palette_description_optional(), false, m_desc, sizeof(m_desc),
		L("A short note about the palette, stored in the palette file."));

	ImGui::Checkbox(Messages.Save_with_bloom_effect(), &m_bloom);
	Tip(L("Saves the palette with the bloom (glow) effect on.") + "\n" + Messages.Bloom_effects_cannot_be_changed_until_a_new_round_is_started());
	ImGui::TextDisabledWrapped("%s", Messages.Bloom_effects_cannot_be_changed_until_a_new_round_is_started());

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Where it goes, and anything that will get in the way, before pressing Save.
	const std::string problem = NameProblem();
	const std::string name = Trimmed(m_name);
	if (!problem.empty())
	{
		ImGui::TextColoredWrapped(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "%s", problem.c_str());
	}
	else
	{
		ImGui::TextDisabled("%s", L("Will be saved as").c_str());
		ImGui::TextWrapped("%s", PalettePath(m_charIndex, name).c_str());

		const bool sameAsOpened = !m_originalName.empty() && _stricmp(name.c_str(), m_originalName.c_str()) == 0;
		const bool taken = GetFileAttributesA(PalettePath(m_charIndex, name).c_str()) != INVALID_FILE_ATTRIBUTES ||
			(g_interfaces.pPaletteManager &&
			 g_interfaces.pPaletteManager->FindCustomPalIndex((CharIndex)m_charIndex, name.c_str()) > 0);
		if (sameAsOpened)
			ImGui::TextDisabledWrapped("%s", L("This replaces the palette you opened.").c_str());
		else if (taken)
			ImGui::TextColoredWrapped(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "%s",
				L("You already have a palette with this name. Saving asks before replacing it.").c_str());
	}

	if (!m_status.empty())
	{
		ImGui::Spacing();
		ImGui::TextColoredWrapped(m_statusIsError ? ImVec4(1.0f, 0.42f, 0.42f, 1.0f) : ImVec4(0.30f, 0.85f, 0.39f, 1.0f),
			"%s", m_status.c_str());
	}
}

// --- Popups ------------------------------------------------------------------------------

void PaletteEditorModal::DrawApplyPalettePopup()
{
	if (m_requestApply)
	{
		m_requestApply = false;
		ImGui::OpenPopup(kApplyId);
	}

	const std::string title = L("Apply palette") + kApplyId;
	ImGui::SetNextWindowSize(ImVec2(1100, 760), ImGuiCond_FirstUseEver);
	if (!ImGui::BeginPopupModal(title.c_str(), nullptr, 0))
		return;

	ImGui::TextWrapped("%s", L("Pick a palette to take every color from. Your current colors are replaced, but Undo brings them back.").c_str());
	ImGui::Spacing();

	const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	bool apply = DrawBaseChooser("##pe_apply_grid", ImVec2(0, -footer), m_applyChoice, m_applyFilter);

	const float buttonWidth = 120.0f;
	ImGui::SetCursorPosX(ImGui::GetWindowWidth() - buttonWidth * 2.0f - ImGui::GetStyle().ItemSpacing.x -
		ImGui::GetStyle().WindowPadding.x);
	if (ImGui::Button(Messages.Cancel(), ImVec2(buttonWidth, 0)))
		ImGui::CloseCurrentPopup();
	Tip(L("Keep your colors as they are."));
	ImGui::SameLine();
	ImGui::BeginDisabled(!m_applyChoice.valid);
	if (ImGui::Button(L("Apply").c_str(), ImVec2(buttonWidth, 0)))
		apply = true;
	ImGui::EndDisabled();
	Tip(L("Replace every color with the selected palette's (double-clicking a picture does the same)."));

	if (apply && m_applyChoice.valid)
	{
		IMPL_data_t data;
		std::string error;
		if (ResolveBase(m_applyChoice, data, error))
		{
			PushUndo(m_palette);
			CopyColourFiles(m_palette, data);
			// "Original" in the picker now means this palette's colours.
			CopyColourFiles(m_base, data);
			m_loadedChoice = m_applyChoice;
			m_choice = m_applyChoice;
			m_dirty = true;
			m_status.clear();
		}
		else
		{
			m_status = error;
			m_statusIsError = true;
		}
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
}

void PaletteEditorModal::DrawFadeSettingsPopup()
{
	if (m_requestFadeSettings)
	{
		m_requestFadeSettings = false;
		ImGui::OpenPopup(kFadeId);
	}

	// A plain popup, not a modal: a modal would dim the very picture these settings are
	// about. Clicking anywhere else closes it.
	if (!ImGui::BeginPopup(kFadeId))
		return;

	ImGui::TextUnformatted(L("Fade out settings").c_str());
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
	ImGui::TextDisabled("%s", L("While a color is open, everything it does not paint is faded like this. Open a color to see changes live.").c_str());
	ImGui::PopTextWrapPos();
	ImGui::Separator();

	bool changed = false;
	const float width = 240.0f;

	float colour[3] = { g_fade.red / 255.0f, g_fade.green / 255.0f, g_fade.blue / 255.0f };
	ImGui::SetNextItemWidth(width);
	if (ImGui::ColorEdit3(L("Fade color").c_str(), colour))
	{
		g_fade.red = (unsigned char)(colour[0] * 255.0f + 0.5f);
		g_fade.green = (unsigned char)(colour[1] * 255.0f + 0.5f);
		g_fade.blue = (unsigned char)(colour[2] * 255.0f + 0.5f);
		changed = true;
	}
	Tip(L("The color faded-out parts are pulled towards. Gray keeps things neutral; something loud makes the edited color pop."));

	int strength = (int)(g_fade.strength * 100.0f + 0.5f);
	ImGui::SetNextItemWidth(width);
	if (ImGui::SliderInt(L("Strength").c_str(), &strength, 0, 100, "%d%%"))
	{
		g_fade.strength = strength / 100.0f;
		changed = true;
	}
	Tip(L("How far faded-out parts move towards the fade color. 0% leaves them untouched, 100% replaces them."));

	int shading = (int)(g_fade.shading * 100.0f + 0.5f);
	ImGui::SetNextItemWidth(width);
	if (ImGui::SliderInt(L("Keep shading").c_str(), &shading, 0, 100, "%d%%"))
	{
		g_fade.shading = shading / 100.0f;
		changed = true;
	}
	Tip(L("How much of each part's own light and shadow survives, so you can still make out the shapes. 0% is one flat color."));

	int opacity = (int)(g_fade.opacity * 100.0f + 0.5f);
	ImGui::SetNextItemWidth(width);
	if (ImGui::SliderInt(L("Opacity").c_str(), &opacity, 0, 100, "%d%%"))
	{
		g_fade.opacity = opacity / 100.0f;
		changed = true;
	}
	Tip(L("How see-through faded-out parts are. Lower lets the canvas background show through them."));

	ImGui::SetNextItemWidth(width);
	if (ImGui::ColorEdit3(L("Canvas background").c_str(), &g_canvasBackground.x))
		changed = true;
	Tip(L("The color behind the sheet. A different one helps when a palette's colors are close to it."));

	ImGui::Separator();
	if (ImGui::Button(L("Reset to defaults").c_str()))
	{
		g_fade = kDefaultFade;
		g_canvasBackground = kDefaultCanvasBackground;
		changed = true;
	}
	Tip(L("Put every fade setting back how it was."));
	ImGui::SameLine();
	if (ImGui::Button(Messages.Close()))
		ImGui::CloseCurrentPopup();
	Tip(L("Settings are kept; they are remembered between sessions."));

	if (changed)
		ImGui::MarkIniSettingsDirty();

	ImGui::EndPopup();
}

void PaletteEditorModal::DrawRebaseConfirm()
{
	if (m_requestRebase)
	{
		m_requestRebase = false;
		ImGui::OpenPopup(kRebaseId);
	}

	const std::string title = L("Start over?") + kRebaseId;
	if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::TextUnformatted(L("You picked a different character or starting palette.").c_str());
	ImGui::TextUnformatted(L("Starting from it throws away the colors you have edited so far.").c_str());
	ImGui::Spacing();

	if (ImGui::Button(L("Start over").c_str(), ImVec2(160, 0)))
	{
		m_dirty = false;
		GoToEditStep();
		ImGui::CloseCurrentPopup();
	}
	Tip(L("Load the new pick and lose the current edits."));
	ImGui::SameLine();
	if (ImGui::Button(L("Keep my edits").c_str(), ImVec2(160, 0)))
	{
		// Back to exactly what the edits were built on.
		m_charIndex = m_loadedChar;
		m_choice = m_loadedChoice;
		m_step = Step_Edit;
		ImGui::CloseCurrentPopup();
	}
	Tip(L("Go back to editing what you had."));

	ImGui::EndPopup();
}

void PaletteEditorModal::DrawDiscardConfirm()
{
	if (m_requestDiscard)
	{
		m_requestDiscard = false;
		ImGui::OpenPopup(kDiscardId);
	}

	const std::string title = L("Unsaved changes") + kDiscardId;
	if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::TextUnformatted(L("Close the editor and lose the changes since the last save?").c_str());
	ImGui::Spacing();
	if (ImGui::Button(L("Discard").c_str(), ImVec2(120, 0)))
	{
		ImGui::CloseCurrentPopup();
		m_closeAfterPopup = true;
	}
	Tip(L("Close without saving."));
	ImGui::SameLine();
	if (ImGui::Button(Messages.Cancel(), ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();
	Tip(L("Keep editing."));
	ImGui::EndPopup();
}

void PaletteEditorModal::DrawOverwriteConfirm(const std::function<void()>& onSaved)
{
	if (m_requestOverwrite)
	{
		m_requestOverwrite = false;
		ImGui::OpenPopup(kOverwriteId);
	}

	const std::string title = L("Overwrite?") + kOverwriteId;
	if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	const std::string fileName = Trimmed(m_name) + IMPL_FILE_EXTENSION;
	ImGui::Text(Messages.Overwrite_confirmation_prompt(), fileName.c_str());
	ImGui::Separator();
	if (ImGui::Button(Messages.OK(), ImVec2(120, 0)))
	{
		ImGui::CloseCurrentPopup();
		Save(onSaved, true);
	}
	Tip(L("Replace the existing palette with this one."));
	ImGui::SameLine();
	if (ImGui::Button(Messages.Cancel(), ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();
	Tip(L("Go back and pick another name."));
	ImGui::EndPopup();
}

void PaletteEditorModal::DrawSavedPopup()
{
	if (m_requestSaved)
	{
		m_requestSaved = false;
		ImGui::OpenPopup(kSavedId);
	}

	const std::string title = L("Palette saved") + kSavedId;
	if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::Text(Messages.s_saved_successfully(), (m_savedName + IMPL_FILE_EXTENSION).c_str());
	ImGui::TextDisabled("%s", FormatText(L("It is in the Palettes window under %s.").c_str(),
		getCharacterNameByIndexA(m_charIndex).c_str()).c_str());
	ImGui::Spacing();
	if (ImGui::Button(Messages.OK(), ImVec2(120, 0)))
	{
		ImGui::CloseCurrentPopup();
		m_closeAfterPopup = true;
	}
	ImGui::EndPopup();
}

bool PaletteEditorModal::Save(const std::function<void()>& onSaved, bool allowOverwrite)
{
	const std::string name = Trimmed(m_name);
	if (name.empty())
	{
		m_status = Messages.Error_no_filename_given();
		m_statusIsError = true;
		return false;
	}
	if (IsReservedName(name.c_str()))
	{
		m_status = Messages.Error_not_a_valid_filename();
		m_statusIsError = true;
		return false;
	}
	if (!g_interfaces.pPaletteManager)
		return false;

	// Saving an opened palette under its own name is the point of opening it; anything
	// else that would land on an existing palette asks first.
	const bool sameAsOpened = !m_originalName.empty() && _stricmp(name.c_str(), m_originalName.c_str()) == 0;
	const std::string path = PalettePath(m_charIndex, name);
	if (!sameAsOpened && !allowOverwrite)
	{
		const bool fileExists = GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
		const bool nameTaken = g_interfaces.pPaletteManager->FindCustomPalIndex((CharIndex)m_charIndex, name.c_str()) > 0;
		if (fileExists || nameTaken)
		{
			m_requestOverwrite = true;
			return false;
		}
	}

	IMPL_data_t out = m_palette;
	out.palInfo = IMPL_info_t();
	strncpy(out.palInfo.palName, name.c_str(), IMPL_PALNAME_LENGTH - 1);
	strncpy(out.palInfo.creator, m_creator, IMPL_CREATOR_LENGTH - 1);
	strncpy(out.palInfo.desc, m_desc, IMPL_DESC_LENGTH - 1);
	out.palInfo.hasBloom = m_bloom;

	// The character folder normally exists already; making sure costs nothing.
	CreateDirectoryA("BBCF_IM", NULL);
	CreateDirectoryA("BBCF_IM\\Palettes", NULL);
	CreateDirectoryA((std::string("BBCF_IM\\Palettes\\") + getCharacterNameByIndexA(m_charIndex)).c_str(), NULL);

	const std::string fileName = name + IMPL_FILE_EXTENSION;
	if (!g_interfaces.pPaletteManager->WritePaletteToFile((CharIndex)m_charIndex, &out))
	{
		m_status = FormatText(Messages.s_save_failed(), fileName.c_str());
		m_statusIsError = true;
		g_imGuiLogger->Log("[error] Palette editor: failed to save '%s'\n", path.c_str());
		return false;
	}

	g_imGuiLogger->Log("[system] Palette editor: saved '%s'\n", path.c_str());
	m_status.clear();
	m_originalName = name;
	m_savedName = name;
	m_dirty = false;
	m_requestSaved = true;
	// What the training character is wearing is now a real palette; leave it on them.
	m_keepLiveOnClose = true;

	// Its thumbnail is cached by name, and a re-save keeps the name - only the colours move.
	PaletteThumbnails::Invalidate(m_charIndex, name);
	if (onSaved)
		onSaved();
	return true;
}
