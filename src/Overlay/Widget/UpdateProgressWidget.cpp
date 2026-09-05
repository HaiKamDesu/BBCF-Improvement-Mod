#include "UpdateProgressWidget.h"

#include "Overlay/imgui_utils.h"

#include "imgui.h"

namespace
{
	float WrappedHeight(const std::string& text, float wrapWidth)
	{
		if (text.empty())
			return 0.0f;
		return ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapWidth).y;
	}
}

bool UpdateProgressWidget::IsBusy(const Updater::UpdateUiSnapshot& snapshot)
{
	return snapshot.state == Updater::UpdateUiState_Downloading ||
		snapshot.state == Updater::UpdateUiState_Verifying ||
		snapshot.state == Updater::UpdateUiState_Staging ||
		snapshot.state == Updater::UpdateUiState_LaunchingUpdater;
}

float UpdateProgressWidget::EstimateHeight(const Updater::UpdateUiSnapshot& snapshot,
	float wrapWidth, bool includeAutoApplyReason)
{
	const float spacing = ImGui::GetStyle().ItemSpacing.y;
	float height = 0.0f;

	if (!snapshot.statusText.empty())
		height += spacing + WrappedHeight(snapshot.statusText, wrapWidth);
	if (IsBusy(snapshot))
		height += spacing + ImGui::GetFrameHeight();
	if (!snapshot.errorText.empty())
		height += spacing + WrappedHeight(snapshot.errorText, wrapWidth);
	if (includeAutoApplyReason && !snapshot.autoApplySupported && !snapshot.autoApplyDisabledReason.empty())
		height += spacing + WrappedHeight(snapshot.autoApplyDisabledReason, wrapWidth);

	return height;
}

void UpdateProgressWidget::Draw(const Updater::UpdateUiSnapshot& snapshot, bool includeAutoApplyReason)
{
	if (!snapshot.statusText.empty())
	{
		ImGui::Spacing();
		ImGui::TextWrapped("%s", snapshot.statusText.c_str());
	}

	if (IsBusy(snapshot))
	{
		ImGui::Spacing();
		ImGui::ProgressBar(snapshot.progressPercent / 100.0f, ImVec2(-1, 0));
	}

	if (!snapshot.errorText.empty())
	{
		ImGui::Spacing();
		ImGui::TextColoredWrapped(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", snapshot.errorText.c_str());
	}

	if (includeAutoApplyReason && !snapshot.autoApplySupported && !snapshot.autoApplyDisabledReason.empty())
	{
		ImGui::Spacing();
		ImGui::TextWrapped("%s", snapshot.autoApplyDisabledReason.c_str());
	}
}
