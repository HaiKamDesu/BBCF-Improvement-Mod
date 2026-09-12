#include "TasDocument.h"

#include "Core/Localization.h"
#include "Core/utils.h"
#include "Game/Playbacks/PlaybackManager.h"
#include "Game/Playbacks/PlaybackSlot.h"
#include "Game/Playbacks/UnlimitedPlaybackManager.h"

#include <algorithm>

namespace {

// Recording slots store one byte a frame; a TAS frame is the same bits in a uint16 with the
// taunt flag above them, which a slot has nowhere to put.
uint16_t PackedFromSlotByte(char byte) {
	return static_cast<uint16_t>(static_cast<unsigned char>(byte));
}

char SlotByteFromPacked(uint16_t packed) {
	return static_cast<char>(static_cast<unsigned char>(packed & 0xFF));
}

std::vector<uint16_t> PackedFromSlotBytes(const std::vector<char>& bytes) {
	std::vector<uint16_t> frames;
	frames.reserve(bytes.size());
	for (char byte : bytes) {
		frames.push_back(PackedFromSlotByte(byte));
	}
	return frames;
}

std::vector<char> SlotBytesFromPacked(const std::vector<uint16_t>& frames) {
	std::vector<char> bytes;
	bytes.reserve(frames.size());
	for (uint16_t packed : frames) {
		bytes.push_back(SlotByteFromPacked(packed));
	}
	return bytes;
}

// The contents the user thinks are in a slot, which is not the same as the bytes in it while
// the playback library has that slot borrowed for a runtime entry.
bool ReadCfSlot(int slot, std::vector<char>* outFrames, char* outFacing) {
	if (UnlimitedPlaybackManager::Instance().ReadBorrowedCfSlot(slot, outFrames, outFacing)) {
		return true;
	}
	PlaybackSlot pslot(slot);
	*outFrames = pslot.get_slot_buffer();
	*outFacing = pslot.get_facing_direction();
	return true;
}

// How many undo steps a playback buffer keeps. A recording is a few hundred bytes, so whole
// copies are cheaper than working out a diff.
constexpr size_t kUndoLimit = 64;

} // namespace

// --- TasMovieDocument ---------------------------------------------------------------

size_t TasMovieDocument::FrameCount() const { return TasManager::Instance().GetFrameCount(); }
TasFrameInput TasMovieDocument::GetFrame(size_t index) const { return TasManager::Instance().GetMovieFrame(index); }
bool TasMovieDocument::SetFrame(size_t index, TasFrameInput input) { return TasManager::Instance().SetFrameInput(index, input); }
bool TasMovieDocument::InsertNeutral(size_t index, size_t count) { return TasManager::Instance().InsertNeutralFrames(index, count); }
bool TasMovieDocument::DeleteFrames(size_t index, size_t count) { return TasManager::Instance().DeleteFrames(index, count); }
bool TasMovieDocument::DuplicateFrames(size_t index, size_t count) { return TasManager::Instance().DuplicateFrames(index, count); }
bool TasMovieDocument::MoveFrames(size_t from, size_t count, size_t to, size_t* outNewIndex) {
	return TasManager::Instance().MoveFrames(from, count, to, outNewIndex);
}
bool TasMovieDocument::CanUndo() const { return TasManager::Instance().CanUndo(); }
bool TasMovieDocument::Undo() { return TasManager::Instance().Undo(); }
bool TasMovieDocument::CanRedo() const { return TasManager::Instance().CanRedo(); }
bool TasMovieDocument::Redo() { return TasManager::Instance().Redo(); }
size_t TasMovieDocument::Playhead() const { return TasManager::Instance().GetCursor(); }
void TasMovieDocument::GoToFrame(size_t index) { TasManager::Instance().SeekToFrame(index); }
bool TasMovieDocument::IsBusy() const { return TasManager::Instance().IsSeeking(); }
std::string TasMovieDocument::Error() const { return TasManager::Instance().GetError(); }

// --- TasPlaybackDocument ------------------------------------------------------------

bool TasPlaybackDocument::OpenCfSlot(int slot) {
	if (slot < 1 || slot > 4) {
		return false;
	}

	std::vector<char> bytes;
	char facing = 0;
	if (!ReadCfSlot(slot, &bytes, &facing)) {
		m_error = L("Could not read that recording slot.");
		return false;
	}

	m_source = Source::CfSlot;
	m_cfSlot = slot;
	m_sourceName = FormatText(L("Recording slot %d").c_str(), slot);
	m_frames = PackedFromSlotBytes(bytes);
	m_facingLeft = facing != 0;
	m_savedFrames = m_frames;
	m_savedFacingLeft = m_facingLeft;
	m_undo.clear();
	m_redo.clear();
	m_cursor = 0;
	m_error.clear();
	m_status.clear();
	return true;
}

bool TasPlaybackDocument::OpenBuffer(const std::string& name, const std::vector<char>& frames, bool facingLeft) {
	m_source = Source::Buffer;
	m_cfSlot = 0;
	m_sourceName = name;
	m_frames = PackedFromSlotBytes(frames);
	m_facingLeft = facingLeft;
	m_savedFrames = m_frames;
	m_savedFacingLeft = m_facingLeft;
	m_undo.clear();
	m_redo.clear();
	m_cursor = 0;
	m_error.clear();
	m_status.clear();
	return true;
}

std::vector<char> TasPlaybackDocument::FramesAsBytes() const {
	return SlotBytesFromPacked(m_frames);
}

void TasPlaybackDocument::Close() {
	m_source = Source::None;
	m_sourceName.clear();
	m_frames.clear();
	m_savedFrames.clear();
	m_undo.clear();
	m_redo.clear();
	m_cursor = 0;
	m_error.clear();
	m_status.clear();
}

bool TasPlaybackDocument::Reload() {
	if (m_source == Source::CfSlot) {
		return OpenCfSlot(m_cfSlot);
	}
	if (m_source == Source::Buffer) {
		// There is nothing to re-read: "as it was handed to us" is the last saved draft.
		PushUndo();
		m_frames = m_savedFrames;
		m_facingLeft = m_savedFacingLeft;
		if (m_cursor > m_frames.size()) {
			m_cursor = m_frames.size();
		}
		return true;
	}
	return false;
}

bool TasPlaybackDocument::Save() {
	const std::vector<char> bytes = SlotBytesFromPacked(m_frames);

	if (m_source == Source::CfSlot) {
		PlaybackManager manager;
		manager.load_into_slot(bytes, m_facingLeft ? 1 : 0, m_cfSlot);
		// If the playback library has this slot borrowed, its pending restore would wipe the
		// write we just did and make Save look like it did nothing at all.
		UnlimitedPlaybackManager::Instance().AbsorbExternalSlotWrite(m_cfSlot, bytes, m_facingLeft);
	}
	else if (m_source != Source::Buffer) {
		return false;
	}

	m_savedFrames = m_frames;
	m_savedFacingLeft = m_facingLeft;
	m_error.clear();
	m_status = L("Saved.");
	return true;
}

void TasPlaybackDocument::Trim() {
	PushUndo();

	PlaybackManager manager;
	const std::vector<char> trimmed = manager.trim_playback(SlotBytesFromPacked(m_frames));
	m_frames = PackedFromSlotBytes(trimmed);
	if (m_cursor > m_frames.size()) {
		m_cursor = m_frames.size();
	}
	m_status = L("Trimmed.");
}

bool TasPlaybackDocument::IsDirty() const {
	return m_frames != m_savedFrames || m_facingLeft != m_savedFacingLeft;
}

void TasPlaybackDocument::SetFacingLeft(bool facingLeft) {
	if (facingLeft == m_facingLeft) {
		return;
	}
	PushUndo();
	m_facingLeft = facingLeft;
}

bool TasPlaybackDocument::IsBorrowedSlot() const {
	return m_source == Source::CfSlot &&
		UnlimitedPlaybackManager::Instance().GetBorrowedCfSlot() == m_cfSlot;
}

TasFrameInput TasPlaybackDocument::GetFrame(size_t index) const {
	TasFrameInput frame;
	frame.p1 = index < m_frames.size() ? m_frames[index] : 5;
	frame.p2 = 5;
	return frame;
}

void TasPlaybackDocument::PushUndo() {
	m_undo.push_back(State{ m_frames, m_facingLeft });
	if (m_undo.size() > kUndoLimit) {
		m_undo.erase(m_undo.begin());
	}
	m_redo.clear();
}

void TasPlaybackDocument::ApplyState(const State& state) {
	m_frames = state.frames;
	m_facingLeft = state.facingLeft;
	if (m_cursor > m_frames.size()) {
		m_cursor = m_frames.size();
	}
}

bool TasPlaybackDocument::SetFrame(size_t index, TasFrameInput input) {
	if (index >= m_frames.size()) {
		return false;
	}
	PushUndo();
	// The taunt bit has nowhere to live in a recording slot, so it is dropped rather than
	// silently reinterpreted as a direction.
	m_frames[index] = static_cast<uint16_t>(input.p1 & 0xFF);
	return true;
}

bool TasPlaybackDocument::InsertNeutral(size_t index, size_t count) {
	if (index > m_frames.size() || count == 0) {
		return false;
	}
	PushUndo();
	m_frames.insert(m_frames.begin() + static_cast<long>(index), count, static_cast<uint16_t>(5));
	return true;
}

bool TasPlaybackDocument::DeleteFrames(size_t index, size_t count) {
	if (index >= m_frames.size() || count == 0) {
		return false;
	}
	count = (std::min)(count, m_frames.size() - index);
	PushUndo();
	m_frames.erase(m_frames.begin() + static_cast<long>(index),
		m_frames.begin() + static_cast<long>(index + count));
	if (m_cursor > m_frames.size()) {
		m_cursor = m_frames.size();
	}
	return true;
}

bool TasPlaybackDocument::DuplicateFrames(size_t index, size_t count) {
	if (index >= m_frames.size() || count == 0) {
		return false;
	}
	count = (std::min)(count, m_frames.size() - index);
	PushUndo();
	const std::vector<uint16_t> block(m_frames.begin() + static_cast<long>(index),
		m_frames.begin() + static_cast<long>(index + count));
	m_frames.insert(m_frames.begin() + static_cast<long>(index + count), block.begin(), block.end());
	return true;
}

bool TasPlaybackDocument::MoveFrames(size_t from, size_t count, size_t to, size_t* outNewIndex) {
	if (from >= m_frames.size() || count == 0) {
		return false;
	}
	count = (std::min)(count, m_frames.size() - from);
	if (to > m_frames.size()) {
		return false;
	}
	// Landing inside the block being moved, or exactly where it already is, is not a move.
	if (to >= from && to <= from + count) {
		return false;
	}

	PushUndo();

	const std::vector<uint16_t> block(m_frames.begin() + static_cast<long>(from),
		m_frames.begin() + static_cast<long>(from + count));
	m_frames.erase(m_frames.begin() + static_cast<long>(from),
		m_frames.begin() + static_cast<long>(from + count));

	// Removing the block shifts everything after it down, so a target past it moves too.
	const size_t landing = to > from ? to - count : to;
	m_frames.insert(m_frames.begin() + static_cast<long>(landing), block.begin(), block.end());

	if (outNewIndex) {
		*outNewIndex = landing;
	}
	return true;
}

bool TasPlaybackDocument::Undo() {
	if (m_undo.empty()) {
		return false;
	}
	m_redo.push_back(State{ m_frames, m_facingLeft });
	ApplyState(m_undo.back());
	m_undo.pop_back();
	return true;
}

bool TasPlaybackDocument::Redo() {
	if (m_redo.empty()) {
		return false;
	}
	m_undo.push_back(State{ m_frames, m_facingLeft });
	ApplyState(m_redo.back());
	m_redo.pop_back();
	return true;
}

void TasPlaybackDocument::GoToFrame(size_t index) {
	// Nothing is simulated here, so this is only where the list is looking.
	m_cursor = (std::min)(index, m_frames.size());
}
