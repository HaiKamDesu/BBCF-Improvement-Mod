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
// Each AA_CInput device object (keyboard / pad; the "controller pointer" the
// SystemInputWrite hook sees in ESI) keeps three per-frame LOGICAL ACTION
// bitmask words, one bit per keyconfig action index (1 << actionIndex,
// actionCount at device+0x38, typically 30):
//   device+0x28 = actions JUST PRESSED this frame (edge)
//   device+0x2C = actions RELEASED this frame (edge)
//   device+0x30 = actions HELD this frame (level)
// Built by FUN_004963F0 (per-action loop calling vtbl+8 with the action
// index, i.e. keyconfig-resolved "is physical binding down"). The mod's
// SystemInputWrite hook at base+0x96408 sits at the loop's zero-init
// instruction, so EBX there is always 0 - to observe real values read the
// words AFTER FUN_004963F0 returns (e.g. from a per-frame tick).
// ---------------------------------------------------------------------------
// Ghidra FUN_004963F0 - per-frame device action-word build (thiscall, ecx=device)
static constexpr uintptr_t ADDR_InputDevice_BuildActionWords = 0x000963F0;
// Ghidra FUN_004968E0 - packs device+0x30 into the 16-bit battle input word
// (digit 1-9 from bits0-3; A=bit14-mask 0x3404000, B=0x3C08000, C=0x3802000,
// D=0x2001000, taunt=0x20000, special=0x80000). The packed battle word can
// NEVER carry more than 0x3FF => training reset does not travel through the
// BattleInputWrite word.
static constexpr uintptr_t ADDR_InputDevice_PackBattleWord = 0x000968E0;
// Action-ID -> action-bit lookup table (31 dwords, .data). ActionIDs are the
// small ints pushed to the check wrappers below (5=menu confirm 0x4000000,
// 6=pause/start 0x8000000, 7-10=up/left/down/right 1/4/8/2, 11=B 0x8000,
// 12=D 0x1000, 13=C 0x2000, 14=A 0x4000, 16=taunt 0x20000, 18=special 0x80000,
// replay-shortcut ids 0x0B-0x14 map to 0x10000..0x200000 range).
static constexpr uintptr_t ADDR_ActionIdMaskTable = 0x005DE0B0;
// Check wrappers (thiscall, ecx = per-controller wrapper from FUN_0047E7B0(id),
// which fans out to both devices of that controller):
// FUN_004641B0(actionId) = just-pressed (maskTable[id] & device+0x28)
static constexpr uintptr_t ADDR_ControllerWrapper_ActionJustPressed = 0x000641B0;
// FUN_00464170(actionId) = pressed-or-key-repeat (menu navigation variant)
static constexpr uintptr_t ADDR_ControllerWrapper_ActionRepeatPressed = 0x00064170;
// FUN_004640E0(rawMask) = just-pressed by raw bitmask (device+0x28 & mask)
static constexpr uintptr_t ADDR_ControllerWrapper_MaskJustPressed = 0x000640E0;
// FUN_0047E7B0(controllerId) cdecl - controller wrapper by id;
// engine+0x25F0 = active controller id, engine+0x25F4/0x25F8 = P1/P2 ids
// (engine object from FUN_0047E860, the same object whose +0x108 is gameMode)
static constexpr uintptr_t ADDR_GetControllerWrapperById = 0x0007E7B0;

// Training/round reset execution (verified, binding-agnostic hook points):
// OBJ_CBase/OBJ_CCharBase script-command vtable slot 1066 (byte offset 0x10A8;
// vtables at .rdata 0x9509E4 / 0x9565F4). FUN_0058F390 / FUN_0058F4D0 /
// FUN_0058F610 are three restart variants; each first clears flag bit
// 0x4000000 in *(battleScene+0x62B7C) (battleScene from FUN_0055C540), then
// restarts the round-flow task container at 0xEC2F98 (task base class:
// +0x8=state, +0xC=frame counter - FUN_004D23E0 sets state=2/frame=0, which
// is why the in-match frame counter jumps back on training reset).
static constexpr uintptr_t ADDR_RoundRestart_VariantA = 0x0018F390;
static constexpr uintptr_t ADDR_RoundRestart_VariantB = 0x0018F4D0;
static constexpr uintptr_t ADDR_RoundRestart_VariantC = 0x0018F610; // vtable slot 1066
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
// SystemManager (base+0x8929C8) controller slots (filled by the creators):
//   +0x0C = keyboard SystemKeyControler   +0x18 = keyboard BattleKeyControler
//   +0x10+4*slot = pad ctrl set A (also aliased at +0x1C+4*slot)
//   +0x24+4*slot = pad ctrl set B (returned by FUN_004C1380(sysMgr, slot))
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
// hooks_palette.cpp hooks all 8 reads and substitutes the chosen personality IN-REGISTER
// (never memory) for the local player's Platinum. The mounted XACT bank + voice handle
// (char+0x1E9E4) are per-client heap, never in the GGPO checksum -> desync-safe vs anyone.
// The audio system is XACT (AA_CWaveBankDataBase_XACT::RegistBank @0x89923F), not CriWare.
