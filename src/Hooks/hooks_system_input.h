#pragma once

#include <cstdint>

bool InstallSystemInputHook();
void RemoveSystemInputHook();

// Scans the captured battle-key controller objects for the training "reset positions"
// action having JUST been pressed this frame (logical action bit 0x08000000 in the
// just-pressed word at [device+0x28] - keyconfig-resolved, so it fires for whatever
// key/button the player has the reset bound to, keyboard or pad). Returns true on the
// press edge; *outUpHeld reports whether the logical Up action (bit 0x1 in the held
// word at [device+0x30]) was down on the same frame. Verified empirically via the
// [ResetProbe] runs of 2026-07-19 (see docs in GhidraDefs.h input action block).
bool PollTrainingResetPressed(bool* outUpHeld);
