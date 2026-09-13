#include "hooks_battle_input.h"
#include "HookManager.h"
#include "Core/logger.h"
#include "Core/RuntimePlatform.h"
#include "Core/interfaces.h"
#include "Core/Settings.h"
#include "Game/gamestates.h"

#include <array>

namespace
{
    constexpr uint16_t INPUT_BUTTON_A = 16;
    constexpr uint16_t INPUT_BUTTON_B = 32;
    constexpr uint16_t INPUT_BUTTON_C = 64;
    constexpr uint16_t INPUT_BUTTON_D = 128;
    constexpr uint16_t INPUT_BUTTON_TAUNT = 256;
    constexpr uint16_t INPUT_BUTTON_SPECIAL = 512;

    constexpr uint16_t INPUT_DIRECTION_NEUTRAL = 5;

    constexpr size_t MAX_BATTLE_PLAYERS = 2;

    struct OverrideState
    {
        bool active = false;
        uint16_t packedValue = INPUT_DIRECTION_NEUTRAL;
        uint32_t framesRemaining = 0; // 0 = infinite
        uint32_t lastConsumedFrame = 0;
        bool hasConsumedFrame = false;
    };
    std::array<OverrideState, MAX_BATTLE_PLAYERS> g_overrideState{};
    std::array<uint16_t, MAX_BATTLE_PLAYERS> g_lastObservedPacked{ INPUT_DIRECTION_NEUTRAL, INPUT_DIRECTION_NEUTRAL };
    std::array<uint16_t, MAX_BATTLE_PLAYERS> g_lastAppliedPacked{ INPUT_DIRECTION_NEUTRAL, INPUT_DIRECTION_NEUTRAL };

    struct BattleInputDiagnosticEntry
    {
        uint32_t rawSlot = UINT32_MAX;
        uintptr_t targetAddress = 0;
        uint16_t lastPacked = UINT16_MAX;
        uint32_t remaining = 32;
        bool initialized = false;
    };

    // Keep P1/P2 candidates separate by the actual writer slot and destination.
    std::array<BattleInputDiagnosticEntry, 8> g_battleInputDiagnostics{};
    std::array<uint16_t, MAX_BATTLE_PLAYERS> g_lastLoggedOverride{ UINT16_MAX, UINT16_MAX };
    uint32_t g_overrideLogBudget = 64;

    uint16_t BuildDirectionFromState(const InputState& state)
    {
        const bool up = state.up && !state.down;
        const bool down = state.down && !state.up;
        const bool left = state.left && !state.right;
        const bool right = state.right && !state.left;

        if (up && left) { return 7; }
        if (up && right) { return 9; }
        if (down && left) { return 1; }
        if (down && right) { return 3; }
        if (up) { return 8; }
        if (down) { return 2; }
        if (left) { return 4; }
        if (right) { return 6; }
        return INPUT_DIRECTION_NEUTRAL;
    }

    // Training input delay ---------------------------------------------------------
    // Holds 1P's inputs back by a few frames so training can be practised at the delay a
    // net match runs at. BBCF's netcode adds a hardcoded 2 frames of input delay whatever
    // the ping (ggpo_set_frame_delay(2) from 0x4E598C - see
    // docs/Research/OnlineSimulationTrainingFeasibility.md), so this is the part of "online
    // feel" that can be reproduced offline honestly. Rollback depth and timesync hitches
    // are the rest of it, and neither is simulated here.
    //
    // 1P only, on purpose: the dummy stays instant, so a recorded playback - which is
    // driven from the 2P side - is never captured through the delay and then replayed
    // through it a second time.
    constexpr size_t MAX_INPUT_DELAY_FRAMES = 10;

    struct InputDelayLine
    {
        // One slot more than the delay itself: this frame's input is queued before the
        // one that has finished waiting is taken out.
        uint16_t queued[MAX_INPUT_DELAY_FRAMES + 1] = {};
        size_t count = 0;
        uint16_t frameOutput = INPUT_DIRECTION_NEUTRAL;
        uint32_t lastFrame = 0;
        bool hasFrame = false;
    };
    InputDelayLine g_inputDelay{};

    uint16_t ApplyTrainingInputDelay(size_t player, uint16_t packedInput)
    {
        // The 2P side is not delayed and must not clear the line either: the writer runs
        // for both players every frame, so resetting from here would empty 1P's queue
        // before it ever reached its depth.
        if (player != 0)
        {
            return packedInput;
        }

        int configured = Settings::settingsIni.trainingInputDelay;
        if (configured < 0) { configured = 0; }
        if (configured > static_cast<int>(MAX_INPUT_DELAY_FRAMES)) { configured = static_cast<int>(MAX_INPUT_DELAY_FRAMES); }

        const bool eligible =
            configured > 0 &&
            g_gameVals.pFrameCount != nullptr &&
            g_gameVals.pGameMode != nullptr &&
            *g_gameVals.pGameMode == GameMode_Training;

        if (!eligible)
        {
            g_inputDelay = InputDelayLine{};
            return packedInput;
        }

        const uint32_t currentFrame = *g_gameVals.pFrameCount;

        // The writer can run more than once within one game frame (see the override block
        // below, which has to account for the same thing). The line must advance exactly
        // once per frame or a single press would be smeared across several queue slots.
        if (g_inputDelay.hasFrame && g_inputDelay.lastFrame == currentFrame)
        {
            return g_inputDelay.frameOutput;
        }

        // A frame counter that did not simply tick forward means the match jumped: a save
        // state was loaded, a round reset, positions were reset. Whatever is in flight
        // belongs to the timeline that was abandoned, so it is dropped instead of being
        // replayed into the new one.
        if (g_inputDelay.hasFrame && currentFrame != g_inputDelay.lastFrame + 1)
        {
            g_inputDelay.count = 0;
        }

        const size_t depth = static_cast<size_t>(configured);

        // Turning the delay down mid-session leaves more in flight than the new depth
        // wants; the oldest go, so the change takes effect now rather than after a
        // backlog of stale inputs has drained.
        while (g_inputDelay.count > depth)
        {
            for (size_t i = 1; i < g_inputDelay.count; ++i)
            {
                g_inputDelay.queued[i - 1] = g_inputDelay.queued[i];
            }
            --g_inputDelay.count;
        }

        g_inputDelay.queued[g_inputDelay.count++] = packedInput;

        uint16_t output = INPUT_DIRECTION_NEUTRAL;
        if (g_inputDelay.count > depth)
        {
            output = g_inputDelay.queued[0];
            for (size_t i = 1; i < g_inputDelay.count; ++i)
            {
                g_inputDelay.queued[i - 1] = g_inputDelay.queued[i];
            }
            --g_inputDelay.count;
        }

        g_inputDelay.frameOutput = output;
        g_inputDelay.lastFrame = currentFrame;
        g_inputDelay.hasFrame = true;
        return output;
    }

    uint16_t __cdecl ProcessBattleInput(
        uint16_t packedInput,
        uint32_t playerIndex,
        uintptr_t targetAddress,
        uint16_t previousValue)
    {
        // This normalization remains for the existing override API only. Diagnostics
        // retain the raw writer slot and destination so P2 can be identified directly.
        const size_t normalizedPlayer = playerIndex == 0 ? 0 : 1;

        g_lastObservedPacked[normalizedPlayer] = packedInput;
        if (g_gameVals.pGameMode && *g_gameVals.pGameMode == GameMode_Training)
        {
            BattleInputDiagnosticEntry* diagnostic = nullptr;
            for (auto& entry : g_battleInputDiagnostics)
            {
                if (entry.initialized &&
                    entry.rawSlot == playerIndex &&
                    entry.targetAddress == targetAddress)
                {
                    diagnostic = &entry;
                    break;
                }
            }

            if (!diagnostic)
            {
                for (auto& entry : g_battleInputDiagnostics)
                {
                    if (!entry.initialized)
                    {
                        entry.initialized = true;
                        entry.rawSlot = playerIndex;
                        entry.targetAddress = targetAddress;
                        diagnostic = &entry;
                        break;
                    }
                }
            }

            if (diagnostic &&
                diagnostic->remaining > 0 &&
                packedInput != diagnostic->lastPacked)
            {
                --diagnostic->remaining;
                diagnostic->lastPacked = packedInput;
                LOG(1,
                    "[TAS][BattleInputProbe] raw_slot=%u target=0x%08X previous=%u observed=%u frame=%u remaining=%u\n",
                    playerIndex,
                    static_cast<unsigned int>(targetAddress),
                    previousValue,
                    packedInput,
                    g_gameVals.pFrameCount ? *g_gameVals.pFrameCount : 0,
                    diagnostic->remaining);
            }
        }

        // The delay runs before the override so an injected input (TAS, dummy playback)
        // still lands on the frame it was scheduled for: what is being delayed is the
        // human holding the pad, not the tooling standing in for one.
        packedInput = ApplyTrainingInputDelay(normalizedPlayer, packedInput);

        OverrideState& overrideState = g_overrideState[normalizedPlayer];
        if (overrideState.active)
        {
            // A finite override lasts for complete game frames. The same input
            // can be written more than once during one frame.
            const uint32_t currentFrame = g_gameVals.pFrameCount ? *g_gameVals.pFrameCount : 0;
            if (overrideState.framesRemaining > 0 &&
                overrideState.hasConsumedFrame &&
                overrideState.lastConsumedFrame != currentFrame)
            {
                --overrideState.framesRemaining;
                if (overrideState.framesRemaining == 0)
                {
                    overrideState.active = false;
                }
            }

            if (overrideState.active)
            {
                const uint16_t originalInput = packedInput;
                packedInput = overrideState.packedValue;
                overrideState.lastConsumedFrame = currentFrame;
                overrideState.hasConsumedFrame = true;
                if (g_overrideLogBudget > 0 &&
                    g_lastLoggedOverride[normalizedPlayer] != packedInput)
                {
                    --g_overrideLogBudget;
                    g_lastLoggedOverride[normalizedPlayer] = packedInput;
                    LOG(1,
                        "[TAS][BattleOverride] raw_slot=%u target=0x%08X original=%u applied=%u frame=%u remaining=%u\n",
                        playerIndex,
                        static_cast<unsigned int>(targetAddress),
                        originalInput,
                        packedInput,
                        currentFrame,
                        overrideState.framesRemaining);
                }
            }
        }

        g_lastAppliedPacked[normalizedPlayer] = packedInput;
        return packedInput;
    }

    DWORD battleInputWrite_JmpBack = 0;

    // Absolute target of the call instruction the hook replaces, resolved from its
    // rel32 before the site is patched. The hook has to perform that call itself.
    DWORD battleInputOriginalCall = 0;
}

uint16_t InputState::ToPackedValue() const
{
    uint16_t packed = BuildDirectionFromState(*this);

    if (A) { packed += INPUT_BUTTON_A; }
    if (B) { packed += INPUT_BUTTON_B; }
    if (C) { packed += INPUT_BUTTON_C; }
    if (D) { packed += INPUT_BUTTON_D; }
    if (taunt) { packed += INPUT_BUTTON_TAUNT; }
    if (special) { packed += INPUT_BUTTON_SPECIAL; }

    return packed;
}

InputState InputState::FromPackedValue(uint16_t packed)
{
    InputState state{};

    switch (packed & 0xF)
    {
    case 1:
        state.down = true;
        state.left = true;
        break;
    case 2:
        state.down = true;
        break;
    case 3:
        state.down = true;
        state.right = true;
        break;
    case 4:
        state.left = true;
        break;
    case 5:
        break;
    case 6:
        state.right = true;
        break;
    case 7:
        state.up = true;
        state.left = true;
        break;
    case 8:
        state.up = true;
        break;
    case 9:
        state.up = true;
        state.right = true;
        break;
    default:
        break;
    }

    state.A = (packed & INPUT_BUTTON_A) != 0;
    state.B = (packed & INPUT_BUTTON_B) != 0;
    state.C = (packed & INPUT_BUTTON_C) != 0;
    state.D = (packed & INPUT_BUTTON_D) != 0;
    state.taunt = (packed & INPUT_BUTTON_TAUNT) != 0;
    state.special = (packed & INPUT_BUTTON_SPECIAL) != 0;

    return state;
}

void OverrideBattleInput(uint32_t playerIndex, const InputState& state, uint32_t framesToHold)
{
    OverrideBattleInputPacked(playerIndex, state.ToPackedValue(), framesToHold);
}

void OverrideBattleInputPacked(uint32_t playerIndex, uint16_t packedValue, uint32_t framesToHold)
{
    if (playerIndex >= MAX_BATTLE_PLAYERS)
    {
        return;
    }

    OverrideState& overrideState = g_overrideState[playerIndex];
    overrideState.active = true;
    overrideState.packedValue = packedValue;
    overrideState.framesRemaining = framesToHold;
    overrideState.lastConsumedFrame = 0;
    overrideState.hasConsumedFrame = false;
}

void ClearBattleInputOverride(uint32_t playerIndex)
{
    if (playerIndex >= MAX_BATTLE_PLAYERS)
    {
        return;
    }

    OverrideState& overrideState = g_overrideState[playerIndex];
    overrideState.active = false;
    overrideState.framesRemaining = 0;
    overrideState.lastConsumedFrame = 0;
    overrideState.hasConsumedFrame = false;
    overrideState.packedValue = INPUT_DIRECTION_NEUTRAL;
}

bool IsBattleInputOverrideActive(uint32_t playerIndex)
{
    if (playerIndex >= MAX_BATTLE_PLAYERS)
    {
        return false;
    }

    return g_overrideState[playerIndex].active;
}

InputState GetLastObservedBattleInput(uint32_t playerIndex)
{
    if (playerIndex >= MAX_BATTLE_PLAYERS)
    {
        return InputState{};
    }

    return InputState::FromPackedValue(g_lastObservedPacked[playerIndex]);
}

InputState GetLastAppliedBattleInput(uint32_t playerIndex)
{
    if (playerIndex >= MAX_BATTLE_PLAYERS)
    {
        return InputState{};
    }

    return InputState::FromPackedValue(g_lastAppliedPacked[playerIndex]);
}

uint16_t GetLastObservedBattleInputPacked(uint32_t playerIndex)
{
    return playerIndex < MAX_BATTLE_PLAYERS ? g_lastObservedPacked[playerIndex] : INPUT_DIRECTION_NEUTRAL;
}

uint16_t GetLastAppliedBattleInputPacked(uint32_t playerIndex)
{
    return playerIndex < MAX_BATTLE_PLAYERS ? g_lastAppliedPacked[playerIndex] : INPUT_DIRECTION_NEUTRAL;
}

void __declspec(naked) BattleInputWrite_Hook()
{
    __asm {
        // Site: 0x0055A338, the thiscall at the common tail of the two-player input
        // loop. Both write paths have converged here, ESI points at the player's
        // input object (stride 0x1C) with the finished packed word at offset 0, ECX
        // has already been loaded with it and EBX is the player index.
        //
        // Do NOT move this back to 0x0055A333: eleven branches in the loop jump to
        // 0x0055A336, which is three bytes inside a 5-byte JMP placed at 0x0055A333.
        // Those paths land mid-instruction and the process dies on a CRT abort.
        pushad
        movzx eax, word ptr[esi]
        push eax // previousValue
        push esi // targetAddress
        push ebx // playerIndex
        push eax // packedInput
        call ProcessBattleInput
        add esp, 16
        mov word ptr[esi], ax
        popad

        // ORIGINAL instruction: call <consume input object> (thiscall, ECX = ESI).
        call dword ptr[battleInputOriginalCall]

        jmp battleInputWrite_JmpBack
    }
}

bool Hook_BattleInput()
{
    if (!IsControllerHooksRuntimeAllowed()) {
        LOG(1, "Hook_BattleInput skipped by runtime controller gate\n");
        return false;
    }

    // The 5 bytes we take over are the call at the tail of the two-player input loop
    // (static 0x0055A338). Resolve the site first without patching it, so the call's
    // rel32 can be read while it is still the original instruction.
    const DWORD siteAddress = HookManager::RegisterHook(
        "BattleInputWriteSite",
        "\xE8\x00\x00\x00\x00\x8B\x45\xF0\x81\x45\xF4\x78\x49\x02\x00\x83\xC0\x04\x43\x83\xC6\x1C",
        "x????xxxxxxxxxxxxxxxxx",
        5);

    if (siteAddress == 0)
    {
        LOG(0, "FAILED TO LOCATE BattleInputWrite HOOK SITE\n");
        return false;
    }

    const int32_t callDisplacement = *reinterpret_cast<const int32_t*>(siteAddress + 1);
    battleInputOriginalCall = static_cast<DWORD>(siteAddress + 5 + callDisplacement);
    LOG(1, "BattleInputWrite site 0x%08X, original call target 0x%08X\n",
        siteAddress, battleInputOriginalCall);

    battleInputWrite_JmpBack = HookManager::SetHook(
        "BattleInputWrite",
        siteAddress,
        5,
        &BattleInputWrite_Hook
    );

    if (battleInputWrite_JmpBack == 0)
    {
        battleInputOriginalCall = 0;
        LOG(0, "FAILED TO INSTALL BattleInputWrite HOOK\n");
        return false;
    }

    LOG(1, "BattleInputWrite hook installed OK\n");
    return true;
}

bool IsBattleInputHookInstalled()
{
    return battleInputWrite_JmpBack != 0;
}
