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

// Directions the GAME sees held at the moment of a training reset, as a bitmask of the
// game's own logical action bits (Up=0x1 Right=0x2 Down=0x4 Left=0x8 - action IDs 7..10 in
// the mask table at ADDR_ActionIdMaskTable).
//
// Read exactly the way FUN_006D2D40 reads Left/Right/Down for its own reset rule: the
// controller wrapper of the training player's controller id (engine[0x25F4 + local*4],
// FUN_0047E7B0) asked with FUN_00464170(actionId) - which ORs the per-device word at
// [device+0x20]->+0xC against the mask (FUN_004964E0). Whatever that returns IS what the
// game acts on for Down/Left/Right on the same press, so the two can never disagree.
//
// Only meaningful from the game thread at the reset hook (it calls game functions). It is
// NOT the packed battle input word: that value was neutral at the hook every time it was
// tried (see docs/Research/ResetLabOverride.md), which is why Down=swap never fired.
enum TrainingResetDirection : uint32_t
{
        TrainingResetDir_Up = 0x1,
        TrainingResetDir_Right = 0x2,
        TrainingResetDir_Down = 0x4,
        TrainingResetDir_Left = 0x8,
        TrainingResetDir_All = 0xF,
};

// What holding a direction while pressing Reset Positions does. Values are the settings
// enum (ResetLabUpAction / Down / Left / Right in settings.def; same order in
// SettingsIniWindow.cpp's kResetLabActionOptions).
enum ResetLabAction
{
        ResetLabAction_Default = 0,      // today's behaviour: Up swaps sides, Down = middle, Left/Right = that corner
        ResetLabAction_Nothing = 1,      // plain reset, the previous reset position stays
        ResetLabAction_Middle = 2,
        ResetLabAction_LeftCorner = 3,
        ResetLabAction_RightCorner = 4,
        ResetLabAction_Swap = 5,
};

// The direction a press counts as, with the game's own priority (Left, then Right, then
// Down, then Up), and the action its setting maps it to. direction is 0 when nothing is
// held (action is then Default).
struct ResetLabDecision
{
        uint32_t direction = 0;
        ResetLabAction action = ResetLabAction_Default;
        // What the decision amounts to once Default is expanded for the direction.
        bool swapSides = false;
};
ResetLabDecision ResolveResetLabAction(uint32_t heldDirections);

// The decision the reset hook made for the most recent reset press, for the swap toggle
// that is applied once the reset actually executes (ScrWindow::TickTrainingResetSwap).
// Returns false when no press has been decided since the last call.
bool ConsumeLastResetPressDecision(ResetLabDecision* out);

// JMP patch at FUN_006D2D40 (the "held direction -> TRI_ResetPosition" step of a training
// reset) that applies the remap above. See docs/Research/ResetLabOverride.md.
bool InstallTrainingResetDirectionHook();
