#include "MaintenanceDialogs.h"

#include "Core/Localization.h"
#include "Core/ModUninstall.h"
#include "Core/utils.h"
#include "Overlay/imgui_utils.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <Windows.h>

namespace
{
	const ImVec4 kDanger(1.0f, 0.42f, 0.38f, 1.0f);
	const ImVec4 kGood(0.45f, 0.90f, 0.45f, 1.0f);

	// What is checked when the popup first opens. Old save snapshots (a restore option on the
	// Online page) and caches (cost a slower next launch) have to be picked on purpose.
	bool DefaultSelected(ModCleanup::Category category)
	{
		return category != ModCleanup::Cat_OldSaveSnapshots && category != ModCleanup::Cat_Caches;
	}

	bool DangerButton(const char* label, const ImVec2& size = ImVec2(0, 0))
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.14f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.22f, 0.19f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.84f, 0.28f, 0.24f, 1.0f));
		const bool pressed = ImGui::Button(label, size);
		ImGui::PopStyleColor(3);
		return pressed;
	}

	void Bullet(const std::string& text)
	{
		ImGui::Bullet();
		ImGui::SameLine();
		ImGui::TextWrapped("%s", text.c_str());
	}
}

void MaintenanceDialogs::DrawButtons()
{
	if (ImGui::Button(L("Clear cache...").c_str()))
	{
		m_openCleanup = true;
		m_confirmClear = false;
		m_showClearResult = false;
		ModCleanup::StartScan();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", L("Free up space: delete old logs, crash reports, update backups and other leftovers from the game folder.").c_str());
	ImGui::SameLine();
	if (ImGui::Button(L("Uninstall...").c_str()))
	{
		m_openUninstall = true;
		m_uninstallError.clear();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", L("Remove the mod from this game folder.").c_str());
}

void MaintenanceDialogs::DrawModals()
{
	if (m_openCleanup)
	{
		ImGui::OpenPopup("###clear_cache_modal");
		m_openCleanup = false;
	}
	if (m_openUninstall)
	{
		ImGui::OpenPopup("###uninstall_modal");
		m_openUninstall = false;
	}
	DrawCleanupModal();
	DrawUninstallModal();
}

void MaintenanceDialogs::DrawCleanupModal()
{
	ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Always);
	const std::string title = L("Clear cache") + "###clear_cache_modal";
	if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize))
		return;

	const bool busy = ModCleanup::IsBusy();
	const ModCleanup::ScanSummary scan = ModCleanup::GetScan();

	ImGui::TextWrapped("%s", L("Deletes leftovers the mod and the game pile up in the game folder. Your settings, palettes, music, recordings, saves and replays are never touched. Deleted files do not go to the Recycle Bin.").c_str());
	ImGui::Spacing();

	if (!scan.valid)
	{
		ImGui::TextDisabled("%s", L("Looking through the game folder...").c_str());
	}
	else
	{
		if (!m_selectionInitialised)
		{
			for (int i = 0; i < ModCleanup::Cat_Count; i++)
				m_selected[i] = DefaultSelected((ModCleanup::Category)i);
			m_selectionInitialised = true;
		}

		int available = 0, checked = 0;
		uint64_t selectedBytes = 0, totalBytes = 0;
		for (int i = 0; i < ModCleanup::Cat_Count; i++)
		{
			if (scan.totals[i].files == 0)
				continue;
			available++;
			totalBytes += scan.totals[i].bytes;
			if (m_selected[i])
			{
				checked++;
				selectedBytes += scan.totals[i].bytes;
			}
		}

		ImGui::BeginDisabled(busy || available == 0);
		bool all = available > 0 && checked == available;
		const bool mixed = checked > 0 && checked < available;
		if (mixed)
			ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
		const std::string allLabel = L("Select all") + "  (" + ModCleanup::FormatBytes(totalBytes) + ")";
		if (ImGui::Checkbox(allLabel.c_str(), &all))
		{
			for (int i = 0; i < ModCleanup::Cat_Count; i++)
				if (scan.totals[i].files > 0)
					m_selected[i] = all;
			m_confirmClear = false;
		}
		if (mixed)
			ImGui::PopItemFlag();
		ImGui::EndDisabled();
		ImGui::Separator();

		if (ImGui::BeginTable("##cleanup_categories", 2, ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##size", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			for (int i = 0; i < ModCleanup::Cat_Count; i++)
			{
				const ModCleanup::Category category = (ModCleanup::Category)i;
				const ModCleanup::CategoryTotals& totals = scan.totals[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushID(i);
				const bool empty = totals.files == 0;
				ImGui::BeginDisabled(busy || empty);
				bool value = m_selected[i] && !empty;
				if (ImGui::Checkbox(L(ModCleanup::CategoryLabel(category)).c_str(), &value))
				{
					m_selected[i] = value;
					m_confirmClear = false;
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::ShowHelpMarker(L(ModCleanup::CategoryDescription(category)).c_str());
				ImGui::PopID();

				ImGui::TableNextColumn();
				if (empty)
					ImGui::TextDisabled("%s", L("nothing to clear").c_str());
				else
					ImGui::TextDisabled("%s", (ModCleanup::FormatBytes(totals.bytes) + ", " + std::to_string(totals.files) + " " +
						L(totals.files == 1 ? "file" : "files")).c_str());
			}
			ImGui::EndTable();
		}

		ImGui::Spacing();
		if (m_showClearResult && !busy)
		{
			const ModCleanup::ClearSummary result = ModCleanup::GetLastClear();
			if (result.valid)
			{
				ImGui::TextColored(kGood, "%s", (L("Done: deleted") + " " + std::to_string(result.deletedFiles) + " " +
					L(result.deletedFiles == 1 ? "file" : "files") + ", " + ModCleanup::FormatBytes(result.freedBytes) + " " + L("freed.")).c_str());
				if (result.skippedInUse > 0)
					ImGui::TextDisabled("%s", (std::to_string(result.skippedInUse) + " " +
						L("in use by the game right now were left; they can be cleared after a restart.")).c_str());
				if (result.failed > 0)
					ImGui::TextColored(kDanger, "%s", (std::to_string(result.failed) + " " +
						L("could not be deleted; see BBCF_IM\\DEBUG.txt.")).c_str());
			}
		}

		if (busy)
		{
			ImGui::TextDisabled("%s", L("Working...").c_str());
		}
		else if (m_confirmClear)
		{
			ImGui::TextColored(kDanger, "%s", (L("Permanently delete") + " " + ModCleanup::FormatBytes(selectedBytes) + "?").c_str());
			if (DangerButton(L("Yes, delete").c_str(), ImVec2(140, 0)))
			{
				ModCleanup::StartClear(m_selected);
				m_confirmClear = false;
				m_showClearResult = true;
			}
			ImGui::SameLine();
			if (ImGui::Button(L("No").c_str(), ImVec2(80, 0)))
				m_confirmClear = false;
		}
		else
		{
			ImGui::BeginDisabled(checked == 0);
			const std::string label = L("Delete selected") + " (" + ModCleanup::FormatBytes(selectedBytes) + ")";
			if (DangerButton(label.c_str(), ImVec2(220, 0)))
			{
				m_confirmClear = true;
				m_showClearResult = false;
			}
			ImGui::EndDisabled();
		}
	}

	ImGui::Separator();
	ImGui::BeginDisabled(busy);
	if (ImGui::Button(L("Close").c_str(), ImVec2(120, 0)))
	{
		m_confirmClear = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::EndPopup();
}

void MaintenanceDialogs::DrawUninstallModal()
{
	ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
	const std::string title = L("Uninstall BBCF Improvement Mod") + "###uninstall_modal";
	if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize))
		return;

	ImGui::TextWrapped("%s", L("Removes the mod from this game folder and restarts the game without it. The game itself, your save data and your replays are not touched.").c_str());
	ImGui::Spacing();

	ImGui::Checkbox(L("Keep my settings, palettes, music and recordings").c_str(), &m_keepUserData);
	ImGui::SameLine();
	ImGui::ShowHelpMarker(L("They stay in the game folder (settings.ini, palettes.ini and the BBCF_IM folder), so installing the mod again later picks them up. Untick to remove everything the mod ever made, including custom songs it converted into data\\Sound\\BGM.").c_str());
	ImGui::Spacing();

	ImGui::TextUnformatted(L("What gets removed:").c_str());
	Bullet(L("The mod itself (dinput8.dll), its updater, readme and old version backups."));
	Bullet(L("4 GB mode's copy of the game (BBCF_IM_4GB.exe) and the Windows and NVIDIA settings it copied from BBCF.exe."));
	Bullet(L("Logs, crash reports and caches."));
	if (!m_keepUserData)
		Bullet(L("Your settings, palettes, music, recordings, rank history and every other file in BBCF_IM."));

	const std::wstring copy = GamePathW(L"BBCF_IM_4GB.exe");
	if (GetFileAttributesW(copy.c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("%s", L("Windows may ask for administrator permission once, to remove the firewall rules 4 GB mode added.").c_str());
		ImGui::PopStyleColor();
	}

	if (!m_uninstallError.empty())
	{
		ImGui::Spacing();
		ImGui::TextColored(kDanger, "%s", m_uninstallError.c_str());
	}

	ImGui::Separator();
	if (DangerButton(L("Uninstall and restart the game").c_str(), ImVec2(240, 0)))
	{
		m_uninstallError.clear();
		std::string error;
		if (!ModUninstall::UninstallAndExit(m_keepUserData, &error))
			m_uninstallError = error;
	}
	ImGui::SameLine();
	if (ImGui::Button(L("Cancel").c_str(), ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}
