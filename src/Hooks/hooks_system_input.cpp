#include "hooks_system_input.h"

#include "RankedAutomationHarness.h"

#include "HookManager.h"
#include "Core/ControllerOverrideManager.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/RuntimePlatform.h"
#include "Core/utils.h"
#include "Game/gamestates.h"

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

bool PollTrainingResetPressed(bool* outUpHeld)
{
        constexpr uint32_t kActionResetPositions = 0x08000000; // just-pressed word bit
        constexpr uint32_t kActionUp = 0x00000001;             // held word bit

        bool pressed = false;
        bool upHeld = false;

        for (size_t i = 0; i < kProbeMaxControllers; ++i)
        {
                const uint8_t* const ptr = static_cast<const uint8_t*>(s_probeControllers[i]);
                if (ptr == nullptr)
                {
                        break;
                }
                if (IsBadReadPtr(ptr + 0x28, 0x0C))
                {
                        continue;
                }

                const uint32_t edges = *reinterpret_cast<const uint32_t*>(ptr + 0x28);
                const uint32_t held = *reinterpret_cast<const uint32_t*>(ptr + 0x30);
                if (edges & kActionResetPositions)
                {
                        pressed = true;
                }
                if (held & kActionUp)
                {
                        upHeld = true;
                }
        }

        if (outUpHeld != nullptr)
        {
                *outUpHeld = upHeld;
        }
        return pressed;
}

