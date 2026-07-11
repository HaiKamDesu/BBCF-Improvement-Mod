#pragma once
/*


SCENE_CVSInfo that is input to the  touches_on_GameSceneState_FUN_00a8f9b0 func

function to change scene probably in CScene_Controller class


Check GAME_CEventCOntrol task because it probably is the thing that is controlling the order of SCENE change and stuff, and its factory. 
It is one of the arguments for the function that seems to do the scene change 
I can manipulate the scene change sometimes forcing by changing the arguments in the function (and remembering to set the GAMESTATE to be set in the minidump args in cheat engine)




room is created with the class GAME_CNetworkTask
the function that seems to call for the creation of the room(base+a56e0) has param1 = GAMESTEAM_CNetworkServer


D-Code load failure / ranked progress rollback bug: full writeup in
docs/Research/DCodeNetworkStallBug.md. Short version: per-room-member async
fetch state machine (state field at *(int*)(*(int*)(row+0x68a0)+0xcc), row =
get_NetUserData()+0x2326c+slot*0x68a4) can get permanently stuck at state 2
("request sent, awaiting completion") with no timeout and no retry if the
underlying P2P exchange silently drops. FUN_004A25C0 drives the state
machine, FUN_0041CCF0 is the "hDCODE" Steam UGC fetch that uses the same
shape of gate (FUN_00407C90). Proposed fix: watchdog that force-resets a
stuck state==2 slot back to 0 after a timeout so the game's own existing
retry path (FUN_0049D560 / FUN_004A1AB0) kicks back in. Not yet confirmed
whether the same stuck state blocks the ranked-progress save-to-disk path
(CSaveDataManager set_next_SaveUtil_action_0_write) — next step is
decompiling FUN_00656490 (0x006567FA -> FUN_004B4360).
*/