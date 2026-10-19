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

// One player's palette storage in the current battle. How the game stores and draws it is in
// docs/Decompilation/Palettes.md; read its "Rewrite design" before changing anything here.
//
// The handle never writes the game's colour number. It writes the palette objects of the colour the
// player picked (sub colour*8+file of the container), marks them dirty and resets the container's
// upload cache, which is what makes the engine re-upload them. The old N <-> N+1 colour toggle only
// survives behind the PaletteLegacyIndexToggle setting.
//
// Lifetime: the GetPalBaseAddresses hook hands over the owner at every battle load, which starts a
// new load generation. The handle is ready only after OnMatchInit has run for that generation and
// while the container is still alive (the battle destructor zeroes owner+0x830 before freeing it).
// So nothing can write a palette with the previous battle's colour, or into a freed container.
class CharPaletteHandle
{
	friend class PaletteManager;

	int* m_pCurPalIndex = nullptr;
	// The colour the player picked, read at OnMatchInit. -1 until then.
	int m_origPalIndex = -1;
	// PlayerResSlot that owns the palette container (BattleScene+0x2B8 + slot*0x20BDC). Static
	// memory, so the pointer itself never dangles; the container inside it is read live.
	char* m_pOwner = nullptr;
	// Starts at 0 and is bumped by every container load, so m_initGeneration == 0 never matches.
	unsigned m_loadGeneration = 0;
	unsigned m_initGeneration = 0;
	IMPL_data_t m_origPalBackup;
	IMPL_data_t m_currentPalData;
	int m_selectedCustomPalIndex = 0;
	// Bumped on every write into the game's palette storage. PaletteManager compares it against
	// the value it last propagated to Platinum's item palettes, so those are re-copied exactly
	// once per change instead of every frame.
	unsigned m_writeGeneration = 0;

	// PaletteLegacyIndexToggle only.
	int m_legacySwitchIndex = -1;
	bool m_legacyToggled = false;
	bool m_updateLocked = false;

public:
	void SetPointerPalIndex(int* pPalIdx);
	// From the GetPalBaseAddresses hook: a new battle loaded this player's container.
	void OnContainerLoaded(char* pOwner);
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

	// Counts writes into the palette storage; see m_writeGeneration.
	unsigned GetWriteGeneration() const { return m_writeGeneration; }

private:
	void ReplacePalData(IMPL_data_t* newPaletteData);
	// Reads the player's colour and backs up their native palette. False when this battle's
	// container is not usable yet; the caller retries.
	bool OnMatchInit();
	bool IsInitializedForCurrentLoad() const;
	// PaletteLegacyIndexToggle only: puts the real colour back after the legacy toggle. A no-op
	// unless that toggle actually ran, so it can never overwrite a colour the game wrote.
	void RestoreNativePalIndex(const char* reason);
	void UnlockUpdate();
	int GetSelectedCustomPalIndex();
	void SetSelectedCustomPalIndex(int index);
	const char* GetCurPalFileAddr(PaletteFile palFile);
	const char* GetOrigPalFileAddr(PaletteFile palFile);
	const IMPL_info_t& GetCurrentPalInfo() const;
	void SetCurrentPalInfo(IMPL_info_t* pPalInfo);
	const IMPL_data_t& GetCurrentPalData();
	char* GetLiveContainer() const;
	bool TryGetPalObject(int palIdx, int fileIdx, char** ppPalObj) const;
	bool TryGetPalFileAddr(int palIdx, int fileIdx, char** ppPalFileAddr) const;
	bool CanResolveAllFiles(int palIdx) const;
	void WritePalObject(char* pPalObj, const void* Src);
	void InvalidateUpload();
	void ReplaceSinglePalFile(const char* newPalData, PaletteFile palFile);
	void ReplaceAllPalFiles(IMPL_data_t* newPaletteData, int palIdx);
	void AfterWrite();
	void BackupOrigPal();
	void RestoreOrigPal();
	void LegacyToggleIndex();
};
