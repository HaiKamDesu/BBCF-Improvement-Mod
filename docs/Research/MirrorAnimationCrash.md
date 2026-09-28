# Crash picking a dummy animation in a mirror match (8.7.1)

Reported by Yukio, 2026-09-27, mod v8.7.1. Dump:
`D:\User\Downloads\IM Debugging\8.7.1\Yukio - Crash on mirror animation\bbcf_2104_crash_2026_9_27T16_58_28C0.mdmp`
(no DEBUG.txt in the folder). Symbolized against `build/symbols/v8.7.1/Release`.

## Signature

```
eax=00000000 ecx=0088e760 esi=00c90000
dinput8!JonbDBReader::parse_all_jonbins+0xc6   (dinput8 base 50090000 -> RVA 0x1fa2d6)
5028a2d6 8b7804   mov edi,dword ptr [eax+4]    ds:00000004  -> read AV on null

JonbDBReader::parse_all_jonbins        src/Game/Jonb/JonbDBReader.cpp   jonb_index_header->offset_to_first_full_entry
parse_scr                              src/Game/Scr/ScrStateReader.cpp
ScrWindow::EnsureDummyScriptFresh      src/Overlay/Window/ScrWindow.cpp
ScrWindow::EnsureDummyScriptLoadedForUi
`anonymous namespace'::DrawAnimationModal   src/Overlay/Window/DummyActionsPanel.cpp
DummyActionsPanel::Draw -> MainMenu::DrawTrainingPage -> ... -> EndScene
```

`ecx = 0x88E760` is `FPAC_JONBIN_OFFSET_FROM_BBCF_P2`, `esi` is BBCF's base: the pointer at
`BBCF+0x88E760` (player 2's jonbin index header) was **null**, and the header's
`offset_to_first_full_entry` at +4 was read through it.

## Root cause

In a mirror match the game loads **one** script pack for the shared character and leaves
player 2's slot pointers null: `0x88E750` (scr FPAC), `0x88E760` (jonbin index),
`0xDB6B5C` / `0xDB6B70` / `0xDB6B74` (pre-init and EA). The dummy runs off player 1's script.

`parse_scr` used to bail out when both charIndex matched. Commit `81c4db5` ("Give frame
history its missing states back", v8.6) removed that bail, and fixed frame history's caller
(`FrameHistory::loadCharData`) to ask for slot 1 twice in a mirror. It did **not** fix the other
caller: `ScrWindow::EnsureDummyScriptFresh` still calls `parse_scr(base, 2)` by number. From
8.6 on, opening the animation picker (or arming any Animation/Burst action, which triggers the
same parse from `TickDummyActions`) in a mirror match reads through the null jonbin pointer.

The panel's "could not be read ... in mirror matches" message only shows *after* the parse
returns empty; the crash happens inside the parse itself, so it never got that far.

## Fix

- `parse_scr` now resolves the slot itself: player 2 in a mirror match reads player 1's slot,
  so every caller is covered (frame history's own redirect is now redundant but harmless).
  A side effect is that the dummy's move list is now available in mirror matches, instead of
  the "could not be read" notice.
- `parse_scr` returns an empty list, with a log line, when any of the slot's pack pointers is
  null (training character select, transitions), instead of reading through it.
- `parse_all_jonbins` returns an empty map when the jonbin index header or CharData is null.

Files: `src/Game/Scr/ScrStateReader.cpp`, `src/Game/Jonb/JonbDBReader.cpp`.

Follow-up for the panel: the localized string "The dummy's move list could not be read. This
happens on the training character select screen, and in mirror matches." can drop the mirror
clause now.

## Test

1. Training mode, same character on both sides (e.g. Ragna vs Ragna).
2. F1 -> Training -> Dummy actions -> add an action -> From Animation. 8.7.1 crashes here;
   fixed build shows the move list.
3. Pick a move on On Wakeup, knock the dummy down: it should come out. Log should contain
   `[Scr] Mirror match: player 2's script requested, reading player 1's slot.`
4. Non-mirror match: same flow, no behaviour change.
