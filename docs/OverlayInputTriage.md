# "Mouse clicks don't register" — triage

Three different faults produce this identical report, and until 8.5.2 a reporter's
`DEBUG.txt` could not tell them apart: opening the mod menu logs nothing, so the log looked
the same whether the overlay worked perfectly or swallowed every click. Two reports in a row
were undiagnosable for that reason.

`[OverlayInput]` now answers it. Read these four fields first:

| field | meaning if wrong |
|---|---|
| `clicksSeen=0` | no mouse button message ever reached ImGui — a delivery problem, not the overlay |
| `cursorDelta=LARGE` | ImGui's cursor and the OS's disagree — hit-testing and rendering are in different coordinate spaces, so visible buttons are unclickable |
| `modal='...'` | a modal is open and swallowing every click outside itself |
| `drew=0` | the overlay produced no geometry — nothing is on screen even if ImGui thinks it is |

## Cause 1: a blocking modal the user may never have answered

`AllowPaletteDownloads = -1` and `UploadReplayData = -1` are the shipped first-launch
prompts, and both are `ImGui::BeginPopupModal`. **An open ImGui modal blocks every click
outside itself, by design.** Until it is answered, nothing else in the overlay responds —
which is indistinguishable from broken mouse input.

Verified against the vendored ImGui, with the popup's own code next to a normal mod window:

```
modal open = 1
click the mod menu's button while the modal is up:
   hovered=0 clicked=0  WantCaptureMouse=1     <- completely unclickable
click the modal's own button:
   popup hovered=1  modal still open=0         <- only the popup responds
```

The prompts are gated one at a time (`PaletteSharePopupWindow::Update` returns early while
the replay or Wine popup is open), so a user can answer the first, never notice the second,
and stay blocked on every subsequent launch — the setting only gets written when the prompt
is actually answered.

Anyone whose `settings.ini` still says `-1` is a candidate. The immediate fix is to set it
to `0` or `1` by hand, or to click the prompt.

**The open question this leaves.** A modal is centred and dims the background, so a user
should see it. If someone reports being blocked and insists they saw nothing, the modal is
open but not visible, and the log distinguishes the ways that can happen:

- `modalOnScreen=0` — its rect is entirely outside the display. Positions persist in
  `menus.ini`, and `SetNextWindowPos(..., ImGuiCond_Appearing, ...)` does not override a
  loaded one, so a layout saved at a different resolution can put it out of view.
- `drew=0` — the overlay produced no vertices at all, so the modal is live in ImGui's state
  while nothing renders.
- `cursorDelta=LARGE` — the modal *is* visible, but clicking it lands somewhere else, so the
  user cannot dismiss it and is hard-stuck. This is the case where the two causes compound.

Not covered: a modal drawn *behind* the game's own rendering. `drew=1` cannot distinguish
that, and it would need a frame capture to confirm.

## Cause 2: viewport mouse scaling

`Viewport = 2` or `3` with a client size different from the override. Full write-up in
`docs/ViewportMouseScaling.md`. Signature: `cursorDelta=LARGE` with `display=` different
from `client=`. Fixed in 8.5.2; on any earlier build the workaround is `Viewport = 1`.

## Cause 3: messages never arrive

`clicksSeen=0` with `msgs button=0` while the overlay is open. The WndProc hook is not
receiving mouse messages at all, so nothing downstream matters. Check whether
`PassMsgToImGui` installed, and for another overlay or input hook in the module list.

## Cost, because this lives in the render loop

Measured over ten minutes at 60 fps by mirroring the shipped gating logic
(`build/crashctx_test/overlaydiag_test.cpp`):

| scenario | lines | syscalls |
|---|---:|---:|
| overlay closed, idle | 1 | 2.9/sec |
| overlay open, cursor still | 1 | 2.9/sec |
| modal up, clicking twice a second | 1 | 2.9/sec |
| adversarial: cursor waved across a window edge every 3 frames + mashing, 10 min | 201 | 1.0/sec |

How that is achieved, in the order the checks run:

1. **Per frame, nothing but memory reads.** The change key is built into a fixed buffer from
   values already in hand — `io`, the draw data ImGui just produced, the modal window's own
   rect — and compared with `strcmp`. No Win32, no allocation.
2. **The rate ceiling comes before the Win32 calls.** Measured the other way round it still
   made 81,324 system calls in the adversarial ten minutes, because change-detection let
   every flap through to the syscalls and only the logging was capped. Moving the ceiling
   first cut that to 603.
3. **The mouse button state is reported but not keyed on.** It flips twice per click, which
   measured at 1200 lines for ten minutes of ordinary clicking while telling us nothing the
   `clicksSeen` bit does not already say.
4. **A 200-line session cap**, after which it says so once and goes quiet. This is a
   diagnostic; a couple of hundred lines is more than enough to diagnose anything, and the
   cap makes the worst case a guarantee rather than an argument about user behaviour.
5. **One separate one-shot warning** when a modal has blocked the overlay for ten seconds —
   the line that would have answered the report that prompted all of this.

Cursor position and vertex count are reported in every line but never decide whether one is
written; both change every frame.
