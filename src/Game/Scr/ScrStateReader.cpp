#pragma once
#include "ScrStateReader.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Game/characters.h"
#include "CmdList.h"
#include <chrono>
#include <iostream>
#include <fstream>
#include <algorithm>


// Per-parse telemetry, summarised into DEBUG.txt at the end of every parse_scr. These are the
// numbers that say whether a script walk was healthy:
//
//  - a state that aborts on an unknown command stops contributing frame data from that point on,
//    so frame history silently mis-classifies the rest of the move;
//  - widestRetroSpan is the widest range one of the retroactive commands (22007/22019/2002/23027)
//    actually rewrote. It must stay within the frame vector. It reaching 4294967295 on Litchi's
//    RodEventDown is what froze the game for billions of iterations the first time frame history
//    was opened, so it is logged every time rather than left to be rediscovered;
//  - elapsed ms is the direct regression check: this is a one-shot cost paid on the render thread.
struct ScrParseStats
{
	int statesParsed = 0;
	int statesAborted = 0;
	unsigned long firstUnknownCmd = 0;
	std::string firstUnknownState;
	unsigned int spriteCmds = 0;
	unsigned int heldSprites = 0;
	unsigned int frameEntries = 0;
	unsigned int longestState = 0;
	unsigned int widestRetroSpan = 0;
};

static ScrParseStats g_scrStats;

// Records how wide one of the retroactive rewrites actually ran. Compared against the longest state
// at the end of the parse; if it ever exceeds it, the loop bound and the vector have drifted apart.
static void RecordRetroSpan(unsigned int prev_frames, size_t vectorSize)
{
	const unsigned int span = (vectorSize > prev_frames) ? (unsigned int)(vectorSize - prev_frames) : 0;
	if (span > g_scrStats.widestRetroSpan)
	{
		g_scrStats.widestRetroSpan = span;
	}
}

// Longest sprite duration still treated as a real animation length. Anything past this is the
// script holding a sprite rather than playing one, and is recorded as a single non-deterministic
// frame instead of being expanded.
constexpr uint32_t SPRITE_FRAME_CAP = 1000;

constexpr auto OFFSET_FROM_FPAC = 0x60;
//constexpr auto EA_PTR_OFFSET_FROM_FPAC = 0x54; //differently from OFFSET_FROM_FPAC, this is an offset to an adress which holds the actual offset, and this offset is from the beginning of the pre_inint, so +0x60 needs to be added to it
constexpr auto FPAC_OFFSET_FROM_BBCF_P1 = 0x88E6F0;
constexpr auto FPAC_OFFSET_FROM_BBCF_P2 = 0x88E750;
constexpr auto PREINIT_OFFSET_FROM_BBCF_P1 = 0xDB6B2C;
constexpr auto PREINIT_OFFSET_FROM_BBCF_P2 = 0xDB6B5C;

constexpr auto EA_INDEX_OFFSET_FROM_BBCF_P1 = 0xDB6B40;
constexpr auto EA_PREINIT_OFFSET_FROM_BBCF_P1 = 0xDB6B44; //there is no pre_init in the ea file, but its meaning is the first state definition
constexpr auto EA_INDEX_OFFSET_FROM_BBCF_P2 = 0xDB6B70;
constexpr auto EA_PREINIT_OFFSET_FROM_BBCF_P2 = 0xDB6B74; //there is no pre_init in the ea file, but its meaning is the first state definition
/*the size of each index entry at least in the ea file is 36 bytes with the offset from first state def starting at 
byte 32 and going to byte 36. the total amount of states is in the first 4 bytes of the index, so to skip the index 
and reach the start of the states definitions you need to do (36 * total n of states).*/

std::vector<scrState*> parse_scr(char* bbcf_base_addr, int player_num) {
	CharData* p1 = g_interfaces.player1.GetData();
	CharData* p2 = g_interfaces.player2.GetData();

	// There used to be a bail here returning nothing when both players had the same charIndex.
	// It defeated its own caller: loadCharData already handles a mirror by asking for player 1's
	// script twice, so the bail turned every mirror match into two empty maps, leaving frame
	// history with no script to classify against and nothing but Idle/Special on both rows for
	// the whole match. Player 1's slot is populated in a mirror like any other, so parsing it is
	// no more dangerous than parsing it in a non-mirror.

	const auto parseStartTime = std::chrono::steady_clock::now();
	g_scrStats = ScrParseStats();

	char** fpac_load = NULL;
	char* scr_index = NULL;
	char** scr_preinit_offset = NULL;

	char* ea_scr_index = NULL;
	char** ea_scr_preinit_offset = NULL;

	int offset = NULL;
	if (player_num == 2) {
		fpac_load = (char**)(bbcf_base_addr + FPAC_OFFSET_FROM_BBCF_P2);
		scr_index = *fpac_load + OFFSET_FROM_FPAC;
		scr_preinit_offset = (char**)(bbcf_base_addr + PREINIT_OFFSET_FROM_BBCF_P2);

		ea_scr_index = *(char**)(bbcf_base_addr + EA_INDEX_OFFSET_FROM_BBCF_P2);
		ea_scr_preinit_offset = (char**)(bbcf_base_addr + EA_PREINIT_OFFSET_FROM_BBCF_P2);
	}
	else if (player_num == 1) {
		
		fpac_load = (char**)(bbcf_base_addr + FPAC_OFFSET_FROM_BBCF_P1);
		scr_index = *fpac_load + OFFSET_FROM_FPAC;
		scr_preinit_offset = (char**)(bbcf_base_addr + PREINIT_OFFSET_FROM_BBCF_P1);

		ea_scr_index = *(char**)(bbcf_base_addr + EA_INDEX_OFFSET_FROM_BBCF_P1);
		ea_scr_preinit_offset = (char**)(bbcf_base_addr + EA_PREINIT_OFFSET_FROM_BBCF_P1);

		

	}
	else {
		return std::vector<scrState*>{};
	}
	std::map<std::string, JonbDBEntry> jonbin_map = JonbDBReader().parse_all_jonbins(bbcf_base_addr, player_num);
	std::vector<scrState*> states_parsed;
	std::vector<scrState*> ea_states_parsed;
	/*doing the EA before the main states*/
	std::map<std::string, scrState*> ea_state_map = {};  //ea_sstate_map to reference in the main state parsing. This way recursive ea_states(ea states called from ea state) won't work, I need to find a better way later.
	int ea_n_funcs;
	int ea_func_num = 0;
	int ea_i = 0;
	memcpy(&ea_n_funcs, ea_scr_index, 4);
	ea_i += 4;
	while (ea_func_num < ea_n_funcs - 1) {
		char ea_name_index[32];
		int ea_pos_before_offset;
		memcpy(ea_name_index, ea_scr_index + ea_i, 32);
		ea_i += 32;
		memcpy(&ea_pos_before_offset, ea_scr_index + ea_i, 4);
		ea_i += 4;

		char* ea_addr = (*ea_scr_preinit_offset + ea_pos_before_offset);
		parse_state(ea_addr, ea_states_parsed, &jonbin_map, &ea_state_map);
		ea_func_num += 1;
	}

	//snapshot so the EA and main passes can be reported separately - Litchi's staff lives in the EA
	const ScrParseStats eaStats = g_scrStats;

	//builds ea_state_map to reference in the main state parsing.
	for (auto& state : ea_states_parsed) {
		ea_state_map[state->name] =  state ;
	}

	/*ending the EA*/

		int n_funcs;
		int func_num = 0;
		int i = 0;
		memcpy(&n_funcs, scr_index, 4);
		i += 4;
		while (func_num < n_funcs - 1) {
			char name_index[32];
			int pos_before_offset;
			memcpy(name_index, scr_index + i, 32);
			i += 32;
			memcpy(&pos_before_offset, scr_index + i, 4);
			i += 4;

			char* addr = (*scr_preinit_offset+ pos_before_offset);
			parse_state(addr, states_parsed, &jonbin_map, &ea_state_map);
			func_num += 1;
		}



	//states_parsed.insert(states_parsed.end(), ea_states_parsed.begin(), ea_states_parsed.end());

	{
		const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - parseStartTime).count();

		CharData* parsed = (player_num == 2) ? p2 : p1;
		const int charIndex = parsed ? parsed->charIndex : -1;

		LOG(2, "[Scr] P%d %s (charIndex %d) parsed in %lld ms | main %d states (%d aborted) | "
		       "EA %d states (%d aborted) | %u sprite cmds, %u held | %u frame entries, "
		       "longest state %u | widest retro span %u | %zu jonbins\n",
			player_num,
			charIndex >= 0 ? getCharacterNameByIndexA(charIndex).c_str() : "?",
			charIndex,
			elapsedMs,
			g_scrStats.statesParsed - eaStats.statesParsed,
			g_scrStats.statesAborted - eaStats.statesAborted,
			eaStats.statesParsed,
			eaStats.statesAborted,
			g_scrStats.spriteCmds,
			g_scrStats.heldSprites,
			g_scrStats.frameEntries,
			g_scrStats.longestState,
			g_scrStats.widestRetroSpan,
			jonbin_map.size());

		// A retro span wider than the longest state means the bound and the vector have drifted
		// apart again, which is the exact shape of the freeze. Shout about it, do not bury it in
		// the summary above.
		if (g_scrStats.widestRetroSpan > g_scrStats.longestState)
		{
			LOG(0, "[Scr] WARNING: widest retroactive span (%u) exceeds the longest state's frame "
			       "vector (%u). The retroactive loops are indexing past their data again.\n",
				g_scrStats.widestRetroSpan, g_scrStats.longestState);
		}

		if (g_scrStats.statesAborted > 0)
		{
			LOG(2, "[Scr] P%d: %d state(s) stopped early on an unrecognised command; first was "
			       "cmd %lu in state '%s'. Frame data past that point in those states is missing.\n",
				player_num, g_scrStats.statesAborted,
				g_scrStats.firstUnknownCmd, g_scrStats.firstUnknownState.c_str());
		}
	}

	return states_parsed;
}
bool is_sprite_active_frame(char* name_addr, std::map<std::string, JonbDBEntry>* jonbin_map) {
	if (name_addr == nullptr) { return false; }
	std::string cmd_str32(name_addr);
	auto match = jonbin_map->find(cmd_str32); //try to find the string[32] of the command in the map
	if (match != jonbin_map->end()) { //safety check
		JonbDBEntry entry = jonbin_map->at(cmd_str32); // if its in the map access it and do whatever you wanna do with it
		return entry.hitbox_count;
		//bool is_active = entry->hitbox_count;
		//...etc
	}
	return false;
}

int parse_state(char* addr, 
				std::vector<scrState*>& states_parsed, 
				std::map<std::string, JonbDBEntry>* jonbin_map, 
				std::map<std::string, scrState*>* ea_state_map) {
	scrState* s = new scrState();

	s->addr = addr;
	unsigned long CMD;
	unsigned int offset = 0;
	unsigned int prev_frames = 0; //saving the frames before the call to sprite, because functions that apply to those begin at the start of the sprite(), not at the end, such as invuln frames and spawning EA effects
	//memcpy(&s->name, addr + offset, 32);
	offset += 4;
	s->name = addr + offset;
	//cout << s->name << endl;
	offset += 32;
	memcpy(&CMD, addr + offset, sizeof(CMD));

	//CMD = *(addr + offset);
	offset += 4;
	FrameInvuln invuln = FrameInvuln::None; //flag to turn on/off invuln windows
	//int iter = 0;
	//PS: I may have been calling uint32 char for some reason here, not sure why tbh, gotta double check before changing the comments
	while (CMD != 0x1) {
		///remember to check for the configuration of defaults, 17000 up to 17006
		if (CMD == 0x2) {
			///sprite call(string[32],char) name of sprite and frames
			//if (s->name == "NmlAtk5B") {
			//	auto tsts = 1;
			//	std::string cmd_str32(addr + offset);
			//}
			bool is_active = is_sprite_active_frame(addr + offset, jonbin_map);//there's some weirdness on some moves, such as izayoi's "CmdActFDash", showing hitboxes when there shouldn't be
			offset += 32;
			// The duration is a 32 bit field. This used to be read as `*(addr + offset)`, a single
			// signed byte, which truncated every sprite longer than 127 frames and turned the
			// engine's 32767 "hold this sprite" sentinel into 0xFFFFFFFF.
			uint32_t frames;
			memcpy(&frames, addr + offset, 4);
			FrameActivity activity_status = is_active? FrameActivity::Active: FrameActivity::Inactive;
			offset += 4;

			prev_frames = (unsigned int)s->frame_activity_status.size();
			g_scrStats.spriteCmds++;

			// A sprite that is held indefinitely (32767 / -1), or one long enough that its length is
			// clearly not a real animation, contributes a single frame flagged non-deterministic.
			if (frames == 0xffffffff || frames == 32767 || frames > SPRITE_FRAME_CAP) {
				g_scrStats.heldSprites++;
				s->frame_activity_status.push_back((FrameActivity)(0x10 | (uint16_t)activity_status));
				s->frame_invuln_status.push_back(invuln);
			}
			else {
				for (uint32_t i = 0; i < frames; i++) {
					s->frame_activity_status.push_back(activity_status);
					//sets the invuln
					s->frame_invuln_status.push_back(invuln);
				}
			}

			// s->frames must stay equal to the number of entries actually pushed - the retroactive
			// loops below (22007/22019/2002/23027) use it as a vector index. When it ran ahead of the
			// vectors it took the parser with it: Litchi's RodEventDown holds `vrrod_000` for 32767
			// frames and is followed immediately by 23027, so s->frames reached 0xFFFFFFFF and that
			// loop spun 4.29 billion times on the render thread the first time frame history opened.
			s->frames = (unsigned int)s->frame_activity_status.size();
		}
		else if (CMD == 4000) {
			//EA state call(string[32],char); name of EA state and position
			if (!ea_state_map->empty()) {
				std::string cmd_str32(addr + offset);
				auto match = ea_state_map->find(cmd_str32); //try to find the string[32] of the command in the map
				if (match != ea_state_map->end()) { //safety check
					scrState* entry = ea_state_map->at(cmd_str32);
					s->frame_EA_effect_pairs.push_back({ prev_frames, *entry });
				}
			}
			offset += 32;
			//offsets the position
			offset += 4;
		}
		else if (CMD == 22007) {
			//setInvincible call(char); 0 if not set invincible, 1 if set. If CMD 22019 doesnt appear later assume full invincibility
			uint32_t argument = *(uint32_t*)(addr + offset);
			invuln = (argument == 1) ? FrameInvuln::All : FrameInvuln::None;
			RecordRetroSpan(prev_frames, s->frame_invuln_status.size());
			//when invuln is turned on/off I need to retroactively remove the last sprite length added, since it applies its effect to the start, not end of the sprite
			for (size_t i = prev_frames; i < s->frame_invuln_status.size(); i++) {
				s->frame_invuln_status[i] = invuln;
			}

			offset += 4;
		}
		else if (CMD == 22019) {
			//setInvincibleArgs call(char,char,char,char,char); they are equivalent to the flags for each property, head, body, leg, approach, throw. 0 for not set 1 for set.
			uint16_t head = *(uint32_t*)(addr + offset) * (uint16_t)FrameInvuln::Head;
			offset += 4;
			uint16_t body = *(uint32_t*)(addr + offset) * (uint16_t)FrameInvuln::Body;
			offset += 4;
			uint16_t leg = *(uint32_t*)(addr + offset) * (uint16_t)FrameInvuln::Foot;
			offset += 4;
			uint16_t approach = *(uint32_t*)(addr + offset); //idk what approach is, I assume its projectile which is not implemented yet
			offset += 4;
			uint16_t thro = *(uint32_t*)(addr + offset) * (uint16_t)FrameInvuln::Throw; //thro is throw, throw is a reserved word
			offset += 4;
			invuln = (FrameInvuln)(head | body | leg  | thro);// note the missing projectile assumed "approach" since its not implemented yet
			RecordRetroSpan(prev_frames, s->frame_invuln_status.size());
			for (size_t i = prev_frames; i < s->frame_invuln_status.size(); i++) {//when invuln is turned on/off I need to retroactively remove the last sprite length added, since it applies its effect to the start, not end of the sprite
				s->frame_invuln_status[i] = invuln;
			}

		}
		else if (CMD == 2002 || CMD == 23027) {
			//refreshMultihit(more like disableHitbox)  call() 2002
			//DisableAttackRestOfMove() call() 23027
			//this will disable the hitbox of the last sprite, its listed by dantation as startMultihit but its more akin to disablehitbox.
			RecordRetroSpan(prev_frames, s->frame_activity_status.size());
			for (size_t i = prev_frames; i < s->frame_activity_status.size(); i++) {//when hitbox is disabled I need to retroactively remove the last sprite length added, since it applies its effect to the start, not end of the sprite
				//keep the non-deterministic bit, only clear the activity itself
				const uint16_t nonDeterministic = (uint16_t)s->frame_activity_status[i] & 0x10;
				s->frame_activity_status[i] = (FrameActivity)(nonDeterministic | (uint16_t)FrameActivity::Inactive);
			}
		}
		//still need to get the guard point CMD
		else if (CMD == 9003) {
			///Damage call(char) set dmg 
			memcpy(&s->damage, addr + offset, 4);
			offset += 4;
		}
		else if (CMD == 9001) {
			///Damage call(char) set atk_type
			//unsigned int atk_type;
			//atk_type = *(addr + offset);
			memcpy(&s->atk_type, addr + offset, 4);

			offset += 4;
			//s->atk_type = atk_type;
		}
		else if (CMD == 9002) {
			///Damage call(char) set atk_level
			///unsigned int atk_level;
			//atk_level = *(addr + offset);
			memcpy(&s->atk_level, addr + offset, 4);
			offset += 4;
			//s->atk_level = atk_level;
		}
		else if (CMD == 9154) {
			///hitstun call(char) set hitstun when not tied to atk_level
			//unsigned int hitstun;
			//memcpy(&hitstun, addr + offset, 4);
			memcpy(&s->hitstun, addr + offset, 4);
			offset += 4;
			//s->hitstun = hitstun;
		}
		else if (CMD == 11000) {
			///hitstop call(char) set hitstop
			//unsigned int hitstop;
			memcpy(&s->hitstop, addr + offset, 4);
			offset += 4;
			//s->hitstop = hitstop;
		}
		else if (CMD == 9274) {
			///attack_p1 call(char) set attack_p1
			unsigned int attack_p1;
			memcpy(&attack_p1, addr + offset, 4);
			offset += 4;
			s->attack_p1 = attack_p1;
		}
		else if (CMD == 9286) {
			///attack_p2 call(char) set attack_p2
			unsigned int attack_p2;
			memcpy(&attack_p2, addr + offset, 4);
			offset += 4;
			s->attack_p2 = attack_p2;
		}
		else if (CMD == 11036) {
			///hitOverhead call(char) set hitOverhead
			unsigned int hit_overhead;
			memcpy(&hit_overhead, addr + offset, 4);
			offset += 4;
			s->hit_overhead = hit_overhead;
		}
		else if (CMD == 11035) {
			///hitLow call(char) set hitLow
			unsigned int hit_low;
			memcpy(&hit_low, addr + offset, 4);
			offset += 4;
			s->hit_low = hit_low;
		}
		else if (CMD == 11037) {
			///HitAirUnblockable call(char) set HitAirUnblockable
			unsigned int hit_air_unblockable;
			memcpy(&hit_air_unblockable, addr + offset, 4);
			offset += 4;
			s->hit_air_unblockable = hit_air_unblockable;
		}
		else if (CMD == 14068) {
			///whiffCancel call(string[32]) set whiffcancel to moves
			std::string whiff_cancel;
			whiff_cancel = (addr + offset);
			//memcpy(&whiff_cancel, addr + offset, 4);
			offset += 32;
			s->whiff_cancel.push_back(whiff_cancel);
		}
		else if (CMD == 14069) {
			///hit or block cancel call(string[32]) set hit or block cancel to moves
			std::string hit_or_block_cancel;
			hit_or_block_cancel = (addr + offset);
			//memcpy(&whiff_cancel, addr + offset, 4);
			offset += 32;
			s->hit_or_block_cancel.push_back(hit_or_block_cancel);
		}

		else if (CMD == 11088) {
			///starter rating call(char) set starter rating
			unsigned int fatal_counter;
			memcpy(&s->fatal_counter, addr + offset, 4);
			offset += 4;
			//s->fatal_counter = fatal_counter;
		}
		else if (CMD == 12051) {
			///starter rating call(char) set starter rating
			unsigned int starter_rating;
			memcpy(&starter_rating, addr + offset, 4);
			offset += 4;
			s->starter_rating = starter_rating;
		}
		else if (CMD == 11028) {
			///blockstun call(char) set blockstun
			unsigned int blockstun;
			memcpy(&blockstun, addr + offset, 4);
			offset += 4;
			s->blockstun = blockstun;
		}

		//these are the commands in the script in not interested in using
		else if (std::find(size_4.begin(), size_4.end(), CMD) != size_4.end()) {
		}
		else if (std::find(size_8.begin(), size_8.end(), CMD) != size_8.end()) {
			offset += 4;
		}
		else if (std::find(size_12.begin(), size_12.end(), CMD) != size_12.end()) {
			offset += 8;
		}
		else if (std::find(size_16.begin(), size_16.end(), CMD) != size_16.end()) {
			offset += 12;
		}
		else if (std::find(size_20.begin(), size_20.end(), CMD) != size_20.end()) {
			offset += 16;
		}
		else if (std::find(size_24.begin(), size_24.end(), CMD) != size_24.end()) {
			offset += 20;
		}
		else if (std::find(size_28.begin(), size_28.end(), CMD) != size_28.end()) {
			offset += 24;
		}
		else if (std::find(size_32.begin(), size_32.end(), CMD) != size_32.end()) {
			offset += 28;
		}
		else if (std::find(size_36.begin(), size_36.end(), CMD) != size_36.end()) {
			offset += 32;
		}
		else if (std::find(size_40.begin(), size_40.end(), CMD) != size_40.end()) {
			offset += 36;
		}
		else if (std::find(size_44.begin(), size_44.end(), CMD) != size_44.end()) {
			offset += 40;
		}
		else if (std::find(size_48.begin(), size_48.end(), CMD) != size_48.end()) {
			offset += 44;
		}
		else if (std::find(size_52.begin(), size_52.end(), CMD) != size_52.end()) {
			offset += 48;
		}
		else if (std::find(size_68.begin(), size_68.end(), CMD) != size_68.end()) {
			offset += 64;
		}
		else if (std::find(size_72.begin(), size_72.end(), CMD) != size_72.end()) {
			offset += 68;
		}
		else if (std::find(size_84.begin(), size_84.end(), CMD) != size_84.end()) {
			offset += 80;
		}
		else if (std::find(size_88.begin(), size_88.end(), CMD) != size_88.end()) {
			offset += 84;
		}
		else if (std::find(size_132.begin(), size_132.end(), CMD) != size_132.end()) {
			offset += 128;
		}
		else if (std::find(size_148.begin(), size_148.end(), CMD) != size_148.end()) {
			offset += 144;
		}


		else if (CMD == 23030) {
			//calls private function (string[32], x, x, x, x, x, x, x, x)
			offset += 32;
			offset += 4 * 8;
		}

		else if (CMD == 23183) {
			///(string[32], x, x, x)
			offset += 32;
			offset += 4 * 3;
		}
		else if (CMD == 4003) {
			///(string[32],string[32])
			offset += 32;
			offset += 32;

		}
		else if (CMD == 7006 || CMD == 7007) {
			///(string[16], x, string[16], x, string[16], x, string[16], x)
			offset += 16 * 3;
			offset += 4 * 3;
		}
		else if (CMD == 12045) {
			///(x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x) 
			offset += 4 * 16;
		}
		else {
			// Unrecognised command. Its argument size is unknown, so the walk cannot continue past
			// it and everything after this point in the state is lost. This used to go to std::cout,
			// which nothing in a windowed game ever reads; it is counted and reported by parse_scr
			// instead.
			g_scrStats.statesAborted++;
			if (g_scrStats.firstUnknownState.empty())
			{
				g_scrStats.firstUnknownCmd = CMD;
				g_scrStats.firstUnknownState = s->name;
			}
			break;
		};

		memcpy(&CMD, addr + offset, sizeof(CMD));
		//CMD = *(unsigned long*)(addr + offset);
		offset += 4;

	}

	g_scrStats.statesParsed++;
	g_scrStats.frameEntries += (unsigned int)s->frame_activity_status.size();
	if (s->frame_activity_status.size() > g_scrStats.longestState)
	{
		g_scrStats.longestState = (unsigned int)s->frame_activity_status.size();
	}

	states_parsed.push_back(s);
	return 0;
}

void override_state(char* addr, char* new_state) {
	int offset = 4;
	offset += 32;
	int enter_state_id = 21;
	memcpy(addr + offset, &enter_state_id, 4);
	offset += 4;
	memcpy(addr + offset, new_state, 32);
	return;
}




/*char* bbcf_base_addr;
	//Get all module related information
			//Get process name
	TCHAR szFileName[MAX_PATH + 1];
	GetModuleFileName(NULL, szFileName, MAX_PATH + 1);

	MODULEINFO modinfo = { 0 };
	HMODULE hModule = GetModuleHandle(szFileName);
	///if (hModule == 0)
	//	return 0;
	GetModuleInformation(GetCurrentProcess(), hModule, &modinfo, sizeof(MODULEINFO));
	////////

	//Assign our base and module size
	//Having the values right is ESSENTIAL, this makes sure
	//that we don't scan unwanted memory and leading our game to crash
	long base = (long)modinfo.lpBaseOfDll;*/