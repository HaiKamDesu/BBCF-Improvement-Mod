# Localization

The overlay text is powered by per-language `.resx` dictionaries stored in `resource/localization/`. Visual Studio can edit these files directly using the built-in resource editor, and the mod loads them at runtime so translators can add or tweak strings without touching the C++ sources.

## File layout

- **Baseline**: `resource/localization/en.resx` is the source of truth. Every new key should be added here first.
- **Language metadata**: Each file includes a `_DisplayName` entry that controls how the language appears in the dropdown.
- **Additional languages**: Create a new `<code>.resx` file (for example `fr.resx`) alongside the baseline and translate the values while keeping the keys identical to English.

## Adding or updating a language

1. Copy `en.resx` to a new file named with the language code you want (e.g., `es.resx` for Spanish).
2. Update the `_DisplayName` value to the human-readable language name.
3. Translate every `<value>` while keeping the `<data name="...">` keys unchanged. Missing keys will cause the language to be marked incomplete.
4. Ship the updated `.resx` files in `resource/localization/` alongside the mod so they can be discovered at runtime.

## Runtime behavior

- The language dropdown lists every `.resx` file found in `resource/localization/`.
- Languages missing any key from `en.resx` are shown as disabled and report how many translations are still required.
- English remains the fallback language for any unresolved keys and for persistence across sessions.
- The selected language is stored in `settings.ini` (`Language`), so it stays consistent between launches.

## Why `.resx`?

`.resx` files are a simple XML format that Visual Studio can display in a spreadsheet-style grid. That makes it easy to scan for missing translations, edit multiple languages side by side, and keep contributors productive without editing code.
