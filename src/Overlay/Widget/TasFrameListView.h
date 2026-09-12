#pragma once

#include <cfloat>
#include <cstddef>
#include <set>
#include <string>

class ITasDocument;

/*
	The frame list: one row per frame, the shape a combo is actually read in.

	Select, shift/ctrl-extend, drag a block to reorder, insert, duplicate, delete, or
	double-click a cell to retype it as notation ("5", "3C", "2AB"). Every edit goes through
	the document, so the same view edits a live TAS movie and a plain recording slot.

	It is a widget rather than a window because it is used twice: on its own in the TAS input
	list window, and inline in the editor when it is in playback mode, where the list IS the
	editor and a second window would be in the way.
*/
class TasFrameListView {
public:
	// Fills the current content region. Draws nothing when the document has no frames
	// beyond a line saying so.
	//
	// showFollowToggle is for hosts where the playhead moves on its own. Editing a recording
	// slot, nothing moves it but the user clicking a row, so the toggle would be a control
	// that never does anything.
	void Draw(ITasDocument& doc, bool showFollowToggle = true);

	void ClearSelection();
	bool HasSelection() const { return !m_selection.empty(); }

	// Off while a playback is running and the user is reading, on by default.
	bool FollowPlayhead() const { return m_followPlayhead; }
	void SetFollowPlayhead(bool follow) { m_followPlayhead = follow; }

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

	void DrawRow(ITasDocument& doc, size_t index, size_t playhead, size_t count,
		Rule& playheadRule, Rule& dropRule);
	void DrawCell(ITasDocument& doc, size_t index, int player, unsigned short packed, bool played);
	void DrawContextMenu(ITasDocument& doc, size_t index);
	void HandleSelectionClick(size_t index, bool ctrlHeld, bool shiftHeld);
	void CommitCellEdit(ITasDocument& doc);
	void DeleteSelection(ITasDocument& doc);
	void SelectRange(size_t start, size_t count);
	// The dragged block is the selection when it is a single unbroken run containing the
	// dragged row, and just that row otherwise.
	bool SelectionBlock(size_t index, size_t& outStart, size_t& outCount) const;

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
