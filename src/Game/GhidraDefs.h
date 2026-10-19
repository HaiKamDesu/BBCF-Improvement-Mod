#pragma once
#include <winsock.h>
typedef unsigned char   undefined;

//typedef unsigned long long    GUID;
//typedef pointer32 ImageBaseOffset32;

//typedef unsigned char    bool;
typedef unsigned char    byte;
typedef unsigned int    dword;
//float10
//typedef long double    longdouble;
typedef long long    longlong;
typedef unsigned char    uchar;
typedef unsigned int    uint;
typedef unsigned long    ulong;
typedef unsigned long long    ulonglong;
typedef unsigned char    undefined1;
typedef unsigned short    undefined2;
typedef unsigned int    undefined4;
typedef unsigned long long    undefined5;
typedef unsigned long long    undefined6;
typedef unsigned long long    undefined8;
typedef unsigned short    ushort;
typedef unsigned short    wchar16;
//typedef short    wchar_t;
typedef unsigned short    word;
typedef struct SteamPeer2PeerBackend SteamPeer2PeerBackend, * PSteamPeer2PeerBackend;

typedef struct GGPOSessionCallbacks GGPOSessionCallbacks, * PGGPOSessionCallbacks;

typedef struct Poll Poll, * PPoll;

typedef struct Sync Sync, * PSync;

typedef struct Udp Udp, * PUdp;

typedef uint UINT_PTR;

typedef UINT_PTR SOCKET;

typedef struct SteamUdpProtocol SteamUdpProtocol, * PSteamUdpProtocol;

typedef int BOOL;

typedef struct UdpMsg__connect_status UdpMsg__connect_status, * PUdpMsg__connect_status;

typedef struct Sync__SavedState Sync__SavedState, * PSync__SavedState;

typedef struct Sync__Config Sync__Config, * PSync__Config;

typedef struct InputQueue InputQueue, * PInputQueue;

typedef struct sockaddr_in sockaddr_in, * Psockaddr_in;

typedef ushort UINT16;

typedef struct SteamUdpProtocol___oo_packet SteamUdpProtocol___oo_packet, * PSteamUdpProtocol___oo_packet;

typedef struct RingBuffer_QueueEntry_256_ RingBuffer_QueueEntry_256_ , * PRingBuffer_QueueEntry_256_;

typedef union SteamUdpProtocol___state SteamUdpProtocol___state, * PSteamUdpProtocol___state;

typedef struct GameInput GameInput, * PGameInput;

typedef ushort uint16_t;

typedef struct TimeSync TimeSync, * PTimeSync;

typedef struct Sync__SavedFrame Sync__SavedFrame, * PSync__SavedFrame;

typedef ushort u_short;

typedef struct in_addr * Pin_addr;

typedef struct UdpMsg UdpMsg, * PUdpMsg;

typedef struct SteamUdpProtocol__QueueEntry SteamUdpProtocol__QueueEntry, * PSteamUdpProtocol__QueueEntry;

typedef struct SteamUdpProtocol__Union_state_sync SteamUdpProtocol__Union_state_sync, * PSteamUdpProtocol__Union_state_sync;

typedef struct SteamUdpProtocol__Union_state_running SteamUdpProtocol__Union_state_running, * PSteamUdpProtocol__Union_state_running;

typedef union _union_1226 _union_1226, * P_union_1226;

typedef struct _struct_1227 _struct_1227, * P_struct_1227;

typedef struct _struct_1228 _struct_1228, * P_struct_1228;

typedef ulong ULONG;

typedef uchar UCHAR;

typedef ushort USHORT;


typedef struct SnapshotManager SnapshotManager, * PSnapshotManager;

//typedef struct 20bdc_sized_plaaceholder 20bdc_sized_plaaceholder, * P20bdc_sized_plaaceholder;

typedef struct SnapshotManager__struct SnapshotManager__struct, * PSnapshotManager__struct;

struct static_DAT_of_PTR_on_load_4{
    byte padding_0x0[12];
    undefined* ptr_OBJ_with_seemingly_list_managers_1;
    undefined padding_0x10[16];
    undefined* ptr_OBJ_with_seemingly_list_managers_2;
    undefined padding_0x24[8];
    uint size_or_counter_for_inline_managers_1;
    struct BATTLE_CObjectManager*ptr_BATTLE_CObjectManager_plus_0x4;
    undefined* ptr_BATTLE_CScreenManager;
    undefined* ptr_GAME_CEff3DInstHndlManager;
    undefined* ptr_AA_CRandomManager;
    undefined* ptr_battle_stat__StatBattleTemp;
    undefined* ptr_BATTLE_CBGManager;
    byte padding_0x48[104];
    int size_or_counter_for_inline_managers_2;
    undefined* ptr_BATTLE_CObjectManager_static;
    undefined* ptr_AA_CParticleManager;
    undefined* ptr_BATTLE_CScreenManager_differen;
    undefined* ptr_AA_CCameraManager;
    undefined* ptr_AA_CRandomManager_different_;
    undefined* ptr_battle_stat__StatBattleTmp_different_;
    undefined* ptr_BATTLE_CBGManager_different_;
    undefined* ptr_game_Stat_PCoinManager;
    undefined* ptr_AA_CModelInstanceManager;
    undefined* ptr_CBattleReplayDataManager;
    undefined* ptr_GAME_CEff3DInstHndlManager_different_;
    undefined* ptr_GAME_CEventManager;
    undefined* ptr_BG_EffectManager;
    undefined* ptr_GAME_CFadeTaskManager;
    undefined* ptr_GAME_CETCManager;
    byte padding_0xf0[128];
    struct SnapshotManager* ptr_snapshot_manager_mine;
    void* field31_0x174;
    void* field32_0x178;
    void* field33_0x17c;
    void* field34_0x180;
    char field35_0x184;
    undefined field36_0x185[335];
};

struct SnapshotManager__struct {
    int _framecount;
    undefined* _ptr_buf_saved_frame;
    int field2_0x8;
    int field3_0xc;
    undefined* field4_0x10;
    undefined* _ptr_buf_save_frame_1_maybe;
    undefined* _ptr_buf_save_frame_1_plus_some_offset;
    undefined* _ptr_BB_CEventInstance_0;
    undefined field8_0x20[12];
    undefined* _ptr_BB_CEventInstance_1;
    undefined* _ptr_BB_CEventInstance_1_plus_some_offset;
    undefined field11_0x34[12];
    undefined* field12_0x40;
    int field13_0x44;
};

struct sized_20bdc_plaaceholder{
    undefined field0_0x0[134108];
};

struct BATTLE_CObjectManager{
    byte field0_0x0[696];
    struct sized_20bdc_plaaceholder field1_0x2b8[3];
    undefined field2_0x6264c[312];
    undefined* ptr_to_smth_with_entity_list_1;
    undefined* ptr_to_smth_with_entity_list_2;
    undefined* ptr_to_smth_with_entity_list_3;
    undefined* array_ptr_to_OBJ_CBase[250];
    undefined* ptr_to_p1_OBJ_CCharBase;
    undefined field8_0x62b7c[135880];
    undefined* field9_0x83e44;
};

struct SnapshotManager {
    struct SnapshotManager__struct _saved_states_related_struct[10];
    int _counter_of_some_sort;
    undefined* field2_0x2d4;
    undefined* _base_of_snapshot_mem;
};


struct _struct_1227 {
    UCHAR s_b1;
    UCHAR s_b2;
    UCHAR s_b3;
    UCHAR s_b4;
};

struct _struct_1228 {
    USHORT s_w1;
    USHORT s_w2;
};

union _union_1226 {
    struct _struct_1227 S_un_b;
    struct _struct_1228 S_un_w;
    ULONG S_addr;
};

//struct in_addr {
//    union _union_1226 S_un;
//};

struct GameInput { /* the bits estimate needs to be checked */
    int frame;
    int size;
    char bits[18];
};

struct TimeSync { /* must double check GameInput size, might be 2 bytes short */
    undefined* field0_0x0;
    int _local[40];
    int _remote[40];
    struct GameInput _last_inputs[10];
    undefined field4_0x248;
    undefined field5_0x249;
    undefined field6_0x24a;
    undefined field7_0x24b;
    undefined field8_0x24c;
    undefined field9_0x24d;
    undefined field10_0x24e;
    undefined field11_0x24f;
    undefined field12_0x250;
    undefined field13_0x251;
    undefined field14_0x252;
    undefined field15_0x253;
    undefined field16_0x254;
    undefined field17_0x255;
    undefined field18_0x256;
    undefined field19_0x257;
    undefined field20_0x258;
    undefined field21_0x259;
    undefined field22_0x25a;
    undefined field23_0x25b;
    int _next_prediction;
};

struct Sync__SavedFrame {
    byte* buf;
    int cbuf;
    int frame;
    int checksum;
};

struct Sync__SavedState {
    struct Sync__SavedFrame frames[10];
    int head;
};

struct GGPOSessionCallbacks {
    //undefined* begin_game;
    int (*begin_game)();//its an empty function for now, will point to the same as log
    //undefined* save_game_state;
    int (*save_game_state)(unsigned char**, int*, int*); //aside from param_1 being pbuf im not sure of the other parameters, param_2 is either counter_of_some_sort or size of buf and param_3 i think its checksum, can be left as an adress to 0
    //undefined* load_game_state;
    int (*load_game_state)(unsigned char*); // param_1 is the adress of the buf to load
    //undefined* maybe_log_game_state;
    int (*maybe_log_game_state)();//its an empty function for now, will point to the same as log
    //undefined* free_buffer;
    int (*free_buffer)(unsigned char*); // param_1 is the adress of the buf to free?
    //undefined* advance_frame;
    int (*advance_frame)();
    //undefined* on_event;
    int (*on_event)(unsigned char*); // idk what param_1 is
};

struct Sync__Config {
    struct GGPOSessionCallbacks callbacks;
    int maybe_num_prediction_frames;
    int maybe_num_players;
    int maybe_input_size;
};

struct Sync { /* prob need to look at Sync__SavedStated again to fix the offset hardcoded */
    void** vftable;
    struct GGPOSessionCallbacks _callbacks;
    struct Sync__SavedState _savedstate;
    struct Sync__Config _config;
    BOOL _rollingback;
    int _last_confirmed_frame;
    int _framecount;
    int _max_prediction_frames;
    struct InputQueue* _input_queues;
    undefined field9_0x100[1036];
    struct UdpMsg__connect_status* _local_connect_status;
};

struct SteamUdpProtocol__Union_state_running {
    uint last_quality_report_time;
    uint last_network_stats_interval;
    uint last_input_packet_recv_time;
};

//struct sockaddr_in {
//    short sin_family;
//    u_short sin_port;
//    struct in_addr sin_addr;
 //   char sin_zero[8];
//};

struct SteamUdpProtocol___oo_packet {
    int send_time;
    struct sockaddr_in dest_addr;
    struct UdpMsg* msg;
};

struct SteamUdpProtocol__Union_state_sync {
    uint roundtrips_remaining;
    uint random;
};

union SteamUdpProtocol___state {
    struct SteamUdpProtocol__Union_state_sync sync;
    struct SteamUdpProtocol__Union_state_running running;
};

struct SteamUdpProtocol__QueueEntry {
    int queue_time;
    struct sockaddr_in dest_addr;
    struct UdpMsg* msg;
};

struct RingBuffer_QueueEntry_256_ {
    struct SteamUdpProtocol__QueueEntry _elements[256];
    int _tail;
    int _size;
    int _head;
};

struct SteamUdpProtocol {
    void** vftable;
    struct Udp* _udp;
    struct sockaddr_in _peer_addr;
    UINT16 _magic_number;
    int _queue;
    UINT16 _remote_magic_number;
    BOOL _connected;
    int _send_latency;
    int _oop_percent;
    struct SteamUdpProtocol___oo_packet _oo_packet;
    struct RingBuffer_QueueEntry_256_ _send_queue;
    undefined* field11_0x1850;
    int _packets_sent;
    int _bytes_sent;
    int _kbps_sent;
    int _stats_start_time;
    undefined field16_0x1864[24];
    union SteamUdpProtocol___state _state;
    undefined field18_0x1888[7188];
    struct GameInput _last_received_input;
    undefined field20_0x34b6;
    undefined field21_0x34b7;
    struct GameInput _last_sent_input;
    undefined field23_0x34d2;
    undefined field24_0x34d3;
    struct GameInput _last_acked_input;
    undefined field26_0x34ee;
    undefined field27_0x34ef;
    uint _last_send_time;
    uint _last_recv_time;
    uint _shutdown_timeout;
    uint _diconnect_event_sent;
    uint _disconnect_timeout;
    uint _disconnect_notify_start;
    bool _disconnect_notify_sent;
    undefined field35_0x3509;
    undefined field36_0x350a;
    undefined field37_0x350b;
    uint16_t _next_send_seq;
    uint16_t _next_recv_seq;
    struct TimeSync _timesync;
    undefined field41_0x3770[8208];
};

struct UdpMsg__connect_status {
    int disconnected : 1;
    int last_frame : 31;
};

struct Udp {
    SOCKET(*SOCKET)(int, int, int);
    struct GGPOSessionCallbacks* _callbacks;
    struct Poll* _poll;
};

struct Poll {
    undefined field0_0x0[1296];
};

struct SteamPeer2PeerBackend { /* PlaceHolder Class Structure */
    void** vftable;
    undefined* field1_0x4;
    undefined* field2_0x8;
    struct GGPOSessionCallbacks _callbacks;
    undefined* field4_0x28;
    struct Poll _poll;
    struct Sync _sync;
    struct Udp _udp;
    struct SteamUdpProtocol* _endpoints;
    int idk_what;
    struct SteamUdpProtocol _spectators[6];
    int _num_spectators;
    int _input_size;
    BOOL _synchronizing;
    int _num_players;
    int _next_recommended_sleep;
    int _next_spectator_frame;
    int _disconnect_timeout;
    int _disconnect_notify_start;
    struct UdpMsg__connect_status _local_connect_status[2];
    undefined field20_0x21788;
    undefined field21_0x21789;
    undefined field22_0x2178a;
    undefined field23_0x2178b;
    undefined field24_0x2178c;
    undefined field25_0x2178d;
    undefined field26_0x2178e;
    undefined field27_0x2178f;
};

struct UdpMsg {
    undefined field0_0x0[4128];
};

struct InputQueue {
    int _id;
    uint _head;
    int _tail;
    int _length;
    int _first_frame;
    int _last_user_added_frame;
    int _last_added_frame;
    int _first_incorrect_frame;
    int _last_frame_requested;
    int _frame_delay;
    struct GameInput _inputs[138];
    struct GameInput _prediction;
    undefined field12_0xe46;
    undefined field13_0xe47;
};


// ---------------------------------------------------------------------------
// D-Code / per-room-member profile fetch state machine (docs/Research/
// DCodeNetworkStallBug.md, DCodeBug8/9GhidraReport.txt). Addresses are
// base-relative RVAs unless noted (Ghidra VAs assume image base 0x00400000).
// row = netUserData(base+0x8AD0C0) + 0x2326C + slot*0x68A4; the row's first
// 0x6800 bytes are the member's profile blob (0x28 per-character 0x180-stride
// ranked entries at +0xD4). subobj = *(row+0x68A0); fetch state at subobj+0xCC:
// 0 idle, 1 queued, 2 in flight, 3 ready, 6 rejected (permanent wedge).
// ---------------------------------------------------------------------------
// Ghidra FUN_0049D440 - per-frame pump, ticks all 6 rows (caller FUN_004A6F70)
static constexpr uintptr_t ADDR_DCodeFetchPump = 0x0009D440;
// Ghidra FUN_004A25C0 - per-slot fetch tick (__fastcall, ecx=row); hooked by
// the DCodeFetchTick JMP patch in hooks_bbcf.cpp
static constexpr uintptr_t ADDR_DCodeFetchTick = 0x000A25C0;
// Ghidra FUN_004A1DD0 - payload validator (size 0x6800 + checksum), sets state 3
static constexpr uintptr_t ADDR_DCodePayloadValidator = 0x000A1DD0;
// Ghidra FUN_0040DF10 - 16-bit ones'-complement checksum, valid iff sum==0xFFFF;
// shared with save-data code (FUN_006C4990/FUN_004BB080)
static constexpr uintptr_t ADDR_ProfileChecksum16 = 0x0000DF10;
// Ghidra FUN_004A0D50 - blob reset (memset 0x6800 + reinit) run before state:=6
static constexpr uintptr_t ADDR_DCodeBlobReset = 0x000A0D50;
// Ghidra FUN_004B8F70 - lazy singleton getter for GAMESTEAM_COnlineStorageTransfer
// (ctor FUN_004717C0, 0x1C bytes) - the transport behind the profile exchange
static constexpr uintptr_t ADDR_OnlineStorageTransferSingleton = 0x000B8F70;

// ---------------------------------------------------------------------------
// Logical input action system (training-reset investigation, 2026-07-19).
// Addresses are base-relative RVAs (Ghidra/dumpbin VAs assume image base
// 0x00400000). Verified against tools/bbcf_disasm_ascii.txt.
//
// CORRECTED 2026-10-07 (docs/Decompilation/Controllers.md): these words live in the
// CONTROLLER TASK (GAME_KeyControler, size 0x58), not in an AA_CInput device. The
// "controller pointer" the SystemInputWrite hook sees in ESI is a KeyControler; its
// +0x18 is a device REGISTRY INDEX, not a device pointer. One bit per keyconfig action
// index (1 << actionIndex, actionCount at +0x38 = 0x1C = 28, so bits 28-31 never set):
//   ctrl+0x28 = actions JUST PRESSED this tick (edge vs. the previous effective word)
//   ctrl+0x2C = actions RELEASED this tick
//   ctrl+0x30 = PHYSICAL held word (before the +0x4C/+0x50 one-tick override)
//   [ctrl+0x20]+0x0C = EFFECTIVE held word (history node; what ActionHeld reads)
// Built by FUN_004963F0 (per-action loop calling vtbl+8 with the action index).
// The SystemInputWrite hook at base+0x96408 sits at the zero-init of +0x30; writing a
// word there MERGES with the device (the loop still ORs device bits in, and the
// effective word is computed from the device), which is why the local input system
// replaces FUN_004963F0 for its controllers instead (src/Input/GameInputBridge.cpp).
// ---------------------------------------------------------------------------
// Ghidra FUN_004963F0 - per-frame device action-word build (thiscall, ecx=device)
static constexpr uintptr_t ADDR_InputDevice_BuildActionWords = 0x000963F0;
// Ghidra FUN_004968E0 - packs device+0x30 into the 16-bit battle input word
// (digit 1-9 from bits0-3; A=bit14-mask 0x3404000, B=0x3C08000, C=0x3802000,
// D=0x2001000, taunt=0x20000, special=0x80000). The packed battle word can
// NEVER carry more than 0x3FF => training reset does not travel through the
// BattleInputWrite word.
static constexpr uintptr_t ADDR_InputDevice_PackBattleWord = 0x000968E0;
// Action-ID -> action-bit lookup table (31 dwords, .data; absolute VA 0x9DE0B0,
// re-read and transcribed in full 2026-09-12). ActionIDs are the small ints
// pushed to the check wrappers below (5=menu confirm 0x4000000, 6=reset
// positions 0x8000000, 7-10=up/down/left/right 1/4/8/2, 11=B 0x8000,
// (8=down/9=left/10=right confirmed 2026-09-27 by FUN_006D2D40, the training
// reset's direction rule: it treats ID 8 as centre and mirrors IDs 9/10 for P2 side),
// 12=D 0x1000, 13=C 0x2000, 14=A 0x4000, 16=taunt 0x20000, 18=special 0x80000,
// replay-shortcut ids 0x0B-0x14 map to 0x10000..0x200000 range; ids 21-24 are a
// second direction group at bits 8-11, unbound on keyboard).
//
// CORRECTION (2026-09-12): ids 8 and 9 were previously labelled left/down here.
// They are DOWN(0x4) and LEFT(0x8). The action INDEX order is 0=Up, 1=Right,
// 2=Down, 3=Left, established two independent ways: the keyboard default table
// FUN_00469AB0 binds indices 0/1/2/3 to W/D/S/A, and the hardcoded fallbacks in
// FUN_00469750 (below) attach VK_UP/VK_RIGHT/VK_DOWN/VK_LEFT to the same four.
// So direction masks are Up=0x1, Right=0x2, Down=0x4, Left=0x8 - which is what
// ControllerOverrideManager::EncodeDirections() already does; this comment was
// the thing that was wrong, not the code.
static constexpr uintptr_t ADDR_ActionIdMaskTable = 0x005DE0B0;
// Check wrappers (thiscall, ecx = per-controller wrapper from FUN_0047E7B0(id),
// which fans out to both devices of that controller):
// FUN_004641B0(actionId) = just-pressed (maskTable[id] & device+0x28)
static constexpr uintptr_t ADDR_ControllerWrapper_ActionJustPressed = 0x000641B0;
// FUN_00464170(actionId) = HELD (effective word & mask). CORRECTED 2026-10-07: it is
// not a repeat check; the menu repeat check is FUN_004641F0 (just pressed, or held
// >= 9 frames and every 4th frame). Name kept for existing callers.
static constexpr uintptr_t ADDR_ControllerWrapper_ActionRepeatPressed = 0x00064170;
static constexpr uintptr_t ADDR_ControllerWrapper_ActionHeld = 0x00064170;
static constexpr uintptr_t ADDR_ControllerWrapper_ActionRepeat = 0x000641F0;
// FUN_004640E0(rawMask) = just-pressed by raw bitmask (device+0x28 & mask)
static constexpr uintptr_t ADDR_ControllerWrapper_MaskJustPressed = 0x000640E0;
// FUN_0047E7B0(controllerId) cdecl - controller wrapper by id;
// engine+0x25F0 = active controller id, engine+0x25F4/0x25F8 = P1/P2 ids
// (engine object from FUN_0047E860, the same object whose +0x108 is gameMode)
static constexpr uintptr_t ADDR_GetControllerWrapperById = 0x0007E7B0;

// Training human side (2026-09-27): dword at RVA 0x891A38 (VA 0xC91A38) = index (0/1) of
// the side the training player controls. Indexes the per-side training tables at
// 0xC904D4 + side * 0x710 (see 0x4802EA), read by the training menu/reset code
// (0x47DD85, 0x47E03D, 0x47EA2E, ...). The replay takeover writes it to hand a side to
// the human. Consumed through Game/TrainingSide.h.
static constexpr uintptr_t ADDR_TrainingHumanSide = 0x00891A38;

// CORRECTION 2026-09-27: the three "restart variants" below are NOT the training
// reset. Tadatys' symbol dump names them OBJ_CBase::bbcmd_30014_EndTimestopEffect
// (0x18F390), bbcmd_30015_StopTimestopEffect (0x18F4D0) and
// bbcmd_30013_TimestopEffect (0x18F610); they are only referenced from the
// script-command vtables (.rdata 0x951A8C.. / 0x95769C..) and the task at
// 0xEC2F98 they restart is the timestop effect's. Kept so old notes still
// resolve; do not hook them for reset work. The real reset path is below.
static constexpr uintptr_t ADDR_TimestopEffect_End = 0x0018F390;   // was ADDR_RoundRestart_VariantA
static constexpr uintptr_t ADDR_TimestopEffect_Stop = 0x0018F4D0;  // was ADDR_RoundRestart_VariantB
static constexpr uintptr_t ADDR_TimestopEffect_Begin = 0x0018F610; // was ADDR_RoundRestart_VariantC

// Training "reset positions" (RE 2026-09-27, docs/Research/ResetLabOverride.md).
// MatchInfo::update_Fight (FUN_0055F780) -> training branch FUN_00560FC0 ->
// FUN_0055EF40 "reset requested?": for P1 and P2 it tests the reset action bit
// (0x8000000, just-pressed) on that player's controller via FUN_00496620 and,
// if any pressed, calls FUN_006D2D40 and then starts the reset fade.
// FUN_006D2D40 is the WHOLE held-direction rule: it reads action IDs 9/10/8
// (left/right/down; 9 and 10 swapped when localPlayer(engine+0x1688) is P2 XOR
// sides are swapped (FUN_0047E790)) with FUN_004966A0 and writes the training
// menu item "TRI_ResetPosition" (= 2 for ID 9, 1 for ID 10, 0 = centre for ID
// 8; nothing when no direction is held, so the previous value is sticky) via
// FUN_006CD5C0(nameHandle, value) -> FUN_006CD700. Up (ID 7) is never read.
static constexpr uintptr_t ADDR_TrainingResetRequested = 0x0015EF40;       // FUN_0055EF40
static constexpr uintptr_t ADDR_TrainingResetDirectionToMenu = 0x002D2D40; // FUN_006D2D40, hooked at entry (6 bytes)
// How FUN_006D2D40 reads a direction (RE 2026-09-27, ResetDirectionSourceGhidraReport.txt):
// FUN_004966A0(ctrlId, actionId, keyconfigName) -> FUN_0047E7B0(ctrlId) = one of the two
// GAME_SynthKeyControler wrappers (FUN_00464410; index 0/1) -> FUN_00464170(actionId):
// for each of the wrapper's devices, FUN_004964E0 = maskTable[actionId] & [device+0x20]->+0xC,
// ORed. The mod's hook reads directions with exactly that chain for the training player's
// controller (engine[0x25F4 + (engine+0x1688)*4]). The packed battle-input word
// (BattleInputWrite hook, FUN_00559FE0 <- task FUN_0056AFF0, vtable 0x94FC78) is NOT usable
// at the hook: it was neutral on every logged reset press (docs/Research/ResetLabOverride.md).
// Training menu-item table walked by FUN_006CD5C0: entries of 0x38 bytes. An entry's
// BASE is 0x17AE3A8 + i*0x38 (the "this" both the setter and the reader get: see the
// `lea ecx,[eax*8+017AE3A8h]` at 0x6CD628 and `lea eax,[ecx*8+017AE3A8h]` at 0x6D369B).
// Relative to that base: +0x00 int "in use" (must be 1), +0x04 flag word, +0x14 the
// C-string item name ("TRI_ResetPosition"; the walk compares names at 0x17AE3BC + i*0x38),
// +0x34 value-slot index. FUN_006CD700 (thiscall this=BASE, value) is the setter: it writes
// optData(FUN_004B9700)+0xA568+idx*4, or +0xBBB8/BC/C0/C4 when flag 4/8/0x10/0x20 is set.
// FUN_006CC0F0 is the matching reader (same offsets, same this).
// CORRECTION 2026-09-27: an earlier note put the flags and index relative to the NAME
// (name+4, name+0x34). The mod's first write used that and passed the name as "this", so
// [this+4] was the string's "Rese" - 'R' = 0x52 has bit 0x10 - and the value landed in
// optData+0xBBC0, a different option, never in TRI_ResetPosition. Do not walk the table
// ourselves: call FUN_006CD5C0 / FUN_006CBF90 with the 32-byte name, as the game does.
static constexpr uintptr_t ADDR_TrainingMenuItemTable = 0x013AE3BC;     // first NAME, not base
static constexpr uintptr_t ADDR_TrainingMenuItemTableEnd = 0x013B009B;
static constexpr uintptr_t TRAINING_MENU_ITEM_STRIDE = 0x38;
static constexpr uintptr_t ADDR_TrainingMenuItem_SetValue = 0x002CD700;   // FUN_006CD700 thiscall(base, value)
// FUN_006CD5C0 __stdcall(const char name[32], int value): find by name, call the setter.
// This is exactly the call FUN_006D2D40 makes (0x6D2E13..0x6D2E1C).
static constexpr uintptr_t ADDR_TrainingMenuItem_SetByName = 0x002CD5C0;
// FUN_006CBF90 __stdcall(const char name[32]) -> int: read by name (sorted lookup
// FUN_006D3510, then FUN_006CC0F0). What FUN_006CBFE0 uses to read TRI_ResetPosition.
static constexpr uintptr_t ADDR_TrainingMenuItem_GetByName = 0x002CBF90;
// FUN_006CBFE0 is the reader that turns TRI_ResetPosition into a side at reset.
static constexpr uintptr_t ADDR_TrainingResetPositionApply = 0x002CBFE0;
// Round-end/reset fade flag word: *(battleScene+0x62B7C) bit 0x4000000 is set
// at FUN_0055F780+0x7EA (RVA 0x15FF6A "or [ebx],0x4000000") when the
// round-transition fade starts, cleared by the restart variants above.
static constexpr uintptr_t ADDR_BattleScene_FlagsOffset = 0x00062B7C; // offset, not RVA

// ---------------------------------------------------------------------------
// Saved keyconfig -> controller-object apply chain (hotplug redetect fix,
// 2026-07-19). All RVAs verified against tools/bbcf_disasm_ascii.txt.
//
// The KeyControler objects created by _create_pad_input_controllers
// (FUN_004722C0) and _create_SystemKeyControler (FUN_00473EF0) are
// GAMESTEAM_BattleKeyControler / GAMESTEAM_SystemKeyControler tasks
// (vtables .rdata 0x89C61C / 0x89C658, RTTI-confirmed). Each holds:
//   +0x18 = physical AA_CInput* device
//   +0x34 = dword bindings[actionCount]  (physical key/button code per
//           logical action index; -1 = unbound)
//   +0x38 = actionCount (0x1C for these classes)
// Init FUN_00496990(this, &devSlot, 0x1C) mallocs +0x34, fills it with -1,
// then calls vtbl+0xC = SetDefaultBindings (Battle: FUN_004699C0, System:
// FUN_00469AB0 - hardcoded default tables). THIS is why a mod-forced
// recreate comes up with default Key Config: the game applies the SAVED
// config in a separate later step (below).
//
// SystemManager (base+0x8929C8, absolute 0xC929C8) controller slots:
//   +0x0C = keyboard ctrl set A           +0x18 = keyboard ctrl set B
//   +0x10+4*slot = pad ctrl set A (also aliased at +0x1C+4*slot)
//   +0x24+4*slot = pad ctrl set B (returned by FUN_004C1380(sysMgr, slot))
// CORRECTION (2026-09-12): +0x18 is NOT a BattleKeyControler. BOTH keyboard
// objects are GAMESTEAM_SystemKeyControler - FUN_00473EF0 builds them from the
// same factory at 0x89DBBC, whose RTTI COL 0x9AC408 names
// GAMESTEAM_SystemKeyControlerFactory. GAMESTEAM_BattleKeyControler comes only
// from the pad creator FUN_004722C0 (factory 0x89DBB4, COL 0x9AC3BC). This
// matters: the two classes resolve bindings differently (see below).
//
// Binding accessors on the controller object:
//   FUN_00496AD0(this, actionIdx, code) stdcall-thiscall = SetBinding
//   vtbl+0x10 = FUN_00469850(actionIdx) = GetBinding ([this+0x34][idx])
static constexpr uintptr_t ADDR_KeyControler_SetBinding = 0x00096AD0;
static constexpr uintptr_t ADDR_BattleKeyControler_SetDefaults = 0x000699C0; // FUN_004699C0
static constexpr uintptr_t ADDR_SystemKeyControler_SetDefaults = 0x00069AB0; // FUN_00469AB0
//
// Saved keyconfig storage: one giant static option/save-data blob at
// 0xCF85D8 (lazy-init ctor FUN_004B9350; serialized to/from bbsave.dat).
//   FUN_004B9700() cdecl -> 0x00CF85E0 = option DATA pointer ("optData")
//   FUN_004B9770() cdecl -> 0x00CF85D8 = option manager (this for applies)
// Inside optData (all byte fields/tables):
//   +0x4254/+0x4255  = P1/P2 selected pad keyconfig profile index
//   +0x4204 (VA 0xCFC7E4) = pad keyconfig records, 20 bytes per
//                     (profile + slot*2) record, applied to ctrl set B
//   +0x4256 (VA 0xCFC836) = keyboard keyconfig records (20-byte stride),
//                     applied to sysMgr+0x18; defaults copied from const
//                     table .data 0x9DFE00 by FUN_004BCAA0 (reset-to-default)
//   +0x54AA5/+0x54AA6 = P1/P2 profile index for the second config set
//   +0x54A55 (VA 0xD4D035) = pad records for ctrl set A (buttons only)
//   +0x54AA7 (VA 0xD4D087) = keyboard records for sysMgr+0x0C
static constexpr uintptr_t ADDR_GetOptionData = 0x000B9700;    // FUN_004B9700
static constexpr uintptr_t ADDR_GetOptionManager = 0x000B9770; // FUN_004B9770
//
// Apply functions (thiscall, ecx = FUN_004B9770() result; they resolve the
// SystemManager themselves via [0xC929C8] and call SetBinding per action):
//   FUN_004BB6E0(mgr, padSlot, profileIdx) - pad set B  (ret 8)
//   FUN_004BB860(mgr, profileIdx)          - keyboard sysMgr+0x18 (ret 4)
//   FUN_004BBA10(mgr, padSlot, profileIdx) - pad set A  (ret 8)
//   FUN_004BBB30(mgr, profileIdx)          - keyboard sysMgr+0x0C (ret 4)
// The game's own apply-all sequence (menu-side, FUN_00483F10 at 0x483F60..
// 0x483FFE, sole caller 0x4832B4) is exactly:
//   optData = FUN_004B9700(); mgr = FUN_004B9770();
//   FUN_004BB6E0(mgr, 0, optData[0x4254]); FUN_004BB6E0(mgr, 1, optData[0x4255]);
//   FUN_004BB860(mgr, optData[0x4254]);
//   FUN_004BBA10(mgr, 0, optData[0x54AA5]); FUN_004BBA10(mgr, 1, optData[0x54AA6]);
//   FUN_004BBB30(mgr, optData[0x54AA5]);
// Run this after ControllerOverrideManager::RedetectControllers_Internal()
// to restore the player's saved Key Config on the recreated controllers.
// (In-match VS mode instead re-applies via 0x612704/0x6A8B11 with per-player
// profile ids from engine+0x25F4/+0x25F8.)
typedef unsigned char* (__cdecl* GAME_GetOptionData_t)();
typedef void* (__cdecl* GAME_GetOptionManager_t)();
static constexpr uintptr_t ADDR_ApplyPadKeyConfig_SetB = 0x000BB6E0;
static constexpr uintptr_t ADDR_ApplyKeyboardKeyConfig_SetB = 0x000BB860;
static constexpr uintptr_t ADDR_ApplyPadKeyConfig_SetA = 0x000BBA10;
static constexpr uintptr_t ADDR_ApplyKeyboardKeyConfig_SetA = 0x000BBB30;

// ---------------------------------------------------------------------------
// SystemKeyControler direction resolution, and why device+0x30 bit 0 is NOT a
// trustworthy "player is holding Up" (old snake's training side-swap report,
// 2026-09-12; DEBUG.txt of 2026-09-11 22:24).
//
// vtbl+8 = "is this action's binding down", called per action index by
// FUN_004963F0. The two classes differ:
//   GAMESTEAM_BattleKeyControler : FUN_00469700 - configured binding only.
//   GAMESTEAM_SystemKeyControler : FUN_00469750 - configured binding, PLUS a
//     jump table at 0x469814 covering action indices 0-3 only, which appends
//     HARDCODED, UNREBINDABLE keys and ORs up to three candidates:
//       idx 0 (Up)    += VK_UP  and  VK_SPACE   <- two extras
//       idx 1 (Right) += VK_RIGHT
//       idx 2 (Down)  += VK_DOWN
//       idx 3 (Left)  += VK_LEFT
//     (game key ids 0x5E/0x41/0x5F/0x60/0x61; see the VK map below.) This is why
//     the arrow keys and Space always steer BBCF on a keyboard no matter what the
//     key config says.
// Both keyboard controllers are SystemKeyControlers, so BOTH carry those extras,
// and each is driven by a DIFFERENT saved key config set (set A / set B above).
// A player who rebound only one set still has the other on the W/D/S/A default,
// so e.g. a Q/W/E+Space layout has "W" lit as Up on one object while it means
// Down in battle. Any code that ORs bit 0 across the controller objects therefore
// reads Up when the player presses W, Space or the up arrow.
//
// Direction state: use the packed battle input word instead
// (hooks_battle_input.h GetLastObservedBattleInput) - that is the player's real
// in-match direction under their own battle key config, pad and keyboard alike,
// and it carries none of the menu-side or hardcoded bits. Action words remain fine
// for the reset EDGE (bit 0x08000000 = action index 0x1B, keyboard default
// Backspace, pad default button 9).
static constexpr uintptr_t ADDR_BattleKeyControler_IsBindingDown = 0x00069700; // FUN_00469700
static constexpr uintptr_t ADDR_SystemKeyControler_IsBindingDown = 0x00069750; // FUN_00469750
static constexpr uintptr_t ADDR_SystemKeyControler_DirExtrasJumpTable = 0x00069814;
//
// Keyboard key-code space (AASTEAM_CInputKeyBoard, vtable .rdata 0x84FCA8):
//   FUN_004194E0 = poll. Copies the current 128-bit pressed map (this+8..+0x17) to
//     the previous one (+0x18), reads GetKeyboardState into this+0x28 (256 VK
//     bytes), then for each VK with 0x80 set maps VK -> game key id through the
//     byte table at 0xA255C8 (0x6A = unmapped) and sets that bit in the map.
//   FUN_00419400 = vtbl+0xC IsPressed(gameKeyId): map[id>>5] & (1 << (id & 31)).
//   The VK -> game-key-id table is built at runtime by the keyboard ctor
//     FUN_00419070 (it lives in BSS, so it is NOT readable from BBCF.exe on disk -
//     reconstruct it from the ctor's stores if you need it again). Layout:
//     ids 0x00-0x19 = VK 'A'-'Z', 0x1A-0x23 = VK '1'-'9','0', 0x24 = RETURN,
//     0x25 = ESCAPE, 0x26-0x31 = F1-F12, 0x41 = SPACE, 0x42 = BACKSPACE,
//     0x54-0x5D = numpad 0-9, 0x5E = UP, 0x5F = RIGHT, 0x60 = DOWN, 0x61 = LEFT.
static constexpr uintptr_t ADDR_Keyboard_VkToGameKeyTable = 0x006255C8; // 256 bytes, BSS
// CORRECTED 2026-10-07: FUN_004194E0 only translates the VK state into the game-key
// map; the GetKeyboardState call is the keyboard's vtbl+8, FUN_004194B0, which skips
// polling while the app is inactive (app object 0xA5A500 +0xD0, set by WM_ACTIVATEAPP).
static constexpr uintptr_t ADDR_Keyboard_Poll = 0x000194E0;             // FUN_004194E0 (translate)
static constexpr uintptr_t ADDR_Keyboard_ReadState = 0x000194B0;        // FUN_004194B0 (vtbl+8)
static constexpr uintptr_t ADDR_Keyboard_IsPressed = 0x00019400;        // FUN_00419400

// ---------------------------------------------------------------------------
// Platinum personality/voice roll (Sena/Luna). See
// docs/Research/PlatinumVoiceChoiceInvestigation.md (2026-07-22 "Resolved: ..."
// section) for the full trace. Flag lives at selectStruct+0x164C
// (== our +0x4 view), player stride 0x20; 1 = Sena, 0 = Luna.
//
// Select-struct singleton getter (ecx-less cdecl, returns base in eax); the
// SAME base our GetPaletteIndexPointers hook derives (edx). Both the roll and
// the battle-time read below index off this.
static constexpr uintptr_t ADDR_GetCharSelectStruct = 0x0007E860; // FUN_0047E860
//
// Personality randomize function. Runs at character-select CONFIRM on both
// clients (lockstep-deterministic), NOT per-frame. Signature roughly:
//   void __thiscall Roll(void* selectStructBase /*ecx*/, int playerIndex /*[ebp+8]*/)
// Draws stream-0 of the shared-seed PRNG (FUN_0040BF00, push 0) and writes
// rand()%2 into [playerIndex*0x20 + selectStruct + 0x164C]. This is the
// authoritative write; everything downstream (battle-time reads) sees it.
static constexpr uintptr_t ADDR_PlatinumPersonalityRoll = 0x000807E0; // FUN_004807E0
//   Write site P1: 0x00480B7E  mov [esi+edi+164Ch],edx  (edx = rolled value)
//   Write site P2: 0x00480BAC  mov [ebx+164Ch],eax      (eax = rolled value)
static constexpr uintptr_t ADDR_PlatinumPersonalityRoll_WriteP1 = 0x00080B7E;
static constexpr uintptr_t ADDR_PlatinumPersonalityRoll_WriteP2 = 0x00080BAC;
//
// Shared-seed PRNG draw: FUN_0040BF00(int streamIndex) -> indexes
// streamIndex*0x9CC + [0xA135C8], calls Next() (FUN_004528A0). This is
// MT19937 (Mersenne Twister): FUN_00452990 = regenerate-624, FUN_00452940 =
// init_genrand(seed) (0x6C078965 multiplier). The personality roll uses
// stream 0.
static constexpr uintptr_t ADDR_SharedSelectRng_Next = 0x0000BF00; // FUN_0040BF00
static constexpr uintptr_t ADDR_MT_InitGenrand = 0x00052940;       // FUN_00452940 init_genrand(seed)
//
// SEED SOURCE (this is the actual online sync). Two seed paths:
//   Offline (training/arcade/vs-CPU): FUN_00471F00 seeds streams 0/1 from a
//     high-res timer (QPC-style) -> non-deterministic, per-boot.
//   Online + replay: FUN_0069D001 seeds streams 0/1 (sites 0x0069D7BA /
//     0x0069D7CE) from a stored per-match value, copied earlier
//     (0x0069D54C..0x0069D563) out of FUN_0055C540()+0x83E48 / +0x83E4C into
//     the per-slot match/replay struct at 0x0155B908 / 0x0155B90C (stride
//     0xA0). Both clients get the SAME seed -> identical MT sequence ->
//     identical personality roll. The flag itself never crosses the wire.
static constexpr uintptr_t ADDR_SelectRngSeed_Offline = 0x00071F00; // FUN_00471F00 (timer)
static constexpr uintptr_t ADDR_SelectRngSeed_OnlineReplay = 0x0029D001; // FUN_0069D001 (shared match seed)
//
// Battle-time READ sites: script-VM boolean condition opcodes (ret 8), case handlers
// in the query-VM jump table at 0x0057DEB8 (evaluator FUN_0057D020). They read the flag
// from the LIVE select struct via FUN_0047E860. Because the flag feeds DETERMINISTIC
// script execution, it is part of the synced/checksummed match state: writing a value
// that differs between clients desyncs the match (confirmed by live test). DO NOT force
// the flag online.
//   0x0057D3F3 cmp [...+164Ch],1 / 0x0057D416 ==2 / 0x0057D439 ==3 (also 0x0057DAE4/DB12/DB41/DB70)
static constexpr uintptr_t ADDR_PlatinumPersonalityRead_Eq1 = 0x0017D3F3;
//
// AUDIO override (how the mod changes the voice - client-side only, NEVER writes the flag).
// Platinum ships TWO separate voice banks: vbtl_pt_0.pac (suffix "a" = Luna cues) and
// vbtl_pt_1.pac (suffix "b" = Sena); only the rolled personality's bank is mounted. So the
// personality is read at TWO kinds of client-side sites, BOTH must be biased together:
//
//  1. Voice-bank LOAD routine FUN_00555A20 (runs at battle load; edi = select-struct base =
//     FUN_0047E860()). Reads personality to index a .pac filename table (0x9DC8D0 "vbtl_%s_%d",
//     0x9DC8F0 "vbtldb_%s_%d") and mount that bank. Slot-0 reads at +0x164C, slot-1 at +0x166C:
static constexpr uintptr_t ADDR_PlatVoiceLoad_VbtlP1 = 0x001560D0; // mov esi,[edi+164Ch] vbtl slot0
static constexpr uintptr_t ADDR_PlatVoiceLoad_VbtlP2 = 0x00156176; // mov esi,[edi+166Ch] vbtl slot1
static constexpr uintptr_t ADDR_PlatVoiceLoad_SubP1  = 0x0015621D; // subvoice slot0
static constexpr uintptr_t ADDR_PlatVoiceLoad_SubP2  = 0x00156319; // subvoice slot1
static constexpr uintptr_t ADDR_PlatVoiceLoad_DbP1   = 0x0015640C; // vbtldb slot0
static constexpr uintptr_t ADDR_PlatVoiceLoad_DbP2   = 0x00156488; // vbtldb slot1 (base = eax here)
//  2. Voice-file PLAY resolvers FUN_005CFC80 / FUN_005D1440 (`mov eax,[eax+ecx+164Ch]`,
//     eax=slot<<5, ecx=base): read personality to pick the "<base><a/b/c/d>.wav" cue name.
static constexpr uintptr_t ADDR_PlatinumVoiceResolverA = 0x001CFC80; // FUN_005CFC80 (read 0x1CFF57)
static constexpr uintptr_t ADDR_PlatinumVoiceResolverB = 0x001D1440; // FUN_005D1440 (read 0x1D15E2)
// The voice-bank LOAD routine itself (FUN_00555A20, __thiscall, ecx = voice manager). It is
// PURE LOAD (contains no play calls) and enqueues async worker-thread loads, so it can be
// re-invoked mid-match to remount the chosen personality's bank without stalling the game
// thread or re-playing voices. hooks_palette.cpp captures its `this` (live in ebx at the load
// hook sites) and re-invokes it when the voice choice changes mid-match.
// FUN_00555A20 is the start-of-match voice-bank LOAD routine (where our LOAD bias hooks fire).
// NOTE: mid-match voice swapping was attempted by re-invoking the load + the cue-table
// registrar below; it CRASHED and was removed. The choice now latches at match load only.
static constexpr uintptr_t ADDR_PlatinumVoiceLoadRoutine = 0x00155A20; // FUN_00555A20
// The voice CUE-TABLE registrar (FUN_0056B850): rebuilds the game-side cue-name tables in
// registry 0xEBFF68 (slots 6/7/8) that the play resolver searches. FUN_00555A20 loads the bank
// but does NOT register the cue table. Driving THIS standalone mid-match to re-register after a
// swap CRASHED in testing - do NOT call it outside the game's own load sequence. Kept only as
// an RE reference. (Preload-both + slot redirect was assessed as not worth the effort/risk.)
static constexpr uintptr_t ADDR_PlatinumVoiceCueTableRegister = 0x0016B850; // FUN_0056B850
// hooks_palette.cpp hooks all 8 reads and substitutes the chosen personality IN-REGISTER
// (never memory) for the local player's Platinum. The mounted XACT bank + voice handle
// (char+0x1E9E4) are per-client heap, never in the GGPO checksum -> desync-safe vs anyone.
// The audio system is XACT (AA_CWaveBankDataBase_XACT::RegistBank @0x89923F), not CriWare.

// ---------------------------------------------------------------------------------------------
// Platinum PT_LinkColor - the item-state palette redirection (fixed 2026-07-26).
//
// While Platinum holds a drive item, her script runs the character-specific BBScript command
// PT_LinkColor. Its handler picks one of eight engine objects named PaletteControlObj1..8 by her
// item type and stores THAT object's palette id into CharData+0x35C (linkedPaletteId), so the
// renderer draws her from that palette instead of her own palette storage. Those eight palettes
// are built once at round load and never refreshed - which is why a mid-match custom palette
// appeared to "stop applying" while she held an item, but palettes.ini (applied at OnMatchInit,
// before the build) worked. The handler itself writes -1 when she has no item, meaning "draw from
// my own palette storage"; PaletteManager::ClearPlatinumItemPaletteLink writes that same -1 while
// a custom palette is active. Full write-up: docs/Research/PlatinumItemPaletteInvestigation.md.
static constexpr uintptr_t ADDR_PlatLinkColorCmdCompare = 0x001B2AC3; // "PT_LinkColor" name compare
static constexpr uintptr_t ADDR_PlatLinkColorClear      = 0x001B2AE1; // mov [ebx+35Ch],-1 (no item)
static constexpr uintptr_t ADDR_PlatLinkColorDispatch   = 0x001B2AFE; // add eax,-3 / cmp 0Dh / jmp table
static constexpr uintptr_t ADDR_PlatLinkColorStore      = 0x001B2B66; // mov [ebx+35Ch],eax (the link)
// Item type (CharData+0x1A0, SLOT_59) -> object, via byte table 0x005B52D0 + jump table 0x005B52B4:
//   3,4 -> Obj2   5,6 -> Obj3   7,8 -> Obj4   9,10 -> Obj5   11,12 -> Obj6   13,14 -> Obj7
//   15,16 -> Obj8   (default -> Obj1, 0 -> no link). Name strings at 0x00954D50..0x00954DDC.
// So the engine has a SEVEN-way per-item palette slot set that vanilla fills with identical
// copies - see docs/Research/PlatinumPerItemPaletteFeasibility.md.
static constexpr uintptr_t ADDR_PlatLinkColorIndexTable = 0x001B52D0; // byte table, item types 3..16
static constexpr uintptr_t ADDR_PlatLinkColorJumpTable  = 0x001B52B4; // jump targets per case
//
// Palette container loader: stores the loaded container at owner+0x830 (the mod's
// GetPalBaseAddresses hook site is the store at 0x005B6372). The owning object carries a
// "vr<charcode>" resource tag at +0x834 matching char_%s_vri.pac; Platinum's is "vrpt". NOTE it
// fires THREE times per match load and the mod keeps only the first two as P1/P2 - the third is
// not a 24-slot character container (it resolves only 2 slots) and must not be written to as if
// it were; doing so corrupts the heap.
static constexpr uintptr_t ADDR_PaletteContainerLoad    = 0x001B6310; // FUN_005B6310
//
// Palette texture REFRESH (2026-08-05, Taokaka round-start palette investigation).
// FUN_005B6310's caller (owner+0x830 store; base+0x567F35 in the load routine at ~0x567E00-0x5680C0)
// is an outer "character render resource" object holding TWO palette-container slots:
//   owner+0x82C - loaded by a sibling loader FUN_005B6390, refreshed by FUN_005B6980
//   owner+0x830 - loaded by FUN_005B6310 (the one GetPalBaseAddresses hooks), refreshed by FUN_005B6940
// Confirmed via disasm: at the GetPalBaseAddresses hook site (0x005B6372, `mov [ecx+830h],eax`),
// ECX is this same "owner" object - the hook currently only captures EAX (the container, which is
// what CharPaletteHandle stores as m_pPalBaseAddr), never ECX. Capturing ECX too would give a
// direct handle to the owner, hence to these refresh functions.
//
// FUN_005B6940(this=owner) - gates on `cmp [owner+18h],0; je skip`. If set, calls owner's
// +0x830 container->vtable[2](owner+4, owner+8, owner+0Ch, 1) then container->vtable[3](0, 0x300).
// 0x300 = 768 = a full 256-color RGB palette in bytes - this is almost certainly a Lock/Unlock
// pair (D3D9-style) that pushes the palette bytes at owner+0x830 into whatever the renderer
// actually samples (a GPU texture, most likely). FUN_005B6980(this=owner) is the analogous
// refresh for the +0x82C container, gated on [owner+10h], pushing 0x400/0x400 (looks like a
// larger, fixed-size texture rather than a raw palette - possibly a full sprite-sheet atlas).
//
// DO NOT CALL THESE. Tried and rejected, 2026-08-07 -- kept documented only so the next person
// does not rediscover them and repeat it. Capturing ECX (the owner) in GetPalBaseAddresses,
// setting [owner+18h]=1 and calling FUN_005B6940(owner) directly was implemented and live-tested.
// It made things strictly worse:
//   - FUN_005B6940 has exactly ONE call site in the whole binary (0x005680BE, in the character
//     load routine). The engine never calls it more than once per character per match, and the
//     vtable[2] parameters it reads from owner+4/+8/+0Ch are engine state the mod never
//     initializes. Calling it repeatedly (the palette editor's hover preview does so many times
//     per second) left Platinum permanently stuck on wrong colors with no self-correction.
//   - Restricting it to one call per match did not help either: Kokonoe then failed to redraw
//     100% of the time, and the diagnostic that "proved" a per-character rendering difference was
//     confounded -- the only character we wrote [owner+18h] on was the one that broke.
//
// The premise behind all of this was wrong anyway. The redraw failure it was chasing was caused by
// the mod itself: a per-frame correction of UpdatePalette()'s index toggle, added in v8.2, undid
// the index change the engine's redraw depends on. Removing that per-frame correction fixed it,
// confirmed by the reporter across ~22 matches on v8.1. See
// docs/Research/TaokakaPaletteRefreshInvestigation.md for the full history.
static constexpr uintptr_t ADDR_PaletteTextureRefresh_Main = 0x001B6940; // FUN_005B6940: actually CreateGpu (D3DXCreateTexture), see Palettes.md
static constexpr uintptr_t ADDR_PaletteTextureRefresh_Alt  = 0x001B6980; // FUN_005B6980 (owner+0x82C container)

// Character-select resume snapshot (0x0047D910). The mod's GetPaletteIndexPointers hook sits
// inside it at 0x0047D92D. It copies the LIVE slot blocks to a snapshot:
//
//   [edx+0x0118] -> [edx+0x16B8]   0x1C4 dwords   P1 CSS player state
//   [edx+0x1648] -> [edx+0x24D8]   8 dwords       P1 slot block        <- hook is here
//   [edx+0x0828] -> [edx+0x1DC8]   0x1C4 dwords   P2 CSS player state
//   [edx+0x1668] -> [edx+0x24F8]   8 dwords       P2 slot block
//
// Corrected 2026-10-18 (docs/Decompilation/Palettes.md): the 0x1648 blocks are the LIVE match
// config, and the battle reads the colour at +8 every time an entity resolves its palette
// (EntityPaletteSource 0x0057C91A). The 0x24D8 copy is a resume snapshot, read back only when
// returning from a ranked match (0x0047FD20), and never read in battle. This routine runs from
// the main menu, offline CSS/VS and the ranked menu, never online, so it is NOT a per-match
// commit. The hook is only useful to learn the two static addresses (0x00C91A00 / 0x00C91A20).
// Any mod write to the colour dword during a battle changes what the game itself draws, and
// characters re-read it every round.
// Character select colour-preview palettes. The preview is indexed "dot art" and it draws
// with the ORDINARY GAMEPLAY palettes: the bake reads char_<tag>_pal.pac out of resource
// slot (ADDR_CssPaletteResourceSlot + charIndex). It never touches paldata.pac inside
// char_select_dot.pac - verified by disassembly of the builder, which pushes "palimg.pac"
// once and "paldata.pac" not at all, which is why replacing that file did nothing.
//
// The bake runs ONCE on entry to character select, over every character x 24 colours, and
// copies each palette into its own buffers (duplicated at +0x800, the same pattern
// CharPaletteHandle::ReplacePalArrayInMemory already handles). There is no per-colour and
// no per-player re-read, so one substitution covers both players - and a palette changed
// while sitting on the screen is not picked up until the screen is entered again.
static constexpr uintptr_t ADDR_CssDotPaletteBake       = 0x00210470; // FUN_00610470, bake loop
static constexpr uintptr_t ADDR_CssDotPaletteEntryFetch = 0x002111CE; // call FPAC::GetEntryPtr (hook site)
static constexpr uintptr_t ADDR_FpacGetEntryPtr         = 0x00057AD0; // FUN_00457AD0, returns the HPAL entry
static constexpr uintptr_t ADDR_CssPaletteResourceSlot  = 0x000000B6; // + charIndex -> char_<tag>_pal.pac
static constexpr uintptr_t ADDR_CssDotPacResourceSlot   = 0x000000FB; // char_select_dot.pac (sprites only)

static constexpr uintptr_t ADDR_CharSelectConfigCommit = 0x0007D910; // FUN_0047D910
static constexpr uintptr_t OFFSET_PendingPalBlockP1    = 0x1648;     // +8 = native color index
static constexpr uintptr_t OFFSET_PendingPalBlockP2    = 0x1668;
static constexpr uintptr_t OFFSET_CommittedPalBlockP1  = 0x24D8;
static constexpr uintptr_t OFFSET_CommittedPalBlockP2  = 0x24F8;

// ---------------------------------------------------------------------------
// Own network profile blob + its checksum seal (RE'd 2026-09-05 while tracking
// down "you don't have a profile" caused by the mod's own avatar writes; see
// src/Network/ProfileBlobSeal.h for the full write-up).
//
// The LOCAL player's profile blob is netUserData(base+0x8AD0C0) + 0xD0, size
// 0x6800 - the same layout as the per-member rows above, and the buffer the
// game FileShare()s to Steam Cloud as bbdc.dat. Confirmed live: netUserData
// 0x0092D0C0, blob 0x0092D190, which is exactly the value the mod already
// tracks as g_gameVals.playerAvatarBaseAddr. The lobby avatar fields sit
// INSIDE it (+0x610C icon, +0x6110 colour, +0x61C4/+0x61C5 accessories), so
// the Online page's avatar sliders edit a checksummed payload.
//
// Blob header, per the sealer:
//   +0x00 uint16 checksum   (+0x02 is left zero: sealer clears a dword, writes a word)
//   +0x04 uint32 stamp      FUN_0040BF90(0), refreshed on every seal
//
// Upload path: FUN_004A96D0 early-returns when the TUS latch DAT_00CF77A8 is
// set, otherwise seals the blob (FUN_004A1C10) and queues the transfer. The
// upload strategy tick then RE-verifies the same buffer at item-state 0 before
// any Steam call, so a write landing between seal and tick fails the upload
// with no Steam round-trip and trips the latch.
// ---------------------------------------------------------------------------
static constexpr uintptr_t OFFSET_OwnProfileBlob       = 0xD0;    // from netUserData
static constexpr size_t    SIZE_ProfileBlob            = 0x6800;
static constexpr uintptr_t OFFSET_ProfileBlobChecksum  = 0x00;    // uint16
static constexpr uintptr_t OFFSET_ProfileBlobStamp     = 0x04;    // uint32
// Ghidra FUN_0040DEC0 - the SEALER: writes stamp at +4, zeroes the dword at +0,
// sums the buffer as size/2 words with end-around carry, stores ~sum as a word
// at +0. Counterpart to ADDR_ProfileChecksum16 (FUN_0040DF10) above.
static constexpr uintptr_t ADDR_ProfileChecksumSeal    = 0x0000DEC0;
// Ghidra FUN_004A1C10 - thin wrapper: FUN_0040DEC0(this, 0x6800)
static constexpr uintptr_t ADDR_ProfileBlobSealBlob    = 0x000A1C10;
// Ghidra FUN_0049D5C0 - returns the own profile blob (== netUserData + 0xD0)
static constexpr uintptr_t ADDR_GetOwnProfileBlob      = 0x0009D5C0;
// Ghidra FUN_004A96D0 - "upload my profile blob as bbdc.dat"
static constexpr uintptr_t ADDR_UploadOwnProfile       = 0x000A96D0;
// Ghidra FUN_0040BF90 - source of the +0x04 stamp
static constexpr uintptr_t ADDR_ProfileStampSource     = 0x0000BF90;

// ---------------------------------------------------------------------------
// Network-entry profile sync: the D-Code sync task (outer network task 3) that
// downloads the own profile and shows "Cannot find player information. Would you
// like to create a new profile?". Fully reimplemented by src/Network/ProfileSync.cpp;
// RE in docs/Research/ProfileResetPromptInvestigation.md. All __fastcall/__thiscall
// as noted, verified against the disassembly 2026-10-06.
// ---------------------------------------------------------------------------
// FUN_004B0970 - the task itself, __fastcall(ecx = task). Single caller 0x4A70BF.
// Starts push ebx / push esi / push edi / mov esi,ecx; replaced at entry.
static constexpr uintptr_t ADDR_ProfileSyncTask          = 0x000B0970;
// FUN_004A96D0 prologue push ebp / mov ebp,esp / sub esp,0D0h (9 bytes); upload guard
static constexpr size_t    SIZE_UploadOwnProfilePrologue = 9;
// FUN_00486070 - returns a pointer to the network interface holder; vtable +0x18
// gives the net object whose +0xC accepts any fetch result, +0x1C is a tick
static constexpr uintptr_t ADDR_NetInterfaceHolder       = 0x00086070;
// FUN_0049A400 - returns the net manager (DAT_00CAD000), the ecx of most calls below
static constexpr uintptr_t ADDR_NetManager               = 0x0009A400;
// FUN_0049A230 - starts the own profile download (loader cc := 1), thiscall(netMgr)
static constexpr uintptr_t ADDR_OwnProfileFetchStart     = 0x0009A230;
// FUN_0049A940 - own profile fetch tick + poll: -100 pending, 0 ok (cc 3/4), 100 failed (cc 6)
static constexpr uintptr_t ADDR_OwnProfileFetchPoll      = 0x0009A940;
// FUN_004A0CF0 - reset a row's loader (cc := 0), fastcall(blob)
static constexpr uintptr_t ADDR_ProfileLoaderReset       = 0x000A0CF0;
// FUN_004A1F70 - build a default profile (reset + cc := 4), fastcall(blob)
static constexpr uintptr_t ADDR_ProfileBuildDefault      = 0x000A1F70;
// FUN_004A9D20 - clears the task's dialog/request block (+0x24..+0xA8), fastcall(task)
static constexpr uintptr_t ADDR_NetTaskClearRequests     = 0x000A9D20;
// FUN_004A7F90 - dialog still busy (+0xAC or +0xB8 set), fastcall(task)
static constexpr uintptr_t ADDR_NetTaskDialogBusy        = 0x000A7F90;
// FUN_004A7C30 - switch outer network task, thiscall(task, id), ret 4
static constexpr uintptr_t ADDR_NetTaskSwitch            = 0x000A7C30;
// FUN_004B8D30 - poll the own upload, thiscall(transfer, blob), ret 4; 0 when latched
static constexpr uintptr_t ADDR_OwnProfileUploadPoll     = 0x000B8D30;
// FUN_004B9100 / FUN_004B9070 - DLC ownership query start / poll, thiscall(transfer)
static constexpr uintptr_t ADDR_DlcQueryStart            = 0x000B9100;
static constexpr uintptr_t ADDR_DlcQueryPoll             = 0x000B9070;
// FUN_00643B40 + FUN_00648210 - D-Code DLC consistency check (nonzero = error)
static constexpr uintptr_t ADDR_DCodeDlcHolder           = 0x00243B40;
static constexpr uintptr_t ADDR_DCodeDlcCheck            = 0x00248210;
// FUN_006983E0 - log a named network error, cdecl(const char*)
static constexpr uintptr_t ADDR_NetLogError              = 0x002983E0;
// "NTER_DCodeDLCCheckError", the string the game passes to it
static constexpr uintptr_t ADDR_Str_DCodeDLCCheckError   = 0x004A9E24;
// FUN_006B2160 + FUN_006AF6F0, FUN_0047C8C0, FUN_00475300 + FUN_004769C0 (thiscall, ret 8),
// FUN_004B9700, FUN_006C4330 - DLC refresh / "new DLC" notice / network-ready latch
static constexpr uintptr_t ADDR_DlcRefreshHolder         = 0x002B2160;
static constexpr uintptr_t ADDR_DlcRefresh               = 0x002AF6F0;
static constexpr uintptr_t ADDR_DlcFlagsHolder           = 0x0007C8C0;
static constexpr uintptr_t ADDR_MessageQueueHolder       = 0x00075300;
static constexpr uintptr_t ADDR_MessageQueuePost         = 0x000769C0;
static constexpr uintptr_t ADDR_NetSessionInfo           = 0x000B9700;
static constexpr uintptr_t ADDR_NetSessionReady          = 0x002C4330;
// DAT_00CF77A8 - TUS latch: nonzero skips every profile upload and D-Code read
static constexpr uintptr_t ADDR_TusLatch                 = 0x008F77A8;
// DAT_00A5A054 - work manager result (mgr+4): 7 read ok, 8 write ok, 9 no data, 0xB error
static constexpr uintptr_t ADDR_WorkMgrResult            = 0x0065A054;
// Loader sub-object: *(blob + 0x68A0); its +0xCC is the loader state
// (0 idle, 1 queued, 2 in flight, 3 loaded from server, 4 default built, 6 failed).
static constexpr uintptr_t OFFSET_ProfileLoaderPtr       = 0x68A0;
static constexpr uintptr_t OFFSET_ProfileLoaderState     = 0xCC;

// ---------------------------------------------------------------------------
// Lobby avatar: the profile-blob fields are a MIRROR, not the source of truth
// (RE'd 2026-09-05). The blob copies at +0x610C icon, +0x6110 colour,
// +0x61C4/+0x61C5 accessories are re-derived from local state and overwritten
// a couple of seconds after every network connect, which is why poking them
// does not survive a relaunch (observed: colour set to 3, colour 0 came back).
//
// Refresh path (FUN_00492xxx region, blob via FUN_0049D5C0):
//   icon      <- [src+0x6388] - 1, clamped   @ 0x004924F8
//   colour    <- [src+0x638C]                 @ 0x0049250A
//   accessory <- FUN_004923C0, from [obj+0x9A4] << 8 into +0x61C4
// Consumer that drives the on-screen avatar: 0x0051D115 reads all four straight
// out of the blob and feeds them to the avatar UI object.
//
// So the blob is the right place to poke for LOCAL display, and it is also the
// payload other members download (their rows share this layout) -- but it is
// re-derived, so it is a cache. Anything that needs to persist or to be
// reliably visible to others should write the SOURCE structs above instead,
// then let the game's own refresh + seal carry it into the blob.
// ---------------------------------------------------------------------------
static constexpr uintptr_t ADDR_AvatarBlobRefresh      = 0x000924D8; // icon/colour copy
static constexpr uintptr_t ADDR_AvatarAccessoryToBlob  = 0x000923C0; // FUN_004923C0
static constexpr uintptr_t ADDR_AvatarBlobToUi         = 0x0011D0FE; // 0x0051D0FE
static constexpr uintptr_t OFFSET_AvatarSourceIcon     = 0x6388;     // from the source struct
static constexpr uintptr_t OFFSET_AvatarSourceColor    = 0x638C;

// ---------------------------------------------------------------------------
// The avatar SOURCE structs, and why writing them is the only safe way to put
// a hidden accessory back on (RE'd 2026-09-08, chasing a reporter whose lobby
// entry took 19-120s per connect with RememberLobbyAvatar on).
//
// The "source struct" the two offsets above are relative to is CSaveDataManager,
// a statically allocated singleton at ADDR_SaveDataManager. FUN_0048DCB0 is its
// MSVC lazy getter (guard bit 0 at ADDR_SaveDataManagerGuard, ctor FUN_00486F40,
// returns the fixed address). Confirmed independently: the compiler folded
// savemgr+0x6388 and savemgr+0x638C into the absolutes ds:[0x00C9E238] and
// ds:[0x00C9E23C], which appear exactly once each, in the avatar getters at
// 0x004884CD / 0x0048851F.
//
// THE TWO DIRECTIONS
//   source -> blob   FUN_004924B0 ("rebuild my profile blob from local state"),
//                    reached only through FUN_00494810(param1 != 0).
//   blob -> source   FUN_0048F310 per accessory slot; FUN_0048F810 does the whole
//                    profile. So the blob is not a write-only mirror: it round-trips.
//
// THE TWO ACCESSORY SLOTS. FUN_004924B0 calls FUN_004923C0 twice, on savemgr+0x63A0
// and savemgr+0x7304 -- two objects of stride 0xF64, each holding its equipped id at
// +0x9A4 and a "which byte do I own" flag at +0xF60 (the object's last dword):
//
//   flag == 1 -> clears blob[0x61C5], ORs (id << 8) into the dword at 0x61C4
//   flag != 1 -> clears blob[0x61C4], ORs  id       into the dword at 0x61C4
//
// The flags are set once in the savemgr init: [savemgr+0x7300] = 0 (slot A, so it
// owns the LOW byte) at 0x0049374B, and [savemgr+0x8264] = 1 (slot B, HIGH byte) at
// 0x00493770. So the mod's "accessory 1" is slot A and "accessory 2" is slot B, and
// every refresh rewrites BOTH accessory bytes from those two ids, unvalidated and
// unclamped -- unlike the icon, which FUN_004924B0 clamps itself against
// FUN_00510790() == 0x30.
//
// WHY POKING THE BLOB ALONE FIGHTS FOREVER. An id the game's own equip menu cannot
// offer is not in either source, so every refresh puts the old value back and a
// mod that re-pokes the blob is in an unbounded loop. Worse, the loop is not benign:
// a blob write landing between the upload path's seal and the upload strategy's
// re-verify fails the step with no Steam round-trip (see OFFSET_OwnProfileBlob above),
// and lobby entry is gated behind it. Measured in the reporter's captures as the
// network state machine looping state1 16 -> 12 at exactly 1 Hz for 120s, each 12
// transition timestamp-identical to a mod blob write, versus 16 -> 13 (success,
// advances immediately) in a session whose remembered ids already matched its sources.
//
// SO: write the source ids and let the game's own refresh derive, seal and upload the
// blob. Source writes touch no checksummed buffer and cannot fail the handshake.
//
// SAFE RANGE. The id reaches FUN_0073C2C0, which indexes AvtAccPoint.bin as
// [table + id*4] with NO bounds check. That file (data/ETC/lobby_acc_bin.pac ->
// accbin.pac -> AvtAccPoint.bin) is 0x4B0 bytes = 300 entries, so ids 0..299 cannot
// read out of bounds; the slot item-state tables (FUN_004B9700() + 0x50EE8 slot A,
// + 0x53984 slot B) hold >= 0x140 entries. Art is %04d_avtacc.hip named id + 1000,
// and lobby_acc.pac tops out at 1207, so 207 is the highest id with art -- which is
// exactly the mod's existing slider cap. id 0 means "no accessory" and is
// short-circuited before any table access. An id with no art simply fails a
// name lookup; there is no index math to overrun. MAX_AvatarAccessoryId below is
// therefore art-limited, well inside the bounds-limited 299.
// ---------------------------------------------------------------------------
static constexpr uintptr_t ADDR_SaveDataManager        = 0x00897EB0; // static obj, base+
static constexpr uintptr_t ADDR_SaveDataManagerGuard   = 0x008A1ECC; // init guard, bit 0
static constexpr uintptr_t ADDR_SaveDataManagerGet     = 0x0008DCB0; // FUN_0048DCB0
static constexpr uintptr_t OFFSET_AvatarAccSlotA       = 0x63A0;     // -> blob 0x61C4
static constexpr uintptr_t OFFSET_AvatarAccSlotB       = 0x7304;     // -> blob 0x61C5
static constexpr uintptr_t OFFSET_AccSlotEquippedId    = 0x9A4;      // within a slot
static constexpr uintptr_t OFFSET_AccSlotTargetsHigh   = 0xF60;      // 1 = high byte
static constexpr uintptr_t ADDR_ProfileBlobToSource    = 0x0008F310; // FUN_0048F310
static constexpr uintptr_t ADDR_ProfileApplyToLocal    = 0x0008F810; // FUN_0048F810
static constexpr uintptr_t ADDR_ProfileBlobRebuild     = 0x000924B0; // FUN_004924B0
static constexpr uintptr_t ADDR_ProfileRebuildAndStats = 0x00094810; // FUN_00494810
static constexpr uintptr_t ADDR_AvatarAccessoryDraw    = 0x0033C2C0; // FUN_0073C2C0
static constexpr int MAX_AvatarIcon                    = 0x2F;       // FUN_00510790 - 1
static constexpr int MAX_AvatarColor                   = 0x3;
static constexpr int MAX_AvatarAccessoryId             = 0xCF;       // highest id with art
static constexpr int COUNT_AvtAccPointEntries          = 300;        // hard bounds limit

// ---------------------------------------------------------------------------
// Abyss HP numbers (RE'd 2026-09-05).
//
// HP NUMBERS. Abyss draws the exact HP over each health bar; nothing else does.
// The HUD update reaches that widget through one mode check:
//   005F08FD call 0047E860            ; scene object (+0x108 game mode, +0x10C game state)
//   005F0902 cmp [eax+108h],10h       ; 7 bytes, imm8 form (83 B8 ..), NOT 81 B8
//   005F0909 jne 005F0969             ; short jne, 2 bytes
//   005F095D call 004EA740            ; widget singleton getter -> 0x00EE0338
//   005F0964 call 004EB410            ; draw/update it
// The widget is fed the ratio computed above the check plus the generic per-player
// accessors, so nothing in it is Abyss-only. AbyssHpNumbersGate in hooks_bbcf.cpp
// replaces the cmp and steers the jne. Siblings gated the same way, leave them off:
// 005E7CB0 builds "DEPTH %d", 005E7DA0 builds "REWARD %d".
// ---------------------------------------------------------------------------
static constexpr uintptr_t ADDR_HudAbyssHpNumberCheck  = 0x001F0902; // the cmp
static constexpr uintptr_t ADDR_AbyssHudSingletonGet   = 0x000EA740;
static constexpr uintptr_t ADDR_AbyssHudDraw           = 0x000EB410;
static constexpr uintptr_t ADDR_AbyssHudSingleton      = 0x00AE0338; // 0x00EE0338 VA
static constexpr uintptr_t ADDR_SceneObjectGet         = 0x0007E860; // FUN_0047E860
static constexpr uintptr_t OFFSET_SceneGameMode        = 0x108;
static constexpr uintptr_t OFFSET_SceneGameState       = 0x10C;

// ---------------------------------------------------------------------------------------------
// Engine custom palette slots - the "9 palettes" of Platinum (2026-09-27).
// Full write-up: docs/Research/PlatinumPaletteSlots.md.
//
// The battle scene singleton (FUN_0055C540 returns the static object at VA 0x011B6AE0) holds 40
// reserved custom palette slots: resource handles at +0x178 (40 dwords) and in-use flags at
// +0x218 (40 dwords). An entity reserves one with FUN_00591BC0(this, true): it finds the first
// free flag, stores the index into CharData+0x358 (ownPaletteId) and calls FUN_005787E0, which
// COPIES the entity's palette (FUN_0057C8B0: player container, color*8+fileIndex) into the slot's
// palette object via FUN_004B1620 -> FUN_00452C60. A palette id resolves to (handle, sub 0)
// through FUN_0057CAA0; FUN_005A6C80 is the render-side chooser: own id (+0x358), else the linked
// id (+0x35C), else the player's palette container.
//
// Resource registry (FUN_0040E6B0 returns the static at VA 0x00A23674): +0 = pointer array,
// +8 = count. registry[handle] is a resource whose sub-objects are at +4 (array) / +0xC (count).
// The mod's CharPaletteHandle::m_pPalBaseAddr IS such a resource (the player's palette container):
// its sub-object color*8+file is the palette object the mod has always patched.
//
// Palette object (AA_CPalette): +0xC dirty flag (1 = re-upload to the GPU), +0x1C color data,
// +0x81C second copy of the same data (the "+0x800 duplicate"), +0x101C width (256),
// +0x1020 bits per pixel (32). FUN_00452C60(dst, src) copies both blocks and sets the flag;
// FUN_00452DE0(index, color) sets one entry and sets the flag. Writing the bytes WITHOUT the
// flag is why the 2026-07 full-memory patch of the 8 mirrors changed nothing on screen.
static constexpr uintptr_t ADDR_BattleSceneSingleton        = 0x00DB6AE0; // VA 0x011B6AE0
static constexpr uintptr_t OFFSET_BattleScene_CustomPalHandles = 0x178;   // 40 resource handles
static constexpr uintptr_t OFFSET_BattleScene_CustomPalInUse   = 0x218;   // 40 in-use flags
static constexpr int       BATTLE_CUSTOM_PALETTE_SLOTS       = 40;
static constexpr uintptr_t OFFSET_BattleScene_EntityList     = 0x62784;   // GetEntityListAddr hook stores here
static constexpr uintptr_t ADDR_ResourceRegistry             = 0x00623674; // VA 0x00A23674
static constexpr uintptr_t OFFSET_Resource_SubObjects        = 0x4;
static constexpr uintptr_t OFFSET_Resource_SubObjectCount    = 0xC;
static constexpr uintptr_t OFFSET_PalObj_Dirty               = 0x0C;
static constexpr uintptr_t OFFSET_PalObj_Data                = 0x1C;
static constexpr uintptr_t OFFSET_PalObj_DataDup             = 0x81C;
static constexpr uintptr_t OFFSET_PalObj_Width               = 0x101C;
static constexpr uintptr_t OFFSET_PalObj_Bpp                 = 0x1020;
static constexpr uintptr_t ADDR_BattleSceneGet               = 0x0015C540; // FUN_0055C540

// Player palette storage, as the rewrite uses it (2026-10-18, docs/Decompilation/Palettes.md).
// BattleScene+0x2B8 holds three PlayerResSlot owners of 0x20BDC bytes: 0 = P1, 1 = P2, 2 = shared
// effects (never a 24-colour character container). At the GetPalBaseAddresses hook site ECX is the
// owner, so its slot is (ecx - (BattleScene+0x2B8)) / 0x20BDC. owner+0x830 is the palette container
// (AADX_CDynamicTextureList); the battle destructor zeroes it (FUN_0055C9E0) before freeing it, so
// reading it live is how a stale container is detected. Sub colour*8+file is an AA_CPalette.
//
// Redraw: AADX_CDynamicTextureList::Bind (FUN_0044C880) skips the GPU upload iff
// container+0x40 == sub && pal+0xC == 0. Writing a palette therefore needs pal+0xC = 1 AND
// container+0x40 = -1 (the engine writes -1 itself in OnResetDevice, FUN_0044C360); dirty alone is
// cleared by any PalObjCopy that reads from the player palette before the draw.
static constexpr uintptr_t OFFSET_BattleScene_PlayerResSlots = 0x2B8;
static constexpr uintptr_t SIZE_PlayerResSlot                = 0x20BDC;
static constexpr uintptr_t OFFSET_PlayerRes_PaletteCount     = 0x18;      // lookups clamp to sub 0 at or above this
static constexpr uintptr_t OFFSET_PlayerRes_PaletteContainer = 0x830;
static constexpr uintptr_t ADDR_DynamicTextureListVtable     = 0x00498C2C; // VA 0x00898C2C
static constexpr uintptr_t OFFSET_TexList_LastUploadedSub    = 0x40;
// Online mirror colour bump: `inc al; ...; cmp al,0Ch; sbb al,al; and al,[..]` wraps colour+1 to 0
// at 12. These are the immediate bytes of the two `cmp al,0Ch` (3C 0C): Record_ResolveMirror
// (FUN_0049A2F0, msg 0x13 on the room authority) and Room_AssembleMatchFromMembers (FUN_004989E0).
static constexpr uintptr_t ADDR_MirrorColourWrapImm_Record   = 0x0009A31A; // VA 0x0049A31A
static constexpr uintptr_t ADDR_MirrorColourWrapImm_Room     = 0x000995F1; // VA 0x004995F1
static constexpr uintptr_t ADDR_ResourceRegistryGet          = 0x0000E6B0; // FUN_0040E6B0
static constexpr uintptr_t ADDR_EntityReserveCustomPalette   = 0x00191BC0; // FUN_00591BC0(this, bool)
static constexpr uintptr_t ADDR_EntityRefreshCustomPalette   = 0x001787E0; // FUN_005787E0(this)
static constexpr uintptr_t ADDR_EntityPaletteSource          = 0x0017C8B0; // FUN_0057C8B0
static constexpr uintptr_t ADDR_PaletteIdResolve             = 0x0017CAA0; // FUN_0057CAA0(id, &handle, &sub)
static constexpr uintptr_t ADDR_PaletteCopyByHandle          = 0x000B1620; // FUN_004B1620
static constexpr uintptr_t ADDR_PalObjCopy                   = 0x00052C60; // FUN_00452C60(dst, src)
static constexpr uintptr_t ADDR_PalObjSetEntry               = 0x00052DE0; // FUN_00452DE0(index, color)
static constexpr uintptr_t ADDR_RenderPaletteChooser         = 0x001A6C80; // FUN_005A6C80
static constexpr uintptr_t ADDR_PlatLightCmdCompare          = 0x001B2B7F; // "PT_Light" - reads +0x358, writes no colors
