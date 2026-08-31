#include "MainMenuPages.h"

#include "Audio/MusicManager.h"
#include "Core/HotkeyManager.h"
#include "Core/Localization.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/WindowContainer/WindowContainer.h"
#include "Overlay/Window/PaletteEditorWindow.h"
#include "Overlay/Window/PalettesConfigWindow.h"

#include "imgui.h"

namespace MainMenu
{
	void DrawLookAndSoundPage(const PageContext& ctx)
	{
		PaletteEditorWindow* editor = ctx.container->GetWindow<PaletteEditorWindow>(WindowType_PaletteEditor);

		GroupLabel(Look_Palettes);

		if (!isInMatch())
		{
			Unavailable(L("Choosing a palette per character needs a match in progress."));
		}
		else
		{
			ImGui::HorizontalSpacing();
			editor->ShowAllPaletteSelections("Main");
		}

		ImGui::VerticalSpacing(8);
		ImGui::HorizontalSpacing();
		editor->ShowReloadAllPalettesButton();

		ImGui::VerticalSpacing(4);
		ImGui::HorizontalSpacing();
		ImGui::BeginDisabled(!isPaletteEditingEnabledInCurrentState());
		if (ImGui::Button(Messages.Palette_editor()))
			ctx.container->GetWindow(WindowType_PaletteEditor)->ToggleOpen();
		ImGui::EndDisabled();
		ImGui::ShowHelpMarkerSameLine(Messages.Palette_editor_tooltip());

		if (ctx.palettesConfigWindow)
		{
			ImGui::SameLineOrWrap(ImGui::ButtonWidth("Palettes"));
			ctx.palettesConfigWindow->DrawOpenButton();
			ImGui::ShowHelpMarkerSameLine(L("Manage the palette files on disk: which ones are loaded, importing, and sharing.").c_str());
			ctx.palettesConfigWindow->DrawModal();
		}

		ImGui::VerticalSpacing(10);
		GroupLabel(Look_Music);

		// Deliberately just the two doors. Every music control lives in the window it belongs
		// to - rotation and the playlist in the Jukebox, swaps in Music Replacement.
		ImGui::HorizontalSpacing();
		if (ImGui::Button(L("Open Jukebox").c_str()))
		{
			GetMusicManager().StartCustomMusicDiscovery();
			ctx.container->GetWindow(WindowType_Jukebox)->ToggleOpen();
		}
		ImGui::ShowHelpMarkerSameLine(L("Choose which songs play during a match and how it moves between them.").c_str());

		ImGui::SameLineOrWrap(ImGui::ButtonWidth(L("Replace songs...").c_str()));
		if (ImGui::Button(L("Replace songs...").c_str()))
		{
			ctx.container->GetWindow(WindowType_BgmReplacement)->ToggleOpen();
		}
		ImGui::ShowHelpMarkerSameLine(L("Swap any song in the game for one of your own.").c_str());

		Hint(FormatText(L("Shortcuts: %s opens the Jukebox, %s skips to the next song.").c_str(),
			HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_ToggleJukebox)).c_str(),
			HotkeyManager::DisplayString(HotkeyManager::GetBinding(HotkeyManager::Hotkey_JukeboxNextTrack)).c_str()));
	}
}
