#include "TasPlaybackEditorView.h"

#include "Core/Localization.h"
#include "Core/utils.h"
#include "Overlay/imgui_utils.h"

#include <algorithm>

namespace {
const ImVec4 kColWarn(1.00f, 0.76f, 0.30f, 1.00f);
}

bool TasPlaybackEditorView::OpenCfSlot(int slot) {
	if (!m_document.OpenCfSlot(slot)) {
		return false;
	}
	m_list.ClearSelection();
	// Nothing moves the cursor here but the user clicking a row, so following it is a
	// control that could never do anything.
	m_list.SetFollowPlayhead(false);
	m_status.clear();
	return true;
}

bool TasPlaybackEditorView::OpenBuffer(const std::string& name, const std::vector<char>& frames, bool facingLeft) {
	if (!m_document.OpenBuffer(name, frames, facingLeft)) {
		return false;
	}
	m_list.ClearSelection();
	m_list.SetFollowPlayhead(false);
	m_status.clear();
	return true;
}

void TasPlaybackEditorView::Close() {
	m_document.Close();
	m_list.ClearSelection();
	m_status.clear();
}

void TasPlaybackEditorView::DrawSourceBar() {
	ImGui::TextUnformatted(L("Editing").c_str());
	ImGui::SameLine();

	if (m_document.GetSource() == TasPlaybackDocument::Source::CfSlot) {
		int slotIndex = m_document.GetCfSlot() - 1;
		const char* slots[] = { "1", "2", "3", "4" };
		ImGui::SetNextItemWidth(60.0f);
		if (ImGui::Combo(L("Recording slot").c_str(), &slotIndex, slots, IM_ARRAYSIZE(slots))) {
			// Switching slot is a fresh read, so anything unsaved is gone; say so rather
			// than carrying half of one recording into another.
			m_status = m_document.IsDirty()
				? L("Unsaved changes to the previous slot were discarded.")
				: std::string();
			m_document.OpenCfSlot(slotIndex + 1);
			m_list.ClearSelection();
		}
	} else {
		ImGui::TextUnformatted(m_document.GetSourceName().c_str());
	}

	ImGui::SameLine();
	if (ImGui::Button(Messages.Refresh())) {
		m_document.Reload();
		m_list.ClearSelection();
		m_status = L("Re-read from the game.");
	}
	ImGui::HoverTooltip(m_document.GetSource() == TasPlaybackDocument::Source::CfSlot
		? L("Throws away unsaved edits and reads the recording again.").c_str()
		: L("Throws away every edit made since this editor was opened.").c_str());

	// Which way the recorded side was facing. Playback mirrors left and right when the
	// character it is driving disagrees with this, so it is part of the recording, not a
	// display option.
	ImGui::TextUnformatted(FormatText(L("Recorded facing: %s").c_str(),
		m_document.FacingLeft() ? L("Left").c_str() : L("Right").c_str()).c_str());
	ImGui::SameLine();
	if (ImGui::Button(Messages.Switch_recording_side())) {
		m_document.SetFacingLeft(!m_document.FacingLeft());
	}
	ImGui::HoverTooltip(L("Use this when a recording plays back mirrored: it was made on the other side of the screen.").c_str());

	if (m_document.IsBorrowedSlot()) {
		ImGui::TextColoredWrapped(kColWarn, FormatText(
			L("Slot %d is on loan to the playback library right now. It holds runtime data, not what you recorded - editing and saving still apply to the contents that get put back.").c_str(),
			m_document.GetCfSlot()).c_str());
	}
}

TasPlaybackEditorView::Outcome TasPlaybackEditorView::Draw(float listHeight) {
	if (!m_document.IsOpen()) {
		ImGui::TextDisabled("%s", L("Nothing is open for editing.").c_str());
		return ImGui::Button(Messages.Close()) ? Outcome::Closed : Outcome::None;
	}

	Outcome outcome = Outcome::None;

	DrawSourceBar();
	ImGui::Separator();

	// Measured rather than given as a negative height: a host can be auto-fitting its
	// content, where "everything but the footer" resolves to nothing at all.
	const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f;
	const float height = listHeight > 0.0f
		? listHeight
		: (std::max)(200.0f, ImGui::GetContentRegionAvail().y - footerHeight);

	ImGui::BeginChild("##playback_frames", ImVec2(0.0f, height), false);
	m_list.Draw(m_document, false);
	ImGui::EndChild();

	ImGui::Separator();

	const bool dirty = m_document.IsDirty();
	const bool writesThrough = m_document.GetSource() == TasPlaybackDocument::Source::CfSlot;

	if (ImGui::Button(L("Trim").c_str())) {
		m_document.Trim();
		m_list.ClearSelection();
		m_status = L("Trailing idle frames removed.");
	}
	ImGui::HoverTooltip(L("Drops the idle frames off the end of the recording. Undoable like any other edit.").c_str());

	ImGui::SameLine();
	ImGui::BeginDisabled(!dirty && writesThrough);
	if (ImGui::Button(Messages.Save())) {
		if (m_document.Save()) {
			m_status = L("Saved.");
			outcome = Outcome::Saved;
		}
	}
	ImGui::EndDisabled();
	ImGui::HoverTooltipEvenDisabled(writesThrough
		? L("Writes these frames back into the recording slot.").c_str()
		: L("Keeps these frames for the dialog that opened this editor. Nothing is written until you save there too.").c_str());

	ImGui::SameLine();
	if (ImGui::Button(dirty ? L("Discard and close").c_str() : L("Close").c_str())) {
		outcome = Outcome::Closed;
	}

	if (dirty) {
		ImGui::TextColored(kColWarn, "%s", L("Unsaved changes.").c_str());
	} else if (!m_status.empty()) {
		ImGui::TextDisabled("%s", m_status.c_str());
	}

	return outcome;
}
