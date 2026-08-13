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
	int m_origPalIndex;
	const char* m_pPalBaseAddr;
	IMPL_data_t m_origPalBackup;
	IMPL_data_t m_currentPalData;
	int m_switchPalIndex1;
	int m_switchPalIndex2;
	int m_selectedCustomPalIndex;
	bool m_updateLocked;

	// UpdatePalette() hot-swaps *m_pCurPalIndex between m_switchPalIndex1/2 to force a
	// redraw, which permanently mutates the native slot value in game memory. These
	// remember the last pair we toggled and which slot of it was the player's real
	// choice, so OnMatchInit can tell "our own toggle artifact" apart from a genuine
	// new native color pick.
	//
	// The artifact is corrected in OnMatchInit and NOWHERE ELSE. Do not add a per-frame
	// correction -- see docs/Research/TaokakaPaletteRefreshInvestigation.md.
	int m_lastLogicalPalIndex = -1;
	int m_lastTogglePairA = -1;
	int m_lastTogglePairB = -1;

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
