#include "GlyphAtlasGuard.h"

#include "NetplayArenaKeeper.h"

#include "Core/logger.h"
#include "Core/utils.h"
#include "Hooks/HookManager.h"

#include <Windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>

namespace
{
	// FUN_0044c880, the glyph upload, just before it locks the staging texture:
	//   0x44C909  8B 43 3C   mov eax, [ebx+3Ch]      ; staging texture (may be null)
	//   0x44C90C  8D 55 D0   lea edx, [ebp-30h]
	//   0x44C90F  8B 08      mov ecx, [eax]          ; <- the crash
	// The two LockRect arguments are already pushed here. No branch in the function lands
	// inside these six bytes.
	constexpr uintptr_t kSiteRva = 0x0004C909;
	constexpr uintptr_t kFailExitRva = 0x0004C9F6;   // the exit a failed LockRect takes (returns 0)
	const uint8_t kSiteBytes[] = { 0x8B, 0x43, 0x3C, 0x8D, 0x55, 0xD0 };

	constexpr size_t kStagingSlot = 0x0F;   // int index of +0x3C
	constexpr size_t kDynamicSlot = 0x06;   // int index of +0x18, the D3DPOOL_DEFAULT twin
	constexpr DWORD kRetryIntervalMs = 2000;

	DWORD g_back = 0;
	DWORD g_failExit = 0;
	DWORD g_lastAttemptMs = 0;
	bool g_loggedSkip = false;

	// Recreates the staging texture the way the atlas's init made it: the same size, levels
	// and format as its video-memory twin, D3DUSAGE_DYNAMIC in D3DPOOL_SYSTEMMEM. Returns it,
	// stored in the atlas, or null. At most once every couple of seconds, since this runs
	// for every glyph while it keeps failing.
	IDirect3DTexture9* __cdecl RecreateStaging(int* atlas)
	{
		const DWORD now = GetTickCount();
		if (g_lastAttemptMs != 0 && now - g_lastAttemptMs < kRetryIntervalMs)
			return nullptr;
		g_lastAttemptMs = now;

		IDirect3DTexture9* twin = reinterpret_cast<IDirect3DTexture9*>(atlas[kDynamicSlot]);
		if (twin == nullptr)
			return nullptr;
		D3DSURFACE_DESC desc = {};
		if (FAILED(twin->GetLevelDesc(0, &desc)))
			return nullptr;
		IDirect3DDevice9* device = nullptr;
		if (FAILED(twin->GetDevice(&device)) || device == nullptr)
			return nullptr;

		IDirect3DTexture9* staging = nullptr;
		HRESULT hr = device->CreateTexture(desc.Width, desc.Height, twin->GetLevelCount(), D3DUSAGE_DYNAMIC,
			desc.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr);
		if (FAILED(hr) && ReleaseNetplayArenaReserve("The text atlas staging texture"))
		{
			hr = device->CreateTexture(desc.Width, desc.Height, twin->GetLevelCount(), D3DUSAGE_DYNAMIC,
				desc.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr);
		}
		device->Release();

		if (FAILED(hr) || staging == nullptr)
		{
			if (!g_loggedSkip)
			{
				LOG(0, "[GlyphAtlas] The text atlas has no staging texture and it cannot be made (hr=0x%08lX); "
				       "skipping glyph uploads instead of crashing.\n", static_cast<unsigned long>(hr));
				g_loggedSkip = true;
			}
			return nullptr;
		}
		atlas[kStagingSlot] = reinterpret_cast<int>(staging);
		g_loggedSkip = false;
		LOG(0, "[GlyphAtlas] Recreated the text atlas staging texture (%ux%u) that had failed to allocate.\n",
			desc.Width, desc.Height);
		return staging;
	}

	void __declspec(naked) GlyphUploadHook()
	{
		__asm
		{
			mov eax, [ebx + 3Ch]
			test eax, eax
			jz missing
		resume:
			lea edx, [ebp - 30h]
			jmp[g_back]
		missing:
			push ebx
			call RecreateStaging    // cdecl; keeps ebx, esi, edi, ebp
			add esp, 4
			test eax, eax
			jnz resume
			add esp, 8              // the two LockRect arguments already pushed
			jmp[g_failExit]
		}
	}
}

void InstallGlyphAtlasGuard()
{
	const uintptr_t base = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
	const uintptr_t site = base + kSiteRva;
	if (IsBadReadPtr(reinterpret_cast<const void*>(site), sizeof(kSiteBytes)) ||
		std::memcmp(reinterpret_cast<const void*>(site), kSiteBytes, sizeof(kSiteBytes)) != 0)
	{
		LOG(0, "[GlyphAtlas] Glyph upload site did not match the expected BBCF.exe; not patched.\n");
		return;
	}
	g_failExit = static_cast<DWORD>(base + kFailExitRva);
	g_back = HookManager::SetHook("GlyphAtlasStagingGuard", static_cast<DWORD>(site), sizeof(kSiteBytes),
		reinterpret_cast<void*>(GlyphUploadHook));
	LOG(1, "[GlyphAtlas] %s\n", g_back ? "Null staging texture guard installed" : "Could not install the staging texture guard");
}
