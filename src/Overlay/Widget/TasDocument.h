#pragma once

#include "Game/TasManager.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/*
	One editable list of frames, whatever is behind it.

	The mod used to have two frame editors. The TAS window drove a live movie - seek, undo,
	drag rows around, retype a frame as notation. The old Playback Editor drove a recording
	slot with three small buttons per row, a hex column and a popup with A/B/C/D toggles.
	They did the same job and only one of them was worth using.

	So the editing UI talks to this instead of to either. Two documents implement it:

	  TasMovieDocument      the live TAS movie. Every edit re-simulates, so it needs a
	                        training match and a base state, and the playhead is the real
	                        position of the match.
	  TasPlaybackDocument   a recording slot, or a loose buffer of frames handed in by
	                        whoever opened the editor. Plain data: no match, no savestate,
	                        no simulation, and the playhead is just where you are reading.

	Frame storage is the same in both: BBCF packs a frame as direction 1-9 in the low nibble
	and A/B/C/D in 0x10/0x20/0x40/0x80, which is exactly the low byte of a TAS frame. The
	only thing a TAS movie can hold that a recording slot cannot is the taunt bit (0x100),
	and TasPlaybackDocument drops it on the way in.
*/
class ITasDocument {
public:
	virtual ~ITasDocument() = default;

	virtual size_t FrameCount() const = 0;
	virtual TasFrameInput GetFrame(size_t index) const = 0;

	virtual bool SetFrame(size_t index, TasFrameInput input) = 0;
	virtual bool InsertNeutral(size_t index, size_t count) = 0;
	virtual bool DeleteFrames(size_t index, size_t count) = 0;
	virtual bool DuplicateFrames(size_t index, size_t count) = 0;
	virtual bool MoveFrames(size_t from, size_t count, size_t to, size_t* outNewIndex) = 0;

	virtual bool CanUndo() const = 0;
	virtual bool Undo() = 0;
	virtual bool CanRedo() const = 0;
	virtual bool Redo() = 0;

	// Where the list is focused. For a movie this is the match's real position; for a
	// playback buffer it is only a reading cursor, so going to a frame costs nothing.
	virtual size_t Playhead() const = 0;
	virtual void GoToFrame(size_t index) = 0;

	// True while the document cannot accept an edit because it is catching up (a seek).
	// Always false for plain data.
	virtual bool IsBusy() const = 0;

	// A recording slot holds one side's inputs, so the list drops its second column.
	virtual bool HasSecondPlayer() const = 0;

	virtual std::string Error() const = 0;
};

// The live TAS movie. A thin forwarder: TasManager already owns the undo stack, the
// keyframes and the re-simulation, and none of that belongs in the UI.
class TasMovieDocument : public ITasDocument {
public:
	size_t FrameCount() const override;
	TasFrameInput GetFrame(size_t index) const override;
	bool SetFrame(size_t index, TasFrameInput input) override;
	bool InsertNeutral(size_t index, size_t count) override;
	bool DeleteFrames(size_t index, size_t count) override;
	bool DuplicateFrames(size_t index, size_t count) override;
	bool MoveFrames(size_t from, size_t count, size_t to, size_t* outNewIndex) override;
	bool CanUndo() const override;
	bool Undo() override;
	bool CanRedo() const override;
	bool Redo() override;
	size_t Playhead() const override;
	void GoToFrame(size_t index) override;
	bool IsBusy() const override;
	bool HasSecondPlayer() const override { return true; }
	std::string Error() const override;
};

// A recording slot or a library entry, edited as bytes. Everything happens in a draft: the
// slot or the file is only touched by Save, so Cancel really does put it back.
class TasPlaybackDocument : public ITasDocument {
public:
	enum class Source {
		None,
		CfSlot,  // one of the game's four recording slots; Save writes it
		Buffer,  // frames handed in by the caller; Save only hands them back
	};

	// Returns false and leaves the document untouched if the slot cannot be read.
	bool OpenCfSlot(int slot);
	// Frames belonging to somebody else's unsaved edit - a library entry being edited in a
	// dialog, say. Saving here commits nothing on its own: the caller reads the frames back
	// out and decides, which is what makes cancelling the dialog above actually cancel.
	bool OpenBuffer(const std::string& name, const std::vector<char>& frames, bool facingLeft);
	void Close();

	// Goes back to the frames this was opened with, throwing away unsaved edits.
	bool Reload();
	// A slot write for Source::CfSlot; for Source::Buffer it only marks the draft clean -
	// the caller is the one that commits, by reading FramesAsBytes().
	bool Save();
	// Drops trailing neutral frames. Undoable like any other edit.
	void Trim();

	Source GetSource() const { return m_source; }
	int GetCfSlot() const { return m_cfSlot; }
	const std::string& GetSourceName() const { return m_sourceName; }
	// The frames as the game stores them, one byte each, for a caller committing a buffer.
	std::vector<char> FramesAsBytes() const;
	bool IsOpen() const { return m_source != Source::None; }
	bool IsDirty() const;

	// Which way the side that performed these inputs was facing when they were recorded.
	// Playback mirrors left and right when the current facing disagrees with it.
	bool FacingLeft() const { return m_facingLeft; }
	void SetFacingLeft(bool facingLeft);

	// A slot the playback library has temporarily borrowed holds its runtime data rather
	// than the contents the user thinks are in it. Reads and writes already follow the
	// contents that will be restored; this is only so the UI can say so.
	bool IsBorrowedSlot() const;

	size_t FrameCount() const override { return m_frames.size(); }
	TasFrameInput GetFrame(size_t index) const override;
	bool SetFrame(size_t index, TasFrameInput input) override;
	bool InsertNeutral(size_t index, size_t count) override;
	bool DeleteFrames(size_t index, size_t count) override;
	bool DuplicateFrames(size_t index, size_t count) override;
	bool MoveFrames(size_t from, size_t count, size_t to, size_t* outNewIndex) override;
	bool CanUndo() const override { return !m_undo.empty(); }
	bool Undo() override;
	bool CanRedo() const override { return !m_redo.empty(); }
	bool Redo() override;
	size_t Playhead() const override { return m_cursor; }
	void GoToFrame(size_t index) override;
	bool IsBusy() const override { return false; }
	bool HasSecondPlayer() const override { return false; }
	std::string Error() const override { return m_error; }

	const std::string& Status() const { return m_status; }

private:
	struct State {
		std::vector<uint16_t> frames;
		bool facingLeft = false;
	};

	void PushUndo();
	void ApplyState(const State& state);

	Source m_source = Source::None;
	int m_cfSlot = 1;
	std::string m_sourceName;

	std::vector<uint16_t> m_frames;
	std::vector<uint16_t> m_savedFrames;
	bool m_facingLeft = false;
	bool m_savedFacingLeft = false;

	std::vector<State> m_undo;
	std::vector<State> m_redo;

	size_t m_cursor = 0;
	std::string m_error;
	std::string m_status;
};
