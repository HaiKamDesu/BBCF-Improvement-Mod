#pragma once

#include <cstdint>

bool InstallSystemInputHook();
void RemoveSystemInputHook();

// Scans the captured controller objects for the training "reset positions" action
// having JUST been pressed this frame (logical action bit 0x08000000 = action index
// 0x1B in the just-pressed word at [device+0x28] - keyconfig-resolved, so it fires for
// whatever key/button the player has the reset bound to, keyboard or pad). Returns true
// on the press edge. Verified empirically via the [ResetProbe] runs of 2026-07-19 (see
// docs in GhidraDefs.h input action block).
//
// This deliberately reports ONLY the reset edge and no direction state. The held word at
// [device+0x30] cannot be used to answer "is the player holding Up": the captured set
// includes the keyboard's two GAMESTEAM_SystemKeyControler objects (sysMgr+0x0C/+0x18),
// whose Up bit is also set by the SECOND, usually-untouched key config set and by the
// hardcoded VK_SPACE / VK_UP fallbacks baked into FUN_00469750. Ask
// GetLastObservedBattleInput() instead - see the "Direction state" note in GhidraDefs.h.
bool PollTrainingResetPressed();
