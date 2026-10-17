#include "LargeAddressAware.h"

#include "logger.h"

#include "D3D9EXWrapper/ID3DXWrapper_Effect.h"

#include <Windows.h>
#include <cstring>

namespace
{
	// d3dx9effect.h: D3DXFX_LARGEADDRESSAWARE (1 << 17).
	constexpr DWORD kD3DXFxLargeAddressAware = 1u << 17;

	typedef HRESULT(WINAPI* D3DXCreateEffectFn)(void* device, const void* srcData, UINT srcDataLen,
		const void* defines, void* include, DWORD flags, void* pool, void** effect, void** compilationErrors);

	D3DXCreateEffectFn g_originalCreateEffect = nullptr;
	bool g_processLaa = false;
	bool g_installed = false;
	const char* g_installFailure = nullptr;
	volatile LONG g_effectsFlagged = 0;

	// ---- null parameter handles ----
	//
	// The game looks its parameters up with GetParameterBySemantic, which returns 0 when an effect
	// has no parameter with that semantic, and then calls the setter with that 0 anyway
	// (FUN_00449990 and siblings check the descriptor, not the handle). Vanilla D3DX answers a null
	// handle with D3DERR_INVALIDCALL, which the game ignores. With D3DXFX_LARGEADDRESSAWARE, D3DX
	// decodes handles as ~pointer, so 0 becomes 0xFFFFFFFF and CEffect::SetMatrix reads address 3
	// (Crash_20261010_155837). These guards give the null handle its vanilla answer back.

	constexpr HRESULT kD3DErrInvalidCall = static_cast<HRESULT>(0x8876086C);
	constexpr int kMaxSlot = 64;
	void* g_originalSlots[kMaxSlot] = {};

	template <int Slot>
	HRESULT WINAPI GuardHandle0(void* self, const void* handle)
	{
		if (!handle)
			return kD3DErrInvalidCall;
		return reinterpret_cast<HRESULT(WINAPI*)(void*, const void*)>(g_originalSlots[Slot])(self, handle);
	}

	// Every extra argument of these methods is 4 bytes (pointers, UINT, INT, BOOL, FLOAT), so
	// passing them on as DWORDs keeps the stack layout and the callee-pop size identical.
	template <int Slot>
	HRESULT WINAPI GuardHandle1(void* self, const void* handle, DWORD a)
	{
		if (!handle)
			return kD3DErrInvalidCall;
		return reinterpret_cast<HRESULT(WINAPI*)(void*, const void*, DWORD)>(g_originalSlots[Slot])(self, handle, a);
	}

	template <int Slot>
	HRESULT WINAPI GuardHandle2(void* self, const void* handle, DWORD a, DWORD b)
	{
		if (!handle)
			return kD3DErrInvalidCall;
		return reinterpret_cast<HRESULT(WINAPI*)(void*, const void*, DWORD, DWORD)>(g_originalSlots[Slot])(self, handle, a, b);
	}

	struct SlotGuard
	{
		int slot;
		void* guard;
	};

	// ID3DXEffect vtable slots (d3dx9effect.h order) whose first argument is a parameter or
	// technique handle.
	const SlotGuard kGuards[] = {
		{ 0x50 / 4, reinterpret_cast<void*>(&GuardHandle2<0x50 / 4>) }, // SetValue(h, data, bytes)
		{ 0x58 / 4, reinterpret_cast<void*>(&GuardHandle1<0x58 / 4>) }, // SetBool
		{ 0x60 / 4, reinterpret_cast<void*>(&GuardHandle2<0x60 / 4>) }, // SetBoolArray
		{ 0x68 / 4, reinterpret_cast<void*>(&GuardHandle1<0x68 / 4>) }, // SetInt
		{ 0x70 / 4, reinterpret_cast<void*>(&GuardHandle2<0x70 / 4>) }, // SetIntArray
		{ 0x78 / 4, reinterpret_cast<void*>(&GuardHandle1<0x78 / 4>) }, // SetFloat
		{ 0x80 / 4, reinterpret_cast<void*>(&GuardHandle2<0x80 / 4>) }, // SetFloatArray
		{ 0x88 / 4, reinterpret_cast<void*>(&GuardHandle1<0x88 / 4>) }, // SetVector
		{ 0x90 / 4, reinterpret_cast<void*>(&GuardHandle2<0x90 / 4>) }, // SetVectorArray
		{ 0x98 / 4, reinterpret_cast<void*>(&GuardHandle1<0x98 / 4>) }, // SetMatrix
		{ 0xA0 / 4, reinterpret_cast<void*>(&GuardHandle2<0xA0 / 4>) }, // SetMatrixArray
		{ 0xA8 / 4, reinterpret_cast<void*>(&GuardHandle2<0xA8 / 4>) }, // SetMatrixPointerArray
		{ 0xB0 / 4, reinterpret_cast<void*>(&GuardHandle1<0xB0 / 4>) }, // SetMatrixTranspose
		{ 0xB8 / 4, reinterpret_cast<void*>(&GuardHandle2<0xB8 / 4>) }, // SetMatrixTransposeArray
		{ 0xC0 / 4, reinterpret_cast<void*>(&GuardHandle2<0xC0 / 4>) }, // SetMatrixTransposePointerArray
		{ 0xC8 / 4, reinterpret_cast<void*>(&GuardHandle1<0xC8 / 4>) }, // SetString
		{ 0xD0 / 4, reinterpret_cast<void*>(&GuardHandle1<0xD0 / 4>) }, // SetTexture
		{ 0xE8 / 4, reinterpret_cast<void*>(&GuardHandle0<0xE8 / 4>) }, // SetTechnique
	};

	void** g_guardedVtable = nullptr;
	bool g_vtableMismatch = false;

	// Every effect D3DX hands out is a D3DXEffects::CEffect sharing one vtable in d3dx9_43.dll, so
	// it is patched once, on the first effect, from the game's main thread.
	void GuardEffectVtable(void* effect)
	{
		void** vtable = *reinterpret_cast<void***>(effect);

		HMODULE owner = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(vtable), &owner))
			return;

		// The mod's own ID3DXEffectWrapper (hook_D3DXCreateEffect, a detour on the export this
		// import leads to) stands in for every effect: guard the real one it forwards to. Missing
		// this let the null-handle crash through on a large-address-aware boot (2026-10-17).
		HMODULE self = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&GuardEffectVtable), &self) && owner == self)
		{
			if (ID3DXEffect* inner = static_cast<ID3DXEffectWrapper*>(static_cast<ID3DXEffect*>(effect))->m_D3DXEffect)
				GuardEffectVtable(inner);
			return;
		}
		if (owner != GetModuleHandleW(L"d3dx9_43.dll"))
			return;

		if (g_guardedVtable)
		{
			if (vtable != g_guardedVtable)
				g_vtableMismatch = true;
			return;
		}

		for (const SlotGuard& guard : kGuards)
		{
			DWORD oldProtect = 0;
			if (!VirtualProtect(&vtable[guard.slot], sizeof(void*), PAGE_READWRITE, &oldProtect))
				continue;
			g_originalSlots[guard.slot] = vtable[guard.slot];
			InterlockedExchangePointer(&vtable[guard.slot], guard.guard);
			VirtualProtect(&vtable[guard.slot], sizeof(void*), oldProtect, &oldProtect);
		}
		g_guardedVtable = vtable;
		LOG(1, "[LAA] Null-handle guards installed on the D3DX effect vtable at 0x%p.\n", vtable);
	}

	HRESULT WINAPI CreateEffectLargeAddressAware(void* device, const void* srcData, UINT srcDataLen,
		const void* defines, void* include, DWORD flags, void* pool, void** effect, void** compilationErrors)
	{
		InterlockedIncrement(&g_effectsFlagged);
		const HRESULT hr = g_originalCreateEffect(device, srcData, srcDataLen, defines, include,
			flags | kD3DXFxLargeAddressAware, pool, effect, compilationErrors);
		if (SUCCEEDED(hr) && effect && *effect)
			GuardEffectVtable(*effect);
		return hr;
	}

	// ---- particle file pointer fix-up ----
	//
	// FUN_0045b280 turns the offsets in a loaded particle file into pointers with 64-bit adds
	// (add [ecx+2Ch],eax / adc [ecx+30h],edx, and so on) over fields that are only 4 bytes
	// apart, so each add also adds the high dword into the NEXT field. Its only caller builds
	// that high dword with cdq at 0x45A414, sign-extending the buffer's address: 0 below 2 GB,
	// 0xFFFFFFFF above it. Above 2 GB every following pointer comes out one byte short and the
	// particle code reads garbage (BBCF.exe+0x5A78D, on the main menu). Zero-extending instead
	// is what every buffer below 2 GB already gets.
	//   0x45A414: 99        cdq              ->  8B C8  mov ecx,eax
	//   0x45A415: 8B C8     mov ecx,eax      ->  33 C0  xor eax,eax
	//   0x45A417: 8B C2     mov eax,edx      ->  90     nop
	// No branch lands inside the patched bytes.
	constexpr DWORD kParticleFixupRva = 0x5A414;
	const BYTE kParticleFixupOriginal[] = { 0x99, 0x8B, 0xC8, 0x8B, 0xC2 };
	const BYTE kParticleFixupPatched[] = { 0x8B, 0xC8, 0x33, 0xC0, 0x90 };
	const char* g_particlePatchResult = "not attempted";

	void PatchParticleFixup(BYTE* base)
	{
		BYTE* site = base + kParticleFixupRva;
		if (std::memcmp(site, kParticleFixupOriginal, sizeof(kParticleFixupOriginal)) != 0)
		{
			g_particlePatchResult = "bytes did not match, left alone";
			return;
		}
		DWORD oldProtect = 0;
		if (!VirtualProtect(site, sizeof(kParticleFixupPatched), PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			g_particlePatchResult = "VirtualProtect failed";
			return;
		}
		std::memcpy(site, kParticleFixupPatched, sizeof(kParticleFixupPatched));
		VirtualProtect(site, sizeof(kParticleFixupPatched), oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), site, sizeof(kParticleFixupPatched));
		g_particlePatchResult = "applied";
	}

	const IMAGE_NT_HEADERS32* MainModuleHeaders(BYTE*& base)
	{
		base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
		if (!base)
			return nullptr;
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return nullptr;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
		return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
	}

	// The exe's import slot for dll!function, or null.
	void** FindImportSlot(BYTE* base, const IMAGE_NT_HEADERS32* nt, const char* dll, const char* function)
	{
		const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress)
			return nullptr;

		for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc)
		{
			if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0)
				continue;

			auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
			auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++slots)
			{
				if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal))
					continue;
				const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
				if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0)
					return reinterpret_cast<void**>(&slots->u1.Function);
			}
		}
		return nullptr;
	}
}

namespace LargeAddressAware
{
	bool IsProcessLargeAddressAware()
	{
		BYTE* base = nullptr;
		const IMAGE_NT_HEADERS32* nt = MainModuleHeaders(base);
		return nt && (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
	}

	void InstallD3DXEffectFlag()
	{
		BYTE* base = nullptr;
		const IMAGE_NT_HEADERS32* nt = MainModuleHeaders(base);
		g_processLaa = nt && (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
		if (!g_processLaa)
			return;

		PatchParticleFixup(base);

		void** slot = FindImportSlot(base, nt, "d3dx9_43.dll", "D3DXCreateEffect");
		if (!slot || !*slot)
		{
			g_installFailure = "BBCF.exe has no resolved d3dx9_43!D3DXCreateEffect import";
			return;
		}

		DWORD oldProtect = 0;
		if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
		{
			g_installFailure = "VirtualProtect on the import slot failed";
			return;
		}
		g_originalCreateEffect = reinterpret_cast<D3DXCreateEffectFn>(*slot);
		InterlockedExchangePointer(slot, reinterpret_cast<void*>(&CreateEffectLargeAddressAware));
		VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
		g_installed = true;
	}

	void LogStatus()
	{
		if (!g_processLaa)
		{
			ForceLog("[LAA] BBCF.exe is not large-address-aware (2 GB address space); D3DX effects created as vanilla.\n");
			return;
		}
		if (g_installed)
			ForceLog("[LAA] BBCF.exe is large-address-aware (4 GB). D3DXCreateEffect import now adds D3DXFX_LARGEADDRESSAWARE "
				"(%ld effect(s) created so far, null-handle guards %s%s). Particle pointer fix-up patch: %s.\n",
				g_effectsFlagged, g_guardedVtable ? "on" : "not yet installed",
				g_vtableMismatch ? ", WARNING: an effect with a different vtable was not guarded" : "",
				g_particlePatchResult);
		else
			ForceLog("[LAA] BBCF.exe is large-address-aware but the D3DX effect flag could not be installed: %s. Effects in memory above 2 GB will fail.\n",
				g_installFailure ? g_installFailure : "unknown");
	}
}
