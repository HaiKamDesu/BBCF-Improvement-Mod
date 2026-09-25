#pragma once

#include "imgui.h"

#include "Palette/impl_format.h"

#include <functional>
#include <string>
#include <vector>

// The palette editor that lives in the Palettes window.
//
// The older in-match editor (PaletteEditorWindow) is a grid of 256 numbered swatches: to
// change a cape you have to already know which of the 256 is the cape. This one starts
// from the picture instead. It draws the character's reference sheet in the palette being
// edited, and because the sheet is stored as palette indices, the pixel under the cursor
// *is* the answer: click the cape and the cape's entry opens in a picker, everything else
// fades out so you see exactly what that entry paints, and the picker recolours all of it
// live. Moving on - clicking another colour, empty space, or anything else - keeps the
// change; only Cancel throws it away.
//
// It is a four-step wizard - character, starting palette, edit, name and save - and works
// outside a match: it edits its own copy and only touches disk on save. A new palette can
// start from one of the game's own 24 colours, read from the character's palette archive.
class PaletteEditorModal
{
public:
	// Registers the ImGui ini handler the fade settings are kept in. Must run before the
	// first frame, which is when that file is parsed.
	static void RegisterLayoutSettings();

	// Starts the wizard at step 1, with `defaultCharIndex` preselected.
	void OpenNew(int defaultCharIndex);
	// Opens an installed palette straight at the editing step. Saving under the same name
	// overwrites it.
	void OpenExisting(int charIndex, const IMPL_data_t& palette);

	// Call every frame from inside the Palettes modal, so these stack on top of it.
	// `onSaved` runs after a palette was written, to re-read the folder.
	void Draw(const std::function<void()>& onSaved);

private:
	enum Step
	{
		Step_Character,
		Step_Base,
		Step_Edit,
		Step_Details,
	};

	// Something to start from: one of the game's colours, or an installed palette.
	struct BaseChoice
	{
		bool valid = false;
		bool isNative = true;
		int nativeIndex = 0;  // 0-based, 5 is "Color 06"
		std::string name;     // installed palette name when !isNative

		bool operator==(const BaseChoice& o) const
		{
			return valid == o.valid && isNative == o.isNative &&
				(isNative ? nativeIndex == o.nativeIndex : name == o.name);
		}
		bool operator!=(const BaseChoice& o) const { return !(*this == o); }
	};

	void DrawEditor(const std::function<void()>& onSaved);
	void DrawStepHeader();
	void DrawNavigation(const std::function<void()>& onSaved);
	void DrawCharacterStep();
	void DrawBaseStep();
	void DrawEditStep();
	void DrawDetailsStep();
	void DrawDetailsPreview();
	void DrawDetailsForm();
	// Why the current name cannot be saved, or empty when it can.
	std::string NameProblem() const;
	void DrawToolbar();
	void DrawCanvas(float width, float height);
	void DrawEffectGrid();
	void DrawCanvasHelp(const ImVec2& origin);
	// The "Show color grid" toggle over the canvas' top-right corner; true while hovered.
	bool DrawGridToggle(const ImVec2& canvasMin, const ImVec2& canvasMax, bool clicked);
	// The grid (when shown) and, while a colour is open, the picker under it.
	void DrawSidePanel(const ImVec2& canvasMin, const ImVec2& canvasMax);
	void DrawPickerPanel(const ImVec2& canvasMin, const ImVec2& canvasMax, float top);
	// All 256 entries of the file on screen as a 16x16 grid at the cursor, `cell` pixels
	// each. Clicking one opens it. Returns whether the grid is hovered.
	bool DrawEntryGrid(const char* id, float cell, float gap);
	void DrawSingleColour();
	void DrawShadeRamp();
	void BeginExportPage();
	void EyedropperButton(const char* id, unsigned char* target, bool isRampEnd);
	void PollEyedropper();
	void ConsumePageDialog();
	std::string PageName(int file) const;
	ImGuiColorEditFlags PickerFlags() const;

	// The two-section grid (game colours, your palettes) steps 2 and "Apply palette" share.
	// Returns true on a double-click, which callers take as "and go on".
	bool DrawBaseChooser(const char* id, const ImVec2& size, BaseChoice& choice, ImGuiTextFilter& filter);
	// One picture-and-name cell. Returns true when clicked; `doubleClicked` on a double-click.
	bool DrawCell(int charIndex, const std::string& key, const char* paletteData,
		const std::string& label, bool selected, float width, float spriteHeight, bool* doubleClicked);

	// Development thumbnail framing tuner, EnableInDevelopmentFeatures only (see the .cpp).
	void DrawAdjustedThumbnail(ImDrawList* draw, ImTextureID texture, int charIndex,
		const ImVec2& boxMin, const ImVec2& boxSize, float pixelScale);
	void DrawThumbnailAdjust();
	int m_adjustChar = -1;
	bool m_openAdjust = false;
	bool m_openAdjustMenu = false;

	void DrawApplyPalettePopup();
	void DrawFadeSettingsPopup();
	void DrawRebaseConfirm();
	void DrawDiscardConfirm();
	void DrawOverwriteConfirm(const std::function<void()>& onSaved);
	void DrawSavedPopup();

	const IMPL_data_t* NativeColour(int charIndex, int colorIndex);
	bool ResolveBase(const BaseChoice& choice, IMPL_data_t& out, std::string& error);
	void LoadBase(const IMPL_data_t& palette);
	void GoToEditStep();
	void RecountUsage();

	// Opens `index` in the picker, keeping whatever the colour open before it became.
	void Select(int index);
	// A click on a colour, sheet or grid: Select, or with Ctrl/Alt/Shift held, add it to
	// the open colour, carry the picker's colour over to it, or move the range end.
	void ClickEntry(int index);
	// Keeps the open colour's change and closes the picker. Anything that means "done with
	// this colour" calls it; there is no Accept button.
	void AcceptPick();
	void CancelPick();
	// A colour is open and the picker has changed something.
	bool PickChanged();
	// What the picker is editing: the open colour, or a gradient end. Null when none is open.
	unsigned char* PickerColour();
	void CopyColour();
	void PasteColour();

	// The file being edited with the open pick (and shade ramp) applied, into m_pendingFile.
	void BuildPendingFile();
	void AutoDetectRamp();
	void ResetRampEnds();
	void ApplyRamp(char* file) const;
	int RampLow() const { return m_rampStart < m_rampEnd ? m_rampStart : m_rampEnd; }
	int RampHigh() const { return m_rampStart < m_rampEnd ? m_rampEnd : m_rampStart; }

	// Training: the characters on screen wear the palette being edited, live.
	static bool InTrainingMatch();
	bool LiveActive() const { return m_live[0].active || m_live[1].active; }
	void UpdateLive(bool editorOpen);

	void PushUndo(const IMPL_data_t& before);
	void Undo();
	void Redo();
	void RequestClose();
	bool Save(const std::function<void()>& onSaved, bool allowOverwrite);

	// The eight files of an IMPL_data_t sit back to back, file0 first.
	static char* FileData(IMPL_data_t& data, int file) { return data.file0 + file * IMPL_PALETTE_DATALEN; }
	static const char* FileData(const IMPL_data_t& data, int file) { return data.file0 + file * IMPL_PALETTE_DATALEN; }
	unsigned char* Entry(int index) { return (unsigned char*)FileData(m_palette, m_file) + index * 4; }
	const unsigned char* BaseEntry(int index) const { return (const unsigned char*)FileData(m_base, m_file) + index * 4; }

	Step m_step = Step_Character;
	bool m_requestOpen = false;
	bool m_requestApply = false;
	bool m_requestFadeSettings = false;
	bool m_requestRebase = false;
	bool m_requestDiscard = false;
	bool m_requestOverwrite = false;
	bool m_requestSaved = false;
	bool m_closeAfterPopup = false;

	int m_charIndex = 0;
	BaseChoice m_choice;         // what step 2 has selected
	BaseChoice m_loadedChoice;   // what m_palette was actually built from
	int m_loadedChar = -1;
	BaseChoice m_applyChoice;    // selection inside the "Apply palette" popup
	ImGuiTextFilter m_baseFilter;
	ImGuiTextFilter m_applyFilter;

	// The game's 24 colours for one character, loaded when step 2 first shows them.
	int m_nativeChar = -1;
	std::vector<IMPL_data_t> m_nativeColours;
	std::vector<bool> m_nativeLoaded;
	std::string m_nativeError;

	IMPL_data_t m_palette = {};  // committed colours
	IMPL_data_t m_base = {};     // the palette last loaded or applied, for "Original"
	std::string m_originalName;  // empty for a new palette
	bool m_dirty = false;
	bool m_editingExisting = false; // opened on an installed palette: only the edit and save steps

	char m_name[IMPL_PALNAME_LENGTH] = {};
	char m_creator[IMPL_CREATOR_LENGTH] = {};
	char m_desc[IMPL_DESC_LENGTH] = {};
	bool m_bloom = false;
	std::string m_status;
	bool m_statusIsError = false;
	std::string m_savedName;

	// The open picker. m_selected >= 0 exactly while it is showing; nothing is committed
	// until AcceptPick, so Cancel is simply dropping m_pending.
	int m_file = 0;              // 0 = character colours, 1-7 = effect files
	int m_selected = -1;
	unsigned char m_pending[4] = {};
	unsigned char m_current[4] = {};
	char m_pendingFile[IMPL_PALETTE_DATALEN] = {};
	// Ctrl+clicked colours opened alongside m_selected, which take m_pending once it differs
	// from m_current. Only meaningful while m_selected >= 0; Select clears them.
	bool m_extra[256] = {};
	int m_extraCount = 0;
	unsigned char m_clipboard[4] = {}; // Ctrl+C / Ctrl+V, kept off the system clipboard
	bool m_haveClipboard = false;

	// Shade ramp: a run of neighbouring entries recoloured together, the way BBCF lays out
	// a material's shading. Mode 0 recolours it to the picked colour keeping each entry's
	// own light and shadow; mode 1 blends evenly between two colours (the old editor's
	// gradient generator).
	bool m_rampOn = false;
	bool m_forceSingleTab = false; // a new click opens on the One color tab

	// The screen eyedropper, while one is running: the colour it writes into, and what that
	// colour was before, for when it is cancelled.
	unsigned char* m_eyedropTarget = nullptr;
	bool m_eyedropIsRampEnd = false;
	unsigned char m_eyedropBefore[4] = {};
	int m_rampMode = 0;
	int m_rampStart = -1;
	int m_rampEnd = -1;
	unsigned char m_rampFrom[4] = {};
	unsigned char m_rampTo[4] = {};
	int m_rampEditEnd = 0;          // which gradient end the picker edits: 0 start, 1 end
	bool m_rampEndsCustom = false;  // the ends were picked by hand, so they stop following the range

	struct LiveSlot
	{
		bool active = false;
		IMPL_data_t backup = {}; // what the character wore before the editor took over
		IMPL_data_t pushed = {}; // what was last written, so unchanged files are skipped
	};
	LiveSlot m_live[2];
	bool m_keepLiveOnClose = false; // a saved palette stays on the character
	bool m_editAlpha = false;
	int m_usage[256] = {};       // pixels on the sheet per entry
	int m_usageKey = -1;         // which picture m_usage was counted on
	std::string m_effectNote;      // why an effect file has no picture, when it has none
	bool m_entryGridOnScreen = false; // a 256-colour grid was drawn this frame, so the range tab needs none
	int m_gridPressed = -1;        // the grid cell a left press started on
	bool m_gridDragged = false;    // ...and it has moved to another cell, choosing a range

	float m_zoom = 1.0f;
	ImVec2 m_pan = ImVec2(0, 0); // image origin, relative to the canvas' top-left
	ImVec2 m_canvasSize = ImVec2(0, 0);
	bool m_fitPending = true;
	bool m_leftPressed = false;  // a left press on the canvas that has not been released yet
	bool m_dragged = false;      // ...and it moved far enough to be a pan, not a click

	std::vector<IMPL_data_t> m_undo;
	std::vector<IMPL_data_t> m_redo;

	// The overlay hotkey hides the editor with the rest of the overlay, and ImGui closes
	// every popup of a window that goes away. These notice that and put it back.
	bool m_openLastDraw = false;
	int m_lastDrawFrame = -1;
};
