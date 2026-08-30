#include "TasInputListWindow.h"

#include "Core/Localization.h"
#include "Game/TasManager.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/WindowContainer/WindowContainer.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <vector>

namespace {

// What a frame contains, not where it sits: neutral recedes, a direction reads as ordinary
// input, and a button is the thing you actually scan a combo for.
const ImVec4 kColNeutral(0.48f, 0.51f, 0.56f, 1.00f);
const ImVec4 kColMovement(0.91f, 0.93f, 0.96f, 1.00f);
const ImVec4 kColAttack(1.00f, 0.78f, 0.36f, 1.00f);

const ImVec4 kColIndex(0.72f, 0.75f, 0.80f, 1.00f);
const ImVec4 kColPlayhead(0.35f, 0.70f, 1.00f, 1.00f);
const ImVec4 kColError(1.00f, 0.40f, 0.40f, 1.00f);
// Yellow reads as "pending" next to the blue playhead, so the two rules never get confused.
const ImVec4 kColDrop(1.00f, 0.85f, 0.25f, 1.00f);

// Already-played rows keep their meaning but drop back, so "where am I" is legible at a
// glance without having to find the playhead line first.
constexpr float kPlayedDim = 0.45f;

const char* kFramePayload = "TAS_FRAME_BLOCK";

ImVec4 InputColour(uint16_t packed) {
    if ((packed & 0xF0) != 0) {
        return kColAttack;
    }
    return (packed & 0x0F) == 5 ? kColNeutral : kColMovement;
}

ImVec4 Dimmed(const ImVec4& colour, bool played) {
    if (!played) {
        return colour;
    }
    return ImVec4(colour.x * kPlayedDim, colour.y * kPlayedDim, colour.z * kPlayedDim, colour.w);
}

} // namespace

void TasInputListWindow::ClearSelection() {
    m_selection.clear();
    m_hasSelectionAnchor = false;
}

void TasInputListWindow::Update() {
    TasManager& manager = TasManager::Instance();

    // The list is a companion to the editor, so it follows it: closed outright when TAS mode
    // ends, and skipped without closing while a presentation hides the TAS interface, so it
    // comes back exactly as the user left it.
    if (!manager.IsActive()) {
        if (IsOpen()) {
            Close();
        }
        ClearSelection();
        m_editingRow = -1;
        return;
    }
    if (manager.IsPlaying() && manager.IsPlaybackUiHidden()) {
        return;
    }

    IWindow::Update();
}

void TasInputListWindow::BeforeDraw() {
    // Tall and narrow by default: this is a list of frames, not a table of data.
    ImGui::SetNextWindowSize(ImVec2(300.0f, 520.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f, 200.0f), ImVec2(FLT_MAX, FLT_MAX));
}

bool TasInputListWindow::SelectionBlock(size_t index, size_t& outStart, size_t& outCount) const {
    outStart = index;
    outCount = 1;
    if (m_selection.size() < 2 || m_selection.find(index) == m_selection.end()) {
        return false;
    }

    const size_t first = *m_selection.begin();
    const size_t last = *m_selection.rbegin();
    if (last - first + 1 != m_selection.size()) {
        // A broken-up selection has no single place to land, so only the row under the
        // cursor travels.
        return false;
    }

    outStart = first;
    outCount = m_selection.size();
    return true;
}

void TasInputListWindow::HandleSelectionClick(size_t index, bool ctrlHeld, bool shiftHeld) {
    if (shiftHeld && m_hasSelectionAnchor) {
        const size_t from = (std::min)(m_selectionAnchor, index);
        const size_t to = (std::max)(m_selectionAnchor, index);
        m_selection.clear();
        for (size_t i = from; i <= to; ++i) {
            m_selection.insert(i);
        }
        return;
    }

    if (ctrlHeld) {
        if (m_selection.find(index) != m_selection.end()) {
            m_selection.erase(index);
        } else {
            m_selection.insert(index);
        }
        m_selectionAnchor = index;
        m_hasSelectionAnchor = true;
        return;
    }

    m_selection.clear();
    m_selection.insert(index);
    m_selectionAnchor = index;
    m_hasSelectionAnchor = true;
}

void TasInputListWindow::CommitCellEdit(TasManager& manager) {
    if (m_editingRow < 0) {
        return;
    }

    std::vector<uint16_t> parsed;
    const char* text = m_editBuffer[0] ? m_editBuffer : "5";
    if (TasManager::TryParseCommand(text, &parsed) && parsed.size() == 1) {
        TasFrameInput frame = manager.GetMovieFrame(static_cast<size_t>(m_editingRow));
        if (m_editingPlayer == 0) {
            frame.p1 = parsed[0];
        } else {
            frame.p2 = parsed[0];
        }
        manager.SetFrameInput(static_cast<size_t>(m_editingRow), frame);
    }

    m_editingRow = -1;
}

void TasInputListWindow::DeleteSelection(TasManager& manager) {
    // Erase from the back so the earlier indices stay valid as the movie shrinks.
    for (auto it = m_selection.rbegin(); it != m_selection.rend(); ++it) {
        manager.DeleteFrames(*it, 1);
    }
    ClearSelection();
}

void TasInputListWindow::DrawCell(TasManager& manager, size_t index, int player, uint16_t packed, bool played) {
    ImGui::PushID(player);

    const bool editing = (m_editingRow == static_cast<int>(index) && m_editingPlayer == player);
    if (editing) {
        if (m_editingJustOpened) {
            ImGui::SetKeyboardFocusHere();
            m_editingJustOpened = false;
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool done = ImGui::InputText("##cell", m_editBuffer, sizeof(m_editBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (done || ImGui::IsItemDeactivated()) {
            CommitCellEdit(manager);
        }
        ImGui::PopID();
        return;
    }

    const std::string text = TasManager::FormatInput(packed);
    ImGui::PushStyleColor(ImGuiCol_Text, Dimmed(InputColour(packed), played));
    // Transparent selectable: the cell is a click target, but the row background already
    // carries the selection highlight and a second one on top reads as noise.
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
    if (ImGui::Selectable(text.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_editingRow = static_cast<int>(index);
            m_editingPlayer = player;
            m_editingJustOpened = true;
            std::snprintf(m_editBuffer, sizeof(m_editBuffer), "%s", text.c_str());
        } else {
            const ImGuiIO& io = ImGui::GetIO();
            HandleSelectionClick(index, io.KeyCtrl, io.KeyShift);
        }
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", L("Double-click to retype this frame.").c_str());
    }
    DrawContextMenu(manager, index);

    ImGui::PopID();
}

void TasInputListWindow::DrawContextMenu(TasManager& manager, size_t index) {
    if (!ImGui::BeginPopupContextItem("##tas_row_menu")) {
        return;
    }

    // Right-clicking outside the selection acts on the row under the cursor instead.
    if (m_selection.find(index) == m_selection.end()) {
        m_selection.clear();
        m_selection.insert(index);
        m_selectionAnchor = index;
        m_hasSelectionAnchor = true;
    }

    const size_t first = m_selection.empty() ? index : *m_selection.begin();

    ImGui::TextDisabled(Messages.u_frames_selected(), static_cast<unsigned int>(m_selection.size()));
    ImGui::Separator();

    if (ImGui::MenuItem(L("Go to this frame").c_str())) {
        manager.SeekToFrame(index);
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt(L("Frames").c_str(), &m_insertCount);
    if (m_insertCount < 1) {
        m_insertCount = 1;
    }
    if (ImGui::MenuItem(L("Insert neutral above").c_str())) {
        manager.InsertNeutralFrames(first, static_cast<size_t>(m_insertCount));
        ClearSelection();
    }
    if (ImGui::MenuItem(L("Insert neutral below").c_str())) {
        manager.InsertNeutralFrames(index + 1, static_cast<size_t>(m_insertCount));
        ClearSelection();
    }

    ImGui::Separator();
    if (ImGui::MenuItem(L("Duplicate").c_str())) {
        size_t start = index;
        size_t count = 1;
        SelectionBlock(index, start, count);
        manager.DuplicateFrames(start, count);
        ClearSelection();
    }
    if (ImGui::MenuItem(L("Delete").c_str(), "Del")) {
        DeleteSelection(manager);
    }

    ImGui::Separator();
    if (ImGui::MenuItem(L("Undo").c_str(), "Ctrl+Z", false, manager.CanUndo())) {
        manager.Undo();
        ClearSelection();
    }
    if (ImGui::MenuItem(L("Redo").c_str(), "Ctrl+Y", false, manager.CanRedo())) {
        manager.Redo();
        ClearSelection();
    }

    ImGui::EndPopup();
}

void TasInputListWindow::SelectRange(size_t start, size_t count) {
    m_selection.clear();
    for (size_t i = 0; i < count; ++i) {
        m_selection.insert(start + i);
    }
    m_selectionAnchor = start;
    m_hasSelectionAnchor = count > 0;
}

void TasInputListWindow::DrawRow(TasManager& manager, size_t index, size_t playhead, size_t count,
    Rule& playheadRule, Rule& dropRule) {

    const TasFrameInput frame = manager.GetMovieFrame(index);
    const bool played = index < playhead;
    const bool selected = m_selection.find(index) != m_selection.end();

    ImGui::TableNextRow();
    if (played) {
        // Darken the whole row, frame number included, so played frames read as spent
        // without losing the colour that says what they were.
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
            ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.30f)));
    }
    if (selected) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header, 0.85f));
    }

    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(static_cast<int>(index));

    char label[32];
    std::snprintf(label, sizeof(label), "%u", static_cast<unsigned int>(index));
    ImGui::PushStyleColor(ImGuiCol_Text, Dimmed(kColIndex, played));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    if (ImGui::Selectable(label, false, 0)) {
        const ImGuiIO& io = ImGui::GetIO();
        HandleSelectionClick(index, io.KeyCtrl, io.KeyShift);
        if (!io.KeyCtrl && !io.KeyShift) {
            manager.SeekToFrame(index);
        }
    }
    ImGui::PopStyleColor(2);

    // Reordering. The payload names the row that was grabbed; what actually travels is
    // decided on the drop, so a contiguous multi-selection moves as one block.
    if (ImGui::BeginDragDropSource(0)) {
        const size_t payload = index;
        ImGui::SetDragDropPayload(kFramePayload, &payload, sizeof(payload));
        size_t start = index;
        size_t blockCount = 1;
        SelectionBlock(index, start, blockCount);
        ImGui::Text(Messages.Move_u_frames(), static_cast<unsigned int>(blockCount));
        ImGui::EndDragDropSource();
    }
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    const float rowRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

    if (ImGui::BeginDragDropTarget()) {
        // AcceptBeforeDelivery hands the payload over while the mouse is still down, which is
        // what lets the drop rule be drawn before anything is committed.
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFramePayload,
            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {

            // Which half of the row the cursor is over decides whether the block lands above
            // or below it, so the very end of the movie is reachable and the rule sits
            // exactly where the frames will go.
            const bool below = ImGui::GetMousePos().y > (rowMin.y + rowMax.y) * 0.5f;
            const size_t target = below ? index + 1 : index;

            const size_t from = *static_cast<const size_t*>(payload->Data);
            size_t start = from;
            size_t blockCount = 1;
            SelectionBlock(from, start, blockCount);

            // Landing inside the block being dragged, or exactly where it already sits,
            // changes nothing - so do not promise a move that will not happen.
            const bool noop = (target >= start && target <= start + blockCount);
            if (!noop) {
                dropRule.y = below ? rowMax.y : rowMin.y;
                dropRule.left = rowMin.x;
                dropRule.right = rowRight;
            }

            if (payload->IsDelivery() && !noop) {
                size_t landedAt = start;
                if (manager.MoveFrames(start, blockCount, target, &landedAt)) {
                    // Keep the moved frames selected so they can be dragged again or acted
                    // on straight away.
                    SelectRange(landedAt, blockCount);
                }
                // A refused move leaves the selection alone: nothing changed, so the user's
                // selection should not vanish either.
            }
        }
        ImGui::EndDragDropTarget();
    }

    DrawContextMenu(manager, index);

    if (index == playhead) {
        playheadRule.y = rowMin.y;
        playheadRule.left = rowMin.x;
        playheadRule.right = rowRight;
    } else if (playhead >= count && index + 1 == count) {
        // Everything has been played: the rule belongs under the last row.
        playheadRule.y = rowMax.y;
        playheadRule.left = rowMin.x;
        playheadRule.right = rowRight;
    }

    ImGui::TableSetColumnIndex(1);
    DrawCell(manager, index, 0, frame.p1, played);

    ImGui::TableSetColumnIndex(2);
    DrawCell(manager, index, 1, frame.p2, played);

    ImGui::PopID();
}

void TasInputListWindow::Draw() {
    TasManager& manager = TasManager::Instance();

    if (!manager.IsActive()) {
        ImGui::TextDisabled("%s", L("TAS mode is not active.").c_str());
        return;
    }

    const size_t count = manager.GetFrameCount();
    const size_t playhead = manager.GetCursor();
    const bool seeking = manager.IsSeeking();

    ImGui::Checkbox(L("Follow playhead").c_str(), &m_followPlayhead);
    ImGui::SameLine();
    ImGui::ShowHelpMarker(L("Keeps the list scrolled to the current frame. Turn it off to read through the movie while it plays.").c_str());

    if (count == 0) {
        ImGui::TextDisabled("%s", L("No frames yet.").c_str());
        return;
    }

    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused && m_editingRow < 0 && !m_selection.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        DeleteSelection(manager);
    }

    ImGui::BeginChild("##tas_input_rows", ImVec2(0.0f, 0.0f), false);

    Rule playheadRule;
    Rule dropRule;

    if (ImGui::BeginTable("##tas_input_table", 3,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("P1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("P2", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // A movie can run to thousands of frames; only build the rows actually on screen.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(count));

        // The playhead row has to be built for SetScrollHereY to have something to scroll
        // to. After frames are appended it sits well outside the clipped range, which is
        // exactly why following it used to fail at the moment the movie grew.
        const size_t followRow = (playhead >= count) ? count - 1 : playhead;
        const bool playheadMoved = (!m_hasLastPlayhead || m_lastPlayhead != playhead);
        if (m_followPlayhead && playheadMoved) {
            clipper.IncludeItemByIndex(static_cast<int>(followRow));
        }

        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const size_t index = static_cast<size_t>(row);
                DrawRow(manager, index, playhead, count, playheadRule, dropRule);

                if (index == followRow && m_followPlayhead && playheadMoved) {
                    ImGui::SetScrollHereY(0.5f);
                }
            }
        }

        ImGui::EndTable();
    }

    const float headerBottom = ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMin().y
        + ImGui::GetTextLineHeightWithSpacing();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const auto drawRule = [&](const Rule& rule, const ImVec4& colour, float thickness) {
        if (!rule.valid() || rule.y < headerBottom) {
            return;
        }
        const ImU32 packed = ImGui::ColorConvertFloat4ToU32(colour);
        drawList->AddLine(ImVec2(rule.left, rule.y), ImVec2(rule.right, rule.y), packed, thickness);
        // A caret on the left edge so the rule reads as a position rather than a separator.
        drawList->AddTriangleFilled(
            ImVec2(rule.left, rule.y - 5.0f),
            ImVec2(rule.left, rule.y + 5.0f),
            ImVec2(rule.left + 7.0f, rule.y),
            packed);
    };

    drawRule(playheadRule, kColPlayhead, 2.0f);
    // Drawn last and heavier: while dragging, this is the line the eye needs.
    drawRule(dropRule, kColDrop, 3.0f);

    ImGui::EndChild();

    m_lastPlayhead = playhead;
    m_hasLastPlayhead = true;

    if (seeking) {
        ImGui::TextColored(kColPlayhead, "%s", L("Seeking...").c_str());
    } else if (!manager.GetError().empty()) {
        ImGui::TextColored(kColError, "%s", manager.GetError().c_str());
    }
}
