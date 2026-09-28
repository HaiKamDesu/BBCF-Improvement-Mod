#include "hooks_system_input.h"

#include "RankedAutomationHarness.h"

#include "HookManager.h"
#include "Core/ControllerOverrideManager.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/RuntimePlatform.h"
#include "Core/Settings.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Game/GhidraDefs.h"

#include <cstring>

namespace
{
        DWORD systemInputWrite_JmpBack = 0;

        // Distinct controller object pointers captured by the SystemInputWrite hook,
        // read by PollTrainingResetPressed for the game's logical action words.
        // Multiple objects can resolve to slot=None, so they're tracked individually.
        constexpr size_t kProbeMaxControllers = 16;
        void* volatile s_probeControllers[kProbeMaxControllers] = {};

        const char* GetSlotLabel(SystemControllerSlot slot)
        {
            switch (slot)
            {
            case SystemControllerSlot::MenuP1: return "MenuP1";
            case SystemControllerSlot::MenuP2: return "MenuP2";
            case SystemControllerSlot::CharP1: return "CharP1";
            case SystemControllerSlot::CharP2: return "CharP2";
            case SystemControllerSlot::UnknownP1: return "UnknownP1";
            case SystemControllerSlot::UnknownP2: return "UnknownP2";
            case SystemControllerSlot::None:
            default:
                return "None";
            }
        }

        bool IsGameStateReadyForSystemInputHook()
        {
            return g_gameVals.pGameMode &&
                g_gameVals.pGameState &&
                *g_gameVals.pGameMode >= 0 &&
                *g_gameVals.pGameState >= 0 &&
                GetGameSceneStatus() >= GameSceneStatus_Running;
        }

        uint32_t __stdcall SystemInputHookInternal(void* controllerPtr, uint32_t currentWord)
        {
            if (!IsControllerHooksRuntimeAllowed())
            {
                return currentWord;
            }

            if (!IsGameStateReadyForSystemInputHook())
            {
                static int skipLogBudget = 16;
                if (skipLogBudget > 0)
                {
                    const int gameMode = g_gameVals.pGameMode ? *g_gameVals.pGameMode : -1;
                    const int gameState = g_gameVals.pGameState ? *g_gameVals.pGameState : -1;
                    LOG(1, "[SystemInputHook] skip before game ready mode=%d state=%d scene=%d word=0x%08X controller=0x%p\n",
                        gameMode,
                        gameState,
                        GetGameSceneStatus(),
                        static_cast<unsigned int>(currentWord),
                        controllerPtr);
                    --skipLogBudget;
                }
                return currentWord;
            }

            auto& mgr = ControllerOverrideManager::GetInstance();
            const auto slot = mgr.ResolveSystemSlotFromControllerPtr(controllerPtr);

            // Remember each distinct controller object so PollTrainingResetPressed()
            // can read its logical-action words ([ptr+0x28]/[ptr+0x30]) after the
            // game's per-frame input builder has finished. (This hook itself fires at
            // the builder's zero-init, so the words aren't final here - see
            // GhidraDefs.h input action system block.)
            for (size_t i = 0; i < kProbeMaxControllers; ++i)
            {
                if (s_probeControllers[i] == controllerPtr)
                {
                    break;
                }
                if (s_probeControllers[i] == nullptr)
                {
                    s_probeControllers[i] = controllerPtr;
                    break;
                }
            }

            uint32_t automationWord = 0;
            static int logBudget = 40;
            static SystemControllerSlot lastLoggedSlot = SystemControllerSlot::None;
            static int lastLoggedGameState = -999;

            const int gameState = g_gameVals.pGameState ? *g_gameVals.pGameState : -1;
            if (logBudget > 0 && (slot != lastLoggedSlot || gameState != lastLoggedGameState))
            {
                LOG(1, "[RankedAutoProbe] system_input slot=%s state=%d word=0x%08X controller=0x%p\n",
                    GetSlotLabel(slot),
                    gameState,
                    static_cast<unsigned int>(currentWord),
                    controllerPtr);
                lastLoggedSlot = slot;
                lastLoggedGameState = gameState;
                --logBudget;
            }

            if (RankedAutomationHarness::TryOverrideSystemInput(slot, currentWord, &automationWord))
            {
                return automationWord;
            }

            // If multiple-keyboard override is off, or the mapping popup is open,
            // we leave system input completely untouched.
            if (!mgr.IsMultipleKeyboardOverrideEnabled() || mgr.IsMappingPopupActive())
            {
                return currentWord;
            }

            if (!mgr.HasSystemOverride(slot))
            {
                return currentWord;
            }

            return mgr.BuildSystemInputWord(slot);
        }

        void __declspec(naked) SystemInputWrite_Hook()
        {
            __asm {
                // EBX = packed system input word from the game
                // ESI = controller pointer

                push ecx
                push edx

                push ebx // currentWord
                push esi // controllerPtr
                call SystemInputHookInternal // __stdcall cleans stack itself

                mov ebx, eax  // apply override word if any

                pop edx
                pop ecx

                mov[esi + 0x30], ebx
                cmp[esi + 0x38], ebx

                jmp systemInputWrite_JmpBack
            }
        }
}

bool InstallSystemInputHook()
{
        if (!IsControllerHooksRuntimeAllowed())
        {
                LOG(1, "InstallSystemInputHook skipped by runtime controller gate\n");
                return false;
        }

        const uintptr_t base = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
        systemInputWrite_JmpBack = HookManager::SetHook(
                "SystemInputWrite",
                static_cast<DWORD>(base + 0x96408),
                6,
                SystemInputWrite_Hook);

        if (systemInputWrite_JmpBack == 0)
        {
                LOG(0, "FAILED TO INSTALL SystemInputWrite HOOK\n");
                return false;
        }

        LOG(1, "SystemInputWrite hook installed OK\n");
        return true;
}

void RemoveSystemInputHook()
{
        HookManager::DeactivateHook("SystemInputWrite");
}

bool PollTrainingResetPressed()
{
        constexpr uint32_t kActionResetPositions = 0x08000000; // just-pressed word bit

        for (size_t i = 0; i < kProbeMaxControllers; ++i)
        {
                const uint8_t* const ptr = static_cast<const uint8_t*>(s_probeControllers[i]);
                if (ptr == nullptr)
                {
                        break;
                }
                if (IsBadReadPtr(ptr + 0x28, 0x04))
                {
                        continue;
                }

                const uint32_t edges = *reinterpret_cast<const uint32_t*>(ptr + 0x28);
                if (edges & kActionResetPositions)
                {
                        return true;
                }
        }
        return false;
}

namespace
{
        // The game's own reads, called the way FUN_006D2D40 calls them. All static VAs from
        // docs/Research/ResetDirectionSourceGhidraReport.txt.
        typedef void* (__cdecl* GetEngineFn)();                       // FUN_0047E860
        typedef void* (__cdecl* GetControllerWrapperFn)(int ctrlId);  // FUN_0047E7B0
        typedef uint32_t (__thiscall* WrapperActionHeldFn)(void* wrapper, int actionId); // FUN_00464170

        // Index (0/1) of the side the training player controls: engine+0x1688 (FUN_0047E9C0).
        int TrainingLocalPlayer(uint8_t* engine)
        {
                return *reinterpret_cast<int*>(engine + 0x1688) != 0 ? 1 : 0;
        }

        uint32_t ReadHeldDirectionsFromGame()
        {
                const uintptr_t base = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
                const auto getEngine = reinterpret_cast<GetEngineFn>(base + ADDR_SceneObjectGet);
                const auto getWrapper = reinterpret_cast<GetControllerWrapperFn>(base + ADDR_GetControllerWrapperById);
                const auto actionHeld = reinterpret_cast<WrapperActionHeldFn>(base + ADDR_ControllerWrapper_ActionRepeatPressed);

                uint8_t* engine = static_cast<uint8_t*>(getEngine());
                if (engine == nullptr)
                {
                        return 0;
                }
                const int local = TrainingLocalPlayer(engine);
                const int ctrlId = *reinterpret_cast<int*>(engine + 0x25F4 + local * 4);
                void* wrapper = getWrapper(ctrlId);
                if (wrapper == nullptr)
                {
                        return 0;
                }
                // Action IDs 7..10 = up/down/left/right; the mask table turns each into its bit,
                // which is what FUN_00464170 returns (mask & word), so the result is already the
                // TrainingResetDirection bit for that direction.
                uint32_t held = 0;
                held |= actionHeld(wrapper, 7) ? TrainingResetDir_Up : 0;
                held |= actionHeld(wrapper, 8) ? TrainingResetDir_Down : 0;
                held |= actionHeld(wrapper, 9) ? TrainingResetDir_Left : 0;
                held |= actionHeld(wrapper, 10) ? TrainingResetDir_Right : 0;
                return held;
        }

        ResetLabAction ClampResetLabAction(int v)
        {
                if (v < ResetLabAction_Default || v > ResetLabAction_Swap)
                {
                        return ResetLabAction_Default;
                }
                return static_cast<ResetLabAction>(v);
        }

        ResetLabDecision s_lastPressDecision;
        volatile bool s_lastPressDecisionPending = false;
}

// enum: 0 Default, 1 Nothing, 2 Middle, 3 Left corner, 4 Right corner, 5 Swap sides. Same
// order as settings.def and SettingsIniWindow.cpp's kResetLabActionOptions.
ResetLabDecision ResolveResetLabAction(uint32_t heldDirections)
{
        ResetLabDecision d;
        // The game's own priority in FUN_006D2D40: Left, then Right, then Down. Up is never
        // read by the game, so it comes last here.
        if (heldDirections & TrainingResetDir_Left)
        {
                d.direction = TrainingResetDir_Left;
                d.action = ClampResetLabAction(Settings::settingsIni.resetLabLeftAction);
        }
        else if (heldDirections & TrainingResetDir_Right)
        {
                d.direction = TrainingResetDir_Right;
                d.action = ClampResetLabAction(Settings::settingsIni.resetLabRightAction);
        }
        else if (heldDirections & TrainingResetDir_Down)
        {
                d.direction = TrainingResetDir_Down;
                d.action = ClampResetLabAction(Settings::settingsIni.resetLabDownAction);
        }
        else if (heldDirections & TrainingResetDir_Up)
        {
                d.direction = TrainingResetDir_Up;
                d.action = ClampResetLabAction(Settings::settingsIni.resetLabUpAction);
        }
        // Default on Up is the mod's historical swap; on every other direction Default is
        // what the game does by itself.
        d.swapSides = d.action == ResetLabAction_Swap
                || (d.action == ResetLabAction_Default && d.direction == TrainingResetDir_Up);
        return d;
}

bool ConsumeLastResetPressDecision(ResetLabDecision* out)
{
        if (!s_lastPressDecisionPending)
        {
                return false;
        }
        s_lastPressDecisionPending = false;
        if (out != nullptr)
        {
                *out = s_lastPressDecision;
        }
        return true;
}

namespace
{
        DWORD trainingResetDirection_JmpBack = 0;

        // Read by the naked hook after Decide() has run. 1 = return from FUN_006D2D40 without
        // running it, i.e. the game writes nothing to TRI_ResetPosition this reset.
        volatile uint8_t s_skipResetDirectionOriginal = 0;

        // Writes the "TRI_ResetPosition" training-menu value (0 = middle, 2 = left, 1 = right)
        // through the game's own name-based setter FUN_006CD5C0, with the same 32-byte
        // zero-padded name buffer FUN_006D2D40 builds (FUN_00427620). The game resolves the
        // entry itself, so there is no table layout for the mod to get wrong - which is
        // exactly what happened the first time (see GhidraDefs.h, "CORRECTION 2026-09-27").
        // Reads the value back through the game's reader FUN_006CBF90 and logs both, so a
        // log line proves the value FUN_006CBFE0 will see at the reset.
        bool SetTrainingResetPositionMenuValue(int value)
        {
                typedef void(__stdcall* SetByNameFn)(const char* name, int value);
                typedef int(__stdcall* GetByNameFn)(const char* name);
                const uintptr_t base = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
                const auto setByName = reinterpret_cast<SetByNameFn>(base + ADDR_TrainingMenuItem_SetByName);
                const auto getByName = reinterpret_cast<GetByNameFn>(base + ADDR_TrainingMenuItem_GetByName);

                char name[32] = {};
                strncpy_s(name, sizeof(name), "TRI_ResetPosition", _TRUNCATE);

                const int before = getByName(name);
                setByName(name, value);
                const int after = getByName(name);
                LOG(1, "[ResetSwap] TRI_ResetPosition %d -> wrote %d -> reads back %d\n", before, value, after);
                return after == value;
        }

        // Runs at the entry of FUN_006D2D40, the game's "held direction -> TRI_ResetPosition"
        // step of a training reset. Decides whether the game's own version may run, and
        // performs the mod's version when it may not. Exactly one of the two handles a press.
        void __cdecl TrainingResetDirection_Decide()
        {
                s_skipResetDirectionOriginal = 0;

                const uint32_t held = ReadHeldDirectionsFromGame();
                const ResetLabDecision decision = ResolveResetLabAction(held);

                if (decision.direction != 0)
                {
                        switch (decision.action)
                        {
                        case ResetLabAction_Default:
                                // The game's own rule for this direction (nothing, for Up).
                                break;
                        case ResetLabAction_Nothing:
                        case ResetLabAction_Swap:
                                // No position write at all: the previous reset position stands.
                                // A swap is ScrWindow::TickTrainingResetSwap's job once the
                                // reset has executed, via ConsumeLastResetPressDecision.
                                s_skipResetDirectionOriginal = 1;
                                break;
                        case ResetLabAction_Middle:
                        case ResetLabAction_LeftCorner:
                        case ResetLabAction_RightCorner:
                        {
                                s_skipResetDirectionOriginal = 1;
                                // TRI_ResetPosition: 0 = middle, 2 = left, 1 = right, in the
                                // convention FUN_006D2D40 writes (action ID 9 -> 2, ID 10 -> 1),
                                // and mirrored the way it mirrors: when the training player is the
                                // P2 side (engine+0x1688 != 0) its left and right swap.
                                int value = 0;
                                if (decision.action != ResetLabAction_Middle)
                                {
                                        const uintptr_t base = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
                                        uint8_t* engine = static_cast<uint8_t*>(
                                                reinterpret_cast<GetEngineFn>(base + ADDR_SceneObjectGet)());
                                        const bool p2Side = engine != nullptr && TrainingLocalPlayer(engine) == 1;
                                        const bool wantLeft = (decision.action == ResetLabAction_LeftCorner) != p2Side;
                                        value = wantLeft ? 2 : 1;
                                }
                                if (!SetTrainingResetPositionMenuValue(value))
                                {
                                        LOG(1, "[ResetSwap] TRI_ResetPosition did not read back %d; the reset keeps its previous position\n", value);
                                }
                                break;
                        }
                        default:
                                break;
                        }
                }

                s_lastPressDecision = decision;
                s_lastPressDecisionPending = true;

                LOG(1, "[ResetSwap] reset pressed: held=0x%X dir=0x%X action=%d swap=%d skipGame=%d\n",
                        held, decision.direction, static_cast<int>(decision.action),
                        decision.swapSides ? 1 : 0, static_cast<int>(s_skipResetDirectionOriginal));
        }

        // Site: FUN_006D2D40 entry, the first three instructions (push ebp / mov ebp,esp /
        // sub esp,2Ch = 6 bytes). Nothing branches into 0x6D2D41..0x6D2D45 (only caller is the
        // call at 0x55EFCC), so we own the prologue: on the skip path nothing has been pushed
        // yet and a plain ret returns to FUN_0055EF40. The function takes no arguments.
        void __declspec(naked) TrainingResetDirection_Hook()
        {
                __asm {
                        pushfd
                        pushad
                        call TrainingResetDirection_Decide
                        popad
                        popfd
                        cmp byte ptr [s_skipResetDirectionOriginal], 0
                        jne skipOriginal
                        push ebp
                        mov ebp, esp
                        sub esp, 0x2C
                        jmp trainingResetDirection_JmpBack
                skipOriginal:
                        ret
                }
        }
}

bool InstallTrainingResetDirectionHook()
{
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetBbcfBaseAdress());
        trainingResetDirection_JmpBack = HookManager::SetHook(
                "TrainingResetDirection",
                static_cast<DWORD>(base + ADDR_TrainingResetDirectionToMenu),
                6,
                TrainingResetDirection_Hook);
        if (trainingResetDirection_JmpBack == 0)
        {
                LOG(0, "FAILED TO INSTALL TrainingResetDirection HOOK\n");
                return false;
        }
        LOG(1, "TrainingResetDirection hook installed OK\n");
        return true;
}
