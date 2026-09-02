# Viewport mouse scaling, and why "last event wins" is false

Symptom, as reported: *"I can't navigate any menus, mouse clicks don't register."* The
overlay draws, windows highlight under the cursor, nothing is clickable. Works for almost
everyone.

Condition: `Viewport` set to **2** (custom render size) or **3** (fixed 1280x768) while the
game window's client area is a different size. `Viewport = 1` is the default and the shipped
`settings.ini` value, which is why this reaches so few people.

## What goes wrong

Those two viewport modes render ImGui in a fixed coordinate space instead of the window's
client size, so mouse input has to be rescaled into the same space or hit-testing and
rendering disagree.

`ApplyViewportOverride()` used to do that by re-emitting a corrected position after
`ImGui_ImplWin32_NewFrame()`, on the stated assumption that "our position event is the last
one queued and therefore wins". That assumption is false on exactly the frames that matter.

`ImGui::UpdateInputEvents()` trickles the queue (`io.ConfigInputTrickleEventQueue`, default
**true**):

```cpp
if (e->Type == ImGuiInputEventType_MousePos)
{
    // Trickling Rule: Stop processing queued events if we already handled a mouse button change
    if (trickle_fast_inputs && (mouse_button_changed != 0 || mouse_wheeled || key_changed || text_inputted))
        break;
    io.MousePos = event_pos;
```

A frame containing a click has this queue:

| # | source | event |
|---|---|---|
| 1 | `WM_MOUSEMOVE` (backend) | `AddMousePosEvent(raw client x, y)` |
| 2 | `WM_LBUTTONDOWN` (backend) | `AddMouseButtonEvent(0, true)` |
| 3 | `ApplyViewportOverride()` | `AddMousePosEvent(scaled x, y)` |

Event 2 sets `mouse_button_changed`, so event 3 hits the trickling rule and **`break`s**. The
click is hit-tested at the raw client pixel. On frames with no click there is no button event,
event 3 applies, and hovering is correct — which is why the overlay looks alive and only
clicking is dead.

When the override is *smaller* than the window (the usual case: render smaller for
performance, or the fixed 1280x768 on a 1080p/1440p screen), the raw coordinate is larger
than the corrected one, so it lands past the bottom-right of the entire ImGui space. Not
"offset by a bit" — outside everything. No click registers anywhere, in any window.

## Proof

Compiled against the vendored ImGui, no renderer, button rectangle read back from ImGui
itself rather than assumed (`build/crashctx_test/trickle_test.cpp`):

```
button rect (imgui space) = (108,127)-(308,177), centre (208,152)
user's screen cursor      = (312,228)

OLD (corrected pos queued after the button):
   io.MousePos = (312,228)  hovered=0  -> click IS LOST

NEW (scaled at the source):
   io.MousePos = (208,152)  hovered=1  -> click REGISTERS
```

## The fix

Scale **at the source**, inside the Win32 backend, so every position event is already in the
overridden space and queue order stops mattering.
`ImGui_ImplWin32_SetMousePosScale(sx, sy)` is applied at both places the backend reports a
position — the `WM_MOUSEMOVE` / `WM_NCMOUSEMOVE` handler and the `GetCursorPos` fallback in
`ImGui_ImplWin32_UpdateMouseData()` — and inverted in the `WantSetMousePos` path, which reads
`io.MousePos` back out. The mouse-leave sentinel (`-FLT_MAX`) passes through untouched.

`ApplyViewportOverride()` sets the scale each frame (1,1 when no override is active) and still
seeds a position, which now agrees with the backend's events instead of competing with them.

## This is the second time this exact patch was lost

The same bug was diagnosed and fixed in July 2026 for release/8-1, as
`ImGui_ImplDX9_SetDisplaySizeOverride` — scaling inside the **DX9** backend's `WM_MOUSEMOVE`.
Commit `3186cca` (*Update Dear ImGui from 1.53 to 1.92.9b*) replaced the vendored backends
with stock upstream files, dropping that patch, and the scaling was reimplemented in
`WindowManager` as the post-hoc re-emit described above. So v8.2 through v8.5 all carry it.

Two rules follow, and they generalise past this file:

- **A local modification to anything in `depends/` must announce itself.** Both the header
  declaration and the implementation now carry a `DO NOT DROP WHEN UPGRADING DEAR IMGUI`
  block naming this document. `WindowManager::Initialize` already learned the same lesson for
  `IniFilename` ("this used to be a one-line edit inside imgui.cpp, which meant updating
  ImGui silently reverted it").
- **Reimplementing a vendored patch "more cleanly" outside the vendored file is where the
  regression came from.** The re-emit looked tidier than patching a backend and was wrong,
  because it depended on ImGui internals (queue ordering) that the vendored file does not
  promise. If the correct place for a fix is inside `depends/`, put it there and label it.

## Checking a report

Ask for the reporter's `Viewport` line from `settings.ini`, and their game resolution.

- `Viewport = 1` → this is not their bug; look elsewhere.
- `Viewport = 2` or `3` with a client size different from the override → this is it, and
  setting `Viewport = 1` is the immediate workaround on any affected build.

The `DEBUG.txt` settings dump at startup lists `Viewport`, `RenderingWidth` and
`RenderingHeight`, so an existing log answers this without asking.
