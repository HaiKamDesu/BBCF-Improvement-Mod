#pragma once
#include "impl_format.h"
#include "PaletteFolderLoader.h"

#include "CharPaletteHandle.h"

#include "Game/characters.h"
#include "Game/Player.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
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

	// Puts the palette vectors into a usable empty state (every character holding only its
	// "Default" entry) without touching the disk, then kicks the real load onto a worker
	// thread. Reading a large palette collection off a cold disk took 14.5s in one report,
	// and doing that inline at the title screen froze the game for exactly that long.
	void StartAsyncPaletteLoad();

	// Swaps in a finished background load. Cheap and non-blocking when the load is still
	// running or already collected; call it once per frame from the game thread.
	void PumpAsyncPaletteLoad();

	// Blocks until the palette data is actually there. For callers that cached the vector
	// reference from GetCustomPalettesVector() and so never go back through a guarded getter.
	void EnsurePalettesReady();

	// Cancels a background load and waits a bounded time for the worker to notice. Called
	// from the destructor, which can run from DllMain - so it must never block forever.
	void ShutdownAsyncPaletteLoad();

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
	// The worker builds into its own PaletteSet and the game thread swaps the result in, so
	// no reader ever observes a half-populated m_customPalettes and no read path needs a lock.
	struct AsyncPaletteLoad
	{
		std::thread worker;
		std::atomic<bool> done { false };
		std::atomic<bool> cancel { false };
		PaletteLoadOutcome outcome;
	};

	std::unique_ptr<AsyncPaletteLoad> m_asyncLoad;

	// Every palette consumer runs on the game thread today, so this only guards against a
	// future caller that does not. Reads take the mutex solely while a load is in flight -
	// m_loadPending is the fast path, and it is false for the whole rest of the session.
	mutable std::mutex m_asyncLoadMutex;
	std::atomic<bool> m_loadPending { false };

	// Blocks until any in-flight background load has been swapped in. Every accessor that
	// reads palette data calls it, so callers can never see the pre-load placeholder set by
	// accident; it costs one atomic load once the data is in.
	void EnsurePalettesLoaded() const;
	void CollectAsyncPaletteLoad();
	void CollectAsyncPaletteLoadLocked();
	void AdoptLoadOutcome(PaletteLoadOutcome& outcome);

	std::vector<std::vector<IMPL_data_t>> m_customPalettes;
	std::vector<std::vector<std::string>> m_paletteSlots;
	std::vector<int> m_onlinePalsStartIndex;
	bool m_loadOnlinePalettes = false;
	bool m_PaletteArchiveDownloaded = false;

	void CreatePaletteFolders();
	void InitCustomPaletteVector();
	void LoadPalettesFromFolder();
	void InitPaletteSlotsVector();
	void InitOnlinePalsIndexVector();
	void ApplyDefaultCustomPalette(CharIndex charIndex, CharPaletteHandle& charPalHandle);
};
