#pragma once
#include <string>
#include <vector>
#include "PlaybackSlot.h"
#include <array>
class PlaybackManager
{
public:


	PlaybackManager();
	char* bbcf_base_adress; // pointer to the base adress of bbcf
	char* training_state_p; // bbcf_base_adress + 0x1392d10
	char* playback_control_p; //bbcf_base_adress + 0x1392d10 + 0x1ac2c; set to 3 to start playback without direction adjustment, 0 for dummy, 1 for recording standby, 2 for bugged recording, 3 for playback, 4 for controller, 5 for cpu, 6 for continuous playback
	char* active_slot_p; // the active slot
	std::vector<PlaybackSlot> slots; //one for each of the 4 playback slots

	
	void save_to_file(std::vector<char> slot_buffer, char facing_direction, char* fname);
	std::vector<char> load_from_file(char* fname);/*returns the contents of the file*/
	std::vector<char> trim_playback(std::vector<char> slot_buffer);
	void load_into_slot(std::vector<char> trimmed_playback, int slot); /*this is the "load_trimmed_playback" function back in ScrWindow.cpp, loads from a buffer into a slot, this assumes the direction byte is already taken care of in caso of the buffer coming from a file*/
	void load_into_slot(std::vector<char> trimmed_playback, int facing_left, int slot); /*this is the "load_trimmed_playback" function back in ScrWindow.cpp, loads from a buffer into a slot, this assumes the direction byte is already taken care of in caso of the buffer coming from a file*/
	void load_raw_into_slot(const std::vector<char>& raw_playback, int facing_left, int slot);

	void load_from_file_into_slot(char* fname, int slot);

	// Path-based versions of the two above, for the Import/Export buttons and the replay
	// capture export. The originals hardcode "./slots/" and append ".playback", which is no
	// use once a file picker is choosing where the file goes.
	//
	// Same on-disk format as save_to_file: one facing-direction byte, then one byte per
	// frame.
	static bool save_playback_to_path(const std::string& path,
		const std::vector<char>& trimmed_playback, char facing_direction);
	static bool load_playback_from_path(const std::string& path,
		std::vector<char>* out_trimmed, char* out_facing);
	// Raw slot data is two bytes per frame; a file holds one. Drops the aux byte.
	static std::vector<char> raw_to_trimmed(const std::vector<char>& raw_playback); /*loads from a file into a slot doing the necessary checks to ensure the file is valid, the facing byte is correctly set and won't crash*/
	void set_active_slot(int slot);
	void set_playback_control(int playback_control); /*set to 3 to start playback without direction adjustment, 0 for dummy, 1 for recording standby, 2 for bugged recording, 3 for playback, 4 for controller, 5 for cpu, 6 for continuous playback*/
	void set_playback_position(int frame_position);
	void set_playback_type(int playback_type);

private:
	int training_state_offset = 0x1392d10; // training mode state block used by the native playback controller
	int playback_control_offset = 0x1392d10 + 0x1ac2c;// playback_control_p = bbcf_base_adress + playback_control_offset
	int active_slot_offset = 0x902C3C; //active_slot_p = bbcf_base_adress + active_slot_offset
};

