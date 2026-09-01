# BBCF Improvement Mod

Additional features and tweaks for **BlazBlue: Central Fiction** (Steam).

Training tools the game never shipped with, quality-of-life for online play, custom palettes and custom music — without touching a single file the game installed.

Join the [BB Improvement Mod Discord](https://discord.gg/j2mCX9s) to report bugs, ask for features, or follow development.

---

## Installing

1. Download the latest release zip from the [Releases page](https://github.com/HaiKamDesu/BBCF-Improvement-Mod/releases).
2. Drop **`dinput8.dll`** and **`BBCFIMUpdater.exe`** next to `BBCF.exe` in your BlazBlue Centralfiction folder.
3. Launch the game. Press **F1**.

That's the whole install. Everything else — `settings.ini`, `palettes.ini`, every folder the mod uses — is created for you on first launch, and the mod keeps itself up to date from then on.

> **Where is my game folder?** In Steam, right-click BlazBlue Centralfiction → Manage → Browse local files.

Nothing the mod does modifies the files Steam installed, so "Verify integrity of game files" has nothing to undo. To uninstall, delete `dinput8.dll`.

---

## What this mod provides

### Training
- **Frame data overlay** — startup/active/recovery, frame advantage, and a scrolling frame meter (see [FrameHistory](#framehistory) below)
- **Hitbox overlay** with per-attribute invulnerability display
- **Freeze and frame step**, so you can walk through a sequence one frame at a time
- **Dummy playback slots** — record, save to disk, and load back; wake-up and gap actions driven from a slot
- **P2 State Library** — put the dummy into any state directly
- **TAS combo tool** — build a combo frame by frame with a real editor: play, step, scrub, undo/redo, typed input composer, editable input list
- **Custom game modes** and freely adjustable in-game currency

### Replays
- **Replay takeover** — jump into a replay at any point and play it out yourself
- **Replay rewind**
- **Local replay file loading** and a replay database browser

### Online
- **Ranked room list** with sorting and filtering
- **Lobby links** — copy and join a lobby from a link (Ctrl+C / Ctrl+V)
- **See each other's custom palettes** in online matches
- Change avatars and accessories in rooms without backing out to a menu

### Look and sound
- **Custom palettes** — create, edit and load palettes and effects, with no file modification at all. Export one as a PNG and you get your character *painted in that palette*, ready to recolour in any image editor and import straight back
- **Music replacement** — hand any track the game ships with (including Astral themes) a song of your own. Supports **mp3, wav, flac, ogg, m4a/aac and wma**, with per-song volume you can hear before you commit to it
- **Jukebox** — an in-game player with rotation, search and manual track selection, plus a `[ custom ]` category fed from your own files
- **Graphics options** beyond what the game exposes, and performance options

### Everything else
- **Automatic controller detection** and per-device overrides, including separate keyboards for P1 and P2
- **Rebindable hotkeys**, on keyboard or controller
- **Switchable overlay language** (English/Spanish, extendable — see [`docs/localization.md`](docs/localization.md))
- **Automatic updates**

---

## Default hotkeys

Every one of these is rebindable in the F1 menu under Controllers → Hotkeys, to a key **or a controller button**.

| Key | Action |
|---|---|
| `F1` | Open/close the mod menu |
| `F2` | Toggle the online overlay |
| `F3` | Toggle the game HUD |
| `F4` | Load replay takeover state |
| `F5` / `F9` | Save / load state |
| `F6` | Unlimited playback trigger |
| `F7` / `F8` | Replay takeover trigger / cancel |
| `F10` / `F11` | Toggle Jukebox / next track |
| `C` / `V` | Freeze / frame step |
| `Ctrl+C` / `Ctrl+V` | Copy / join lobby link |

Some defaults overlap (F7 and F8 each do two things depending on which feature is active). The rebind UI warns you when a binding is already in use.

---

## Custom music

**Replacing a track:** F1 → Look & Sound → Music Replacement. Pick any track the game ships with, hand it a file, and that's the song now — everywhere, permanently, until you undo it. The mod keeps its converted copy off to one side and redirects the game to it, so nothing in your game folder is touched and undoing a swap is one click.

Two things the window will also tell you: Astral music is loaded when a match starts, so a swap made mid-match is heard on the *next* one; and your Astral Heat BGM sound option decides which set of tracks actually plays, so replace all three variants of a set to be sure.

**Adding songs to the Jukebox:** put audio files in `data/Sound/BGM/custom/` (create the folder if it isn't there) and open the Jukebox. New files are converted in the background and appear under the cyan `[ custom ]` category. Keep the originals in place — that's how the mod finds them on later launches.

**On volume:** by default the mod measures each song and levels it against the rest of the game's music, so one track isn't twice as loud as the next. If you'd rather set it yourself, untick that and use the slider — **Listen** plays the song at that volume immediately, anywhere in the game, so you're not converting and starting a match just to find out it's too loud.

**On quality:** everything is re-encoded to the format the game's audio engine expects (~96 kbps WMA). Importing a FLAC gets you the convenience of not converting it yourself, not lossless audio in-game.

---

## Crash reports and troubleshooting

If the game crashes while the mod is loaded, a bundle is written to `BBCF_IM/CrashReports/Crash_<timestamp>/` containing `crash.dmp`, `logs.txt` and `crash_context.txt`.

**Zip the whole `Crash_<timestamp>` folder and send it to us in Discord.** It already carries the log stream inside the dump, which is usually enough to find the cause.

### FrameHistory

A frame meter with two rows per character. For each non-idle frame it draws a column of rectangles.

- **First row — player state:**
  - Hard landing recovery → blush
  - Startup → green
  - Active → red
  - Recovery → blue
  - Blockstun → yellow
  - Hitstun → purple
  - Special (states that resist classification, e.g. dashes) → aquamarine
  - Unclassified but unable to act → byzantium
  - Unclassified otherwise → burgundy
- **Second row — attribute invulnerability**, colours and shapes mixed and overlaid respectively:
  - Head → red rectangle
  - Body → green rectangle
  - Foot → blue rectangle
  - Projectile → green disk
  - Throw → red disk

---

## Building

Visual Studio 2022, platform toolset **v143**, Windows SDK 10. Load `BBCF_IM.sln` and build — no solution or source changes needed.

```
MSBuild.exe BBCF_IM.sln /m /p:Configuration=Debug /p:Platform=Win32 /p:PlatformToolset=v143
```

Configurations are `Debug`, `Release`, and the `DebugDeploy` / `ReleaseDeploy` variants, which additionally copy the built DLL into your game folder (path stored in `BBCF_IM.vcxproj.user`, which is not checked in). Output lands in `bin/<Configuration>/`.

UI strings live in `resource/localization/Localization.csv` and are compiled into the DLL by a build step; see [`docs/localization.md`](docs/localization.md) to add a language.

Contributors: start with [`AGENTS.md`](AGENTS.md) and [`docs/AI_REPO_MAP.md`](docs/AI_REPO_MAP.md), which map the codebase far better than this file can.

---

## Credits

This mod is a fork of **[libreofficecalc](https://github.com/libreofficecalc/BBCF-Improvement-Mod)**'s continuation of the original **[BBCF Improvement Mod by kovidomi](https://github.com/kovidomi/BBCF-Improvement-Mod)**. Practically everything load-bearing here — the D3D9 hooking, the palette system, the overlay, the online layer — traces back to their work. Thank you.

### Features contributed by the community
- **SJS** — the TAS combo tool, built from scratch and brought to the Discord
- **aikuxa** — the Jukebox
- **rekijitsu** — custom Jukebox track fixes
- **HIKARI** — the CFPL↔PNG converters that established how BBCF palettes travel as images. The mod's PNG palette support follows their convention, so palettes move between the two freely
- **Tadatys (sublimacija)** — replay list downloader fixes, and a great deal of Ghidra reverse engineering
- **libreofficecalc** — the replay database, and its migration to the current server
- **KDing0**, **philippejaram**, **LGriebsch**, **GrimFlash**, **SIY**, **PC_volt**, **MorphRed**, **Unisectyn**, **Tyler Coble**, **gauntlet36**, **AnthonyYoManz** — code contributions across the project's history

### Thanks
GrimFlash · KoviDomi · Neptune · Rouzel · Dormin · NeoStrayCat · KDing · PC_volt · MorphRed · Tadatys (sublimacija) · corpse warblade · Euchre · SegGel2009 · Noel

…and everybody in the BlazBlue PC community who has reported a bug, tested a build, or argued with me about where a menu item belongs. A lot of what's in this list started as someone complaining in Discord.

### Third party
- **Atom0s** — DirectX 9.0 hooking article
- **Durante** — dsfix source
- [Dear ImGui](https://github.com/ocornut/imgui) · [Microsoft Detours](https://github.com/microsoft/Detours) · [miniaudio](https://github.com/mackron/miniaudio) · [stb](https://github.com/nothings/stb)

---

## Legal

```
BBCF Improvement Mod is NOT associated with Arc System Works or any of its partners / affiliates.
BBCF Improvement Mod is NOT intended for malicious use.
BBCF Improvement Mod is NOT intended to give players unfair advantages in online matches.
BBCF Improvement Mod is NOT intended to unlock unreleased / unpurchased contents of the game.
BBCF Improvement Mod should only be used on the official version that you legally purchased and own.

Use BBCF Improvement Mod at your own risk.
The maintainers and contributors are not responsible for what happens while using
BBCF Improvement Mod. You take full responsibility for any outcome that happens to you
while using this application.
```
