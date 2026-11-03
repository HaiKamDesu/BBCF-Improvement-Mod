#pragma once

#include "Core/ModCleanup.h"

#include <string>

// The "Clear cache..." and "Uninstall..." buttons in the Settings window's footer and the two
// popups they open. Both functions must be called from inside the Settings popup, so the
// popups stack on top of it.
class MaintenanceDialogs
{
public:
	void DrawButtons();
	void DrawModals();

private:
	void DrawCleanupModal();
	void DrawUninstallModal();

	bool m_openCleanup = false;
	bool m_openUninstall = false;

	bool m_selected[ModCleanup::Cat_Count] = {};
	bool m_selectionInitialised = false;
	bool m_confirmClear = false;
	bool m_showClearResult = false;

	bool m_keepUserData = true;
	std::string m_uninstallError;
};
