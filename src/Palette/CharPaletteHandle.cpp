#include "CharPaletteHandle.h"

#include "Core/logger.h"
#include "Core/Settings.h"
#include "Core/utils.h"
#include "Game/GhidraDefs.h"

#include <cstring>

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

void CharPaletteHandle::SetPointerPalIndex(int* pPalIndex)
{
	m_pCurPalIndex = pPalIndex;
}

void CharPaletteHandle::OnContainerLoaded(char* pOwner)
{
	m_pOwner = pOwner;
	++m_loadGeneration;
	// The legacy toggle, if any, was on the previous battle's colour; that battle is gone.
	m_legacyToggled = false;
}

// The palette container of the battle that is loaded right now, or null. owner+0x830 is zeroed by
// the battle destructor (FUN_0055C9E0) before the container is freed, and the vtable check rejects
// anything else that might have landed there.
char* CharPaletteHandle::GetLiveContainer() const
{
	if (m_pOwner == nullptr)
		return nullptr;

	__try
	{
		char* const container = *reinterpret_cast<char**>(m_pOwner + OFFSET_PlayerRes_PaletteContainer);
		if (container == nullptr)
			return nullptr;

		const uintptr_t vtable = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress()) + ADDR_DynamicTextureListVtable;
		if (*reinterpret_cast<uintptr_t*>(container) != vtable)
			return nullptr;

		return container;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

// Sub colour*8+file of the container, with the same bounds the game's own lookup applies
// (FUN_005B6AB0 clamps anything at or above owner+0x18 to sub 0).
bool CharPaletteHandle::TryGetPalObject(int palIndex, int fileID, char** ppPalObj) const
{
	if (ppPalObj == nullptr)
		return false;

	*ppPalObj = nullptr;

	if (palIndex < 0 || palIndex > MAX_PAL_INDEX || fileID < 0 || fileID >= TOTAL_PALETTE_FILES)
		return false;

	char* const container = GetLiveContainer();
	if (container == nullptr)
		return false;

	__try
	{
		const uint32_t sub = static_cast<uint32_t>(palIndex * TOTAL_PALETTE_FILES + fileID);
		const uint32_t subCount = *reinterpret_cast<const uint32_t*>(container + OFFSET_Resource_SubObjectCount);
		const uint32_t ownerCount = *reinterpret_cast<const uint32_t*>(m_pOwner + OFFSET_PlayerRes_PaletteCount);
		if (sub >= subCount || sub >= ownerCount)
			return false;

		char* const* const subs = *reinterpret_cast<char* const* const*>(container + OFFSET_Resource_SubObjects);
		if (subs == nullptr || subs[sub] == nullptr)
			return false;

		char* const obj = subs[sub];
		if (*reinterpret_cast<const int*>(obj + OFFSET_PalObj_Width) != 256 ||
			*reinterpret_cast<const int*>(obj + OFFSET_PalObj_Bpp) != 32)
			return false;

		*ppPalObj = obj;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

bool CharPaletteHandle::TryGetPalFileAddr(int palIndex, int fileID, char** ppPalFileAddr) const
{
	char* obj = nullptr;
	if (!TryGetPalObject(palIndex, fileID, &obj))
	{
		if (ppPalFileAddr)
			*ppPalFileAddr = nullptr;
		return false;
	}

	*ppPalFileAddr = obj + OFFSET_PalObj_Data;
	return true;
}

bool CharPaletteHandle::CanResolveAllFiles(int palIndex) const
{
	char* obj = nullptr;
	for (int fileID = 0; fileID < TOTAL_PALETTE_FILES; ++fileID)
	{
		if (!TryGetPalObject(palIndex, fileID, &obj))
			return false;
	}
	return true;
}

int CharPaletteHandle::GetOrigPalIndex() const
{
	return m_origPalIndex;
}

bool CharPaletteHandle::IsNullPointerPalIndex() const
{
	return m_pCurPalIndex == nullptr;
}

bool CharPaletteHandle::IsInitializedForCurrentLoad() const
{
	return m_initGeneration != 0 && m_initGeneration == m_loadGeneration;
}

bool CharPaletteHandle::IsPaletteDataReady() const
{
	if (m_pCurPalIndex == nullptr || !IsInitializedForCurrentLoad())
		return false;

	if (!CanResolveAllFiles(m_origPalIndex))
		return false;

	if (Settings::settingsIni.paletteLegacyIndexToggle && !CanResolveAllFiles(m_legacySwitchIndex))
		return false;

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
	ReplaceAllPalFiles(newPaletteData, m_origPalIndex);
	if (Settings::settingsIni.paletteLegacyIndexToggle)
		ReplaceAllPalFiles(newPaletteData, m_legacySwitchIndex);

	AfterWrite();
}

void CharPaletteHandle::ReplaceSinglePalFile(const char* newPalData, PaletteFile palFile)
{
	if (!IsPaletteDataReady())
	{
		LOG(1, "CharPaletteHandle::ReplaceSinglePalFile skipped because palette storage is not ready (palFile=%d)\n", (int)palFile);
		return;
	}

	char* pDst1 = nullptr;
	if (!TryGetPalObject(m_origPalIndex, (int)palFile, &pDst1))
	{
		LOG(1, "CharPaletteHandle::ReplaceSinglePalFile skipped because the palette became invalid during update (palFile=%d)\n", (int)palFile);
		return;
	}
	WritePalObject(pDst1, newPalData);

	char* pDst2 = nullptr;
	if (Settings::settingsIni.paletteLegacyIndexToggle && TryGetPalObject(m_legacySwitchIndex, (int)palFile, &pDst2))
		WritePalObject(pDst2, newPalData);

	AfterWrite();
}

bool CharPaletteHandle::OnMatchInit()
{
	if (m_pCurPalIndex == nullptr || GetLiveContainer() == nullptr)
	{
		LOG(1, "CharPaletteHandle::OnMatchInit: this battle's palette container is not available yet (pCurPalIndex=%p owner=%p)\n",
			(void*)m_pCurPalIndex, (void*)m_pOwner);
		return false;
	}

	// The game wrote this before the battle loaded (character select offline, the network records
	// online) and nothing of ours has touched it since: the mod no longer writes it.
	const int rawIndex = *m_pCurPalIndex;
	if (rawIndex < 0 || rawIndex > MAX_PAL_INDEX)
	{
		LOG(1, "CharPaletteHandle::OnMatchInit: colour %d out of range\n", rawIndex);
		return false;
	}

	m_origPalIndex = rawIndex;
	m_legacySwitchIndex = rawIndex == MAX_PAL_INDEX ? rawIndex - 1 : rawIndex + 1;
	m_legacyToggled = false;
	m_initGeneration = m_loadGeneration;

	if (!IsPaletteDataReady())
	{
		LOG(1, "CharPaletteHandle::OnMatchInit: colour %d cannot be resolved in the container yet\n", rawIndex);
		m_initGeneration = 0;
		return false;
	}

	LOG(1, "CharPaletteHandle::OnMatchInit native color slot = %d (load %u)\n", m_origPalIndex, m_loadGeneration);

	BackupOrigPal();

	m_selectedCustomPalIndex = 0;

	IMPL_info_t palInfo = { "Default" };
	SetCurrentPalInfo(&palInfo);
	return true;
}

void CharPaletteHandle::RestoreNativePalIndex(const char* reason)
{
	if (!m_legacyToggled || m_pCurPalIndex == nullptr || m_origPalIndex < 0)
		return;

	m_legacyToggled = false;

	if (*m_pCurPalIndex == m_origPalIndex)
		return;

	LOG(1, "CharPaletteHandle::RestoreNativePalIndex (%s) native color slot %d -> %d\n",
		reason, *m_pCurPalIndex, m_origPalIndex);

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
	if (!IsInitializedForCurrentLoad())
		return nullptr;

	char* pPalFileAddr = nullptr;
	if (!TryGetPalFileAddr(m_origPalIndex, (int)palFile, &pPalFileAddr))
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
		if (!TryGetPalFileAddr(m_origPalIndex, i, &pSrc))
		{
			LOG(1, "CharPaletteHandle::GetCurrentPalData returning cached data because palette file %d became invalid during readback\n", i);
			return m_currentPalData;
		}
		memcpy(pDst, pSrc, IMPL_PALETTE_DATALEN);
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
	if (!IsInitializedForCurrentLoad())
		return false;

	char* pLivePalData = nullptr;
	if (!TryGetPalFileAddr(m_origPalIndex, PaletteFile_Character, &pLivePalData))
		return false;

	return memcmp(pLivePalData, m_origPalBackup.file0, IMPL_PALETTE_DATALEN) != 0;
}

// Both copies: the working copy (+0x81C) is what gets uploaded, and the base copy (+0x1C) is what
// character commands such as VH_Frame or NT_HairSwitch restore the working copy from. The dirty flag
// is what AA_CPalette::LockWorking sets when the game edits a palette itself.
void CharPaletteHandle::WritePalObject(char* pPalObj, const void* Src)
{
	memcpy(pPalObj + OFFSET_PalObj_Data, Src, IMPL_PALETTE_DATALEN);
	memcpy(pPalObj + OFFSET_PalObj_DataDup, Src, IMPL_PALETTE_DATALEN);
	*reinterpret_cast<int*>(pPalObj + OFFSET_PalObj_Dirty) = 1;
	++m_writeGeneration;
}

// Bind re-uploads a sub only when it differs from the last one it uploaded or the palette is dirty,
// and a PalObjCopy from the player palette (own-slot copies, tint modes) clears dirty before the
// draw gets there. -1 makes the next Bind of any sub upload, exactly as OnResetDevice does.
void CharPaletteHandle::InvalidateUpload()
{
	char* const container = GetLiveContainer();
	if (container == nullptr)
		return;

	*reinterpret_cast<int*>(container + OFFSET_TexList_LastUploadedSub) = -1;
}

void CharPaletteHandle::AfterWrite()
{
	InvalidateUpload();

	if (Settings::settingsIni.paletteLegacyIndexToggle)
		LegacyToggleIndex();
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

		// An .impl with an empty effect file keeps this colour's native one. Since only the picked
		// colour is ever written now, that is the same native file every round.
		if (!memcmp(NULLBLOCK, pSrc, IMPL_PALETTE_DATALEN))
			pSrc = m_origPalBackup.file0 + (i * IMPL_PALETTE_DATALEN);

		char* pDst = nullptr;
		if (!TryGetPalObject(palIndex, i, &pDst))
		{
			LOG(1, "CharPaletteHandle::ReplaceAllPalFiles skipped palette file %d because destination is invalid (palIndex=%d)\n", i, palIndex);
			continue;
		}
		WritePalObject(pDst, pSrc);
	}
}

void CharPaletteHandle::BackupOrigPal()
{
	LOG(2, "CharPaletteHandle::BackupOrigPal\n");

	char* pDst = m_origPalBackup.file0;

	for (int i = 0; i < TOTAL_PALETTE_FILES; i++)
	{
		char* pSrc = nullptr;
		if (!TryGetPalFileAddr(m_origPalIndex, i, &pSrc))
		{
			LOG(1, "CharPaletteHandle::BackupOrigPal aborted because palette file %d became invalid during backup\n", i);
			return;
		}
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

// PaletteLegacyIndexToggle: the pre-8.9.1 redraw. Flips the live colour between the picked colour
// and its neighbour (both hold the palette). Kept only as a fallback; it changes the colour the game
// itself reads, which is why the default path does not do it.
void CharPaletteHandle::LegacyToggleIndex()
{
	// Must not switch more than once per frame, or palette doesn't get updated!
	if (m_updateLocked || m_pCurPalIndex == nullptr)
		return;

	*m_pCurPalIndex = *m_pCurPalIndex == m_origPalIndex
		? m_legacySwitchIndex
		: m_origPalIndex;
	m_legacyToggled = true;

	m_updateLocked = true;
}
