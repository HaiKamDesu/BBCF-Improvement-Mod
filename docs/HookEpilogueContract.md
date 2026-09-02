# The JMP-patch contract

Every naked-function JMP hook in `src/Hooks` splices our code into the middle of a game
function that is already mid-execution. That function has invariants we inherit whether we
think about them or not. Two shipped crashes came from breaking one of them, both with the
same shape: the mod worked fine for almost everyone, then destroyed the process for the one
reporter whose environment happened to take the unusual path.

Neither was caught by a build, and neither produced a useful log line. Both were only found
from a crash dump. So the checklist below is not style advice - it is the part of hook work
that has no other safety net.

## Before you place a hook

### 1. Nothing may branch into the bytes you overwrite

A 5-byte `JMP` covers `site .. site+4`. If any instruction anywhere in the module jumps to
`site+1 .. site+4`, that path lands in the middle of our jump and executes the tail of the
displacement as code. The process dies on a CRT fastfail (`0xC0000409`), which **bypasses
the unhandled-exception filter** - so there is no crash bundle and no `BBCF_FIX STOP`, just
a log that stops mid-sentence.

Check it, every time, against the disassembly:

```bash
# any branch landing inside a 5-byte patch at 0055A333?
grep -nE "0055A33[4-7]$" tools/bbcf_disasm_ascii.txt
```

Real case: PR #37 put the battle-input hook at `0x0055A333`. Eleven branches in that same
input loop jump to `0x0055A336`, three bytes into the patch. Entering training killed the
process instantly. Fixed in `5ec6dc3` by moving five bytes forward to `0x0055A338`, where
nothing branches into the range.

### 2. Know what the prologue already did to the stack

Write down where your site sits relative to the function's prologue. If the site is *after*
`push ebp` / `push ebx` / `push esi` / `push edi`, those saved registers are sitting between
your `esp` and the return address, and any `ret` you execute will pop a saved register
instead of the return address.

### 3. Replacing a `call` means performing it

If the overwritten bytes are a `call rel32`, resolve the absolute target with
`HookManager::RegisterHook` *before* patching (the rel32 is only readable while the original
instruction is intact), keep it, and call through it from the hook before jumping back. See
`Hook_BattleInput` in `src/Hooks/hooks_battle_input.cpp` for the pattern.

## If your hook returns instead of jumping back

A hook that ends in `jmp [SomethingJmpBackAddr]` is fine: the game continues, its own
epilogue runs, and the stack is its problem.

A hook that ends in `ret` is claiming to *be* the game's epilogue. Then you own the unwind,
and you must undo the prologue's pushes in reverse order before the `ret`, matching what the
game's own epilogues do.

Read the real epilogue out of the disassembly and copy it. For `0x0044F350`:

```
0044F387: pop edi
0044F388: pop esi
0044F389: xor eax,eax
0044F38B: pop ebx
0044F38C: pop ebp
0044F38D: ret 0Ch
```

so a hook spliced in after that prologue must do the same four pops:

```asm
EXIT:
    popad           ; balance our own pushad first
    mov eax, 1      ; return value (pops below don't touch eax)
    pop edi
    pop esi
    pop ebx
    pop ebp
    retn 0Ch
```

Also confirm the `ret` immediate matches the callee's calling convention - `ret 0Ch` for a
thiscall with three stack args, and so on. Getting the immediate right but skipping the pops
still corrupts control flow.

## Case study: `PassMsgToImGui`, v8.2 - v8.5

`src/Hooks/hooks_bbcf.cpp` hooks BBCF's window-message dispatcher at `0x0044F359`:

```
0044F350 push ebp
0044F351 mov  ebp,esp
0044F353 push ebx
0044F354 push esi
0044F355 mov  esi,[ebp+8]
0044F358 push edi
0044F359 <-- our 5-byte JMP (overwrote "mov edi,[ebp+0Ch]; mov ebx,ecx")
```

When ImGui claimed a message, the hook's exit path was:

```asm
popad
mov eax, 1
retn 0Ch        ; four saved registers still on the stack
```

The `ret` popped the saved `edi` as its return address. That `edi` belongs to the *outer*
WndProc frame at `0x0044F2D0`, where `mov edi,[ebp+10h]` loads **wParam** - so the mod
jumped to whatever wParam happened to be.

**Why it stayed hidden.** The path is only reached when
`ImGui_ImplWin32_WndProcHandler` returns 1. Under ImGui 1.53 it never returned 1 for any
message BBCF sees, so the bug was unreachable dead code. `3186cca` (*Update Dear ImGui from
1.53 to 1.92.9b*, 2026-08-02, first shipped in **v8.2**) added:

```cpp
case WM_IME_COMPOSITION:
{
    LRESULT result = ::DefWindowProcW(hwnd, msg, wParam, lParam);
    return (lParam & GCS_RESULTSTR) ? 1 : result;
}
```

Upgrading a vendored dependency made eight-year-old dead code live. Anyone with an IME
active now crashed on every committed composition; players with no IME never generated the
message and saw nothing wrong. `WM_SETCURSOR` reaches the same exit and was a second live
trigger on any machine.

The dump matched exactly: `eip=0x77`, `edi=0x77` (wParam `'w'`), `esi=0x10f`
(`WM_IME_COMPOSITION`), `esp=0x01f3eec4` holding the untouched real return address
`0x005cf32c`, `ebp` still `0x01f3eec0`.

**Lessons worth carrying:**

- An unreachable-but-wrong hook epilogue is a landmine with an unknown timer. Vendor updates
  can arm it years later, so fix the epilogue rather than reasoning about whether the path is
  currently reachable.
- "Works for me and 99% of users" is not evidence for hook correctness. Environment-specific
  paths (IME, locale, remote desktop, compatibility shims) are exactly where these fire.
- Bisecting reporters are valuable. "Only v8.1 and a few commits after it work for me"
  pinned the ImGui upgrade before anyone had looked at a dump.

## Triaging one of these

Start with `crash_context.txt` from the reporter's bundle. Since 8.5.1 it carries the
module+offset of the faulting address, the access type and target, the register set, and a
walked stack - which is enough to classify the failure without asking anyone to upload a
900 MB dump. Two lines in it decide the direction:

- `Exception address: 0x00000077  <unmapped>` plus the "control flow was transferred to a
  bad address" note - the instruction pointer is nowhere, so this is a hook, not a bad
  pointer. Go to the steps below.
- `Access type: read/write at 0x...` with the exception address inside a module - ordinary
  bad-pointer code at a known offset. Normal debugging.

If the dump is warranted after all:

1. `.ecxr; r; k 60` - a frame reading `0xNN` with `WARNING: Frame IP not in any known module`
   above a `BBCF+0x...` return address means control flow, not a null deref.
2. `u <game function> L30` on the **runtime** address. This is the step that finds the hook:
   a `jmp dinput8+0x...` in the middle of game code is ours, and the runtime bytes also
   expose any mismatch against the static disassembly.
3. `u <mod hook address> L40` to read the hook's real epilogue.
4. Reconcile `esp`/`ebp` against the prologue's pushes by hand. `esp` above `ebp` means the
   frame was already unwound, so a `ret` ate a saved register. In the case study
   `ret 0Ch` moved `esp` from `ebp-12` to `ebp+4`: four bytes popped as the return address
   plus twelve of callee arg cleanup.

A missing `BBCF_FIX STOP` with **no bundle at all** means a fastfail (`0xC0000409`), which
the handler cannot see - look for a hook site with an interior branch target. A
`0xC0000005` bundle means the handler ran, so the context file exists; read it first.

## Keeping the log worth reading

Two habits, both learned from the same 134 KB reporter log in which the useful content was
about twenty lines:

**A diagnostic must log on change, never on a timer.** A line that repeats because a second
elapsed carries nothing the previous line did not. `SteadyStateDiagnostic` in
`RankedListConnectionFilter.cpp` is the pattern to copy: it de-duplicates, counts what it
collapsed, and reports how long a value stood plus why sampling stopped. An idle screen that
used to cost one line per second forever now costs one line, and states its own duration.
Where a field genuinely moves every frame (measured RTT), give it a rate floor and let it
hold the newest value rather than dropping it.

**A diagnostic that touches game memory must be gated on being on the right screen.** The
ranked-list diagnostics called through a game vtable every frame of every screen, including
training matches where no ranked list exists or ever will. Gate on the cheap plain-memory
state reads first, then do the expensive or risky work only where it means something. This
is not just log hygiene - an unnecessary indirect call into game code is an unnecessary
crash surface, on a stale pointer nobody was thinking about.
