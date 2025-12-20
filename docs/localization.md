# Localization

The overlay text is powered by per-language `.resx` dictionaries stored in `resource/localization/`. Visual Studio can edit these files directly using the built-in resource editor, and the mod loads them at runtime so translators can add or tweak strings without touching the C++ sources. Keeping all cultures under the same base filename lets resource editors show every language in a single grid (key + English + Spanish, etc.).

At build time, every `Localization*.resx` file is embedded into `dinput8.dll` via `resource/resource.rc`, so end users only need `dinput8.dll`, `settings.ini`, and `palettes.ini` in their game folder. If present, `.resx` files on disk will still be loaded (useful while testing new translations) but are not required for normal installs.

## File layout

- **Baseline**: `resource/localization/Localization.resx` is the source of truth (English). Every new key should be added here first.
- **Language metadata**: Each file includes `_LanguageCode` (culture code) and `_DisplayName` (dropdown label) entries.
- **Additional languages**: Copy `Localization.resx` to `Localization.<culture>.resx` (for example `Localization.fr.resx`) and translate the values while keeping the keys identical to the baseline.

## Adding or updating a language

1. Copy `Localization.resx` to a new file named with the culture you want (e.g., `Localization.es.resx` for Spanish).
2. Update `_LanguageCode` to the culture code and `_DisplayName` to the human-readable language name.
3. Translate every `<value>` while keeping the `<data name="...">` keys unchanged. Missing keys will cause the language to be marked incomplete.
4. Update `resource/resource.rc` to include the new `.resx` file so it is baked into the DLL (keep the string-based names ending in `.resx`).
5. Ship the updated `.resx` files in `resource/localization/` alongside the mod if you want on-disk overrides while testing; the embedded copies cover normal installs.

## Runtime behavior

- The language dropdown lists every `.resx` file that shares the `Localization` base name, starting with the versions embedded in the DLL and then any overrides found in `resource/localization/` on disk.
- Languages missing any key from `Localization.resx` are shown as disabled and report how many translations are still required.
- English remains the fallback language for any unresolved keys and for persistence across sessions. If no English file is present, the fallback becomes the first culture found.
- If neither embedded nor on-disk resources are available, the DLL falls back to built-in English/Spanish dictionaries so UI strings stay valid (preventing formatting errors during startup).
- The selected language is stored in `settings.ini` (`Language`), so it stays consistent between launches.

## Regenerating bindings

Any time you add or rename a key in `Localization.resx`, regenerate `src/Core/LocalizationKeys.autogen.h` so C++ callers can fetch it via `Messages.<SanitizedKey>()`.

```powershell
pwsh -File tools/generate_localization_bindings.ps1 resource/localization/Localization.resx src/Core/LocalizationKeys.autogen.h
```

The generator converts any characters outside `[0-9A-Za-z_]` into underscores, collapses repeated underscores, and prefixes an underscore when the key starts with a digit. The original text stays untouched inside the resx files.

## Why `.resx`?

`.resx` files are a simple XML format that Visual Studio can display in a spreadsheet-style grid. With a shared `Localization` base name, tools like the built-in resource editor or the ResX Resource Manager extension can show every culture in one table (columns for key/English/Spanish/etc.), making it easy to compare and hide columns as needed.
