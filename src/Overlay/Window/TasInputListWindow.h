#pragma once

#include "Overlay/Window/IWindow.h"

#include <cfloat>
#include <cstddef>
#include <set>
#include <string>

class WindowContainer;
class TasManager;

/*
	The movie as an editable list, one row per frame.

	Vertical is the shape that fits a combo, and it lives in its own window so it can be made
	as tall as the screen and parked beside the editor. Everything structural happens here -
	select, drag to reorder, insert, delete, or retype a single frame in place - while the
	editor window drives playback and appends new input at the end.

	Editing a frame the match has already played re-seeks so the picture keeps matching the
	list; editing ahead of the playhead is free. Every edit is undoable.
*/
class TasInputListWindow : public IWindow {
public:
    TasInputListWindow(const std::string& windowTitle, bool windowClosable,
        WindowContainer& windowContainer, ImGuiWindowFlags windowFlags = 0)
        : IWindow(windowTitle, windowClosable, windowFlags), m_pWindowContainer(&windowContainer) {}

    ~TasInputListWindow() override = default;

public:
    void Update() override;

protected:
    void BeforeDraw() override;
    void Draw() override;

private:
    // Where a horizontal rule should be drawn across the list, in screen space. Collected
    // while the rows lay out and drawn after the table closes, because a table cell's clip
    // rect would otherwise cut a full-width line down to one column.
    struct Rule {
        float y = -FLT_MAX;
        float left = 0.0f;
        float right = 0.0f;
        bool valid() const { return y > -FLT_MAX; }
    };

    void DrawRow(TasManager& manager, size_t index, size_t playhead, size_t count,
        Rule& playheadRule, Rule& dropRule);
    void SelectRange(size_t start, size_t count);
    void DrawCell(TasManager& manager, size_t index, int player, uint16_t packed, bool played);
    void DrawContextMenu(TasManager& manager, size_t index);
    void HandleSelectionClick(size_t index, bool ctrlHeld, bool shiftHeld);
    void CommitCellEdit(TasManager& manager);
    void DeleteSelection(TasManager& manager);
    // The dragged block is the selection when it is a single unbroken run containing the
    // dragged row, and just that row otherwise.
    bool SelectionBlock(size_t index, size_t& outStart, size_t& outCount) const;
    void ClearSelection();

    WindowContainer* m_pWindowContainer = nullptr;

    std::set<size_t> m_selection;
    size_t m_selectionAnchor = 0;
    bool m_hasSelectionAnchor = false;

    // Which cell is being retyped, if any. -1 means nothing is being edited.
    int m_editingRow = -1;
    int m_editingPlayer = 0;
    bool m_editingJustOpened = false;
    char m_editBuffer[32] = "";

    int m_insertCount = 1;

    // Only scroll to the playhead when it moved on its own, so a user reading through the
    // list is not dragged back to the cursor every frame.
    size_t m_lastPlayhead = 0;
    bool m_hasLastPlayhead = false;
    bool m_followPlayhead = true;
};
