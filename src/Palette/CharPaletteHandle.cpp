#include "CharPaletteHandle.h"

#include "Core/logger.h"
//#include "impl_templates.cpp"

#define BLOOM_PALETTE_INDEX 21

char* palFileNames[TOTAL_PALETTE_FILES] =
{
	"Character",
	"Effect01",
	"Effect02",
	"Effect03",
	"Effect04",
	"Effect05",
	"Effect06",
	"Effect07"
};

char* CharPaletteHandle::GetPalFileAddr(const char* base, int palIndex, int fileID)
{
	//dereferencing the multi-level pointer:
	// [[[baseaddr + 0x4] + palIndex * 0x20] + fileID * 0x4] + 0x1C

	// Leave for debugging:
	DWORD* deref1 = (DWORD*)base + 1;
	DWORD* deref2 = (DWORD*)*deref1 + (palIndex * 8);
	DWORD* deref2Offset = deref2 + + fileID;
	DWORD* finalAddr = (DWORD*)*deref2Offset + 7;

	DWORD* paletteAddress = (DWORD*)*((DWORD*)*((DWORD*)base + 1) + (palIndex * 8) + fileID) + 7;
	return (char*)paletteAddress;
}

bool CharPaletteHandle::TryGetPalFileAddr(int palIndex, int fileID, char** ppPalFileAddr) const
{
	if (ppPalFileAddr == nullptr)
		return false;

	*ppPalFileAddr = nullptr;

	if (m_pPalBaseAddr == nullptr)
		return false;

	if (palIndex < 0 || palIndex > MAX_PAL_INDEX || fileID < 0 || fileID >= TOTAL_PALETTE_FILES)
		return false;

	__try
	{
		const DWORD* deref1 = reinterpret_cast<const DWORD*>(m_pPalBaseAddr) + 1;
		if (*deref1 == 0)
			return false;

		const DWORD* deref2 = reinterpret_cast<const DWORD*>(*deref1) + (palIndex * 8) + fileID;
		if (*deref2 == 0)
			return false;

		char* paletteAddress = reinterpret_cast<char*>(reinterpret_cast<DWORD*>(*deref2) + 7);

		// Probe the first byte so stale game pointers fail here instead of in later memcpy paths.
		volatile char firstByte = paletteAddress[0];
		(void)firstByte;

		*ppPalFileAddr = paletteAddress;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

void CharPaletteHandle::SetPointerPalIndex(int* pPalIndex)
{
	m_pCurPalIndex = pPalIndex;
}

void CharPaletteHandle::SetPointerBasePal(char* pPalBaseAddr)
{
	m_pPalBaseAddr = pPalBaseAddr;
}

void CharPaletteHandle::SetPaletteIndex(int palIndex)
{
	if (palIndex < 0 || palIndex > MAX_PAL_INDEX)
		return;

	*m_pCurPalIndex = palIndex;
}

int CharPaletteHandle::GetOrigPalIndex() const
{
	return m_origPalIndex;
}

bool CharPaletteHandle::IsNullPointerPalBasePtr() const
{
	return m_pPalBaseAddr == nullptr;
}

bool CharPaletteHandle::IsNullPointerPalIndex() const
{
	return m_pCurPalIndex == nullptr;
}

bool CharPaletteHandle::CanResolvePalFileAddr(int palIndex, int fileID) const
{
	char* pPalFileAddr = nullptr;
	return TryGetPalFileAddr(palIndex, fileID, &pPalFileAddr);
}

bool CharPaletteHandle::IsPaletteDataReady() const
{
	if (m_pCurPalIndex == nullptr)
		return false;

	if (m_switchPalIndex1 < 0 || m_switchPalIndex1 > MAX_PAL_INDEX ||
		m_switchPalIndex2 < 0 || m_switchPalIndex2 > MAX_PAL_INDEX)
	{
		return false;
	}

	for (int fileID = 0; fileID < TOTAL_PALETTE_FILES; ++fileID)
	{
		if (!CanResolvePalFileAddr(m_switchPalIndex1, fileID) ||
			!CanResolvePalFileAddr(m_switchPalIndex2, fileID))
		{
			return false;
		}
	}

	return true;
}

int& CharPaletteHandle::GetPalIndexRef()
{
	return *m_pCurPalIndex;
}

void CharPaletteHandle::ReplacePalData(IMPL_data_t* newPaletteData)
{
	if (!IsPaletteDataReady())
	{
		LOG(1, "CharPaletteHandle::ReplacePalData skipped because palette storage is not ready\n");
		return;
	}

	SetCurrentPalInfo(&newPaletteData->palInfo);
	ReplaceAllPalFiles(newPaletteData, m_switchPalIndex1);
	ReplaceAllPalFiles(newPaletteData, m_switchPalIndex2);

	UpdatePalette();
}

void CharPaletteHandle::ReplaceSinglePalFile(const char* newPalData, PaletteFile palFile)
{
	if (!IsPaletteDataReady())
	{
		LOG(1, "CharPaletteHandle::ReplaceSinglePalFile skipped because palette storage is not ready (palFile=%d)\n", (int)palFile);
		return;
	}

	char* pDst1 = nullptr;
	char* pDst2 = nullptr;
	if (!TryGetPalFileAddr(m_switchPalIndex1, (int)palFile, &pDst1) ||
		!TryGetPalFileAddr(m_switchPalIndex2, (int)palFile, &pDst2))
	{
		LOG(1, "CharPaletteHandle::ReplaceSinglePalFile skipped because palette file pointers became invalid during update (palFile=%d)\n", (int)palFile);
		return;
	}
	
	ReplacePalArrayInMemory(pDst1, newPalData);
	ReplacePalArrayInMemory(pDst2, newPalData);
	

	UpdatePalette();
}

void CharPaletteHandle::OnMatchInit()
{
	int rawIndex = *m_pCurPalIndex;

	// The native color byte may still be sitting on last match's toggle-pair artifact left by
	// UpdatePalette()'s hot-swap redraw trick. We deliberately do NOT correct that byte in game
	// memory (earlier versions did, restoring it here and once per frame via
	// CorrectToggleArtifact) -- upstream never corrects it either, and a full year of upstream
	// play never reported this class of bug. A 2026-08-05 investigation found that reverting the
	// byte races the engine's own redraw: the engine appears to need to *observe* the toggled
	// value to notice the palette changed and refresh the character's texture, and correcting it
	// back -- even a few frames later -- intermittently won the race and left the redraw never
	// happening (worst on Taokaka; see docs/Research/TaokakaPaletteRefreshInvestigation.md).
	// Instead, if this looks like our own artifact, use the logical value we remembered
	// ourselves for OUR OWN bookkeeping (which palettes.ini slot to look up, where to read the
	// "Default" backup from) -- the live byte is left exactly as the engine wants it.
	int logicalIndex = rawIndex;
	if (m_lastLogicalPalIndex >= 0 &&
		(rawIndex == m_lastTogglePairA || rawIndex == m_lastTogglePairB) &&
		rawIndex != m_lastLogicalPalIndex)
	{
		LOG(1, "CharPaletteHandle::OnMatchInit raw slot %d looks like our own toggle artifact, using remembered logical slot %d for lookups (native byte left untouched)\n",
			rawIndex, m_lastLogicalPalIndex);
		logicalIndex = m_lastLogicalPalIndex;
	}

	m_origPalIndex = logicalIndex;
	m_lastLogicalPalIndex = logicalIndex;

	LOG(1, "CharPaletteHandle::OnMatchInit raw native color slot = %d, logical slot = %d (pCurPalIndex=%p)\n",
		rawIndex, logicalIndex, (void*)m_pCurPalIndex);

	BackupOrigPal(logicalIndex);

	m_selectedCustomPalIndex = 0;

	m_switchPalIndex1 = logicalIndex;

	m_switchPalIndex2 = m_switchPalIndex1 == MAX_PAL_INDEX
		? m_switchPalIndex1 - 1
		: m_switchPalIndex1 + 1;

	m_lastTogglePairA = m_switchPalIndex1;
	m_lastTogglePairB = m_switchPalIndex2;

	// Clear palette info
	IMPL_info_t palInfo = { "Default" };
	SetCurrentPalInfo(&palInfo);
}

void CharPaletteHandle::OnMatchRematch()
{
	// In case of a rematch we want to start with the original palindex
	*m_pCurPalIndex = m_origPalIndex;
}

void CharPaletteHandle::UnlockUpdate()
{
	m_updateLocked = false;
}

int CharPaletteHandle::GetSelectedCustomPalIndex()
{
	return m_selectedCustomPalIndex;
}

void CharPaletteHandle::SetSelectedCustomPalIndex(int index)
{
	m_selectedCustomPalIndex = index;
}

const char* CharPaletteHandle::GetCurPalFileAddr(PaletteFile palFile)
{
	char* pPalFileAddr = nullptr;
	if (!TryGetPalFileAddr(m_switchPalIndex1, (int)palFile, &pPalFileAddr))
		return nullptr;

	return pPalFileAddr;
}

const char* CharPaletteHandle::GetOrigPalFileAddr(PaletteFile palFile)
{
	const char* ret = m_origPalBackup.file0;
	ret += palFile * IMPL_PALETTE_DATALEN;
	return ret;
}

const IMPL_info_t& CharPaletteHandle::GetCurrentPalInfo() const
{
	return m_currentPalData.palInfo;
}

void CharPaletteHandle::SetCurrentPalInfo(IMPL_info_t* pPalInfo)
{
	memcpy_s(&m_currentPalData.palInfo, sizeof(IMPL_info_t), pPalInfo, sizeof(IMPL_info_t));
}

const IMPL_data_t& CharPaletteHandle::GetCurrentPalData()
{
	if (!IsPaletteDataReady())
	{
		LOG(1, "CharPaletteHandle::GetCurrentPalData returning cached data because palette storage is not ready\n");
		return m_currentPalData;
	}

	for (int i = 0; i < TOTAL_PALETTE_FILES; i++)
	{
		char* pDst = m_currentPalData.file0 + (i * IMPL_PALETTE_DATALEN);
		char* pSrc = nullptr;
		if (!TryGetPalFileAddr(m_switchPalIndex1, i, &pSrc))
		{
			LOG(1, "CharPaletteHandle::GetCurrentPalData returning cached data because palette file %d became invalid during readback\n", i);
			return m_currentPalData;
		}
		memcpy(pDst, pSrc, IMPL_PALETTE_DATALEN);
		pDst += IMPL_PALETTE_DATALEN;
	}

	return m_currentPalData;
}

bool CharPaletteHandle::IsCurrentPalWithBloom() const
{
	return m_origPalIndex == BLOOM_PALETTE_INDEX ||
		m_currentPalData.palInfo.hasBloom;
}

bool CharPaletteHandle::IsCustomPaletteActive() const
{
	if (m_pCurPalIndex == nullptr)
		return false;

	char* pLivePalData = nullptr;
	if (!TryGetPalFileAddr(*m_pCurPalIndex, PaletteFile_Character, &pLivePalData))
		return false;

	return memcmp(pLivePalData, m_origPalBackup.file0, IMPL_PALETTE_DATALEN) != 0;
}

void CharPaletteHandle::ReplacePalArrayInMemory(char* Dst, const void* Src)
{
	// The palette datas are duplicated in the memory at offset 0x800
	// We have to overwrite both, as some characters use the duplication's address instead
	memcpy(Dst, Src, IMPL_PALETTE_DATALEN);
	memcpy(Dst + 0x800, Src, IMPL_PALETTE_DATALEN);
}

void CharPaletteHandle::ReplaceAllPalFiles(IMPL_data_t* newPaletteData, int palIndex)
{
	static const char NULLBLOCK[IMPL_PALETTE_DATALEN]{ 0 };

	// If palname is "Default" then we load the original palette from backup
	if (strncmp(newPaletteData->palInfo.palName, "Default", IMPL_PALNAME_LENGTH) == 0)
		newPaletteData = &m_origPalBackup;

	for (int i = 0; i < TOTAL_PALETTE_FILES; i++)
	{
		const char* pSrc = newPaletteData->file0 + (i * IMPL_PALETTE_DATALEN);

		if (!memcmp(NULLBLOCK, pSrc, IMPL_PALETTE_DATALEN))
			continue;

		char* pDst = nullptr;
		if (!TryGetPalFileAddr(palIndex, i, &pDst))
		{
			LOG(1, "CharPaletteHandle::ReplaceAllPalFiles skipped palette file %d because destination pointer is invalid (palIndex=%d)\n", i, palIndex);
			continue;
		}
		ReplacePalArrayInMemory(pDst, pSrc);
	}
}

void CharPaletteHandle::BackupOrigPal(int palIndex)
{
	LOG(2, "CharPaletteHandle::BackupOrigPal\n");

	if (!IsPaletteDataReady())
	{
		LOG(1, "CharPaletteHandle::BackupOrigPal skipped because palette storage is not ready\n");
		return;
	}

	const char* pSrc = 0;
	char* pDst = m_origPalBackup.file0;

	for (int i = 0; i < TOTAL_PALETTE_FILES; i++)
	{
		char* pResolvedSrc = nullptr;
		if (!TryGetPalFileAddr(palIndex, i, &pResolvedSrc))
		{
			LOG(1, "CharPaletteHandle::BackupOrigPal aborted because palette file %d became invalid during backup\n", i);
			return;
		}
		pSrc = pResolvedSrc;
		memcpy(pDst, pSrc, IMPL_PALETTE_DATALEN);
		pDst += IMPL_PALETTE_DATALEN;
	}

	strncpy(m_origPalBackup.palInfo.palName, "Default", IMPL_PALNAME_LENGTH);
}

void CharPaletteHandle::RestoreOrigPal()
{
	LOG(2, "CharPaletteHandle::RestoreOrigPalette\n");

	ReplacePalData(&m_origPalBackup);
}

void CharPaletteHandle::UpdatePalette()
{
	// Must not switch more than once per frame, or palette doesn't get updated!
	if (m_updateLocked)
		return;

	*m_pCurPalIndex = *m_pCurPalIndex == m_switchPalIndex1
		? m_switchPalIndex2
		: m_switchPalIndex1;

	// This byte is left exactly as toggled -- see OnMatchInit for why we never correct it back.

	LOG(1, "CharPaletteHandle::UpdatePalette flipped native color slot to %d\n", *m_pCurPalIndex);

	LockUpdate();
}

void CharPaletteHandle::LockUpdate()
{
	m_updateLocked = true;
}
