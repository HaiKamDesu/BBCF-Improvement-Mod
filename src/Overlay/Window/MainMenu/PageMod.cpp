#include "MainMenuPages.h"

#include "Core/Localization.h"
#include "Core/Settings.h"
#include "Core/utils.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include "Overlay/Window/SettingsIniWindow.h"
#include "Updater/UpdateCoordinator.h"

#include "imgui.h"

namespace MainMenu
{
	namespace
	{
		void DrawLanguageSelector()
		{
			const auto& options = Localization::GetAvailableLanguages();

			int currentIndex = 0;
			for (size_t i = 0; i < options.size(); ++i)
			{
				if (options[i].code == Localization::GetCurrentLanguage())
				{
					currentIndex = static_cast<int>(i);
					break;
				}
			}

			std::string pendingLanguage;
			bool shouldReload = false;

			ImGui::SetNextItemWidth(220.0f);
			if (ImGui::BeginCombo(Messages.Language(), options[currentIndex].displayName.c_str()))
			{
				for (size_t i = 0; i < options.size(); ++i)
				{
					const auto& option = options[i];
					std::string label = option.displayName;
					if (!option.complete)
						label += Messages.Language_incomplete_label();

					if (!option.complete)
						ImGui::BeginDisabled();

					if (ImGui::Selectable(label.c_str(), currentIndex == static_cast<int>(i)))
					{
						pendingLanguage = option.code;
						shouldReload = true;
						currentIndex = static_cast<int>(i);
					}

					if (!option.complete)
						ImGui::EndDisabled();
				}

				ImGui::EndCombo();
			}
			ImGui::SameLine();
			ImGui::ShowHelpMarker(Messages.Language_selection_help());

			if (shouldReload)
			{
				Localization::Reload(pendingLanguage);
				Settings::settingsIni.language = Localization::GetCurrentLanguage();
				Settings::changeSetting("Language", Settings::settingsIni.language);
			}
		}
	}

	// Nothing on this page is big enough to earn a heading, so it has none: four loose rows,
	// in the order you are likely to want them.
	void DrawModPage(const PageContext& ctx)
	{
		Anchor(Mod_Settings);
		if (ctx.settingsIniWindow)
		{
			ctx.settingsIniWindow->DrawOpenButton();
			ImGui::SameLine();
			ImGui::ShowHelpMarker(L("One searchable list of everything the mod can be configured to do, hotkeys included, with a description for each option.").c_str());
		}

		ImGui::VerticalSpacing(6);

		Anchor(Mod_Language);
		DrawLanguageSelector();

		ImGui::VerticalSpacing(8);
		ImGui::Separator();
		ImGui::VerticalSpacing(4);

		Anchor(Mod_Updates);
		if (ImGui::Button(L("Release notes").c_str()))
			ctx.container->GetWindow(WindowType_ReleaseChecker)->ToggleOpen();
		ImGui::SameLine();
		ImGui::ShowHelpMarker(Messages.Releases_checker_tooltip());
		Updater::UpdateCoordinator::GetInstance().DrawSkippedMainMenuLink();

		ImGui::VerticalSpacing(6);

		Anchor(Mod_Diagnostics);
		if (ImGui::Button(Messages.Log()))
			ctx.container->GetWindow(WindowType_Log)->ToggleOpen();
		ImGui::SameLine();
		ImGui::ShowHelpMarker(Messages.Log_window_tooltip());

#ifdef _DEBUG
		const bool showDebugButton = true;
#else
		// Debug builds always get the DEBUG window; other builds only show it once the user
		// opts into dev/diagnostic tooling (settings.def: EnableInDevelopmentFeatures).
		const bool showDebugButton = Settings::settingsIni.enableInDevelopmentFeatures;
#endif
		if (showDebugButton)
		{
			ImGui::SameLine();
			if (ImGui::Button("DEBUG"))
				ctx.container->GetWindow(WindowType_Debug)->ToggleOpen();
			ImGui::SameLine();
			ImGui::ShowHelpMarker(Messages.Debug_window_tooltip());
		}

		// Drawn last: the modal has to be issued from the same window the button was pressed
		// in, and it covers the whole menu once open.
		if (ctx.settingsIniWindow)
			ctx.settingsIniWindow->DrawModal();
	}

	void DrawPage(PageId page, const PageContext& ctx)
	{
		switch (page)
		{
		case Page_Game:         DrawGamePage(ctx); break;
		case Page_Training:     DrawTrainingPage(ctx); break;
		case Page_Overlays:     DrawOverlaysPage(ctx); break;
		case Page_Online:       DrawOnlinePage(ctx); break;
		case Page_Replays:      DrawReplaysPage(ctx); break;
		case Page_LookAndSound: DrawLookAndSoundPage(ctx); break;
		case Page_Controllers:  DrawControllersPage(ctx); break;
		case Page_Mod:          DrawModPage(ctx); break;
		default: break;
		}
	}
}
