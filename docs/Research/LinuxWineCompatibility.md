# Linux (Wine/Proton) compatibility

**Date:** 2026-09-01
**Status:** both hard blockers fixed; six items open. §2 and §3 are written to be lifted
straight into release notes.
**Question:** users keep reporting "this doesn't work on Linux" — most often that custom
music does nothing. Which of the mod's Windows-only dependencies actually break under
Wine/Proton, how badly, and what would each cost to fix?

**Method:** a source audit of every Windows-only API the mod calls, then a purpose-built
probe (`tools/WineProbe/`) run natively on Windows for a baseline and again under Wine, so
the answers are measured rather than inferred. Raw output for both runs is in
`docs/Research/LinuxWineProbeRuns/`.

> "Linux" throughout means **Wine/Proton**. The mod is a Win32 DLL injected into a Win32
> game; nothing here needs a native port. Every question is "does Wine implement this".

---

## 1. Results

Probe run 2026-09-01. Wine 11.16 staging (amd64-wow64), 32-bit probe, no host GStreamer.
"After" reflects the fixes in §2.

| Subsystem | Windows | Wine, before | Wine, after |
|---|---|---|---|
| Mod DLL can load (`mfplat`/`mfreadwrite`/`mf`) | PASS | PASS | PASS (now delay-loaded) |
| Custom music / BGM replacement | PASS — 1 encoder | **FAIL — 0 encoders** | **FIXED** — PCM wave banks |
| Auto-updater (Shell zip extraction) | PASS | **FAIL** | **FIXED** — in-process inflate |
| Opus decoding | PASS | **FAIL** | still FAIL — §6 |
| Crash minidumps | PASS | PASS | PASS (walkability unverified) |
| Controller `ContainerId` | 25/25 devices | 2/2 synthetic only | still blocked — §8 |
| Platform shown in bug reports | correct | "Windows 10", no Wine version | **FIXED** |

---

## 2. What was fixed

Nine changes. Each line is phrased for release notes; the evidence is in the numbered
sections below.

### User-visible

- **Custom music and BGM replacement now work on Linux.** Wine provides no audio encoder,
  so the converter writes uncompressed PCM wave banks instead of WMA, chosen automatically.
  New `MusicWaveBankFormat` setting (Auto / Always WMA / Always PCM). Converted tracks are
  roughly ten times larger on Linux as a result. See §5.
- **The auto-updater works on Linux.** It no longer asks Windows' shell to open the zip —
  something Wine cannot do — and unpacks the update itself. See §6.
- **`ForceEnableControllerSettingHooks` actually does something now.** It was read *after*
  the Wine check that it exists to override, so setting it to 1 changed nothing; and the
  startup path rewrote `EnableControllerHooks = 0` into `settings.ini` on every launch, so
  the setting appeared to revert by itself each time the game started. Both fixed, and the
  flag now genuinely bypasses the platform block — see the table below.
- **Restarting the game from the Settings window goes through Steam** instead of launching
  the executable directly, so the overlay and Cloud saves survive it. Falls back to the old
  behaviour where the `steam://` handler is not registered.
- **Install instructions for Linux and Steam Deck**, including the launch option the mod
  needs in order to load at all (see below). `README.md` and `USER_README.txt`.

### Diagnostics

- **`DEBUG.txt` now reports the real platform.** Wine version, host kernel, and whichever
  of the SteamOS / Steam Deck / Proton / gamescope environment variables are set. The
  Windows version line is now marked as Wine's synthetic value rather than presented as a
  real Windows install. Every Linux bug report from here on is legible.
- **Wine detection is more reliable.** It now also checks for ntdll's `wine_get_version`
  export, which is present in every prefix — the previous registry and environment checks
  both miss setups where neither happens to be set.

### Robustness

- **Media Foundation DLLs are delay-loaded.** `mfplat`/`mfreadwrite` were load-time
  imports, so a prefix missing them, or a user setting `WINEDLLOVERRIDES=mfplat=d`, got a
  mod that silently failed to load. Verified with `dumpbin` that they moved out of the
  load-time import table.
- **The game directory is no longer round-tripped through the ANSI code page.** Under
  Proton the path runs through the Linux home directory (`Z:\home\<user>\...`), so a
  username not representable in that code page previously turned into question marks.

### What the force flag does, verified

All three gates consult it: the master gate `IsControllerHooksRuntimeAllowed()`
(`RuntimePlatform.cpp`), which every one of the ~20 functional call sites goes through;
the startup path that used to rewrite the ini (`dllmain.cpp`); and the Wine popup and reset
in `WindowManager.cpp`.

Measured by linking the real `RuntimePlatform.cpp` into a harness and running it natively
and under Wine 11.16:

| `EnableControllerHooks` | `ForceEnableControllerSettingHooks` | Windows | Wine |
|---|---|---|---|
| 0 | 0 | blocked | blocked |
| 0 | **1** | ALLOWED | **ALLOWED** |
| 1 | 0 | ALLOWED | blocked |
| 1 | **1** | ALLOWED | **ALLOWED** |

**The force flag alone is sufficient** — it does not also require `EnableControllerHooks`.
That is deliberate: older builds wrote `EnableControllerHooks = 0` into people's `settings.ini`
on every Wine launch, so requiring both would leave exactly the users who hit the original
bug still stuck, having to find and fix a second setting. The trade-off is that someone who
turned the hooks off deliberately *and* has the force flag set gets them back; that
combination is unlikely, and the gate reason in `DEBUG.txt` now names which branch decided.

The startup log line was also wrong here and is fixed: with the flag set on Wine it printed
`blocked: Wine/Proton detected` next to `allowed=1`, contradicting itself in the one line a
Linux bug report gets read for. It now reports
`allowed: FORCED ON over the Wine/Proton block - unsupported, expect problems`.

### The launch option, worth repeating

Proton users generally need this in the game's Steam launch options:

```
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

Without it Wine may prefer its own built-in `dinput8` over the mod's, and the mod never
loads — no error, no log, nothing in the F1 menu. This is plausibly a large share of
historical "doesn't work on Linux" reports and cost nothing but documentation to address.

---

## 3. Known issues and open questions

### Known issues

- **All controller features are disabled under Wine.** Assignment and reordering, keyboard
  separation, multiple keyboards, controller position swap, pad hotkeys, TAS mode and URT
  pad input. `ForceEnableControllerSettingHooks = 1` overrides the gate at your own risk
  (and now genuinely does so). The game's own controller support is unaffected. See §8 —
  the cause is now understood and is structural.
- **`.opus`, `.m4a`, `.aac` and `.wma` will not import on Linux.** These depend on codecs
  Windows supplies and Wine does not. `.mp3`, `.wav`, `.flac` and `.ogg` work everywhere.
  The file picker still offers all of them, so the failure currently happens mid-import
  rather than up front. See §7.
- **Custom music files are about 14.6x larger on Linux.** A 3:12 track is 33.9 MB as PCM
  against 2.3 MB as WMA. Unavoidable without an encoder; only Linux pays it, and only in
  exchange for a feature that previously did not work at all. See §5.

### Open questions

- **Do file dialogs appear at all in fullscreen?** The picker is deliberately unowned, which
  is correct on Windows but may put it behind the game under a Linux compositor —
  a button that appears to do nothing. Untested; needs observation before any code.
- **Is the Jukebox custom-track path fine with PCM?** Only the BGM replacement path has been
  exercised. The custom-track path generates its own sound bank rather than reusing a
  shipped one. The format lives in the wave bank, so it should be indifferent, but that is
  reasoning rather than a result. Drop a file into `data/Sound/BGM/custom/` to settle it.
- **Are Wine's minidumps actually usable?** A 132 MB dump with a valid `MDMP` header is
  produced (§9), but a valid header is not a walkable dump. One CDB run answers it.
- **Would the `winebus` registry flags fix controllers outright?** See §8. Worth testing
  before writing any correlation fallback, because it may make one unnecessary.

### The caveat that applies to all of the above

**Nothing has been run end-to-end under Proton.** Every Linux fix here is verified on
Windows and inferred for Linux from the probe. The mod has never been loaded into the game
under Wine with a real prefix. This is the single largest remaining gap, and it is worth
one Linux tester more than any further static analysis.

---

## 4. Investigated and dismissed

Recorded so nobody re-opens them.

| Item | Verdict |
|---|---|
| GPU shows blank in Linux bug reports | **Not true.** `SystemSpecsLogger::LogGpu` already reads the D3D9 adapter identifier, which DXVK fills with the real card. The registry scan at `hooks_detours.cpp:433` that prompted the concern is `[HookDiag]` logging for overlay rescue mode, not a source of reported specs. |
| `joy.cpl` button does nothing under Wine | **Already handled.** `OpenControllerControlPanel` returns early on `ControllerHooksEnabled()`, which is false under Wine unless forced — and Wine ships a `joy.cpl` regardless. |
| Updater cannot relaunch the game under Proton | **Already handled.** `BBCFIMUpdater.cpp:413` tries `steam://rungameid/586140` and falls back to the executable path. This is the pattern since copied into `RestartGame()`. |
| `mf.dll` is a load-time import | **Only two of the three were.** `dumpbin` shows `MFPlat.DLL` and `MFReadWrite.dll` in the import table; nothing references `mf.dll`'s symbols, so its delay-load entry is inert. Harmless, but the original claim was wrong. |

---

## 5. Custom music — no audio encoders at all

`src/Audio/CustomMusicConverter.cpp:167` asks Media Foundation for a PCM → WMAudioV8
encoder MFT. Under Wine that enumeration returns nothing:

```
PCM -> WMAudioV8 (what the mod needs)  0 encoder(s)
PCM -> WMAudioV9                       0 encoder(s)
PCM -> AAC                             0 encoder(s)
Total audio encoders                   0
```

Windows has ten, including the `WMAudio Encoder MFT` the converter needs. The finding is
stronger than "no WMA encoder": Wine's Media Foundation exposes **no audio encoder of any
kind**, so there is no other codec to retarget and no MF-based fallback to build. Both
entry points die with it — `ConvertAudioToPac` (Jukebox custom tracks) and
`ConvertAudioToReplacementPac` (BGM replacement).

Decoding is unaffected for the common cases: mp3/wav/flac/ogg go through miniaudio, which
is portable C.

**The only remaining path is to stop producing WMA.** The XACT wave bank's
`WAVEBANKMINIWAVEFORMAT.wFormatTag` also accepts `0` (PCM) and `2` (MS-ADPCM). That field
lives in the WBND the converter already builds byte-for-byte, and the replacement path
reuses the original sound bank verbatim, which does not encode the format at all. PCM
needs no encoder and no seek table — roughly 42 MB for a four-minute track; MS-ADPCM is
about four times smaller for ~200 lines of encoder.

**Answered, 2026-09-01: yes.** Rather than disassemble the runtime, the question was put
to the game's own data — scanning all 2302 `.pac` files under `data/Sound` for the
`WAVEBANKMINIWAVEFORMAT` of every wave-bank entry:

| tag | ch | rate | blockAlign | bits | entries |
|---|---|---|---|---|---|
| WMA | 1 | 44100 | 99 | — | 29829 |
| **PCM** | 1 | 44100 | 2 | 16 | **1836** |
| **PCM** | 1 | 48000 | 2 | 16 | **151** |
| WMA | 2 | 44100 | 6 | — | 135 |
| WMA | 2 | 48000 | 13 | — | 129 |
| **PCM** | 1 | 22050 | 1 | 8 | **2** |

The game ships **1989 PCM wave-bank entries and plays them every match** — every character
voice line is `wFormatTag = 0`. Voice banks are mixed: `Voice/vbtl_vh_0.pac` holds 121 WMA
entries and one PCM entry in the same bank, so the runtime dispatches on the per-entry
format field. `xactengine2_10` demonstrably handles PCM.

Facts the construction depends on, all verified across every shipped PCM entry:

- `duration x channels x bytesPerSample == PlayRegion.length` — **1989 of 1989**, no
  exceptions. This is also the check that confirms the bit-field decode is right, since it
  cannot hold by accident.
- `blockAlign == channels * bytesPerSample` — every entry.
- Bit 31 is bits-per-sample: `1` = 16-bit, `0` = 8-bit (the 22050 Hz entries are 8-bit with
  `blockAlign` 1, the rest 16-bit with `blockAlign` 2).
- **No seek table.** 1985 of the 1989 sit in banks with no SeekTables segment at all; the
  other four are inside mixed banks and mark their entry `0xFFFFFFFF`. Every bank still
  sets `dwFlags = 0x00080000`, so the flag is carried whether or not the segment exists.

**Residual unknown:** every shipped PCM entry is **mono**. Stereo PCM follows from the same
`blockAlign = channels * bytesPerSample` rule (so 4), and that is XACT's documented
formula, but it is an extrapolation rather than an observation. Both a stereo and a mono
test bank were built so the two cases can be told apart in one sitting.

`tools/make_pcm_pac.py` builds such a bank: it keeps the original track's sound bank byte
for byte and swaps only the wave bank, which is what `ConvertAudioToReplacementPac` does.
Output round-trips through the scanner and satisfies every invariant above.

**Confirmed in-game 2026-09-01**, in two stages:

1. A hand-built stereo PCM bank (`tools/make_pcm_pac.py`) standing in for `001_btl_jn`
   played correctly - tone at the right pitch, with clean left-then-right separation
   sustained over 20 s. That rules out a frame-layout error, which is the only way
   `blockAlign` or the channel count can be wrong, and settles the stereo extrapolation.
2. The converter's own PCM path then replaced `003_btl_rc` with a user `.ogg`
   (`MusicWaveBankFormat = 2`, forcing PCM on a machine that has the WMA encoder). It
   played correctly, and the emitted `.pac` verifies structurally: `tag=0 ch=2 rate=44100
   blockAlign=4 bits=16`, no SeekTables segment, sound bank reused byte-identical, bank
   name preserved, and `duration * ch * 2 == PlayRegion.length` exactly (33898496).

**Implemented.** `MusicWaveBankFormat` (settings.def, `SettingsIniWindow.cpp` metadata and
enum): 0 = Auto, 1 = WMA, 2 = PCM. Auto probes for a PCM->WMAudioV8 encoder MFT once and
picks WMA when it exists, PCM when it does not - which is the entire Windows/Proton split.
`TranscodeToPcm` decodes and applies gain with no Media Foundation involvement at all, and
the replacement path now only calls `MFStartup` on the WMA branch, since requiring it
would fail conversion on exactly the systems PCM exists to serve. The cache stamp records
the format (`"5 PCM"`), so switching rebuilds.

**Cost.** PCM is ~14.6x the size of the WMA the converter used to write: the 3:12 test
track is 33.9 MB against 2.3 MB. Acceptable for a handful of replacements, heavy for a
large Jukebox library. MS-ADPCM (`wFormatTag = 2`) would cut that by about four, but **no
shipped bank uses tag 2**, so unlike PCM it is unverified - it would need the same
evidence before being relied on.

**Still untested:** the Jukebox custom-track path (`ConvertAudioToPac`), which generates
its own sound bank rather than reusing a shipped one. Only the replacement path has been
exercised with PCM. The format lives in the wave bank, so the generated sound bank should
be indifferent to it, but that is reasoning rather than a result.

**Not yet confirmed end-to-end on Linux.** The probe shows no WMA encoder exists under
Wine, and PCM banks are proven to play, so Auto should select a working path there - but
the mod has never been run under Proton with the game attached.

## 6. Auto-updater — fixed

`ExtractZipWithShell` used `Shell.Application`. Wine creates the object happily and then
returns nothing for the archive, which is why this failed with a misleading error:

```
CoCreateInstance(Shell.Application)    hr=0x00000000 created
NameSpace(<zip>) returns a Folder      hr=0x00000000  NO
```

Replaced with in-process extraction (`ExtractUpdateZip`). It consumes the
`ZipValidationResult` the caller already produced, so every path, size, symlink and
compression-method check applies to exactly the bytes written, and an unvalidated archive
is refused outright. Entry CRCs are now verified explicitly — the shell used to do that
implicitly, and dropping it would have let a corrupt download install silently.

**On the inflate implementation.** stb_image's inflate was tried first, since the DLL
already compiles it. It is not safe here: it rejects valid deflate streams whenever the
data compresses heavily enough to leave a single distance code.

| payload | ratio | stb inflate |
|---|---|---|
| real 16 MB release `dinput8.dll` | 2.3:1 | ok |
| incompressible random data | 1.0:1 | ok |
| `"hello world"` | — | ok |
| repetitive text | 308:1 | **fails** |
| `"a" × 100000` | 870:1 | **fails** |

It happens to handle today's payload, but an updater that works or not depending on what
the compressor chose is not something to ship. `depends/miniz/` (MIT) is vendored for
this instead.

Verified by `tools/` harness against real archives — deflate and stored entries extracted
byte-for-byte, corrupted streams refused, traversal refused — passing both natively and
under Wine.

## 7. Opus and the other Windows-only codecs

`src/Audio/AudioDecode.cpp:481` creates `CLSID_MSOpusDecoder`, a Windows-only MFT. Wine
returns `0x80040154` (`REGDB_E_CLASSNOTREG`). `m4a`/`aac`/`wma` go through
`MFCreateSourceReaderFromURL` and are likely in the same position, though Proton's
GStreamer-backed mfplat may cover some — this run had no host GStreamer, so the decoder
count here understates a real Proton system.

`AudioDecode::SupportedExtensions()` therefore over-promises by up to four entries in the
Linux file picker. Downstream of §5; not worth touching until conversion works at all.

## 8. Controllers — the open question, now understood

`src/Core/RuntimePlatform.cpp:39` disables the entire controller subsystem under Wine, and
20+ call sites bail out on it. Dead on Linux: controller assignment and reordering,
keyboard separation, multi-keyboard, position swap, pad hotkeys, TAS mode
(`src/Game/TasManager.cpp:197`) and URT pad input. The commit that introduced the gate
(`9370876`) recorded no diagnosis of *which* call misbehaved.

The probe reported `ContainerId` present on 2/2 devices — but those two are Wine's
synthetic HID mouse and keyboard, because WSL2 has no USB passthrough. `joyGetNumDevs`
found 16 slots with 0 attached; XInput found 0 pads. **This proves only that Wine
implements the property, not that a real gamepad carries a usable one.**

Prime suspect remains `src/Core/ControllerOverrideManager.cpp:1139-1180`, which reads
`ContainerID` out of `HKLM\SYSTEM\...\Enum\...` to correlate one physical pad across
DirectInput, XInput and RawInput.

Closing this needs a physical pad visible to Wine — `usbipd-win` into WSL2, a VM with USB
passthrough, or a Linux tester.

### What the Proton project says about this (researched 2026-09-01)

The duplication we would be trying to resolve is **structural**, and it is not the same
problem `ContainerId` solves on Windows.

`winebus.sys` exposes controllers through two independent backends:

- `bus_udev.c`, reading `/dev/hidraw*` directly, and
- `bus_sdl.c`, going through SDL's evdev backend and synthesising HID/XInput devices.

One physical pad can therefore surface **twice**, as two separately synthesised Windows
devnodes originating from two different bus drivers. Proton's own issue on this
([#4707](https://github.com/ValveSoftware/Proton/issues/4707)) resolves duplicates by
comparing the **udev parent device path** — a Linux-side fact with no Windows-side
equivalent, and the maintainers describe the fix as Linux-side filtering rather than
anything an application inside the prefix can do.

This matters for our approach. On Windows, `ContainerId` works because the PnP manager
assigns one container to all devnodes of a single physical device. On Wine the two views
did not come from one PnP enumeration at all, so even where the property is populated
there is no reason for the two to share a value. **The mod's correlation strategy is
structurally unable to work under Proton**, which is a stronger statement than "the API is
missing" — and it means a Wine fallback has to correlate on something else entirely
(VID/PID plus interface path), accepting that an SDL-synthesised pad may report itself as a
generic virtual Xbox controller rather than its real hardware identity.

There is a lever worth knowing about: Proton 8.7 added two `winebus` settings that collapse
the split by making SDL the only backend, at
`HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\winebus`:

    "DisableHidraw" = dword:00000001
    "Enable SDL"    = dword:00000001

Wine-GE has set both by default since Proton8-7. If duplication is confirmed to be the
cause, checking these from inside the prefix would let the mod explain the situation
precisely instead of disabling itself silently — and a user who sets them may find the
hooks work without any code change at all. **Worth testing before writing a correlation
fallback**, since it may make one unnecessary.

Sources: [Proton #4707](https://github.com/ValveSoftware/Proton/issues/4707),
[proton-ge-custom CONTROLLERS.md](https://github.com/GloriousEggroll/proton-ge-custom/blob/master/docs/CONTROLLERS.md).

## 9. Crash dumps work — an earlier assumption was wrong

Wine's dbghelp exported `MiniDumpWriteDump` and produced a 132 MB dump with a valid `MDMP`
header. Rewriting crash reporting as a text report for Linux is **not needed**.

Not yet established: whether a Windows debugger can walk the stacks in that dump. A valid
header and a plausible size are not proof. Test with CDB before closing this out.

## 10. Reproducing this

No Linux machine is required. Wine runs in WSL2 with no root:

```bash
curl -LO https://github.com/Kron4ek/Wine-Builds/releases/download/11.16/wine-11.16-staging-amd64-wow64.tar.xz
tar -xf wine-11.16-staging-amd64-wow64.tar.xz
export WINEPREFIX=$HOME/winetest/pfx
wine-11.16-staging-amd64-wow64/bin/wineboot -u
wine-11.16-staging-amd64-wow64/bin/wine WineProbe.exe
```

The `amd64-wow64` build runs 32-bit Windows binaries without i386 host libraries, which is
what makes this work without installing anything.

**Caveat for §5 and §7:** this is upstream Wine, not Proton, and Proton bundles a more
complete GStreamer-backed mfplat. The encoder result almost certainly holds — no free WMA
encoder exists to bundle — but confirming under real Proton would need 32-bit host
libraries and a Proton runtime.

## 11. Next steps

Ordered by value per hour, not by difficulty.

1. **Get one Proton run.** Everything below is worth less until something has actually been
   loaded into the game under Wine. A tester, `usbipd-win` into WSL2, or a VM.
2. **Test the `winebus` flags** (§8) before writing any controller correlation fallback.
   They may resolve it outright, and it is a registry edit rather than a code change.
3. **Filter the file picker's format list on Linux** (§7) so an unsupported codec is refused
   up front with a reason, instead of failing part-way through an import.
4. **One CDB run against the Wine minidump** (§9) — 20 minutes, closes the item either way.
5. **Exercise the Jukebox custom-track path with PCM** (§5) — a file dropped into
   `data/Sound/BGM/custom/`.
6. **Look at a file dialog in fullscreen and in gamescope** (§3) before spending anything on
   an in-overlay picker.

## 12. Where the artefacts live

| What | Where |
|---|---|
| The probe, and how to build it | `tools/WineProbe/` |
| Raw probe output, both runtimes | `docs/Research/LinuxWineProbeRuns/` |
| Hand-built PCM wave-bank generator | `tools/make_pcm_pac.py` |
| Format selection and the PCM path | `src/Audio/CustomMusicConverter.cpp` |
| In-process zip extraction | `src/Updater/PackageStager.cpp` |
| Platform logging | `src/Core/SystemSpecsLogger.cpp` |
| Wine detection and the controller gate | `src/Core/RuntimePlatform.cpp` |

