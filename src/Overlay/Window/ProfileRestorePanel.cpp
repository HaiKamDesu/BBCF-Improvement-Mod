#include "ProfileRestorePanel.h"

#include "Core/Localization.h"
#include "Core/SaveDataSafety.h"
#include "Network/ProfileSync.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/NotificationBar/NotificationBar.h"

#include "imgui.h"

#include <string>
#include <vector>

namespace
{
	const ImVec4 kWarning(1.0f, 0.45f, 0.40f, 1.0f);
	const ImVec4 kPending(1.0f, 0.8f, 0.3f, 1.0f);

	void ColoredWrapped(const ImVec4& color, const char* fmt, const char* arg)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::TextWrapped(fmt, arg);
		ImGui::PopStyleColor();
	}
}

void ProfileRestorePanel::Draw()
{
	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
	const bool open = ImGui::CollapsingHeader(L("Restore from a pre-update copy (cannot be undone)").c_str());
	ImGui::PopStyleColor();
	if (!open)
	{
		return;
	}

	ImGui::TextWrapped("%s", L("Only for when an update has broken your profile. The first time each mod version runs, it copies your local save data before the game starts, and your network profile the first time it downloads it. Restoring puts that copy back and throws away everything you earned since.").c_str());
	ImGui::Spacing();
	ColoredWrapped(kWarning, "%s", L("A restore cannot be undone.").c_str());
	ImGui::Spacing();

	const std::string pendingProfile = ProfileSync::PendingSnapshotRestore();
	const std::string pendingLocal = SaveDataSafety::PendingLocalRestore();
	if (!pendingProfile.empty())
	{
		ColoredWrapped(kPending, L("Network profile from before %s will be restored the next time you enter Network mode.").c_str(), pendingProfile.c_str());
		if (ImGui::SmallButton((L("Cancel") + "##profile_restore").c_str()))
		{
			ProfileSync::CancelSnapshotRestore();
		}
	}
	if (!pendingLocal.empty())
	{
		ColoredWrapped(kPending, L("Local save data from before %s will be restored the next time you start the game.").c_str(), pendingLocal.c_str());
		if (ImGui::SmallButton((L("Cancel") + "##local_restore").c_str()))
		{
			SaveDataSafety::CancelLocalRestore();
		}
	}

	const std::vector<SaveDataSafety::Snapshot> snapshots = SaveDataSafety::List();
	if (snapshots.empty())
	{
		ImGui::TextDisabled("%s", L("No copy has been taken yet. One is taken the next time the game starts.").c_str());
		return;
	}

	// Re-armed every time the window opens: the box must be ticked on purpose each time.
	static bool s_understood = false;
	if (ImGui::IsWindowAppearing())
	{
		s_understood = false;
	}
	ImGui::Checkbox(L("I understand a restore cannot be undone").c_str(), &s_understood);
	ImGui::Spacing();

	static std::string s_confirmVersion;
	static bool s_confirmProfile = false;
	for (const SaveDataSafety::Snapshot& snapshot : snapshots)
	{
		ImGui::PushID(snapshot.version.c_str());
		ImGui::Text(L("Before %s, taken %04u-%02u-%02u %02u:%02u").c_str(), snapshot.version.c_str(),
			snapshot.takenLocal.wYear, snapshot.takenLocal.wMonth, snapshot.takenLocal.wDay,
			snapshot.takenLocal.wHour, snapshot.takenLocal.wMinute);

		const bool hasProfile = ProfileSync::IsInstalled() && ProfileSync::HasSnapshot(snapshot.version);
		ImGui::BeginDisabled(!s_understood || !hasProfile);
		if (ImGui::Button(L("Restore network profile").c_str()))
		{
			s_confirmVersion = snapshot.version;
			s_confirmProfile = true;
			ImGui::OpenPopup("##profile_restore_confirm");
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!s_understood || !snapshot.hasLocalSave);
		if (ImGui::Button(L("Restore local save data").c_str()))
		{
			s_confirmVersion = snapshot.version;
			s_confirmProfile = false;
			ImGui::OpenPopup("##profile_restore_confirm");
		}
		ImGui::EndDisabled();
		if (!hasProfile)
		{
			ImGui::TextDisabled("%s", L("No network profile copy for this Steam account in this snapshot yet. It is taken the first time you enter Network mode with this version.").c_str());
		}

		const ImVec2 display = ImGui::GetIO().DisplaySize;
		ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (ImGui::BeginPopupModal("##profile_restore_confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
		{
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
			ImGui::TextWrapped(s_confirmProfile
				? L("Replace your network profile with the copy taken before %s? It happens the next time you enter Network mode. Everything you earned since that copy is lost.").c_str()
				: L("Replace your local save data with the copy taken before %s? It happens the next time you start the game. Everything you earned since that copy is lost.").c_str(),
				s_confirmVersion.c_str());
			ImGui::Spacing();
			ColoredWrapped(kWarning, "%s", L("This cannot be undone.").c_str());
			ImGui::PopTextWrapPos();
			ImGui::Spacing();
			// Cancel first and focused: the default answer is not to do it.
			if (ImGui::IsWindowAppearing())
			{
				ImGui::SetKeyboardFocusHere();
			}
			if (ImGui::Button(L("Cancel").c_str()))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.12f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.20f, 0.15f, 1.0f));
			if (ImGui::Button(L("Restore, I understand it cannot be undone").c_str()))
			{
				const bool ok = s_confirmProfile
					? ProfileSync::ScheduleSnapshotRestore(s_confirmVersion)
					: SaveDataSafety::ScheduleLocalRestore(s_confirmVersion);
				if (!ok && g_notificationBar != nullptr)
				{
					g_notificationBar->AddNotification("%s", L("Couldn't schedule the restore. Check DEBUG.txt.").c_str());
				}
				s_understood = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::PopStyleColor(2);
			ImGui::EndPopup();
		}
		ImGui::PopID();
		ImGui::Spacing();
	}
}
