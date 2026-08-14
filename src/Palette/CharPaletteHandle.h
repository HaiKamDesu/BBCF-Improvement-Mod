#pragma once
#include "Palette/impl_format.h"

#define MAX_PAL_INDEX 23
#define TOTAL_PALETTE_FILES 8

extern char* palFileNames[TOTAL_PALETTE_FILES];

enum PaletteFile
{
	PaletteFile_Character,
	PaletteFile_Effect1,
	PaletteFile_Effect2,
	PaletteFile_Effect3,
	PaletteFile_Effect4,
	PaletteFile_Effect5,
	PaletteFile_Effect6,
	PaletteFile_Effect7
};

class CharPaletteHandle
{
	friend class PaletteManager;

	int* m_pCurPalIndex;
	// -1 until OnMatchInit has read the player's real color. Teardown must not write a color
	// back before then: match-end can fire without a preceding match-init (menu transitions),
	// and an uninitialized value would be written straight into the game's color byte.
	int m_origPalIndex = -1;
	const char* m_pPalBaseAddr;
	IMPL_data_t m_origPalBackup;
	IMPL_data_t m_currentPalData;
	int m_switchPalIndex1;
	int m_switchPalIndex2;
	int m_selectedCustomPalIndex;
	bool m_updateLocked;

public:
	void SetPointerPalIndex(int* pPalIdx);
	void SetPointerBasePal(char* pPalBaseAddr);
	bool IsNullPointerPalBasePtr() const;
	bool IsNullPointerPalIndex() const;
	bool IsPaletteDataReady() const;
	int& GetPalIndexRef();
	int GetOrigPalIndex() const;
	bool IsCurrentPalWithBloom() const;

	// True when the palette storage currently holds something other than the colors backed up at
	// match start, i.e. a custom palette is in effect (from palettes.ini, the palette editor, or
	// an opponent's synced palette). Content-based rather than flag-based so it stays correct no
	// matter which path applied the palette.
	bool IsCustomPaletteActive() const;

private:
	void SetPaletteIndex(int palIndex);
	void ReplacePalData(IMPL_data_t* newPaletteData);
	void OnMatchInit();
	void OnMatchRematch();
	// Puts the player's real native color back after UpdatePalette()'s redraw toggle. Character
	// select is the only valid moment for this -- read the comment on the definition before
	// moving it or adding a caller.
	void RestoreNativePalIndex(const char* reason);
	void LockUpdate();
	void UnlockUpdate();
	int GetSelectedCustomPalIndex();
	void SetSelectedCustomPalIndex(int index);
	const char* GetCurPalFileAddr(PaletteFile palFile);
	const char* GetOrigPalFileAddr(PaletteFile palFile);
	const IMPL_info_t& GetCurrentPalInfo() const;
	void SetCurrentPalInfo(IMPL_info_t* pPalInfo);
	const IMPL_data_t& GetCurrentPalData();
	char* GetPalFileAddr(const char* base, int palIdx, int fileIdx);
	bool TryGetPalFileAddr(int palIdx, int fileIdx, char** ppPalFileAddr) const;
	bool CanResolvePalFileAddr(int palIdx, int fileIdx) const;
	void ReplacePalArrayInMemory(char* Dst, const void* Src);
	void ReplaceSinglePalFile(const char* newPalData, PaletteFile palFile);
	void ReplaceAllPalFiles(IMPL_data_t* newPaletteData, int palIdx);
	void BackupOrigPal();
	void RestoreOrigPal();
	void UpdatePalette();
};
