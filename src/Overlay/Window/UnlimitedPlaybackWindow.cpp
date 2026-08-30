#include "UnlimitedPlaybackWindow.h"

#include "Core/NativeFileDialog.h"

#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Core/HotkeyManager.h"
#include "Overlay/Widget/HotkeyBindWidget.h"
#include "Game/Playbacks/UnlimitedPlaybackManager.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Window/PlaybackEditorWindow.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include "Overlay/WindowContainer/WindowType.h"

#include <Windows.h>
#include <commdlg.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <algorithm>
#include <set>
#include <vector>

namespace {
const char* kUnlimitedPlaybackProfileFolder = "BBCF_IM/unlimited_playbacks/profiles";
const char* kUnlimitedPlaybackImportFolder = "BBCF_IM/unlimited_playbacks/imports";
const char* kUnlimitedPlaybackExportFolder = "BBCF_IM/unlimited_playbacks/exports";
const char* kUnlimitedPlaybackDragDropPayload = "UP_SLOT";
std::string NormalizePlaybackFileName(const char* input) {
    std::string out;
    const char* source = input ? input : "";
    for (const char* p = source; *p != '\0'; ++p) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.') {
            out.push_back(c);
        } else if (c == ' ') {
            out.push_back('_');
        }
    }
    if (out.empty()) {
        return "";
    }
    if (out.size() < 9 || out.substr(out.size() - 9) != ".playback") {
        out += ".playback";
    }
    return out;
}

const char* TriggerLabel(UnlimitedPlaybackManager::TriggerType t) {
    switch (t) {
    case UnlimitedPlaybackManager::Trigger_Wakeup: return L("Wakeup").c_str();
    case UnlimitedPlaybackManager::Trigger_Gap: return L("Gap").c_str();
    case UnlimitedPlaybackManager::Trigger_OnBlock: return L("On Block").c_str();
    case UnlimitedPlaybackManager::Trigger_OnHit: return L("On Hit").c_str();
    case UnlimitedPlaybackManager::Trigger_ThrowTech: return L("Throw Tech").c_str();
    case UnlimitedPlaybackManager::Trigger_KeyPress: return L("Key Press").c_str();
    case UnlimitedPlaybackManager::Trigger_OnLoop: return L("On loop").c_str();
    default: return L("Unknown").c_str();
    }
}

const char* SelectionModeLabel(int mode) {
    switch (mode) {
    case UnlimitedPlaybackManager::Selection_Random: return L("Random").c_str();
    case UnlimitedPlaybackManager::Selection_Sequential: return L("Sequential").c_str();
    case UnlimitedPlaybackManager::Selection_NonRepeatingRandom: return L("Non-repeating Random").c_str();
    default: return L("Random").c_str();
    }
}

const char* LoopResetModeLabel(int mode) {
    switch (mode) {
    case UnlimitedPlaybackManager::LoopReset_Middle: return L("Middle").c_str();
    case UnlimitedPlaybackManager::LoopReset_Left: return L("Left").c_str();
    case UnlimitedPlaybackManager::LoopReset_Right: return L("Right").c_str();
    case UnlimitedPlaybackManager::LoopReset_Custom: return L("Custom Snapshot").c_str();
    default: return L("Middle").c_str();
    }
}

// Renders the shared rebind control for one hotkey action and persists any change straight
// away. Unlike the Settings window there is no draft/Cancel here: this panel is a live
// training tool, so a rebind takes effect the moment it is made.
void DrawPlaybackHotkeyBind(UnlimitedPlaybackManager& mgr, HotkeyManager::Action action) {
    HotkeyBinding binding = HotkeyManager::GetBinding(action);
    const HotkeyManager::Action conflict = HotkeyManager::FindConflict(binding, action);
    std::string warning;
    if (conflict != HotkeyManager::Hotkey_Count) {
        warning = FormatText(L("Already used by \"%s\". Pressing it will do both.").c_str(),
            HotkeyManager::DisplayName(conflict));
    } else if (HotkeyManager::IsControllerBinding(binding)) {
        warning = L("Controller button: this also works during a match, so pick one you never press while playing.");
    }

    if (ImGuiHotkey::BindWidget(HotkeyManager::IniKey(action), binding,
            HotkeyManager::DefaultBindingString(action), warning.c_str())) {
        HotkeyManager::SetBinding(action, binding);
        // The playback trigger must not fire on the very press that assigned it.
        mgr.ForceResetTriggers("");
        mgr.PushToast(FormatText(L("Mapped bind: %s").c_str(),
            HotkeyManager::DisplayString(binding).c_str()));
    }
}

enum class NativeFileDialogAction {
    None,
    LoadProfile,
    SaveProfile,
    ImportPlayback,
    ExportEntryPlayback,
};

const char* kNativeFileDialogToastKey = "up_native_file_dialog";
constexpr const char* kFileDialogOwner = "unlimited_playback_window";

// The shared picker carries one opaque int, which the action claims, so the entry an
// export belongs to is parked here alongside it.
int g_pendingExportEntryIndex = -1;

NativeFileDialog::Request BuildFileDialogRequest(NativeFileDialogAction action, const std::string& initialPath) {
    NativeFileDialog::Request request;
    request.contextId = static_cast<int>(action);

    switch (action) {
    case NativeFileDialogAction::LoadProfile:
        request.title = L("Load unlimited playback profile");
        request.filters.push_back({ "Unlimited Playback Profile (*.upl)", "*.upl" });
        request.defaultExtension = "upl";
        break;
    case NativeFileDialogAction::SaveProfile:
        request.save = true;
        request.title = L("Save unlimited playback profile");
        request.filters.push_back({ "Unlimited Playback Profile (*.upl)", "*.upl" });
        request.defaultExtension = "upl";
        break;
    case NativeFileDialogAction::ImportPlayback:
        request.title = L("Import unlimited playback entry");
        request.filters.push_back({ "Unlimited Playback Entry (*.playback)", "*.playback" });
        request.defaultExtension = "playback";
        break;
    case NativeFileDialogAction::ExportEntryPlayback:
        request.save = true;
        request.title = L("Export unlimited playback entry");
        request.filters.push_back({ "Unlimited Playback Entry (*.playback)", "*.playback" });
        request.defaultExtension = "playback";
        break;
    default:
        break;
    }

    request.initialPath = initialPath.empty() ? kUnlimitedPlaybackProfileFolder : initialPath;
    return request;
}

bool DrawContextButton(const char* label, bool enabled) {
    if (!enabled) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.20f, 0.60f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.20f, 0.20f, 0.60f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.20f, 0.20f, 0.60f));
        ImGui::Button(label);
        ImGui::PopStyleColor(3);
        return false;
    }
    return ImGui::Button(label);
}

void DrawSectionTitle(const char* title) {
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.25f));
    const ImVec2 textSize = ImGui::CalcTextSize(title);
    const float textX = ImGui::GetCursorPosX() + ((ImGui::GetContentRegionAvail().x - textSize.x) * 0.5f);
    const float textY = ImGui::GetCursorPosY();
    ImGui::SetCursorPosX((std::max)(0.0f, textX));
    ImGui::TextUnformatted(title);
    const ImVec2 baseMin = ImGui::GetItemRectMin();
    ImGui::SetCursorScreenPos(ImVec2(baseMin.x + 0.75f, baseMin.y));
    ImGui::TextUnformatted(title);
    ImGui::SetCursorScreenPos(ImVec2(baseMin.x, baseMin.y + textSize.y));
    ImGui::Separator();
}

float ComputeSectionTitleHeight() {
    return
        (ImGui::GetStyle().ItemSpacing.y * 0.25f) +
        ImGui::GetTextLineHeightWithSpacing() +
        ImGui::GetStyle().ItemSpacing.y;
}

bool DrawMiniIconButton(const char* label, bool enabled) {
    if (enabled) {
        return ImGui::SmallButton(label);
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.20f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.20f, 0.20f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.20f, 0.20f, 0.60f));
    ImGui::SmallButton(label);
    ImGui::PopStyleColor(3);
    return false;
}

int ComputeButtonsPerRow(float buttonWidth, int buttonCount) {
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const int perRow = static_cast<int>((availableWidth + spacing) / (buttonWidth + spacing));
    return (std::max)(1, (std::min)(buttonCount, perRow));
}

float ComputeWrappedTextHeight(const char* text) {
    const float width = ImGui::GetContentRegionAvail().x;
    return ImGui::CalcTextSize(text, nullptr, false, width).y;
}

int ComputeWrappedLineCount(const char* text) {
    const float lineHeight = (std::max)(1.0f, ImGui::GetTextLineHeight());
    return (std::max)(1, static_cast<int>((ComputeWrappedTextHeight(text) + (lineHeight * 0.25f)) / lineHeight));
}

void DrawButtonTooltip(const char* text);

void DrawWrappedButtonRow(const char* const* labels, const char* const* tooltips, int buttonCount, float buttonWidth, bool* outPressedStates) {
    const int buttonsPerRow = ComputeButtonsPerRow(buttonWidth, buttonCount);
    for (int rowStart = 0; rowStart < buttonCount; rowStart += buttonsPerRow) {
        const int rowCount = (std::min)(buttonsPerRow, buttonCount - rowStart);
        const float rowWidth =
            (buttonWidth * static_cast<float>(rowCount)) +
            (ImGui::GetStyle().ItemSpacing.x * static_cast<float>((std::max)(0, rowCount - 1)));
        ImGui::AlignItemHorizontalCenter(rowWidth);
        for (int rowOffset = 0; rowOffset < rowCount; ++rowOffset) {
            const int i = rowStart + rowOffset;
            if (rowOffset > 0) {
                ImGui::SameLine();
            }
            outPressedStates[i] = ImGui::Button(labels[i], ImVec2(buttonWidth, 0.0f));
            if (tooltips && tooltips[i]) {
                DrawButtonTooltip(tooltips[i]);
            }
        }
    }
}

void DrawHelpInline(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

void DrawButtonTooltip(const char* text) {
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

void DrawCenteredPopupText(const char* text) {
    if (!text) {
        return;
    }
    const float textWidth = ImGui::CalcTextSize(text).x;
    const float textX = (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - textWidth) * 0.5f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textX);
    ImGui::TextUnformatted(text);
}

void CenterNextButtonsRow(float totalWidth) {
    const float offset = (std::max)(0.0f, (ImGui::GetContentRegionAvail().x - totalWidth) * 0.5f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
}

void DrawContextMenuHeader(const char* text) {
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.58f, 0.58f, 1.0f));
    ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 0.86f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::PopStyleColor();
}

int AdjustSelectedEntryAfterMove(int selectedEntry, int fromIndex, int toIndex) {
    if (selectedEntry < 0 || fromIndex == toIndex) {
        return selectedEntry;
    }
    if (selectedEntry == fromIndex) {
        return toIndex;
    }
    if (fromIndex < toIndex && selectedEntry > fromIndex && selectedEntry <= toIndex) {
        return selectedEntry - 1;
    }
    if (toIndex < fromIndex && selectedEntry >= toIndex && selectedEntry < fromIndex) {
        return selectedEntry + 1;
    }
    return selectedEntry;
}

// Click-selection helper shared by the library slot list. `ctrlHeld` toggles the clicked
// slot in/out of the selection; `shiftHeld` selects the contiguous range from `anchor` to
// `clickedIndex` (replacing the current selection), matching common file-explorer behavior.
void ApplySlotSelectionClick(std::set<int>& selection, int& anchor, int clickedIndex, bool ctrlHeld, bool shiftHeld) {
    if (ctrlHeld) {
        if (selection.count(clickedIndex)) {
            selection.erase(clickedIndex);
        } else {
            selection.insert(clickedIndex);
        }
        anchor = clickedIndex;
    } else if (shiftHeld && anchor >= 0) {
        selection.clear();
        const int lo = (std::min)(anchor, clickedIndex);
        const int hi = (std::max)(anchor, clickedIndex);
        for (int k = lo; k <= hi; ++k) {
            selection.insert(k);
        }
    } else {
        selection.clear();
        selection.insert(clickedIndex);
        anchor = clickedIndex;
    }
}

int ComputeMoveTargetFromInsertionIndex(int fromIndex, int insertionIndex, int entryCount) {
    if (fromIndex < 0 || fromIndex >= entryCount || insertionIndex < 0 || insertionIndex > entryCount) {
        return fromIndex;
    }

    int toIndex = insertionIndex;
    if (toIndex > fromIndex) {
        --toIndex;
    }
    if (toIndex < 0) {
        toIndex = 0;
    }
    if (toIndex >= entryCount) {
        toIndex = entryCount - 1;
    }
    return toIndex;
}

bool TrainingMatchAvailable() {
    return g_gameVals.pGameMode &&
        g_gameVals.pGameState &&
        (*g_gameVals.pGameMode == GameMode_Training) &&
        (*g_gameVals.pGameState == GameState_InMatch) &&
        (GetGameSceneStatus() >= GameSceneStatus_Running) &&
        !g_interfaces.player2.IsCharDataNullPtr();
}
}

void UnlimitedPlaybackWindow::BeforeDraw() {
    ImGui::SetNextWindowSize(ImVec2(980, 680), ImGuiCond_FirstUseEver);
}

void UnlimitedPlaybackWindow::Draw() {
    auto& mgr = UnlimitedPlaybackManager::Instance();
    mgr.InitializeIfNeeded();
    mgr.PruneExpiredToasts();

    const auto beginNativeDialog = [&mgr](NativeFileDialogAction action, const std::string& initialPath, const char* activityText, int contextIndex = -1) {
        if (NativeFileDialog::IsOpen()) {
            mgr.PushToast(L("A native file dialog is already open."));
            return false;
        }
        g_pendingExportEntryIndex = contextIndex;
        if (!NativeFileDialog::Open(kFileDialogOwner, BuildFileDialogRequest(action, initialPath))) {
            return false;
        }
        mgr.PushStickyToast(kNativeFileDialogToastKey, activityText);
        return true;
    };

    static std::set<int> selectedEntries;
    static int selectionAnchor = -1;
    static std::vector<int> entriesPendingDelete;
    static int entryPendingEdit = -1;
    static int entryPendingSend = -1;
    static int entryPendingSetIndex = -1;
    static int entrySetIndexValue = 1;
    static int selectedTriggerType = UnlimitedPlaybackManager::Trigger_Wakeup;
    static char captureName[128] = "";
    static char replayCaptureName[128] = "";
    static char editEntryName[128] = "";
    static float editEntryWeight = 1.0f;
    static int captureSlot = 1;
    static int sendSlot = 1;
    static bool showProfileCompatibilityPopup = false;
    static bool profileCompatibilityCanForce = false;
    static char pendingProfilePath[MAX_PATH] = {};
    static CompatibilityManager::Result pendingProfileCompatibility = {};
    static bool showPlaybackCompatibilityPopup = false;
    static bool playbackCompatibilityCanForce = false;
    static char pendingPlaybackPath[MAX_PATH] = {};
    static CompatibilityManager::Result pendingPlaybackCompatibility = {};
    static bool openCaptureSlotModal = false;
    static bool openReplayCaptureModal = false;
    static bool openEntryEditModal = false;
    static bool openEntryPlaybackEditorModal = false;
    static bool openSendToSlotModal = false;
    static bool openSetIndexModal = false;
    static bool openDefaultConfirmModal = false;
    static bool openDeleteEntryConfirmModal = false;
    const bool inTrainingMatch = TrainingMatchAvailable();
    const bool inReplayMatch =
        g_gameVals.pGameMode && g_gameVals.pGameState &&
        (*g_gameVals.pGameMode == GameMode_ReplayTheater) &&
        (*g_gameVals.pGameState == GameState_InMatch) &&
        !g_interfaces.player1.IsCharDataNullPtr() &&
        !g_interfaces.player2.IsCharDataNullPtr();

    NativeFileDialogAction completedDialogAction = NativeFileDialogAction::None;
    std::string completedDialogPath;
    bool completedDialogCanceled = true;
    const int completedDialogContextIndex = g_pendingExportEntryIndex;
    NativeFileDialog::Result completedDialogResult;
    if (NativeFileDialog::Consume(kFileDialogOwner, &completedDialogResult)) {
        completedDialogAction = static_cast<NativeFileDialogAction>(completedDialogResult.contextId);
        completedDialogPath = completedDialogResult.path;
        completedDialogCanceled = !completedDialogResult.accepted;
        mgr.RemoveStickyToast(kNativeFileDialogToastKey);
    }
    if (!completedDialogCanceled && !completedDialogPath.empty()) {
        if (completedDialogAction == NativeFileDialogAction::LoadProfile) {
            auto compatibility = mgr.ProbeProfileCompatibility(completedDialogPath);
            if (compatibility.action == CompatibilityManager::Action_Load) {
                mgr.LoadProfile(completedDialogPath);
            } else {
                std::strncpy(pendingProfilePath, completedDialogPath.c_str(), MAX_PATH - 1);
                pendingProfilePath[MAX_PATH - 1] = '\0';
                pendingProfileCompatibility = compatibility;
                profileCompatibilityCanForce = compatibility.canForce;
                showProfileCompatibilityPopup = true;
            }
        } else if (completedDialogAction == NativeFileDialogAction::SaveProfile) {
            mgr.SaveProfile(completedDialogPath);
        } else if (completedDialogAction == NativeFileDialogAction::ImportPlayback) {
            auto compatibility = mgr.ProbePlaybackCompatibility(completedDialogPath);
            if (compatibility.action == CompatibilityManager::Action_Load) {
                mgr.AddPlaybackFile(completedDialogPath, "");
            } else {
                std::strncpy(pendingPlaybackPath, completedDialogPath.c_str(), MAX_PATH - 1);
                pendingPlaybackPath[MAX_PATH - 1] = '\0';
                pendingPlaybackCompatibility = compatibility;
                playbackCompatibilityCanForce = compatibility.canForce;
                showPlaybackCompatibilityPopup = true;
            }
        } else if (completedDialogAction == NativeFileDialogAction::ExportEntryPlayback) {
            if (completedDialogContextIndex >= 0 && completedDialogContextIndex < static_cast<int>(mgr.GetEntries().size())) {
                mgr.SaveEntryToFile(static_cast<size_t>(completedDialogContextIndex), completedDialogPath);
            }
        }
    }

    if (showProfileCompatibilityPopup) {
        const ImVec2 displayCenter = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(displayCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
        ImGui::OpenPopup(L("Profile Compatibility").c_str());
        showProfileCompatibilityPopup = false;
    }
    if (ImGui::BeginPopupModal(L("Profile Compatibility").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            L("Profile version mismatch.\n\nFile version: %s\nCode version: %s\n\n%s").c_str(),
            CompatibilityManager::ToString(pendingProfileCompatibility.detected).c_str(),
            CompatibilityManager::ToString(pendingProfileCompatibility.current).c_str(),
            pendingProfileCompatibility.reason.c_str());

        if (profileCompatibilityCanForce) {
            CenterNextButtonsRow(220.0f + ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::Button(L("Load Anyway").c_str())) {
                mgr.LoadProfile(pendingProfilePath, true);
                pendingProfilePath[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(L("Cancel").c_str())) {
                pendingProfilePath[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
        } else {
            CenterNextButtonsRow(90.0f);
            if (ImGui::Button(L("OK").c_str())) {
                pendingProfilePath[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (showPlaybackCompatibilityPopup) {
        const ImVec2 displayCenter = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(displayCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
        ImGui::OpenPopup(L("Playback Compatibility").c_str());
        showPlaybackCompatibilityPopup = false;
    }
    if (ImGui::BeginPopupModal(L("Playback Compatibility").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            L("Playback version mismatch.\n\nFile version: %s\nCode version: %s\n\n%s").c_str(),
            CompatibilityManager::ToString(pendingPlaybackCompatibility.detected).c_str(),
            CompatibilityManager::ToString(pendingPlaybackCompatibility.current).c_str(),
            pendingPlaybackCompatibility.reason.c_str());

        if (playbackCompatibilityCanForce) {
            CenterNextButtonsRow(230.0f + ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::Button(L("Import Anyway").c_str())) {
                mgr.AddPlaybackFile(pendingPlaybackPath, "", true);
                pendingPlaybackPath[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(FormatText("%s##playback_compat", L("Cancel").c_str()).c_str())) {
                pendingPlaybackPath[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
        } else {
            CenterNextButtonsRow(140.0f);
            if (ImGui::Button(FormatText("%s##playback_compat", L("OK").c_str()).c_str())) {
                pendingPlaybackPath[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::BeginChild("up_main", ImVec2(0, 0), false);
    ImGui::Columns(2);

    ImGui::BeginChild("up_left_column", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float captureButtonWidth = 128.0f;
    const char* captureButtons[] = {
        L("Add from CF Slot").c_str(),
        L("Add from file").c_str(),
        L("Add from replay").c_str()
    };
    const char* captureButtonTooltips[] = {
        L("Capture a playback from one of the 4 CF slots").c_str(),
        L("Import a playback file into the library").c_str(),
        L("Record a new playback entry from a replay").c_str()
    };
    bool capturePressed[3] = {};
    const int captureButtonsPerRow = ComputeButtonsPerRow(captureButtonWidth, 3);
    const int captureButtonRows = (3 + captureButtonsPerRow - 1) / captureButtonsPerRow;
    const float childVerticalPadding = ImGui::GetStyle().WindowPadding.y * 2.0f;
    const float captureSectionHeight =
        ComputeSectionTitleHeight() +
        childVerticalPadding +
        ImGui::GetFrameHeightWithSpacing() +
        (ImGui::GetFrameHeight() * static_cast<float>(captureButtonRows)) +
        (ImGui::GetStyle().ItemSpacing.y * static_cast<float>((std::max)(1, captureButtonRows)));
    const float leftColumnAvailableHeight = ImGui::GetContentRegionAvail().y;
    const float librarySectionHeight = (std::max)(64.0f, leftColumnAvailableHeight - captureSectionHeight - ImGui::GetStyle().ItemSpacing.y);
    ImGui::BeginChild("up_library", ImVec2(0, librarySectionHeight), true);
    DrawSectionTitle(FormatText(L("Library (%d)").c_str(), static_cast<int>(mgr.GetEntries().size())).c_str());
    if (!mgr.GetActiveProfilePath().empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColoredAlignedHorizontalCenter(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), mgr.GetActiveProfilePath().c_str());
        ImGui::PopTextWrapPos();
    }
    const float libraryButtonsWidth = 86.0f;
    const int libraryButtonsPerRow = ComputeButtonsPerRow(libraryButtonsWidth, 3);
    const int libraryButtonRows = (3 + libraryButtonsPerRow - 1) / libraryButtonsPerRow;
    const float libraryControlsHeight =
        (ImGui::GetFrameHeight() * static_cast<float>(libraryButtonRows)) +
        ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("up_library_entries", ImVec2(0, -libraryControlsHeight), true);
    const auto& entries = mgr.GetEntries();
    std::vector<int> entryMoveFromList;
    int entryMoveInsertion = -1;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        const auto& e = entries[i];
        ImGui::PushID(i);
        const float iconButtonWidth = 24.0f;
        const float rowSpacing = ImGui::GetStyle().ItemSpacing.x;
        const float controlsWidth = (iconButtonWidth * 3.0f) + (rowSpacing * 2.0f);
        bool enabled = e.enabled;
        if (ImGui::Checkbox("##enabled", &enabled)) {
            if (selectedEntries.size() > 1 && selectedEntries.count(i)) {
                std::vector<size_t> indices(selectedEntries.begin(), selectedEntries.end());
                mgr.SetEntriesEnabled(indices, enabled);
            } else {
                mgr.GetEntriesMutable()[i].enabled = enabled;
            }
        }
        ImGui::SameLine();
        const float labelWidth = (std::max)(1.0f, ImGui::GetContentRegionAvail().x - controlsWidth - 8.0f);
        if (ImGui::Selectable(e.name.c_str(), selectedEntries.count(i) != 0, 0, ImVec2(labelWidth, 0.0f))) {
            const bool ctrlHeld = ImGui::GetIO().KeyCtrl;
            const bool shiftHeld = ImGui::GetIO().KeyShift;
            ApplySlotSelectionClick(selectedEntries, selectionAnchor, i, ctrlHeld, shiftHeld);
        }
        const ImVec2 slotMin = ImGui::GetItemRectMin();
        const ImVec2 slotMax = ImGui::GetItemRectMax();
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            std::vector<int> dragIndices;
            if (selectedEntries.size() > 1 && selectedEntries.count(i)) {
                dragIndices.assign(selectedEntries.begin(), selectedEntries.end());
            } else {
                dragIndices.assign({ i });
                selectedEntries.clear();
                selectedEntries.insert(i);
                selectionAnchor = i;
            }
            ImGui::SetDragDropPayload(
                kUnlimitedPlaybackDragDropPayload,
                dragIndices.data(),
                dragIndices.size() * sizeof(int));
            if (dragIndices.size() > 1) {
                ImGui::Text("%s", FormatText(L("Dragged %d slots").c_str(), static_cast<int>(dragIndices.size())).c_str());
            } else {
                ImGui::Text("%s", FormatText(L("Dragged slot: %s").c_str(), e.name.c_str()).c_str());
            }
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            const ImGuiDragDropFlags flags =
                ImGuiDragDropFlags_AcceptBeforeDelivery |
                ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kUnlimitedPlaybackDragDropPayload, flags)) {
                if (payload->DataSize > 0 && payload->DataSize % sizeof(int) == 0) {
                    const int* fromData = static_cast<const int*>(payload->Data);
                    const int fromCount = payload->DataSize / static_cast<int>(sizeof(int));
                    const int entryCount = static_cast<int>(entries.size());
                    bool allValid = fromCount > 0;
                    for (int k = 0; k < fromCount && allValid; ++k) {
                        if (fromData[k] < 0 || fromData[k] >= entryCount) {
                            allValid = false;
                        }
                    }
                    if (allValid) {
                        const float midpointY = (slotMin.y + slotMax.y) * 0.5f;
                        const bool insertAfterHovered = ImGui::GetIO().MousePos.y >= midpointY;
                        const int insertionIndex = i + (insertAfterHovered ? 1 : 0);
                        const float y = insertAfterHovered ? slotMax.y : slotMin.y;
                        ImGui::GetWindowDrawList()->AddLine(
                            ImVec2(slotMin.x, y),
                            ImVec2(slotMax.x, y),
                            IM_COL32(120, 200, 255, 220),
                            1.5f);
                        if (payload->IsDelivery()) {
                            entryMoveFromList.assign(fromData, fromData + fromCount);
                            entryMoveInsertion = insertionIndex;
                        }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("entry_context")) {
            DrawContextMenuHeader(FormatText(L("Current Slot - %s").c_str(), e.name.c_str()).c_str());
            if (ImGui::MenuItem(L("Send to CF Slot").c_str(), nullptr, false, inTrainingMatch)) {
                entryPendingSend = i;
                openSendToSlotModal = true;
            }
            if (ImGui::MenuItem(L("Save to File").c_str())) {
                const std::string initialExportPath =
                    std::string(kUnlimitedPlaybackExportFolder) + "/" + NormalizePlaybackFileName(e.name.c_str());
                beginNativeDialog(
                    NativeFileDialogAction::ExportEntryPlayback,
                    initialExportPath,
                    L("Export playback entry file dialog open...").c_str(),
                    i);
            }
            std::vector<int> moveGroup;
            if (selectedEntries.size() > 1 && selectedEntries.count(i)) {
                moveGroup.assign(selectedEntries.begin(), selectedEntries.end());
            } else {
                moveGroup.assign({ i });
            }
            std::sort(moveGroup.begin(), moveGroup.end());
            const int moveGroupMin = moveGroup.front();
            const int moveGroupMax = moveGroup.back();
            const bool canMoveUp = entries.size() > 1 && moveGroupMin > 0;
            const bool canMoveDown = entries.size() > 1 && moveGroupMax < static_cast<int>(entries.size()) - 1;
            if (ImGui::MenuItem(L("Move up").c_str(), nullptr, false, canMoveUp)) {
                entryMoveFromList = moveGroup;
                entryMoveInsertion = moveGroupMin - 1;
            }
            if (ImGui::MenuItem(L("Move down").c_str(), nullptr, false, canMoveDown)) {
                entryMoveFromList = moveGroup;
                entryMoveInsertion = moveGroupMax + 2;
            }
            if (ImGui::MenuItem(L("Set index...").c_str(), nullptr, false, entries.size() > 1)) {
                entryPendingSetIndex = i;
                entrySetIndexValue = i + 1;
                openSetIndexModal = true;
            }
            DrawContextMenuHeader(L("Slot list").c_str());
            if (ImGui::MenuItem(L("Turn ALL slots off").c_str())) {
                mgr.SetAllEntriesEnabled(false);
            }
            if (ImGui::MenuItem(L("Turn ALL slots on").c_str())) {
                mgr.SetAllEntriesEnabled(true);
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - controlsWidth - 2.0f);
        if (DrawMiniIconButton(">", inTrainingMatch)) {
            mgr.PlayEntryNow(static_cast<size_t>(i));
        }
        if (inTrainingMatch) {
            DrawButtonTooltip(L("Play").c_str());
        } else if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", L("Play (Disabled due to not being in lab)").c_str());
        }
        ImGui::SameLine();
        if (DrawMiniIconButton("/", true)) {
            entryPendingEdit = i;
            openEntryEditModal = true;
        }
        DrawButtonTooltip(L("Edit").c_str());
        ImGui::SameLine();
        if (DrawMiniIconButton("X", true)) {
            if (selectedEntries.size() > 1 && selectedEntries.count(i)) {
                entriesPendingDelete.assign(selectedEntries.begin(), selectedEntries.end());
            } else {
                entriesPendingDelete.assign({ i });
            }
            openDeleteEntryConfirmModal = true;
        }
        DrawButtonTooltip(L("Delete").c_str());
        ImGui::PopID();
    }
    if (!entryMoveFromList.empty() && entryMoveInsertion >= 0) {
        std::vector<size_t> fromIndices(entryMoveFromList.begin(), entryMoveFromList.end());
        std::sort(fromIndices.begin(), fromIndices.end());
        size_t insertedAt = 0;
        if (mgr.MoveEntries(fromIndices, static_cast<size_t>(entryMoveInsertion), &insertedAt)) {
            selectedEntries.clear();
            for (size_t k = 0; k < fromIndices.size(); ++k) {
                selectedEntries.insert(static_cast<int>(insertedAt + k));
            }
            selectionAnchor = static_cast<int>(insertedAt);
        }
    }
    ImGui::EndChild();
    const char* libraryButtons[] = { L("Load").c_str(), L("Save").c_str(), L("Default").c_str() };
    const char* libraryButtonTooltips[] = {
        L("Load a library profile").c_str(),
        L("Save the current library as a profile").c_str(),
        L("Reset the current library").c_str()
    };
    bool libraryPressed[3] = {};
    DrawWrappedButtonRow(libraryButtons, libraryButtonTooltips, 3, libraryButtonsWidth, libraryPressed);
    if (libraryPressed[0]) {
        beginNativeDialog(
            NativeFileDialogAction::LoadProfile,
            kUnlimitedPlaybackProfileFolder,
            L("Load profile file dialog open...").c_str());
    }
    if (libraryPressed[1]) {
        const std::string initialSavePath = mgr.GetActiveProfilePath().empty()
            ? std::string(kUnlimitedPlaybackProfileFolder) + "/profile.upl"
            : mgr.GetActiveProfilePath();
        beginNativeDialog(
            NativeFileDialogAction::SaveProfile,
            initialSavePath,
            L("Save profile file dialog open...").c_str());
    }
    if (libraryPressed[2]) {
        openDefaultConfirmModal = true;
    }
    ImGui::EndChild();

    ImGui::BeginChild("up_capture", ImVec2(0, captureSectionHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawSectionTitle(L("Add Playback Entry").c_str());
    const int captureButtonsPerRowDynamic = ComputeButtonsPerRow(captureButtonWidth, 3);
    for (int rowStart = 0; rowStart < 3; rowStart += captureButtonsPerRowDynamic) {
        const int rowCount = (std::min)(captureButtonsPerRowDynamic, 3 - rowStart);
        const float rowWidth =
            (captureButtonWidth * static_cast<float>(rowCount)) +
            (ImGui::GetStyle().ItemSpacing.x * static_cast<float>((std::max)(0, rowCount - 1)));
        ImGui::AlignItemHorizontalCenter(rowWidth);
        for (int rowOffset = 0; rowOffset < rowCount; ++rowOffset) {
            const int buttonIndex = rowStart + rowOffset;
            if (rowOffset > 0) {
                ImGui::SameLine();
            }
            const bool enabled = (buttonIndex != 2) || inReplayMatch;
            capturePressed[buttonIndex] = DrawContextButton(captureButtons[buttonIndex], enabled);
            if (buttonIndex == 2 && !inReplayMatch && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", L("Add from replay is only available while a replay match is active in Replay Theater.").c_str());
            } else if (captureButtonTooltips[buttonIndex]) {
                DrawButtonTooltip(captureButtonTooltips[buttonIndex]);
            }
        }
    }
    if (capturePressed[0]) {
        openCaptureSlotModal = true;
    }
    if (capturePressed[1]) {
        beginNativeDialog(
            NativeFileDialogAction::ImportPlayback,
            kUnlimitedPlaybackImportFolder,
            L("Import playback file dialog open...").c_str());
    }
    if (capturePressed[2]) {
        openReplayCaptureModal = true;
    }
    if (openReplayCaptureModal) {
        const ImVec2 displayCenter = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(displayCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_Appearing);
        ImGui::OpenPopup(L("Add from Replay").c_str());
        openReplayCaptureModal = false;
    }
    if (ImGui::BeginPopupModal(L("Add from Replay").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", L("Capture from replay").c_str());
        if (mgr.IsReplayRecording()) {
            ImGui::TextWrapped("%s", L("Replay recording is already running. Use the non-modal recording window to stop or cancel.").c_str());
            if (ImGui::Button(FormatText("%s##replay_capture", L("Close").c_str()).c_str())) {
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::InputText(FormatText("%s##replay_capture_name", L("Name").c_str()).c_str(), replayCaptureName, IM_ARRAYSIZE(replayCaptureName));
            if (DrawContextButton(L("Record P1 Inputs").c_str(), inReplayMatch)) {
                if (mgr.StartReplayRecording(true)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (DrawContextButton(L("Record P2 Inputs").c_str(), inReplayMatch)) {
                if (mgr.StartReplayRecording(false)) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(FormatText("%s##replay_capture", L("Close").c_str()).c_str())) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::TextDisabled("%s", L("Replay capture only starts while a replay match is active.").c_str());
        ImGui::EndPopup();
    }
    if (mgr.IsReplayRecording()) {
        ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Appearing);
        if (ImGui::Begin(L("Replay Recording").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextDisabled(L("Recording %s inputs (start frame %d)").c_str(),
                mgr.IsReplayRecordingAsP1() ? "P1" : "P2",
                mgr.GetReplayRecordingStartFrame());
            ImGui::InputText(FormatText("%s##replay_capture_name_floating", L("Name").c_str()).c_str(), replayCaptureName, IM_ARRAYSIZE(replayCaptureName));
            if (ImGui::Button(FormatText("%s##replay_capture_floating", L("Stop and Save").c_str()).c_str())) {
                mgr.StopReplayRecordingAndSave(replayCaptureName);
            }
            ImGui::SameLine();
            if (ImGui::Button(FormatText("%s##replay_capture_floating", L("Cancel").c_str()).c_str())) {
                mgr.CancelReplayRecording();
            }
        }
        ImGui::End();
    }
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::NextColumn();

    ImGui::BeginChild("up_settings", ImVec2(0, 0), true);
    DrawSectionTitle(L("Library Settings").c_str());
    ImGui::Dummy(ImVec2(0, 2));
    int selectionMode = mgr.GetSelectionMode();
    const char* selectionModes[] = {
        SelectionModeLabel(UnlimitedPlaybackManager::Selection_Random),
        SelectionModeLabel(UnlimitedPlaybackManager::Selection_Sequential),
        SelectionModeLabel(UnlimitedPlaybackManager::Selection_NonRepeatingRandom)
    };
    ImGui::TextUnformatted(L("Playback Mode").c_str());
    DrawHelpInline(L("How the library chooses the next enabled entry when playback is triggered.").c_str());
    ImGui::PushItemWidth(-1.0f);
    if (ImGui::Combo("##up_playback_mode", &selectionMode, selectionModes, IM_ARRAYSIZE(selectionModes))) {
        mgr.SetSelectionMode(selectionMode);
        mgr.PushToast(FormatText(L("Playback mode: %s").c_str(), selectionModes[selectionMode]));
    }
    ImGui::PopItemWidth();
    ImGui::Dummy(ImVec2(0, 4));
    bool autoMirrorOnSideSwap = mgr.GetAutoMirrorOnSideSwap();
    if (ImGui::Checkbox(L("Auto-mirror on side swap").c_str(), &autoMirrorOnSideSwap)) {
        mgr.SetAutoMirrorOnSideSwap(autoMirrorOnSideSwap);
    }
    DrawHelpInline(L("Mirrors directional inputs when the recorded side and current side differ.").c_str());

    for (int i = 0; i < UnlimitedPlaybackManager::Trigger_Count; ++i) {
        if (mgr.GetTrigger(static_cast<UnlimitedPlaybackManager::TriggerType>(i)).enabled) {
            selectedTriggerType = i;
            break;
        }
    }
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));
    const char* triggerNames[] = {
        TriggerLabel(UnlimitedPlaybackManager::Trigger_Wakeup),
        TriggerLabel(UnlimitedPlaybackManager::Trigger_Gap),
        TriggerLabel(UnlimitedPlaybackManager::Trigger_OnBlock),
        TriggerLabel(UnlimitedPlaybackManager::Trigger_OnHit),
        TriggerLabel(UnlimitedPlaybackManager::Trigger_ThrowTech),
        TriggerLabel(UnlimitedPlaybackManager::Trigger_KeyPress),
        TriggerLabel(UnlimitedPlaybackManager::Trigger_OnLoop)
    };
    ImGui::TextUnformatted(L("Playback Trigger Type").c_str());
    DrawHelpInline(L("Selects the single trigger type that can fire library playback.").c_str());
    ImGui::PushItemWidth(-1.0f);
    if (ImGui::Combo("##up_trigger_type", &selectedTriggerType, triggerNames, IM_ARRAYSIZE(triggerNames))) {
        for (int i = 0; i < UnlimitedPlaybackManager::Trigger_Count; ++i) {
            mgr.GetTrigger(static_cast<UnlimitedPlaybackManager::TriggerType>(i)).enabled = (i == selectedTriggerType);
        }
        mgr.PushToast(FormatText(L("Playback trigger: %s").c_str(), triggerNames[selectedTriggerType]));
    }
    ImGui::PopItemWidth();

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::Text(L("%s Config").c_str(), triggerNames[selectedTriggerType]);
    auto selectedTrigger = static_cast<UnlimitedPlaybackManager::TriggerType>(selectedTriggerType);
    auto& triggerConfig = mgr.GetTrigger(selectedTrigger);
    ImGui::Dummy(ImVec2(0, 2));
    if (selectedTrigger != UnlimitedPlaybackManager::Trigger_OnLoop) {
        ImGui::TextUnformatted(L("Cooldown Frames").c_str());
        DrawHelpInline(L("Blocks the same trigger from firing again for this many frames after it activates.").c_str());
        ImGui::PushItemWidth(-1.0f);
        ImGui::InputInt("##up_cooldown_frames", &triggerConfig.cooldownFrames);
        ImGui::PopItemWidth();
        if (triggerConfig.cooldownFrames < 1) {
            triggerConfig.cooldownFrames = 1;
        }
    }
    ImGui::Dummy(ImVec2(0, 4));
    if (selectedTrigger == UnlimitedPlaybackManager::Trigger_KeyPress) {
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", L("Maps the button or key used by the Key Press trigger.").c_str());
        }
        ImGui::SameLine();
        DrawPlaybackHotkeyBind(mgr, HotkeyManager::Hotkey_UnlimitedPlaybackTrigger);
    }
    if (selectedTrigger == UnlimitedPlaybackManager::Trigger_OnLoop) {
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", L("Maps the button or key used to start and stop loop playback.").c_str());
        }
        ImGui::SameLine();
        DrawPlaybackHotkeyBind(mgr, HotkeyManager::Hotkey_UnlimitedPlaybackLoop);
        ImGui::SameLine();
        ImGui::TextColored(mgr.IsLoopActive() ? ImVec4(0.25f, 0.9f, 0.45f, 1.0f) : ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
            "%s",
            mgr.IsLoopActive() ? L("Running").c_str() : L("Stopped").c_str());

        float setupSeconds = mgr.GetLoopSetupSeconds();
        ImGui::TextUnformatted(L("Setup Time (seconds)").c_str());
        DrawHelpInline(L("Seconds to show a setup countdown after optional snapshot restore before playing the next slot.").c_str());
        ImGui::PushItemWidth(-1.0f);
        if (ImGui::InputFloat("##up_loop_setup_seconds", &setupSeconds, 0.1f, 0.5f, "%.2f")) {
            mgr.SetLoopSetupSeconds(setupSeconds);
            Settings::settingsIni.unlimitedPlaybackLoopSetupSeconds = mgr.GetLoopSetupSeconds();
            Settings::changeSetting("UnlimitedPlaybackLoopSetupSeconds", std::to_string(mgr.GetLoopSetupSeconds()));
        }
        ImGui::PopItemWidth();

        float endingSeconds = mgr.GetLoopEndingSeconds();
        ImGui::TextUnformatted(L("Ending Time (seconds)").c_str());
        DrawHelpInline(L("Seconds to wait after both players return to idle before starting the next setup.").c_str());
        ImGui::PushItemWidth(-1.0f);
        if (ImGui::InputFloat("##up_loop_ending_seconds", &endingSeconds, 0.1f, 0.5f, "%.2f")) {
            mgr.SetLoopEndingSeconds(endingSeconds);
            Settings::settingsIni.unlimitedPlaybackLoopEndingSeconds = mgr.GetLoopEndingSeconds();
            Settings::changeSetting("UnlimitedPlaybackLoopEndingSeconds", std::to_string(mgr.GetLoopEndingSeconds()));
        }
        ImGui::PopItemWidth();

        bool restartLabState = mgr.GetLoopRestartLabState();
        if (ImGui::Checkbox(L("Restart lab state in-between").c_str(), &restartLabState)) {
            mgr.SetLoopRestartLabState(restartLabState);
            Settings::settingsIni.unlimitedPlaybackLoopRestartLabState = restartLabState;
            Settings::changeSetting("UnlimitedPlaybackLoopRestartLabState", restartLabState ? "1" : "0");
        }
        if (restartLabState) {
            static const int kResetModeOrder[] = {
                UnlimitedPlaybackManager::LoopReset_Left,
                UnlimitedPlaybackManager::LoopReset_Middle,
                UnlimitedPlaybackManager::LoopReset_Right,
                UnlimitedPlaybackManager::LoopReset_Custom,
            };
            const int currentResetMode = mgr.GetLoopRestartMode();
            ImGui::TextUnformatted(L("Reset Position").c_str());
            DrawHelpInline(L("Lab state restored before each loop. Left/Middle/Right briefly take control the first time the loop starts to reset there and auto-save a snapshot; Custom Snapshot uses a snapshot you save manually.").c_str());
            ImGui::PushItemWidth(-1.0f);
            if (ImGui::BeginCombo("##up_loop_reset_mode", LoopResetModeLabel(currentResetMode))) {
                for (int mode : kResetModeOrder) {
                    const bool selected = (mode == currentResetMode);
                    if (ImGui::Selectable(LoopResetModeLabel(mode), selected)) {
                        mgr.SetLoopRestartMode(mode);
                        Settings::settingsIni.unlimitedPlaybackLoopRestartMode = mgr.GetLoopRestartMode();
                        Settings::changeSetting("UnlimitedPlaybackLoopRestartMode", std::to_string(mgr.GetLoopRestartMode()));
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();

            if (currentResetMode == UnlimitedPlaybackManager::LoopReset_Custom) {
                const bool customReady = mgr.IsLoopSnapshotReadyForMode(UnlimitedPlaybackManager::LoopReset_Custom);
                ImGui::TextUnformatted(L("Custom Snapshot").c_str());
                DrawHelpInline(L("Loop restart restores this session-only snapshot before each slot. It is cleared when leaving lab.").c_str());
                if (ImGui::Button(L("Save").c_str(), ImVec2(-1.0f, 0))) {
                    mgr.CaptureLoopCustomSnapshot();
                }
                DrawButtonTooltip(L("Save current lab state as the loop restart snapshot.").c_str());
                if (customReady) {
                    if (ImGui::Button(L("Load").c_str(), ImVec2(-1.0f, 0))) {
                        mgr.LoadLoopCustomSnapshot();
                    }
                    DrawButtonTooltip(L("Restore the saved loop snapshot now for verification.").c_str());
                }
                ImGui::TextColored(
                    customReady ? ImVec4(0.25f, 0.9f, 0.45f, 1.0f) : ImVec4(0.95f, 0.55f, 0.35f, 1.0f),
                    "%s",
                    customReady ? L("Snapshot loaded").c_str() : L("No snapshot loaded").c_str());
            } else {
                const bool positionReady = mgr.IsLoopSnapshotReadyForMode(currentResetMode);
                ImGui::TextColored(
                    positionReady ? ImVec4(0.25f, 0.9f, 0.45f, 1.0f) : ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
                    "%s",
                    positionReady
                        ? L("Reset position snapshot ready").c_str()
                        : L("Position set up automatically when the loop starts").c_str());
            }
        }
    }
    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button(L("Fix Triggers").c_str())) {
        mgr.ForceResetTriggers();
    }
    DrawHelpInline(L("Use this after Training reset if triggers temporarily stop firing. It clears trigger cooldown and edge state.").c_str());

    const auto& toasts = mgr.GetToasts();
    if (!toasts.empty()) {
        ImGui::Separator();
        ImGui::Text("%s", L("Activity").c_str());
        ImGui::BeginChild("toast_list", ImVec2(0, 120), true);
        for (const auto& toast : toasts) {
            ImGui::TextWrapped("- %s", toast.text.c_str());
        }
        ImGui::EndChild();
    }

    ImGui::EndChild();

    ImGui::Columns(1);
    ImGui::EndChild();

    if (openDefaultConfirmModal) {
        const ImVec2 displayCenter = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(displayCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
        ImGui::OpenPopup(L("Reset Library?").c_str());
        openDefaultConfirmModal = false;
    }
    if (ImGui::BeginPopupModal(L("Reset Library?").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        DrawCenteredPopupText(L("Reset the current library and clear all loaded entries?").c_str());
        CenterNextButtonsRow(170.0f + ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button(L("Reset").c_str())) {
            mgr.ClearAll();
            selectedEntries.clear();
            selectionAnchor = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(FormatText("%s##reset_library", L("Cancel").c_str()).c_str())) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (openDeleteEntryConfirmModal && !entriesPendingDelete.empty()) {
        const ImVec2 displayCenter = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(displayCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(500.0f, 0.0f), ImGuiCond_Appearing);
        ImGui::OpenPopup(L("Delete Entry?").c_str());
        openDeleteEntryConfirmModal = false;
    }
    if (ImGui::BeginPopupModal(L("Delete Entry?").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (entriesPendingDelete.size() > 1) {
            DrawCenteredPopupText(FormatText(
                L("Delete these %d playback entries from the library?").c_str(),
                static_cast<int>(entriesPendingDelete.size())).c_str());
        } else {
            DrawCenteredPopupText(L("Delete this playback entry from the library?").c_str());
        }
        CenterNextButtonsRow(190.0f + ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button(FormatText("%s##confirm_entry", L("Delete").c_str()).c_str())) {
            std::vector<size_t> indicesToDelete(entriesPendingDelete.begin(), entriesPendingDelete.end());
            mgr.RemoveEntriesByIndices(indicesToDelete);
            std::sort(entriesPendingDelete.begin(), entriesPendingDelete.end());
            const auto shiftAfterDeletes = [](int idx) {
                int shifted = idx;
                for (int deletedIdx : entriesPendingDelete) {
                    if (deletedIdx < idx) {
                        --shifted;
                    }
                }
                return shifted;
            };
            std::set<int> adjustedSelection;
            for (int idx : selectedEntries) {
                if (std::find(entriesPendingDelete.begin(), entriesPendingDelete.end(), idx) != entriesPendingDelete.end()) {
                    continue;
                }
                adjustedSelection.insert(shiftAfterDeletes(idx));
            }
            selectedEntries = adjustedSelection;
            if (selectionAnchor >= 0) {
                if (std::find(entriesPendingDelete.begin(), entriesPendingDelete.end(), selectionAnchor) != entriesPendingDelete.end()) {
                    selectionAnchor = -1;
                } else {
                    selectionAnchor = shiftAfterDeletes(selectionAnchor);
                }
            }
            entriesPendingDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(FormatText("%s##confirm_entry", L("Cancel").c_str()).c_str())) {
            entriesPendingDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (openSetIndexModal) {
        const ImVec2 displayCenter = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(displayCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_Appearing);
        ImGui::OpenPopup(L("Set Slot Index").c_str());
        openSetIndexModal = false;
    }
    if (ImGui::BeginPopupModal(L("Set Slot Index").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const int entryCount = static_cast<int>(mgr.GetEntries().size());
        if (entryPendingSetIndex < 0 || entryPendingSetIndex >= entryCount || entryCount <= 0) {
            ImGui::TextDisabled("%s", L("Entry no longer exists.").c_str());
            if (ImGui::Button(FormatText("%s##set_index_missing", L("Close").c_str()).c_str())) {
                entryPendingSetIndex = -1;
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::InputInt(L("Index").c_str(), &entrySetIndexValue);
            if (entrySetIndexValue < 1) {
                entrySetIndexValue = 1;
            }
            if (entrySetIndexValue > entryCount) {
                entrySetIndexValue = entryCount;
            }
            ImGui::TextDisabled(L("Valid range: 1-%d").c_str(), entryCount);
            CenterNextButtonsRow(180.0f + ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::Button(FormatText("%s##set_index", L("Move").c_str()).c_str())) {
                const int from = entryPendingSetIndex;
                const int to = entrySetIndexValue - 1;
                if (mgr.MoveEntry(static_cast<size_t>(from), static_cast<size_t>(to))) {
                    std::set<int> adjustedSelection;
                    for (int idx : selectedEntries) {
                        adjustedSelection.insert(AdjustSelectedEntryAfterMove(idx, from, to));
                    }
                    selectedEntries = adjustedSelection;
                    if (selectionAnchor >= 0) {
                        selectionAnchor = AdjustSelectedEntryAfterMove(selectionAnchor, from, to);
                    }
                }
                entryPendingSetIndex = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(FormatText("%s##set_index", L("Cancel").c_str()).c_str())) {
                entryPendingSetIndex = -1;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    if (openCaptureSlotModal) {
        ImGui::OpenPopup(L("Add from CF Slot").c_str());
        openCaptureSlotModal = false;
    }
    if (ImGui::BeginPopupModal(L("Add from CF Slot").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputInt(L("Capture Slot").c_str(), &captureSlot);
        if (captureSlot < 1) captureSlot = 1;
        if (captureSlot > 4) captureSlot = 4;
        ImGui::InputText(L("Name").c_str(), captureName, IM_ARRAYSIZE(captureName));
        if (ImGui::Button(FormatText("%s##capture_slot", L("Save").c_str()).c_str())) {
            mgr.CaptureSlotToLibrary(captureSlot, captureName);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(FormatText("%s##capture_slot", L("Cancel").c_str()).c_str())) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (openEntryEditModal && entryPendingEdit >= 0 && entryPendingEdit < static_cast<int>(mgr.GetEntries().size())) {
        const auto& entry = mgr.GetEntries()[entryPendingEdit];
        std::strncpy(editEntryName, entry.name.c_str(), IM_ARRAYSIZE(editEntryName) - 1);
        editEntryName[IM_ARRAYSIZE(editEntryName) - 1] = '\0';
        editEntryWeight = entry.weight;
        const ImVec2 displayCenter = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(displayCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::OpenPopup(L("Edit Library Entry").c_str());
        openEntryEditModal = false;
    }
    if (ImGui::BeginPopupModal(L("Edit Library Entry").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (entryPendingEdit >= 0 && entryPendingEdit < static_cast<int>(mgr.GetEntries().size())) {
            ImGui::InputText(L("Name").c_str(), editEntryName, IM_ARRAYSIZE(editEntryName));
            ImGui::InputFloat(L("Weight").c_str(), &editEntryWeight, 0.1f, 1.0f);
            if (editEntryWeight < 0.01f) {
                editEntryWeight = 0.01f;
            }
            if (ImGui::Button(L("Edit Playback").c_str())) {
                auto& entry = mgr.GetEntriesMutable()[entryPendingEdit];
                entry.name = editEntryName;
                entry.weight = editEntryWeight;
                if (m_pWindowContainer) {
                    auto* editorWindow = m_pWindowContainer->GetWindow<PlaybackEditorWindow>(WindowType_PlaybackEditor);
                    if (editorWindow) {
                        if (editorWindow->BeginUnlimitedEntryEdit(static_cast<size_t>(entryPendingEdit))) {
                            openEntryPlaybackEditorModal = true;
                        }
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(FormatText("%s##edit_entry", L("Save").c_str()).c_str())) {
                auto& entry = mgr.GetEntriesMutable()[entryPendingEdit];
                entry.name = editEntryName;
                entry.weight = editEntryWeight;
                mgr.PushToast(L("Entry updated."));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(FormatText("%s##edit_entry", L("Cancel").c_str()).c_str())) {
                ImGui::CloseCurrentPopup();
            }
            if (openEntryPlaybackEditorModal) {
                const ImVec2 displayCenter = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
                ImGui::SetNextWindowPos(displayCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(900.0f, 620.0f), ImGuiCond_Appearing);
                ImGui::OpenPopup(L("Edit Entry Playback").c_str());
                openEntryPlaybackEditorModal = false;
            }
            if (ImGui::BeginPopupModal(L("Edit Entry Playback").c_str(), nullptr, ImGuiWindowFlags_NoResize)) {
                auto* editorWindow = m_pWindowContainer ? m_pWindowContainer->GetWindow<PlaybackEditorWindow>(WindowType_PlaybackEditor) : nullptr;
                if (editorWindow) {
                    editorWindow->DrawEmbeddedEditor();
                } else {
                    ImGui::TextDisabled("%s", L("Playback editor is unavailable.").c_str());
                    if (ImGui::Button(FormatText("%s##edit_entry_playback_missing", L("Close").c_str()).c_str())) {
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }
        } else {
            ImGui::TextDisabled("%s", L("Entry no longer exists.").c_str());
            if (ImGui::Button(FormatText("%s##edit_entry_missing", L("Close").c_str()).c_str())) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    if (openSendToSlotModal && entryPendingSend >= 0 && entryPendingSend < static_cast<int>(mgr.GetEntries().size())) {
        ImGui::OpenPopup(L("Send to CF Slot").c_str());
        openSendToSlotModal = false;
    }
    if (ImGui::BeginPopupModal(L("Send to CF Slot").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputInt(L("CF Slot").c_str(), &sendSlot);
        if (sendSlot < 1) sendSlot = 1;
        if (sendSlot > 4) sendSlot = 4;
        if (ImGui::Button(L("Send").c_str())) {
            if (entryPendingSend >= 0 && entryPendingSend < static_cast<int>(mgr.GetEntries().size())) {
                mgr.LoadEntryIntoSlot(static_cast<size_t>(entryPendingSend), sendSlot);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(FormatText("%s##send_slot", L("Cancel").c_str()).c_str())) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawUnlimitedPlaybackLoopSetupIndicatorStandalone() {
    auto& mgr = UnlimitedPlaybackManager::Instance();

    if (mgr.IsLoopPositionSetupActive()) {
        // Deliberately NOT a modal: a focused ImGui window sets WantCaptureKeyboard, which makes
        // PassKeyboardInputToGame() freeze the game's keyboard state (hooks_bbcf.cpp) and the
        // forced reset combo's key releases would never reach the game.
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.25f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("##up_loop_position_setup_banner", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextUnformatted(L("Setting up loop reset position...").c_str());
            ImGui::TextUnformatted(L("Inputs are temporarily overridden; this happens once per lab session.").c_str());
        }
        ImGui::End();
    }
    float loopSetupRemaining = 0.0f;
    float loopSetupTotal = 0.0f;
    if (mgr.GetLoopSetupCountdown(&loopSetupRemaining, &loopSetupTotal)) {
        ImGui::OpenPopup(L("Loop Setup Countdown").c_str());
    }
    if (ImGui::BeginPopupModal(L("Loop Setup Countdown").c_str(), nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (loopSetupTotal > 0.0f && loopSetupRemaining > 0.0f) {
            const float progress = loopSetupRemaining / loopSetupTotal;
            ImGui::ProgressBar(progress, ImVec2(360.0f, 0.0f));
            ImGui::Text("%s", FormatText(L("%.1f seconds").c_str(), loopSetupRemaining).c_str());
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
