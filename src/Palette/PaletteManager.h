#pragma once
#include "impl_format.h"

#include "CharPaletteHandle.h"

#include "Game/characters.h"
#include "Game/Player.h"

#include <vector>
#include <string>

class PaletteManager
{
public:
	PaletteManager();
	~PaletteManager();
	std::vector<std::vector<IMPL_data_t>> &GetCustomPalettesVector();

	bool PushImplFileIntoVector(IMPL_t &filledPal);
	bool PushImplFileIntoVector(CharIndex charIndex, IMPL_data_t &filledPalData);
	bool WritePaletteToFile(CharIndex charIndex, IMPL_data_t *filledPalData);
	bool WriteDownloadedPaletteToFile(CharIndex charIndex, IMPL_data_t* filledPalData, std::string* savedPalName = nullptr);

	void LoadAllPalettes();
	void ReloadAllPalettes();

	int GetOnlinePalsStartIndex(CharIndex charIndex);
	void OverwriteIMPLDataPalName(std::string fileName, IMPL_data_t& palData);

	// Builds a palette from the character's built-in template with only the character
	// color file replaced. Used by the PNG importer: a PNG carries a single 256-color
	// palette, so the seven effect files are taken from the template instead of being
	// left as zeroes (which would render every effect black).
	bool CreatePaletteFromCharacterFile(CharIndex charIndex, const std::string& palName,
		const char* characterFileData, IMPL_data_t& outPalData);

	// Return values:
	// ret > 0, index found
	// ret == -1, index not found
	// ret == -2, charindex out of bound
	// ret == -3, default palette or no name given
	int FindCustomPalIndex(CharIndex charIndex, const char* palNameToFind);
	bool PaletteArchiveDownloaded();
	bool SwitchPalette(CharIndex charIndex, CharPaletteHandle& palHandle, int newCustomPalIndex);
	void ReplacePaletteFile(const char* newPalData, PaletteFile palFile, CharPaletteHandle& palHandle);
	void RestoreOrigPal(CharPaletteHandle& palHandle);
	const char* GetCurPalFileAddr(PaletteFile palFile, CharPaletteHandle& palHandle);
	// The palette the character had when the match started - i.e. whichever in-game colour
	// the player picked. This is what "Default" means, and the only place its actual
	// colours exist: the Default entry in the custom list is an empty placeholder.
	const char* GetOrigPalFileAddr(PaletteFile palFile, CharPaletteHandle& palHandle);
	const char* GetCustomPalFile(CharIndex charIndex, int palIndex, PaletteFile palFile, CharPaletteHandle& palHandle);
	int GetCurrentCustomPalIndex(CharPaletteHandle& palHandle) const;
	const IMPL_info_t& GetCurrentPalInfo(CharPaletteHandle& palHandle) const;
	void SetCurrentPalInfo(CharPaletteHandle& palHandle, IMPL_info_t& palInfo);
	const IMPL_data_t& GetCurrentPalData(CharPaletteHandle& palHandle);
	void LoadPaletteSettingsFile();
	const std::vector<std::vector<std::string>>& GetPaletteSlots() const { return m_paletteSlots; }
	// A custom palette's data by index, without needing a live CharPaletteHandle - the
	// character select preview has no handle to go through. Null if out of range.
	const IMPL_data_t* GetCustomPalData(CharIndex charIndex, int palIndex) const;
	bool SavePaletteSettingsFile(const std::vector<std::vector<std::string>>& slots);

	// Call it ONCE per frame
	void OnUpdate(CharPaletteHandle& P1, CharPaletteHandle& P2);

	// Call it ONCE per frame, after OnUpdate. Keeps Platinum's custom palette applied while she
	// is holding a drive item.
	void ClearPlatinumItemPaletteLink(Player& playerOne, Player& playerTwo);

	// Call it ONCE upon match start
	void OnMatchInit(Player& playerOne, Player& playerTwo);

	void OnMatchRematch(Player& playerOne, Player& playerTwo);
	// Call it when the game enters character select. Undoes UpdatePalette()'s color-index toggle
	// while the player still cannot have picked a new color -- see the definition before moving.
	void OnCharacterSelect(CharPaletteHandle& playerOne, CharPaletteHandle& playerTwo);
	void OnMatchEnd(CharPaletteHandle& playerOne, CharPaletteHandle& playerTwo);

private:
	std::vector<std::vector<IMPL_data_t>> m_customPalettes;
	std::vector<std::vector<std::string>> m_paletteSlots;
	std::vector<int> m_onlinePalsStartIndex;
	bool m_loadOnlinePalettes = false;
	bool m_PaletteArchiveDownloaded = false;

	void CreatePaletteFolders();
	void InitCustomPaletteVector();
	void LoadPalettesIntoVector(CharIndex charIndex, std::wstring& wFolderPath);
	void LoadPalettesFromFolder();
	void LoadImplFile(const std::string& fullPath, const std::string& fileName, CharIndex charIndex);
	void LoadHplFile(const std::string& fullPath, const std::string& fileName, CharIndex charIndex);
	void InitPaletteSlotsVector();
	void InitOnlinePalsIndexVector();
	void ApplyDefaultCustomPalette(CharIndex charIndex, CharPaletteHandle& charPalHandle);
};
