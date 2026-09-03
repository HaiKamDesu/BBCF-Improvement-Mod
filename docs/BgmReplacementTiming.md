# When a BGM replacement actually starts playing

A replacement is a pointer written into the game's filename table. Writing it is instant;
**hearing it depends entirely on when the game next loads that file**, and the game does not
load every track the same way. That gap is why people swapped a track, saw "Now playing your
song", heard the original, and reported it as broken.

`BgmLoadTiming` in `BgmReplacementManager.h` carries the answer per track, and the row in the
replacement window states it. Three classes:

## `GameRestart` — two tracks, and only these two

`201_charaselect.pac` (index 72) and `251_rannyu.pac` (index 85).

Established from the disassembly: each is read at exactly **one** address — `0x00483B55` and
`0x00483ACF` — and both sit inside `0x004837D0`, a function that loads only those two files
and registers them into fixed audio slots 6 and 7. That function is reached from the game's
own boot initialisation: the enclosing code carries the strings `InitParticle`, `InitFade`,
`InitSystemFont` and `InitGameSystem`.

So the pointer is consulted once, during startup, and the loaded bank stays registered for
the life of the process. A swap made mid-session is correct and saved, and inaudible until
the next launch. This is what the character-select report turned out to be.

The UI distinguishes the two cases via `IsLiveNow()`, which is true only when the pointer was
already in place before boot read it — i.e. the assignment came from the saved file rather
than from a swap made this session.

## `NextMatch` — battle, boss, astral, and the 9xx duplicates

Categories `btl`, `boss`, `old`, `astral`. These load as part of match setup, so a swap is
heard from the next match with no restart. Astral specifically was established earlier: idx
97-108, read by absolute address inside `FUN_00555A20`, the per-match audio loader running on
an async worker, so Astral tracks load at match load rather than on trigger.

## `NextScreen` — everything else

Menus, lobby, versus, results, endings. Loaded when their screen comes up, so a swap applies
the next time you reach it.

This is also the safe default for anything unclassified: nothing outside the two boot entries
is loaded once-per-process, so "the next time the game loads this" is always true, and the
worst case is telling someone to revisit a screen when a restart would also have worked.

## The correction this supersedes

`docs/Research/BgmReplacementFeasibility.md` records that the table is "re-read on every
load, not cached — a swap applies on the next load of that track, no restart". That was
verified with `000_btl_rg`, a **battle** track, and it is true of the class it was tested on.
It does not generalise: it was read as a property of the table when it is really a property of
how often each caller runs. The table genuinely is re-read every time; some callers just only
run once.

## Two table facts worth keeping

- **`205_abyss.pac` occupies two indices, 76 and 109** — the only duplicated filename in the
  175-entry table. Patching one leaves the other pointing at the original, and whichever the
  game reads decides whether the swap is heard. `ApplyPointer`/`RestorePointer` cover every
  index sharing a filename, and the catalogue lists such a file once.
- **A `.pac`'s cue name must equal the original base filename.** `ReadPacCueName` verifies it
  at apply time and logs a mismatch, because a wrong cue loads without error and plays
  nothing. Checked against a real install: all 186 shipped files satisfy it.

## Diagnosing a "it says replaced but plays the original" report

1. `[BgmReplace] Entry N: cue "x" verified` — the file is structurally right. A mismatch line
   here means rebuild it.
2. `[Init] BGM replacements applied before game audio init.` — the startup apply ran, so a
   `GameRestart` track should be live this session.
3. If both look right, overwrite the shipped `.pac` in `data/Sound/BGM/` with the converted
   one. If that plays, the file is fine and the pointer is not being honoured for that track,
   which means its load path needs classifying — find its table index, then find what reads
   that index and when.
