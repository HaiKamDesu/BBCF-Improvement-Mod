# Memory and address space

Why long sessions crash with out-of-memory, what the game allocates and how, and what it takes to
run BBCF large-address-aware (4 GB instead of 2 GB). Mapped 2026-10-10 from the archived build
11073075, six crash dumps, and a stress harness that forces every allocation above 2 GB.

## 0. The answer so far

**The crashes are address-space fragmentation, not leaks.** BBCF.exe is a 32-bit process without
the large-address-aware bit (PE characteristics 0x0102), so it gets 2 GB. In every dump from a
long session, 600-1100 MB of that 2 GB is held by things neither the game nor the mod controls:

| Holder | Size in the dumps | Notes |
|---|---|---|
| GPU driver write-combine mappings (`PAGE_WRITECOMBINE`, 16-32 MB each) | 168-468 MB | grows over a session |
| Third-party DLL images | 220-366 MB | nvgpucomp32 75 MB, nvd3dum 65 MB, steamclient 21 MB |
| Shell extensions loaded by a file dialog | ~60 MB | drivefsext.dll (Google Drive) 34 MB, OneDrive, shell32 |
| Mapped files | 120-200 MB | |

The game then asks for single contiguous blocks of 62 MB (the unpacked char_select_stage.pac),
100.6 MB (the netplay arena) or 16 MB (a SYSTEMMEM texture). With 380 MB free in total the largest
hole was 27 MB (Crash_20261008_195724). Each per-site workaround (NetplayArenaKeeper,
GlyphAtlasGuard) moves the failure to the next big request; fragmentation itself is structural.

**The real fix is large-address-aware**, which gives the process 4 GB under WOW64 with the upper
2 GB empty. That is only safe once the code that breaks above 2 GB is found and fixed. Status:

| # | Breaks above 2 GB | Symptom | Fix | State |
|---|---|---|---|---|
| 1 | D3DX9 effects created without `D3DXFX_LARGEADDRESSAWARE` | 62 of 63 `D3DXCreateEffect` calls return E_OUTOFMEMORY, null effect, crash at BBCF.exe+0x4972C | `src/Core/LargeAddressAware.cpp` patches BBCF.exe's import of D3DXCreateEffect from DllMain to OR the flag in | fixed, verified under stress |
| 2 | D3DX in large-address mode decodes a null handle as `~0` | the game calls setters with the 0 that GetParameterBySemantic returns for missing semantics; CEffect::SetMatrix reads address 3 | guards on the 18 handle-taking slots of the CEffect vtable return D3DERR_INVALIDCALL for a null handle, as vanilla D3DX does | fixed, verified under stress |
| 3 | Particle file pointer fix-up sign-extends the buffer address | every pointer after the first comes out 1 byte short, crash at BBCF.exe+0x5A78D on the main menu | 5-byte patch at 0x45A414: `cdq` becomes a zero-extend | fixed, verified under stress |
| 4 | Battle logic | in a training match under stress both characters spawn stacked mid-stage and nothing animates; the game loop keeps running, no crash | unknown | **open** |
| 5 | (mod) a `std::map<int, std::string>` swapped while another thread uses it, at startup | AV in `std::_Container_base12::_Swap_proxy_and_iterators_unlocked` writing through 0xFEEEFEEE, only seen under the debugger | unknown | **open**, not LAA-related, a startup race |

Fixes 1-3 apply only when the running exe carries the large-address-aware bit; on a stock exe the
mod behaves exactly as before.

**Parked 2026-10-10, resumed 2026-10-17 with a different safety argument.** The bit stays the
fix, but the upper 2 GB is no longer handed to the game from boot. `src/Core/HighMemoryReserve.cpp`
reserves all of it from DllMain (before WinMain, before the D3D device), so the game, the GPU driver
and everything else lay memory out exactly as on a stock exe. It is released only when the game
would otherwise run out below 2 GB, which on a stock exe is a crash. Whatever still breaks above
2 GB (bug 4, or bugs not found yet) can then only break in a session that would have ended anyway.
Release triggers, all funnelled through `ReleaseNetplayArenaReserve` / `HighMemoryReserve::Release`:

| Trigger | Where | Covers |
|---|---|---|
| operator new's `call _callnewh` at 0x0079E091 retargeted | NetplayArenaKeeper.cpp `OperatorNewOutOfMemory` | every `new` in BBCF (554 call sites), on any exe |
| netplay arena / file-slot sites, malloc-first | `RescuingAlloc` | the 100.6 MB arena and the 62 MB stage file |
| D3D texture creation returning E_OUTOFMEMORY | ID3D9EXWrapper_Device.cpp, GlyphAtlasGuard.cpp | textures |
| background watcher, every 2 s, largest truly free block below 2 GB < 32 MB | HighMemoryReserve.cpp | plain malloc callers that would get a null |

Verified 2026-10-17 on the run copy (`BBCF_laa.exe` as BBCF.exe): stock exe unchanged; LAA exe normal
session holds the half and plays a training match; `BBCF_IM_LAA_STRESS=4` (low half plugged 25 s in,
watcher off) released it through the stage file (training match then played normally, combos and
effects) and through the arena (a replay played with the arena at 0x8000F020); a probe `new` released
it through the _callnewh hook; mode 3 released it through the watcher. Bug 4 did NOT reproduce after
a release (only under the full from-boot stress). Also found and fixed: the D3DX null-handle guards
were skipped because the mod's own ID3DXEffectWrapper (hook_D3DXCreateEffect detour) stood in front
of the D3DX vtable; GuardEffectVtable now unwraps it.

Caveat measured the same day: in an ordinary LAA training match on the dev machine the largest truly
free block below 2 GB was already 65.5 MB, so on weaker machines the watcher will release in some
sessions that would not have crashed. Bug 4 therefore still matters. Not done yet: the mod setting
the bit on players' exes (rename-and-replace, takes effect next launch).

## 1. How the game allocates

Everything goes to the process default heap. The static VS2012 CRT takes `GetProcessHeap()` in
`_heap_init` (0x7B4E99) and stores it in `_crtheap` (0x0182B7BC). The game imports no
VirtualAlloc, HeapCreate, LocalAlloc or MapViewOfFile; it never reserves address space itself. Any
heap block over ~508 KB becomes its own VirtualAlloc region inside ntdll, so every big request
needs a fresh contiguous hole.

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x007A106D | CRT malloc | | L1 | HeapAlloc on _crtheap at 0x7A10B0 |
| 0x007A1035 | CRT free | | L1 | HeapFree |
| 0x0079E086 | operator new | | L1 | malloc / _callnewh / throw bad_alloc loop; 554 call sites |
| 0x00787469 | operator new thunk | | L1 | 194 call sites |
| 0x0079E864 | delete thunk | | L1 | -> 0x79E0D5 -> 0x7A1035 |
| 0x0079FAD9 | _aligned_malloc | | L1 | 4 callers, all 16-byte MemPools |
| 0x0079FAC1 | _aligned_free | | L1 | pool destructors |
| 0x007A3165 | _heapmin | | L1 | HeapCompact, called by the slot loader before every alloc and free |
| 0x0047C930 | FileSlotAlloc | | L2 | thiscall `ret 8` (slot, size). 0x3D4 slots x {ptr,size}; frees the old buffer, `_heapmin`, allocs. 273 of 278 callers pass a file size; rejects sizes >= 2 GB with `test esi,esi; js` |
| 0x0047C9A0 | FileSlotFree | | L1 | thiscall `ret 4` |
| 0x0047C9F0 | FileSlotFreeRange | | L1 | frees slots 0x82-0x3D3; 0-0x81 are resident |
| 0x0044FB70 | FileReaderThread | | L1 | allocs the compressed size (0x44FD2C), decompresses via 0x814AA0 into the slot buffer, frees it (0x44FF06): a load briefly holds both |
| 0x0040AA20 | AA_CParticleManager ctor | | L1 | MemPool {base, align 0x10, cap 0x260000, used, ...} at +0x5C |
| 0x00409930 | sys_ModelMngr init | | L1 | ScriptMemPool 1.19 MB + NodeMemPool |
| 0x00558A20 | battle_ObjectMngr pool | | L1 | 2.44 MB aligned pool per battle |
| 0x006408E0 | ETC_ObjectMngr pool | | L1 | 2.31 MB aligned pool |
| 0x006941D0 | Johchu buffer | | L1 | 10,000,000 B resident file buffer at 0x13874C8, never freed |
| 0x007864C0 | Netplay arena alloc | | L2 | `push 64A0000h` at 0x7864C8; one per online match. See NetplayArenaKeeper |
| 0x00786A30 | Netplay arena free | | L2 | call 0x79E864 at 0x786A3E |
| 0x00784910 | GameStateManager ring | | L1 | 2 MB per online match |

Large requests, by size: netplay arena 100.6 MB; file-sized slot buffers up to 62.3 MB (plus their
compressed copy while loading); Johchu 9.54 MB; per-scene pools 2.3-2.5 MB; GameStateManager
2 MB. Sizes computed at runtime (containers, curl, zlib, GGPO rings) are not covered.

## 2. D3D and D3DX

The device comes from Direct3DCreate9Ex, so there is no MANAGED pool; textures are DEFAULT, some
with a SYSTEMMEM twin that costs process address space: the glyph atlas (AADX_CDynamicTextureList,
vtable 0x898C2C, 2048x2048x32 = 16 MB), a 1024x1024 dynamic texture (0x44BBC0) and movie textures
(AADX_CMovieTexture 0x439796) while a movie plays. AADX_CStaticTextureList's staging twin is
released right after UpdateTexture.

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x00449AE0 | ShaderEffect::Create | | L2 | thiscall. device->CreateVertexDeclaration (+0x158), then D3DXCreateEffect (thunk 0x7D8B1E) with Flags 0 and the shared pool at 0xA5B7E8; caches 16 techniques by name (table 0x9D5DB8) at +0x0C. Returns 0 on failure and leaves the effect null |
| 0x004497B0 | ShaderEffect::BindParam | | L2 | thiscall. `desc[1] = effect->GetParameterBySemantic(NULL, semantics[desc[0]])` (table 0x9D5DF8), then appends desc to the list at +0x50. The handle is 0 when the effect lacks the semantic |
| 0x00449700 | ShaderEffect::Begin | | L2 | thiscall. SetTechnique(+0xE8) with the cached handle (falls back to +0x0C), SetVertexDeclaration, Begin(+0xFC). Crashes on a null effect |
| 0x00449990 | ShaderEffect::SetFloat | | L2 | finds desc by id, SetFloat(+0x78) with desc[1]; checks desc, not the handle |
| 0x00449A00 | ShaderEffect::SetMatrix | | L2 | same, SetMatrix(+0x98) |
| 0x00449A70 | ShaderEffect::SetMatrixArray | | L2 | same, SetMatrixArray(+0xA0) |

ID3DXEffect slots the game uses: GetParameterBySemantic 0x28, GetTechniqueByName 0x34, SetFloat
0x78, SetFloatArray 0x80, SetVector 0x88, SetMatrix 0x98, SetMatrixArray 0xA0, SetTexture 0xD0,
SetTechnique 0xE8, Begin 0xFC, BeginPass 0x100, EndPass 0x108, End 0x10C. Every string the game
passes goes to an LPCSTR parameter (semantic or technique name); no string is ever passed where a
handle is expected, which is the one condition of D3DXFX_LARGEADDRESSAWARE.

## 3. The particle pointer fix-up (LAA bug 3)

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x0045A3B0 | ParticleData::Relocate | | L1 | walks the descriptors of a loaded particle file and calls 0x45B280 on each with the file buffer's address. Builds the 64-bit addend with `cdq` at 0x45A414 |
| 0x0045B280 | ParticleDesc::Relocate | | L2 | thiscall `ret 8` (addLow, addHigh). `add [ecx+2Ch],eax / adc [ecx+30h],edx`, then the same for +0x30/+0x34, and +0x38/+0x3C/+0x40 when non-zero: 64-bit adds over 32-bit fields 4 bytes apart, so each add also adds addHigh into the next field. Harmless while addHigh is 0 |
| 0x0045A780 | ParticleEmitter::Start | | L1 | reads desc+0x2C -> +0xAC; this is where the wrong pointer crashes |
| 0x0045A9E0 | ParticleEmitter::Build | | L1 | calls 0x45A780 per sub-emitter with desc[0xE..0x10] |

Found by watching the field with a hardware breakpoint in the stressed run: the decompressor
writes offset 0x236B0, 0x45B28F turns it into 0x236AF (the previous field's add carried
addHigh = 0xFFFFFFFF into it), 0x45B292 adds the base. Same file (slot 0xB) and same offsets
as a vanilla run; only the base differs. A scan for other 64-bit memory adds found 26 functions;
the other game-side ones are counters and timers whose callers do not sign-extend an address.

## 4. Tools

- `src/Core/LaaStress.cpp` (on the parked branch): with the environment variable `BBCF_IM_LAA_STRESS=1` and an exe that
  carries the large-address-aware bit, reserves every free range below 2 GB from DllMain (before
  the CRT creates its heap) and again every 2 s, so every later allocation lands above 2 GB.
  `=2` sweeps once, so freed low memory can be reused as in a real session. Never set in a build
  that ships. A copy of BBCF.exe with the bit set lives in the run copy as `BBCF_laa.exe`.
- Run copy of the archived build: `C:\BBCF_ArchiveRun\BlazBlue Centralfiction` (steam_appid.txt
  lets it start without Steam's launcher). Drive it with `BBCF_GAME_DIR=<that dir>
  WSLENV=BBCF_GAME_DIR bbcfctl ...`; the tool then starts that exe directly instead of the
  Steam install.
- Address-space breakdown of a minidump: the parser used here groups MemoryInfoList regions by
  allocation base and type (image / mapped / private, write-combine as GPU). It lived in the
  session scratchpad; rebuild it from this description if needed.

## 5. Next

1. Find bug 4 (battle under stress). Compare a vanilla and a stressed training match: which
   update path stops, then watch the bad field the way bug 3 was found.
2. Exercise the rest under stress: versus, replays, Abyss/arcade, online lobby and a match.
3. Decide how the bit gets set on players' machines (the exe has no SteamStub; Steam's "verify
   files" and every game update revert it). The build guard already ignores Characteristics and
   CheckSum, so a patched exe is still recognised.
