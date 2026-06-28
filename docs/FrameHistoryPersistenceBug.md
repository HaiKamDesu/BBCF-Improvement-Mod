# FrameHistory Checkbox Persistence Bug — Full Investigation Log

## What was broken

Two checkboxes in the **Frame History** section of the main mod menu did not persist across game sessions:

- **Enable** (shows/hides the FrameHistory overlay window)
- **Auto Reset** (auto-resets bars after each idle frame)

Behaviour: user checks both boxes in training mode, alt+F4s, re-enters game → both boxes are back at defaults (Enable = OFF, Auto Reset = ON).

---

## Feature context

### UI location
Main mod menu → "Frame History" collapsing header → two checkboxes at the top of the section, followed by Width/Height/Spacing sliders.

The Width/Height/Spacing sliders **do** persist correctly (they existed in `settings.ini` as original keys before this work began).

### State variables
| UI element | C++ variable | Stored in |
|---|---|---|
| Enable checkbox | `Settings::settingsIni.frameHistoryEnabled` | settings.def → settingsIni_t |
| Auto Reset checkbox | `FrameHistoryWindow::resetting` + `Settings::settingsIni.frameHistoryAutoReset` | same |

### Relevant files
| File | Role |
|---|---|
| `src/Core/settings.def` | X-macro definitions for all settings, including the two new bool entries |
| `src/Core/Settings.cpp` | `loadSettingsFile()`, `applySettingsIni()`, `changeSetting()` |
| `src/Core/interfaces.h` | `modValues_t` struct (passes settings to windows at construction time) |
| `src/Overlay/Window/FrameHistory/FrameHistoryWindow.h` | Constructor reads from `g_modVals` |
| `src/Overlay/Window/MainWindow.cpp` | `DrawFrameHistorySection()` — reads/writes both checkboxes |

---

## What was implemented (in a previous session)

### 1. New settings entries in `settings.def`
```cpp
SETTING(bool, frameHistoryAutoReset, "FrameHistoryAutoReset", "1");
SETTING(bool, frameHistoryEnabled,   "FrameHistoryEnabled",   "0");
```

### 2. `modValues_t` field added in `interfaces.h`
```cpp
bool frame_history_auto_reset;
```

### 3. `applySettingsIni()` in `Settings.cpp`
```cpp
g_modVals.frame_history_auto_reset = Settings::settingsIni.frameHistoryAutoReset;
```

### 4. `FrameHistoryWindow` constructor reads `g_modVals`
```cpp
resetting = g_modVals.frame_history_auto_reset;
```

### 5. `DrawFrameHistorySection()` in `MainWindow.cpp`

**Enable checkbox** (was previously `static bool isOpen = false` — never connected to settings):
```cpp
bool isOpen = Settings::settingsIni.frameHistoryEnabled;
if (ImGui::Checkbox(Messages.Enable_framehistory_section(), &isOpen)) {
    Settings::settingsIni.frameHistoryEnabled = isOpen;
    Settings::changeSetting("FrameHistoryEnabled", isOpen ? "1" : "0");
}
if (isOpen) frameHistWin->Open(); else frameHistWin->Close();
```

**Auto Reset checkbox** (was previously unwrapped — changes lost on close):
```cpp
if (ImGui::Checkbox(Messages.Auto_Reset_Reset_after_each_idle_frame(), &frameHistWin->resetting)) {
    Settings::settingsIni.frameHistoryAutoReset = frameHistWin->resetting;
    Settings::changeSetting("FrameHistoryAutoReset", frameHistWin->resetting ? "1" : "0");
}
```

All of this code is correct and in place. The persistence still did not work.

---

## Root cause (discovered by reading the actual deployed `settings.ini`)

The user's `settings.ini` (in the BBCF game folder) had **multiple `[Settings]` section headers** — an artifact of the auto-updater system appending new template blocks to existing files over multiple mod versions:

```ini
[Settings]          ← first section
ToggleButton=F1
...
FrameHistoryWidth = 10
FrameHistoryHeight = 10
FrameHistorySpacing=1.000000
...
# ShowRankedProgress added automatically #
ShowRankedProgress = 1

[Settings]          ← second section (auto-updater artifact)
ShowRankedPrediction=1

[Settings]          ← third section
RankedProgressShowMatches=1

... (more duplicate sections) ...

[Settings]          ← last section
UnlimitedPlaybackLoopEndingSeconds=0.0
# FrameHistoryAutoReset added automatically #
FrameHistoryAutoReset = 1
# FrameHistoryEnabled added automatically #
FrameHistoryEnabled = 0
```

### Why this broke persistence

`GetPrivateProfileString` (used by `loadSettingsFile`) only reads from the **first** occurrence of a `[Settings]` section. When it encounters a second `[Settings]` header, it treats it as the start of a new — different — section, and stops reading the first one.

The old `changeSetting` implementation used `std::fstream` to do line-by-line replace/append:
- For **new keys** (not yet in any section): it appended at the **very end of the file**, which was in the last `[Settings]` section.
- For **existing keys** that had already been appended once: it found and replaced them — again in the last `[Settings]` section.

Result: `changeSetting("FrameHistoryEnabled", "1")` wrote to the last `[Settings]` section. `GetPrivateProfileString(L"Settings", L"FrameHistoryEnabled", L"0", ...)` read from the first `[Settings]` section, didn't find the key, returned default `"0"`. Checkbox appeared OFF on next startup regardless of what was saved.

This is why the Width/Height/Spacing sliders **did** persist: those keys existed in the **first** `[Settings]` section from the beginning (they were original template keys). The fstream approach found and replaced them within that first section correctly.

---

## Attempts that did NOT fix the problem

### Attempt 1 — Fix `isOpen` static variable
The `DrawFrameHistorySection` had `static bool isOpen = false;` which was never connected to settings. Fixed to read from `Settings::settingsIni.frameHistoryEnabled` each frame. Necessary fix, but not sufficient — persistence still failed because of the section bug.

### Attempt 2 — Add seeding of missing keys in `loadSettingsFile`
Added this to `loadSettingsFile` after the X-macro read block:
```cpp
if (IsSettingMissingInIni(L"FrameHistoryEnabled", strINIPath))
    changeSetting("FrameHistoryEnabled", settingsIni.frameHistoryEnabled ? "1" : "0");
if (IsSettingMissingInIni(L"FrameHistoryAutoReset", strINIPath))
    changeSetting("FrameHistoryAutoReset", settingsIni.frameHistoryAutoReset ? "1" : "0");
```

Intention: pre-seed the keys into the file so subsequent `changeSetting` calls would hit the "replace" path rather than the "append" path.

Did not fix persistence because the old `changeSetting` used `std::fstream` and appended to the end of the file (last `[Settings]` section), which is still not visible to `GetPrivateProfileString`.

### Attempt 3 — Add keys to `resource/settings.ini` template
Added `FrameHistoryEnabled = 0` and `FrameHistoryAutoReset = 1` to the template. Correct for new installs going forward, but the user's existing `settings.ini` already had the stale multi-section structure and was not affected.

---

## Actual fix applied

Rewrote `changeSetting` in `src/Core/Settings.cpp` to use `WritePrivateProfileStringW` instead of the fstream approach:

```cpp
int Settings::changeSetting(std::string setting_name, std::string new_value) {
    wchar_t wAbsPath[MAX_PATH] = {};
    if (_wfullpath(wAbsPath, L"settings.ini", MAX_PATH) == nullptr) {
        LOG(2, "[error] Settings::changeSetting: Unable to resolve absolute path.");
        return 1;
    }

    wchar_t wKey[512] = {};
    wchar_t wVal[4096] = {};
    MultiByteToWideChar(CP_ACP, 0, setting_name.c_str(), -1, wKey, 512);
    MultiByteToWideChar(CP_ACP, 0, new_value.c_str(), -1, wVal, 4096);

    if (!WritePrivateProfileStringW(L"Settings", wKey, wVal, wAbsPath)) {
        LOG(2, "[error] Settings::changeSetting: WritePrivateProfileStringW failed (GLE=%lu).", GetLastError());
        return 1;
    }

    LOG(2, "Settings::changeSetting: File updated successfully.");
    return 0;
}
```

**Why this fixes it:** `WritePrivateProfileStringW` by spec targets the **first** matching section. It both updates existing keys and inserts new ones — always into the first `[Settings]` section. `GetPrivateProfileString` also reads from the first `[Settings]` section. Now both operations target the same place.

**Key requirement:** `WritePrivateProfileStringW` ignores relative paths (it would look in `%WINDIR%` instead of CWD). The fix uses `_wfullpath` to resolve the absolute path first, mirroring what `loadSettingsFile` already does.

---

## Remaining concern

The user's `settings.ini` still contains multiple `[Settings]` section headers. Keys that were written into later sections over multiple sessions (e.g. `ShowRankedPrediction`, `RankedProgressShowMatches`, etc.) may not be loading correctly either. Those bugs were not reported but may exist.

If a future agent needs to address this: the cleanest fix is a one-time settings.ini cleanup that collapses all duplicate `[Settings]` blocks into a single section. This should run once in `loadSettingsFile` when duplicate sections are detected, then rewrite the file with a single merged section.

---

## Key invariants to know before touching this again

1. `loadSettingsFile` uses `GetPrivateProfileString(L"Settings", ...)` — reads from **first** `[Settings]` section only.
2. `changeSetting` (post-fix) uses `WritePrivateProfileStringW(L"Settings", ...)` — writes to **first** `[Settings]` section only.
3. `applySettingsIni` runs during `CreateDeviceEx`, **before** `WindowManager::Initialize()` and `FrameHistoryWindow` construction. `g_modVals` must be populated here.
4. `FrameHistoryWindow::resetting` is initialized from `g_modVals.frame_history_auto_reset` in the constructor — this is the only place where startup value is applied.
5. Both new SETTING entries are in `settings.def` and `resource/settings.ini`. Seeding code in `loadSettingsFile` writes them into any existing settings.ini that is missing them.
6. `DrawFrameHistorySection` is `const` but legally writes to `Settings::settingsIni` (static member, not `MainWindow` instance member).
