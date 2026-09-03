#include "TasRecordingWindow.h"

#include "Core/Localization.h"
#include "Core/NativeFileDialog.h"
#include "Game/StandaloneTasRecorder.h"
#include "Overlay/imgui_utils.h"

#include <cfloat>
#include <cstdio>

namespace
{
    constexpr const char* kFileDialogOwner = "tas_recording_window";
    constexpr int kFileDialogExportRecording = 1;
}

void TasRecordingWindow::Update()
{
    // Keep the async result alive independently of whether the ImGui window is open.
    auto& recorder = StandaloneTasRecorder::Instance();
    NativeFileDialog::Result result;
    if (NativeFileDialog::Consume(kFileDialogOwner, &result) &&
        result.contextId == kFileDialogExportRecording)
    {
        if (result.accepted && !result.path.empty())
            recorder.Export(result.path);
        recorder.FinalizeExport();
    }

    IWindow::Update();
}

void TasRecordingWindow::BeforeDraw()
{
    ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
}

void TasRecordingWindow::Draw()
{
    auto& recorder = StandaloneTasRecorder::Instance();

    const bool dialogBusy = NativeFileDialog::IsOpen();
    if (recorder.IsRecording())
    {
        char frames[128];
        std::snprintf(frames, sizeof(frames), L("Recording: %u frames").c_str(),
            static_cast<unsigned int>(recorder.GetFrameCount()));
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", frames);

        if (ImGui::Button(L("End recording and export .txt").c_str()))
        {
            recorder.Stop();
            if (recorder.GetFrameCount() > 0)
            {
                NativeFileDialog::Request request;
                request.save = true;
                request.title = L("Export TAS recording");
                request.filters.push_back({ L("TAS Movie (*.txt)"), "*.txt" });
                request.defaultExtension = "txt";
                request.initialPath = "tas_recording.txt";
                request.contextId = kFileDialogExportRecording;
                if (!NativeFileDialog::Open(kFileDialogOwner, request))
                    recorder.FinalizeExport();
            }
            else
            {
                recorder.FinalizeExport();
            }
        }
    }
    else
    {
        ImGui::BeginDisabled(dialogBusy || recorder.IsPendingExport());
        if (ImGui::Button(L("Record P1").c_str()))
            recorder.Start();
        ImGui::EndDisabled();
        ImGui::ShowHelpMarkerSameLine(L("Records P1's inputs frame by frame during a training match, then saves them as a TAS movie the combo editor can open.").c_str());

        if (!recorder.GetError().empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", recorder.GetError().c_str());
        }
    }
}
