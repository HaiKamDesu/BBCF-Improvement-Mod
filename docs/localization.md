# Localization

The overlay text now runs through a lightweight localization layer so releases can ship with multiple languages without touching the UI code.

## Adding a language

1. Add your language strings to `src/Core/Localization.cpp` using the existing `AddTranslation` calls as a guide. The English map is the source of truth; every key that exists there must be present in your new language map for it to be selectable.
2. Give the language a short code (for example `en` or `es`) and a display name.
3. Rebuild the DLL. Incomplete languages automatically appear disabled in the language dropdown, and the tooltip reports how many dictionary entries are missing.

## Selecting a language

* A persistent `Language` setting in `settings.ini` drives the default choice (`en` by default). The setting can be changed in the UI or directly in the file.
* At runtime, the main window exposes a dropdown near the top that lists every configured language. Disabled entries indicate missing dictionary entries.
* English is the fallback for any unresolved strings.

