// Typed signatures of BBCF.exe functions, applied to the decompile archive by
// docs/Research/ghidra_scripts/ApplyRepoTypes.py (run through tools/decomp/sync_archive.sh).
// This is documentation, not compiled: the mod never includes it.
//
// One declaration per line, prefixed by the function's address:
//   /*0x0057EA70*/ int __cdecl Hit_PiercesInvul(CharData* defender, CharData* attacker);
//
// Only add a line once the convention is checked in the assembly (ret N, which registers carry
// arguments). The name must match the function's ledger row or ADDR_ name.
// - __thiscall: the first parameter is ECX, the rest are on the stack, callee pops them.
// - __fastcall: the first two are ECX and EDX.
// - __stdcall: all on the stack, callee pops. __cdecl: all on the stack, caller pops.
// Types: void, char, int8_t..uint64_t, int, uint32_t, float, double, bool, pointers, and every
// struct or enum in structs.json (generated from the mod's headers by gen_struct_layouts.py).
// A function with no line here still gets parameter names and its convention from its ledger
// note when the note says e.g. "thiscall(kbd, gameKey), ret 4".

// ---- Hit check (docs/Decompilation/Invulnerability.md) ------------------------------------
/*0x00573E50*/ void __thiscall Hit_InvulGuardCheck(CharData* attacker, CharData* defender, int flag, uint32_t* outFlags, int* outValue);
/*0x0057EA70*/ int __cdecl Hit_PiercesInvul(CharData* defender, CharData* attacker);

// ---- Character state and frame ------------------------------------------------------------
/*0x005975C0*/ void __thiscall Char_EnterState(CharData* self, void* state);
/*0x005CC490*/ void __fastcall Char_FrameUpdate(CharData* self);

// ---- Script interpreter --------------------------------------------------------------------
/*0x00582BB0*/ int __thiscall Scr_ExecCommand(CharData* self, uint32_t* command);
/*0x0058B250*/ int __cdecl Scr_CommandSize(uint32_t opcode);
/*0x0058B3E0*/ uint32_t* __cdecl Scr_SkipBlock(uint32_t* command);

// Script command handlers (vtable 0x9509E4). `on` is the command's 0/1 argument.
/*0x00597110*/ void __thiscall ScrOp22006_InvulBit1(CharData* self, int on);
/*0x00597010*/ void __thiscall ScrOp22007_SetInvincible(CharData* self, int on);
/*0x005973B0*/ void __thiscall ScrOp22008_InvulTimer(CharData* self, int frames);
/*0x00597130*/ void __thiscall ScrOp22009_InvulBit4(CharData* self, int on);
/*0x00597030*/ void __thiscall ScrOp22019_InvulAttributes(CharData* self, int head, int body, int foot, int projectile, int throwInvul);
/*0x005970E0*/ void __thiscall ScrOp22026_BurstProof(CharData* self, int on);
/*0x0058E850*/ void __thiscall ScrOp11026_BurstAttack(CharData* self, char on);

// ---- Input buffer --------------------------------------------------------------------------
/*0x005CA700*/ void __fastcall Input_BuildBuffer(CharData* self);
/*0x005C5AA0*/ int __fastcall Input_ReplayMoveInputs(CharData* self);
