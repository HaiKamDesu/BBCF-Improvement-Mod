#include "hooks_battle_input.h"
#include "HookManager.h"
#include "Core/logger.h"

static DWORD battleInputWrite_JmpBack = 0;

// Shared state: true = P1 pressed "2", so P2 should be forced to "6".
static bool shouldForceP2Forward = false;

void __declspec(naked) BattleInputWrite_Hook()
{
    __asm {
        // AX contains original input
        // EBX = player index
        // ESI = address to write into

        // Only use volatile registers that the original code clobbers anyway
        cmp ebx, 0        // P1?
        jne checkP2

        // --- P1 logic ---
        // if P1 input == 2 → activate trigger
        cmp ax, 2
        jne p1_not_down
        mov byte ptr[shouldForceP2Forward], 1
        jmp continueWrite

        p1_not_down :
        mov byte ptr[shouldForceP2Forward], 0
            jmp continueWrite

            checkP2 :
        cmp ebx, 1        // P2?
            jne continueWrite

            cmp byte ptr[shouldForceP2Forward], 1
            jne continueWrite

            mov ax, 6         // Force P2 input to 6

            continueWrite:
        // ORIGINAL instructions:
        movzx edi, ax
            mov[esi], di

            jmp battleInputWrite_JmpBack
    }
}


bool Hook_BattleInput()
{
    // Pattern from the confirmed CE hook:
    // 0F B7 F8 66 89 3E E9 ?? ?? ?? ??
    battleInputWrite_JmpBack = HookManager::SetHook(
        "BattleInputWrite",
        "\x0F\xB7\xF8\x66\x89\x3E\xE9",
        "xxxxxxx",
        6,
        &BattleInputWrite_Hook
    );

    if (battleInputWrite_JmpBack == 0)
    {
        LOG(0, "FAILED TO INSTALL BattleInputWrite HOOK\n");
        return false;
    }

    LOG(1, "BattleInputWrite hook installed OK\n");
    return true;
}