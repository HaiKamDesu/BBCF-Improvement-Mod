# Centring an ImGui modal

Recurring bug, fixed many times: a modal appears at the **top middle** of the screen for its
first frame (or stays there), instead of dead centre.

## Why

Centring is expressed as a position plus a pivot:

```cpp
ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
    ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
```

ImGui can only apply a pivot once it knows how big the window is: it positions the window at
`pos - pivot * size`. On the frame a popup **first appears** it has not measured its content
yet, so for an `ImGuiWindowFlags_AlwaysAutoResize` popup that size is zero, and `pos - 0`
puts the window's **top-left corner** at the centre of the screen - up and to the left of
where it belongs, which reads as "top middle".

`ImGuiCond_Appearing` then makes it permanent. The request is consumed on that one frame, so
the frame after - when the size finally is known - nothing re-centres it.

## The rule

**Give every centred modal an explicit size, and set the size before the position.**

```cpp
ImGui::SetNextWindowSize(ImVec2(430.0f, 165.0f), ImGuiCond_Appearing);
ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
    ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoResize)) { ... }
```

With a real size on frame one the pivot is right on frame one, and there is nothing to
correct later. Drop `AlwaysAutoResize` when you do this - the two are answers to the same
question and the explicit size is the one that can be centred.

A width with `0.0f` height (`ImVec2(520.0f, 0.0f)`) is the common half-measure in this repo.
It fixes the horizontal half, which is the visible half, and works in practice because ImGui
hides an auto-fitting window for its first frame. Prefer both dimensions for anything new.

## When the content really cannot be sized up front

A modal that grows or shrinks as you use it needs re-centring when it changes, not once:
`src/Overlay/Window/DummyActionsPanel.cpp` measures its content, calls `SetNextWindowSize`
with `ImGuiCond_Always` on the frame the fit changes, and passes
`fittedThisFrame ? ImGuiCond_Always : ImGuiCond_Appearing` to `SetNextWindowPos`. That keeps
the window draggable on every other frame, which centring every frame would not.

## Worked examples

- Explicit size, centred once: `ReplayExtrasWindow::DrawCloseConfirm`,
  `ScrWindow::DrawTakeoverSetupModal`.
- Fit-and-remember: `DummyActionsPanel` configuration modals.
