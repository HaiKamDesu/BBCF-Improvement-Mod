#pragma once

#include "Overlay/Widget/TasDocument.h"
#include "Overlay/Widget/TasFrameListView.h"

#include <string>
#include <vector>

/*
	The frame editor as a block of UI, so it can live wherever it is needed.

	Two hosts use it: the TAS window, switched to playback mode, when you edit a recording
	slot straight from the training page; and the playback library's entry dialog, which
	hosts it as a modal on top of its own so you resolve the dialogs in order.

	That second host is why Save does not always write. A library entry being edited is
	already somebody's unsaved draft - name, weight, and now its frames - and it is the
	dialog above that decides whether any of it survives. So the view hands its frames back
	on Save (Outcome::Saved) and writes nothing itself; only a slot, which has no dialog
	above it, is written through.
*/
class TasPlaybackEditorView {
public:
	enum class Outcome {
		None,
		Saved,   // the user pressed Save this frame
		Closed,  // the user closed the editor; unsaved edits are gone
	};

	bool OpenCfSlot(int slot);
	// name is what the source bar shows; the frames are copied, never aliased.
	bool OpenBuffer(const std::string& name, const std::vector<char>& frames, bool facingLeft);
	void Close();

	bool IsOpen() const { return m_document.IsOpen(); }
	bool IsDirty() const { return m_document.IsDirty(); }

	// Draws the source bar, the frame list and the footer into the current content region.
	// listHeight of 0 means "whatever is left after the footer".
	Outcome Draw(float listHeight = 0.0f);

	// What the caller commits after Outcome::Saved on a buffer.
	std::vector<char> Frames() const { return m_document.FramesAsBytes(); }
	bool FacingLeft() const { return m_document.FacingLeft(); }

private:
	void DrawSourceBar();

	TasPlaybackDocument m_document;
	TasFrameListView m_list;
	std::string m_status;
};
