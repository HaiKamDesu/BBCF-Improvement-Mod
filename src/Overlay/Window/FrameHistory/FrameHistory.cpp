#include "FrameHistory.h"
#include "Game/Playbacks/UnlimitedPlaybackManager.h"
#include "Game/TrainingSide.h"
#include "Overlay/Window/FrameAdvantage/PlayerExtendedData.h"
#include <cstddef>
#include "Core/logger.h"
#include "Game/characters.h"
#include "Overlay/Logger/ImGuiLogger.h"
#define MAX(a,b)            (((a) > (b)) ? (a) : (b))


// thanks to PCVolt
const std::vector<std::string> idleWords = {
    // now classified under "Special"
    // "CmnActFDash",

    
    "_NEUTRAL", "CmnActStand", "CmnActStandTurn", "CmnActStand2Crouch",
    "CmnActCrouch", "CmnActCrouchTurn", "CmnActCrouch2Stand", "CmnActFWalk",
    "CmnActBWalk", "CmnActFDashStop", "CmnActJumpUpper",
    "CmnActJumpDown", "CmnActJumpUpperEnd", "CmnActJumpLanding",
    "CmnActLandingStiffEnd",
    "CmnActUkemiLandNLanding", // to fix, 12F too long!
    // Proxi block is triggered when an attack is closing in without being
    // actually blocked If the player.blockstun is = 0, then those animations
    // are still considered idle
    "CmnActCrouchGuardPre", "CmnActCrouchGuardLoop", "CmnActCrouchGuardEnd", // Crouch
    "CmnActCrouchHeavyGuardPre", "CmnActCrouchHeavyGuardLoop", "CmnActCrouchHeavyGuardEnd", // Crouch Heavy
    "CmnActMidGuardPre", "CmnActMidGuardLoop", "CmnActMidGuardEnd", // Mid
    "CmnActMidHeavyGuardPre", "CmnActMidHeavyGuardLoop", "CmnActMidHeavyGuardEnd", // Mid Heavy
    "CmnActHighGuardPre", "CmnActHighGuardLoop", "CmnActHighGuardEnd", // High
    "CmnActHighHeavyGuardPre", "CmnActHighHeavyGuardLoop", "CmnActHighHeavyGuardEnd", // High Heavy
    "CmnActAirGuardPre", "CmnActAirGuardLoop", "CmnActAirGuardEnd", // Air
    // Character specifics
    "com3_kamae" // Mai 5xB stance
};

// TODO: Make this use arbitrary bases vectors
std::array<float, 3> attributetoColor(Attribute attr, std::array<Attribute, 3> rgb_attr) {
    std::array<float, 3> res = {};
    
    res[0] = static_cast<int>(rgb_attr[0] & attr) > 0;
    res[1] = static_cast<int>(rgb_attr[1] & attr) > 0;
    res[2] = static_cast<int>(rgb_attr[2] & attr) > 0;
    
    return res;
}

std::array<float, 3> kindtoColor(FrameKind kind) {
    std::array<float, 3> res;
    // it's not so clear cut. Moves that don't let you be actionable in air may still be classified as idle,
    // TODO: because we remove hardlanding from immediate classification. Needs more testing
    FrameKind cleankind = kind & ~(static_cast<FrameKind>(0x40) | FrameKind::HardLanding);
    switch (cleankind)
    {
    case FrameKind::Idle:
        if (static_cast<int>(kind & FrameKind::HardLanding) != 0) {
            res = BLUSH;
        }
        else {
            res = BLACK;
        }
        break;
    case FrameKind::Startup:
        res = GREEN;
        break;
    case FrameKind::Recovery:
        if (static_cast<int>(kind & FrameKind::HardLanding) != 0) {
            res = BLUSH;
        }
        else {
            res = BLUE;
        }
        break;
    case FrameKind::Active:
        res = RED;
        break;
    case FrameKind::Blockstun:
        res = YELLOW;
        break;
    case FrameKind::Hitstun:
        res = PURPLE;
        break;
    case FrameKind::Special:
        res = AQUAMARINE;
        break;
    default:
        if (static_cast<int>(cleankind & FrameKind::Active) != 0) {
            res = RED;
        }

        else if (static_cast<int>(cleankind & FrameKind::Startup) != 0) {
            res = GREEN;
        }
        else if (static_cast<int>(cleankind & FrameKind::Recovery) != 0) {
            res = PURPLE;
        }
        else if (static_cast<int>(cleankind & FrameKind::Disarmed) != 0) {
            res = BYZANTIUM;

        }
        else {
            res = BURGUNDY;
        }
        break;
    }
    return res;
}

int first_det_active(std::vector<FrameActivity>& activity_status) {
    for (size_t i = 0; i < activity_status.size(); i++) {
        if (activity_status[i] == FrameActivity::Active) {
            return i;
        }
    }
    return -1;
}


Attribute parse_dyn_invul(uint32 invul_field, uint32 gp_bitfield) {
    if ((invul_field & 0x02) == 0) {
        return Attribute::N;
    }

    Attribute invul = Attribute::N;

    if (gp_bitfield & 0x01)     invul = invul | Attribute::GP;
    if (invul_field & 0x08)     invul = invul | Attribute::H;
    if (invul_field & 0x10)     invul = invul | Attribute::B;
    if (invul_field & 0x20)     invul = invul | Attribute::F;
    if (invul_field & 0x80000)  invul = invul | Attribute::P;
    if (invul_field & 0x40)     invul = invul | Attribute::T;



    return invul;
}

PlayerFrameState::PlayerFrameState(scrState* state, unsigned int frame,
    CharData* player, BackedUpCharData old_data) {

    // set state variables
    invul = parse_dyn_invul(player->invuln_bitfield, player->guard_point_bitfield);
    is_new = frame == 0;
    
    
    // Set kind
    std::string currentAction = std::string(player->currentAction);
    int fst_det_active;
    Attribute det_invul = Attribute::N;
    bool is_idle_state = std::find(std::begin(idleWords), std::end(idleWords), currentAction) != std::end(idleWords);
    
    if (state == nullptr) {
        fst_det_active = -1;
    }
    else {
        fst_det_active = first_det_active(state->frame_activity_status);
    }
    

    if (is_new && currentAction == "CmnActUkemiLandNLanding") {
        kind = FrameKind::Recovery;
    }
    else if (is_idle_state) {
        kind = FrameKind::Idle;
    }
    else {
        kind = FrameKind::Special;
    }
    if (player->hitboxCount > 0
        && (player->bitflags_for_curr_state_properties_or_smth & (0x400 | 0x200)) == 0) {
        kind = FrameKind::Active | kind;
    }
    if (player->blockstun > 0) {
        kind = FrameKind::Blockstun | kind;
    }
    if (player->hitstun > 0) {
        kind = FrameKind::Hitstun | kind;
    }

    // hardlanding is set even if the player is still airborn. We only want to flag the *landing* portion
    if (/*player->hardLandingRecovery > 0 && player->position_y + old_data.position_y == 0 && */currentAction == "CmnActLandingStiffLoop") {
        kind = FrameKind::HardLanding | kind;
    }
    // NOTE: Startup is only defined in a context with deterministic active frames
    if (fst_det_active > -1
        && (player->hitboxCount <= 0 || (player->bitflags_for_curr_state_properties_or_smth & (0x400 | 0x200)) == 0)
        && !is_idle_state) {
        
        if (frame < fst_det_active) {
            kind = FrameKind::Startup | kind;
        }
        else {
            kind = FrameKind::Recovery | kind;
        }
    }

    // Clear the "Special" specification, if the state is already well defined
    if ((kind & FrameKind::Disarmed) != FrameKind::Idle) {
        kind = FrameKind::NotSpecial & kind;
    }


    return;
}
PlayerFrameState::PlayerFrameState()
{
}

bool FrameHistory::getPlayerFrameStates(CharData* player1,
    CharData* player2, StatePair* res) {
    // get the state
    // we need the amount of frames spent on the current state, in order to index into the state frames.
    // TODO: Might want to count frames since last update, and add those to the p1_frames. However, what if a state was changed in between updates, then we can't know.
    // This is all the more reason to query states purely dynamically.
    std::string currentAction = std::string(player1->currentAction);
    // If the actionTime hasn't yet changed, don't register this frame.
    bool condition1 = p1_frames == player1->actionTime - 1;
    if (player1->stateChangedCount != p1_stateChangedCount) {
        // if it is not, fetch the new states from the map
        auto p1_new_state = p1_StateMap.find(currentAction);
        if (p1_new_state != p1_StateMap.end()) {
            p1_State = p1_StateMap.at(currentAction);
        }
        else {
            p1_State = nullptr;
        }
        p1_frames = 0;
    }
    else {
        // could instead use the difference between local and global framecounts, to
        // increment this. If there is hitstop, do not add a frame.
        p1_frames = player1->actionTime - 1;
    }
    currentAction = std::string(player2->currentAction);
    bool condition2 = p2_frames == player2->actionTime - 1;
    if (player2->stateChangedCount != p2_stateChangedCount) {
        auto p2_new_state = p2_StateMap.find(currentAction);
        if (p2_new_state != p2_StateMap.end()) {
            p2_State = p2_StateMap.at(currentAction);
        }
        else {
            p2_State = nullptr;
        }
        p2_frames = 0;
    }
    else {
        p2_frames = player2->actionTime - 1;
    }

    // Note that frame calculation is first performed, because we don't want to
    // miss a state switch because of hitstop

    // Only skip writing to the grid, if both players are experiencing hitstop
    // (might be redundant)
    if (condition1 && condition2) {
        return false;
    }
    else {
        *res = { PlayerFrameState(p1_State, p1_frames, player1, p1_old_data),
            PlayerFrameState(p2_State, p2_frames, player2, p2_old_data) };
        return true;
    }
}

const char* kindToString(FrameKind kind) {
    if (static_cast<int>(kind & FrameKind::Blockstun) != 0) return "Blockstun";
    if (static_cast<int>(kind & FrameKind::Hitstun) != 0) return "Hitstun";
    if ((static_cast<int>(kind) & static_cast<int>(FrameKind::NonDeterministicActive)) == static_cast<int>(FrameKind::NonDeterministicActive)) return "Active (non-det.)";
    if (static_cast<int>(kind & FrameKind::Active) != 0) return "Active";
    if (static_cast<int>(kind & FrameKind::Startup) != 0) return "Startup";
    if ((static_cast<int>(kind) & static_cast<int>(FrameKind::NonDeterministicRecovery)) == static_cast<int>(FrameKind::NonDeterministicRecovery)) return "Recovery (non-det.)";
    if (static_cast<int>(kind & FrameKind::Recovery) != 0) return "Recovery";
    if (static_cast<int>(kind & FrameKind::HardLanding) != 0) return "Hard Landing";
    if (static_cast<int>(kind & FrameKind::Special) != 0) return "Special";
    return "Idle";
}

// The bar draws these as unlabelled slices, so spell the attributes out rather
// than handing back the one-letter codes nobody can decipher.
std::string attributeToString(Attribute attr) {
    if (attr == Attribute::N) return "None";
    std::string res;
    auto append = [&res](const char* name) {
        if (!res.empty()) res += ", ";
        res += name;
    };
    if (bool(attr & Attribute::GP)) append("Guard Point");
    if (bool(attr & Attribute::H))  append("Head");
    if (bool(attr & Attribute::B))  append("Body");
    if (bool(attr & Attribute::F))  append("Foot");
    if (bool(attr & Attribute::T))  append("Throw");
    if (bool(attr & Attribute::P))  append("Projectile");
    return res;
}

/// update the history queue with the new player states. Only call after the
/// game time has moved and by NO MORE than 1 frame
void FrameHistory::updateHistory(bool resetting, bool countEmptyFrames, int maxHistoryFrames) {
    CharData* p1 = g_interfaces.player1.GetData();
    CharData* p2 = g_interfaces.player2.GetData();

    if (p1->charIndex != p1_charIndex || p2->charIndex != p2_charIndex) {
        loadCharData();
    }

    // update the player states, return their parsed versions
    StatePair states = {   };

    if (!getPlayerFrameStates(p1, p2, &states)) {
        return;
    }
    ++m_columnSerial;

    // The trigger markers for this frame, on the dummy's row.
    //
    // Which frame the events describe was established from the disassembly (2026-09-27,
    // docs/Research/TriggerTimingInvestigation.md "Frame ordering"), not assumed:
    //
    //   per-tick Update FUN_0056B1F0
    //     -> FUN_005521B0 (0x56B52D): walks the object list, FUN_005DA7A0 per object -
    //        the players' scripts advance HERE (currentAction/actionTime/hitstun change),
    //        then, last thing at 0x552322, steps the round-flow task container FUN_004D27D0
    //     -> FUN_004D2400 (its only caller): the GetFrameCounter hook sits on the first two
    //        instructions ("mov eax,[esi]; inc [esi+0Ch]"), so UnlimitedPlaybackManager::Tick
    //        runs with the counter still at N, the players already at frame N+1's state;
    //        then the counter becomes N+1 and the task's own update ([vtbl+0x10] ->
    //        FUN_005613E0 / MatchInfo::update_Fight FUN_0055F780, reachable only through
    //        that vtable) runs the round flow.
    //   render (EndScene) -> this function: sees counter N+1 and the same N+1 state.
    //
    // So events stamped with frame N describe the state this column samples when
    // *pFrameCount == N+1. The equality this used to require (events frame == current frame)
    // could therefore never hold, which is why no marker ever appeared. Both offsets are
    // accepted, and the one seen is logged once, so a change in the game's order would show
    // up in DEBUG.txt rather than as silently missing markers.
    {
        static_assert(PlayerFrameState::kTriggerSlots == UnlimitedPlaybackManager::Trigger_Count,
            "PlayerFrameState::triggerMarks must hold one byte per trigger");
        const int currentFrame = g_gameVals.pFrameCount ? static_cast<int>(*g_gameVals.pFrameCount) : -1;
        states[0].gameFrame = currentFrame;
        states[1].gameFrame = currentFrame;
        states[0].actionTime = p1->actionTime;
        states[1].actionTime = p2->actionTime;
        const UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();
        const int eventsFrame = playback.GetTriggerFrameEventsFrame();
        const bool provenOffset = (eventsFrame >= 0 && eventsFrame == currentFrame - 1);
        const bool sameFrame = (eventsFrame >= 0 && eventsFrame == currentFrame);
        if (provenOffset || sameFrame) {
            static int loggedOffset = -99;
            const int offset = currentFrame - eventsFrame;
            if (loggedOffset != offset) {
                loggedOffset = offset;
                LOG(1, "[FrameHistory] trigger events run %d frame(s) behind the counter the history sees%s\n",
                    offset, offset == 1 ? " (as proven from the tick order)" : " (UNEXPECTED - the tick order changed?)");
            }
            PlayerFrameState& dummyState = states[TrainingSide::DummyIndex()];
            const auto& events = playback.GetTriggerFrameEvents();
            for (int i = 0; i < UnlimitedPlaybackManager::Trigger_Count; ++i) {
                unsigned char mark = 0;
                if (events[i].windowStart) mark |= PlayerFrameState::Mark_WindowStart;
                if (events[i].actionable)  mark |= PlayerFrameState::Mark_Actionable;
                if (events[i].detected)    mark |= PlayerFrameState::Mark_Detected;
                // Fired is NOT drawn on the frame the code ran on but on the next one: the
                // playback (or forced state) it starts is first consumed by the following
                // tick, so that is the first frame the inputs are applied on. The marker
                // means "inputs start here", and with a -N delay the move then starts N
                // squares after it when it comes out on the first actionable frame.
                static int firedTargetFrame[UnlimitedPlaybackManager::Trigger_Count] = {
                    -1, -1, -1, -1, -1, -1, -1, -1 };
                if (events[i].fired) {
                    firedTargetFrame[i] = eventsFrame + 1;
                }
                else if (firedTargetFrame[i] >= 0 && eventsFrame >= firedTargetFrame[i]) {
                    mark |= PlayerFrameState::Mark_Fired;
                    if (eventsFrame != firedTargetFrame[i]) {
                        LOG(1, "[FrameHistory] fired marker for trigger %d drawn %d frame(s) late (history skipped a frame)\n",
                            i, eventsFrame - firedTargetFrame[i]);
                    }
                    firedTargetFrame[i] = -1;
                }
                dummyState.triggerMarks[i] = mark;
                if (events[i].windowStart && events[i].stateName) dummyState.triggerState = events[i].stateName;

                // Evidence for the marker count: which column each marker landed on, by the
                // dummy's actionTime there, and how many history columns separate a firing
                // from the actionable frame that follows it. Columns, not frames, because the
                // columns are what the user counts.
                static int columnOfFired[UnlimitedPlaybackManager::Trigger_Count] = {};
                static bool haveFired[UnlimitedPlaybackManager::Trigger_Count] = {};
                if (mark & PlayerFrameState::Mark_Fired) {
                    columnOfFired[i] = m_columnSerial;
                    haveFired[i] = true;
                    LOG(1, "[FrameHistory] marker fired (first input applied): trigger %d on column %d (game frame %d, dummy %s actionTime %d)\n",
                        i, m_columnSerial, eventsFrame, TrainingSide::Dummy().GetData()->currentAction, dummyState.actionTime);
                }
                if (events[i].actionable) {
                    LOG(1, "[FrameHistory] marker actionable (first frame a move can start): trigger %d on column %d (game frame %d, dummy %s actionTime %d)%s\n",
                        i, m_columnSerial, eventsFrame, TrainingSide::Dummy().GetData()->currentAction, dummyState.actionTime,
                        haveFired[i] ? FormatText(" - %d column(s) after the fired marker", m_columnSerial - columnOfFired[i]).c_str() : "");
                    haveFired[i] = false;
                }
                if (events[i].windowStart) {
                    LOG(1, "[FrameHistory] marker window began: trigger %d on column %d (game frame %d, dummy actionTime %d)\n",
                        i, m_columnSerial, eventsFrame, dummyState.actionTime);
                }
            }
        }
    }

    // sync up everything
    p1_frameCountMinus_1 = p1->frame_count_minus_1;
    p2_frameCountMinus_1 = p2->frame_count_minus_1;
    p1_stateChangedCount = p1->stateChangedCount;
    p2_stateChangedCount = p2->stateChangedCount;


    // TODO: If you add attack and guardp, add them to this condition
    // Reset on completely idle frames
    bool bothIdle = (states[0].kind == FrameKind::Idle && states[0].invul == Attribute::N)
                 && (states[1].kind == FrameKind::Idle && states[1].invul == Attribute::N)
                 && !states[0].HasTriggerMarks() && !states[1].HasTriggerMarks();

    if (bothIdle) {
        is_old = true;
        if (countEmptyFrames) {
            while (queue.size() >= (size_t)maxHistoryFrames) {
                queue.pop_front();
            }
            queue.push_back(states);
        }
    }
    else {
        // if something is happening, push the info. But not before clearing out
        if (is_old && resetting) {
            queue.clear();
        }

        while (queue.size() >= (size_t)maxHistoryFrames) {
            queue.pop_front();
        }

        queue.push_back(states);
        is_old = false;
    }
    // after doing updates, store this information for later.
    backupChars();
}

void FrameHistory::backupChars()
{
    CharData* p1 = g_interfaces.player1.GetData();
    CharData* p2 = g_interfaces.player2.GetData();
    p1_old_data = BackedUpCharData();
    p2_old_data = BackedUpCharData();
    p1_old_data.position_y = p1->position_y;
    p2_old_data.position_y = p2->position_y;
}

StatePairQueue& FrameHistory::read() { return queue; }

// TODO: Add safety checks
void FrameHistory::loadCharData() {
    CharData* p1 = g_interfaces.player1.GetData();
    CharData* p2 = g_interfaces.player2.GetData();

    p1_charIndex = p1->charIndex;
    p2_charIndex = p2->charIndex;
    p1_frameCountMinus_1 = p1->frame_count_minus_1;
    p2_frameCountMinus_1 = p2->frame_count_minus_1;
    p1_stateChangedCount = p1->stateChangedCount - 1;
    p2_stateChangedCount = p2->stateChangedCount - 1;

    char* bbcf_base_adress = GetBbcfBaseAdress();

    // The previous character's states are about to be replaced. They used to be neither freed nor
    // unmapped, so each character change leaked a script's worth of states and left names unique to
    // the old character still resolvable - a state could be classified against the wrong script.
    // p1_State/p2_State point into these maps, so they have to let go first.
    p1_State = nullptr;
    p2_State = nullptr;
    p1_StateMap.clear();
    p2_StateMap.clear();
    p1_OwnedStates.clear();
    p2_OwnedStates.clear();

    const bool isMirrorMatch = p1_charIndex == p2_charIndex;

    std::vector<scrState*> states_p1 = parse_scr(bbcf_base_adress, 1);

    // In a mirror both players run the same script, so player 1's slot is parsed a second time
    // rather than reading player 2's. The two passes are kept separate on purpose: each player's
    // map owns its own scrState objects, so neither can free the other's out from under it.
    if (isMirrorMatch) {
        LOG(2, "[Scr] Mirror match (%s on both sides): parsing player 1's script a second time "
               "for player 2's state map.\n",
            getCharacterNameByIndexA(p1_charIndex).c_str());
    }

    std::vector<scrState*> states_p2 = parse_scr(bbcf_base_adress, isMirrorMatch ? 1 : 2);

    // Take ownership as well as mapping them. A duplicate state name overwrites the map entry but
    // both objects stay owned exactly once, so nothing is freed twice.
    for (scrState* state : states_p1) {
        p1_StateMap[state->name] = state;
        p1_OwnedStates.emplace_back(state);
    }

    for (scrState* state : states_p2) {
        p2_StateMap[state->name] = state;
        p2_OwnedStates.emplace_back(state);
    }

    LOG(2, "[Scr] Frame history state maps loaded: P1 %zu states, P2 %zu states.\n",
        p1_StateMap.size(), p2_StateMap.size());
}

void FrameHistory::clear() { queue.clear(); }

FrameHistory::FrameHistory() {
    queue = std::deque<StatePair>();
    p1_old_data = BackedUpCharData();
    p2_old_data = BackedUpCharData();
}

FrameHistory::~FrameHistory() {}
