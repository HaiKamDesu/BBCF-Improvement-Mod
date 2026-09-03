#include "MusicManager.h"
#include "BgmReplacementManager.h"
#include "CustomMusicConverter.h"
#include "Core/logger.h"
#include "Core/utils.h"
#include "Game/gamestates.h"
#include "Core/interfaces.h"
#include "Overlay/Logger/ImGuiLogger.h"

#include <windows.h>
#include <algorithm>
#include <random>
#include <map>
#include <fstream>
#include <cstdio>
#include <psapi.h>
#include <sstream>
#include <cstdarg>
#include <iomanip>
#pragma comment(lib, "psapi.lib")

// Helper to log to both file (debug) and ImGui log (release)
static void LogMusic(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    
    LOG(1, "%s", buf);
    if (g_imGuiLogger) {
        std::ostringstream oss;
        oss << "[music] ";
        oss << buf;
        g_imGuiLogger->Log("%s", oss.str().c_str());
    }
}

// Static member initialization
int* MusicManager::s_musicSelectX = nullptr;
int* MusicManager::s_musicSelectY = nullptr;
std::vector<std::pair<int, std::string>> MusicManager::s_customTrackFiles;
std::vector<MusicManager::ReplacementTrackFile> MusicManager::s_replacementTrackFiles;

// Custom tracks are numbered from here up, so they can never collide with a native id -
// and so they can be told apart from native ones when a rescan replaces them.
// Track id ranges live on MusicManager itself; see MusicManager.h.

// Audio engine constants (from reverse engineering BBCF.exe)
static constexpr uintptr_t AUDIO_MGR_RVA = 0x008903B0;
static constexpr uintptr_t BGM_ID_OFFSET = 0x1690;
static constexpr uintptr_t LOADER_RVA = 0x0014FEE0;
static constexpr uintptr_t REGISTER_RVA = 0x0007C930;
static constexpr uintptr_t PLAY_SOUND_OBJECT_RVA = 0x00008E70;
static constexpr uintptr_t PLAY_CONTROLLER_RVA = 0x006128BC;
static constexpr uintptr_t GET_SOUND_OBJ_RVA = 0x0007CA50;
static constexpr uintptr_t SOUND_ENGINE_RVA = 0x00623630;  // CSoundEngine_XACT singleton
static constexpr uintptr_t BANK_STOP_RVA = 0x00051CF0;     // CSoundBank_XACT::vtable[0x1C] - Stop by index
static constexpr uintptr_t SOUND_SLOT_MGR_RVA = 0x0088E4C8; // Sound slot manager (returned by RVA 0x7CA90)
static constexpr uintptr_t PLAYBGM_RVA = 0x0007FF60;       // Native PlayBgm function
static constexpr uintptr_t XACT_REGISTER_RVA = 0x0014FF30; // Native BGM registration function
static constexpr uintptr_t ADD_SLOT_TO_QUEUE_RVA = 0x00050270; // Method 5 of AAWin_CFileReader_Thread vtable

// Forward declaration (defined below, near PlayTrackPhysically): read a track's
// true length in frames (60fps) from its .pac wave bank. Needed by
// UpdateMusicState, which runs earlier in this translation unit.
static int GetTrackDurationFramesFromPac(int trackId);

// Function pointer types — LoaderFunc returns loaded file pointer in EAX
typedef void* (__cdecl *LoaderFuncType)(const char* path, int flag);
typedef int (__thiscall *RegisterFuncType)(void* audioMgr, int index, void* loadedFilePtr);
// The game's "string" type is a simple 256-byte (0x100) fixed char buffer, NOT std::string.
// PlaySoundObject's path arg is a pointer to this 256-byte buffer.
typedef int (__thiscall *PlaySoundObjectFuncType)(void* playController, void* soundObject, const char* pathBuf256, int arg3, int arg4, int arg5, int* voiceHandleOut);
typedef void* (__thiscall *GetSoundObjFuncType)(void* audioMgr, int slotIndex);
typedef void (__thiscall *BankStopFuncType)(int* indexPtr);  // ecx = bank, push &index
typedef int (__cdecl *XactRegisterFnType)(void* loadedFilePtr, const char* path, int arg3, int arg4, int arg5, int* voiceHandleOut);

// Singleton instance
MusicManager& MusicManager::GetInstance() {
    static MusicManager instance;
    return instance;
}

MusicManager& GetMusicManager() {
    return MusicManager::GetInstance();
}

MusicManager::MusicManager() {
    m_pendingPlay = false;
    m_pendingSoundObj = nullptr;
    m_pendingCueName = "";
    m_pendingPlayRetries = 0;
    BuildTrackList();
}

// Filename lookup for unknown tracks (BGM files are .pac archives)
static std::pair<int, const char*> UNKNOWN_TRACK_FILES[] = {
    { 0,   "000_btl_rg" },      { 1,   "001_btl_jn" },      { 2,   "002_btl_no" },
    { 3,   "003_btl_rc" },      { 4,   "004_btl_tk" },      { 5,   "005_btl_tg" },
    { 6,   "006_btl_lc" },      { 7,   "007_btl_ar" },      { 8,   "008_btl_bn" },
    { 9,   "009_btl_ca" },      { 10,  "010_btl_ha" },      { 11,  "011_btl_ny" },
    { 12,  "012_btl_tb" },      { 13,  "013_btl_hz" },      { 14,  "014_btl_mu" },
    { 15,  "015_btl_mk" },      { 16,  "016_btl_vh" },      { 17,  "017_btl_pt" },
    { 18,  "018_btl_rl" },      { 19,  "019_btl_iz" },      { 20,  "020_btl_am" },
    { 21,  "021_btl_bl" },      { 22,  "022_btl_az" },      { 23,  "023_btl_kg" },
    { 24,  "024_btl_kk" },      { 25,  "025_btl_tm" },      { 26,  "026_btl_ce" },
    { 27,  "027_btl_rm" },      { 28,  "028_btl_hb" },      { 29,  "029_btl_ph" },
    { 30,  "030_btl_nt" },      { 31,  "031_btl_mi" },      { 32,  "032_btl_su" },
    { 33,  "033_btl_es" },      { 34,  "034_btl_ma" },      { 35,  "035_btl_jb" },
    { 50,  "050_btl_rgvsjn" },  { 51,  "051_btl_novsjn" },  { 52,  "052_btl_rcvsrg" },
    { 53,  "053_btl_rgvshz" },  { 54,  "054_btl_tbvsno" },  { 55,  "055_btl_tbvsjn" },
    { 56,  "056_btl_arvslc" },  { 57,  "057_btl_rlvsca" },  { 58,  "058_btl_rlvsvh" },
    { 59,  "059_btl_havsiz" },  { 60,  "060_btl_rgvsha" },  { 61,  "061_btl_phvsXXX" },
    { 62,  "062_btl_rgvsmi" },  { 63,  "063_btl_havssu" },
    { 80,  "080_btl_bosshz" },  { 81,  "081_btl_bossrg" },  { 82,  "082_btl_douchara" },
    { 83,  "083_btl_bangthem" },{ 84,  "084_btl_bangthem_short" },
    { 85,  "085_btl_seven" },   { 86,  "086_btl_sixheroes" },{ 87,  "087_btl_bangthem2" },
    { 88,  "088_btl_bangthem2_short" },
    { 89,  "089_btl_boss" },    { 90,  "090_btl_highlander" },{ 91,  "091_btl_bossmi" },
    { 92,  "092_btl_bosssu" },  { 94,  "094_btl_esvsXXX" },
    { 100, "100_opening" },     { 101, "101_staffroll" },   { 102, "102_ending" },
    { 103, "103_opening2" },    { 104, "104_staffroll2" },
    { 150, "150_cs_opening" },  { 152, "152_cs_staffroll" },
    { 200, "200_title" },       { 201, "201_charaselect" }, { 202, "202_introduction" },
    { 203, "203_continue" },    { 204, "204_mainmenu" },    { 205, "205_abyss" },
    { 206, "206_mainmenu2" },   { 207, "207_Lobby" },       { 208, "208_Lobby2" },
    { 209, "209_Lobby3" },      { 210, "210_Lobby4" },      { 211, "211_Lobby5" },
    { 212, "212_Lobby6" },      { 250, "250_vsinfo" },      { 251, "251_rannyu" },
    { 252, "252_winner" },      { 253, "253_gameover" },
    { 900, "900_btl_rg_old" },  { 901, "901_btl_jn_old" },  { 902, "902_btl_no_old" },
    { 903, "903_btl_rc_old" },  { 904, "904_btl_tk_old" },  { 905, "905_btl_tg_old" },
    { 906, "906_btl_lc_old" },  { 907, "907_btl_ar_old" },  { 908, "908_btl_bn_old" },
    { 909, "909_btl_ca_old" },  { 910, "910_btl_ha_old" },  { 911, "911_btl_ny_old" },
    { 912, "912_btl_tb_old" },  { 913, "913_btl_hz_old" },  { 914, "914_btl_mu_old" },
    { 915, "915_btl_mk_old" },  { 916, "916_btl_vh_old" },  { 917, "917_btl_pt_old" },
    { 918, "918_btl_rl_old" },
    { 950, "950_btl_rgvsjn_old" },{ 951, "951_btl_novsjn_old" },
    { 952, "952_btl_rcvsrg_old" },{ 953, "953_btl_rgvshz_old" },
    { 954, "954_btl_tbvsno_old" },{ 955, "955_btl_tbvsjn_old" },
    { 956, "956_btl_arvslc_old" },{ 957, "957_btl_rlvsca_old" },
    { 980, "980_btl_bosshz_old" },{ 981, "981_btl_bossrg_old" },
    { 982, "982_btl_douchara_old" },
    { 300, "300_smop" },         { 301, "301_nichijo" },     { 302, "302_btl_tbvsjn" },
    { 303, "303_memory" },       { 304, "304_btl_no" },      { 305, "305_fuan" },
    { 306, "306_inaka" },        { 307, "307_dorama" },      { 308, "308_nichijo_b" },
    { 309, "309_nichijo_e" },    { 310, "310_bgm_ar" },      { 311, "311_hurry" },
    { 312, "312_horror" },       { 313, "313_btl_sixheroes" },{ 314, "314_god" },
    { 315, "315_gothic" },       { 316, "316_Techno" },      { 317, "317_nichijo_a" },
    { 318, "318_nichijo_d" },    { 319, "319_btl_am" },      { 320, "320_wafuu" },
    { 321, "321_btl_az" },       { 322, "322_kinpaku" },
    { 400, "400_nichijo_a" },    { 401, "401_nichijo_b" },   { 403, "403_nichijo_d" },
    { 404, "404_nichijo_e" },    { 405, "405_nichijo_f" },   { 406, "406_dorama" },
    { 407, "407_horror" },       { 408, "408_hurry" },       { 409, "409_gothic" },
    { 410, "410_pop" },          { 411, "411_techno" },      { 412, "412_gyagu" },
    { 413, "413_kinpaku" },      { 414, "414_memory" },      { 415, "415_shissou" },
    { 416, "416_god" },          { 417, "417_shinpi" },      { 418, "418_sad" },
    { 419, "419_inaka" },        { 420, "420_fuan" },        { 421, "421_wafuu" },
    { 422, "422_sougen" },       { 423, "423_kuroki" },
    { 450, "450_smop" },         { 451, "451_smop" },        { 452, "452_mkd_appear" },
    { 453, "453_noelpoem" },     { 454, "454_tsubakisasayaki" },{ 455, "455_mkd_highlander" },
    { 456, "456_masterunit" },
    { 500, "500_pandoratears" }, { 501, "501_shinsou" },     { 502, "502_stardust_memory" },
    { 600, "600_astral_a" },     { 601, "601_astral_b" },    { 602, "602_astral_c" },
    { 603, "603_cs_astral_a" },  { 604, "604_cs_astral_b" }, { 605, "605_cs_astral_c" },
    { 606, "606_v2_astral_a" },  { 607, "607_v2_astral_b" }, { 608, "608_v2_astral_c" },
    { 609, "609_cs_astral_a" },  { 610, "610_cs_astral_b" }, { 611, "611_cs_astral_c" },
};

const char* MusicManager::GetBgmFilename(int trackId) {
	for (const auto& entry : UNKNOWN_TRACK_FILES) {
		if (entry.first == trackId) {
			return entry.second;
		}
	}
	// Custom tracks (IDs >= 10000, discovered at startup). Returning a non-null
	// filename here is what marks an id as "valid" everywhere it is checked.
	for (const auto& entry : s_customTrackFiles) {
		if (entry.first == trackId) {
			return entry.second.c_str();
		}
	}
	// BGM replacements published as tracks of their own. The path carries a subdirectory,
	// which the loader already supports - that is how PreviewPac auditions one.
	for (const auto& entry : s_replacementTrackFiles) {
		if (entry.id == trackId) {
			return entry.pacPath.c_str();
		}
	}
	return nullptr;
}

const char* MusicManager::GetBgmCueName(int trackId) {
	if (IsReplacementTrackId(trackId)) {
		for (const auto& entry : s_replacementTrackFiles) {
			if (entry.id == trackId) {
				return entry.cueName.c_str();
			}
		}
		return nullptr;
	}
	// Custom Jukebox tracks are built from the complete native 000_btl_rg bank and keep its
	// cue; only their outer .pac filename is their own.
	if (IsCustomTrackId(trackId)) {
		return "000_btl_rg";
	}
	// Native track: the cue is the base filename.
	return GetBgmFilename(trackId);
}

// True for an id the GAME itself can name: a shipped track. The Jukebox's dynamic ids
// (custom songs from kCustomTrackIdBase, replacements from kReplacementTrackIdBase) are ours
// alone and the game knows nothing about them, so a value read out of the game's own fields
// that lands in one of those ranges can only be garbage. That distinction is load-bearing:
// such a value can otherwise become m_anchorTrackId, and the anchor is written straight back
// into musicSelect_X on scene exit - presenting a non-selectable id there is exactly what
// errors Character Select into the red debug screen.
static bool IsNativeGameTrackId(int trackId) {
	return trackId >= 0 && trackId < MusicManager::kCustomTrackIdBase &&
		MusicManager::GetBgmFilename(trackId) != nullptr;
}

// Precomputed EXACT track durations in seconds, generated offline from the
// game's own audio data (tools/validate_durations.py: parse each .pac's FPAC
// table -> wave bank -> Duration field). Used as a fallback when a track's
// length can't be read live from the loaded wave bank. Covers every known BGM.
static std::pair<int, int> TRACK_DURATIONS_SEC[] = {
	{ 0, 259 }, { 1, 318 }, { 2, 258 }, { 3, 262 }, { 4, 180 },
	{ 5, 324 }, { 6, 259 }, { 7, 229 }, { 8, 280 }, { 9, 227 },
	{ 10, 221 }, { 11, 369 }, { 12, 339 }, { 13, 267 }, { 14, 307 },
	{ 15, 307 }, { 16, 266 }, { 17, 254 }, { 18, 319 }, { 19, 221 },
	{ 20, 280 }, { 21, 209 }, { 22, 260 }, { 23, 231 }, { 24, 294 },
	{ 25, 246 }, { 26, 293 }, { 27, 306 }, { 28, 314 }, { 29, 278 },
	{ 30, 284 }, { 31, 247 }, { 32, 253 }, { 33, 254 }, { 34, 212 },
	{ 35, 313 }, { 50, 301 }, { 51, 354 }, { 52, 286 }, { 53, 276 },
	{ 54, 328 }, { 55, 321 }, { 56, 242 }, { 57, 323 }, { 58, 259 },
	{ 59, 266 }, { 60, 274 }, { 61, 351 }, { 62, 346 }, { 63, 292 },
	{ 80, 343 }, { 81, 252 }, { 82, 255 }, { 83, 117 }, { 84, 97 },
	{ 85, 262 }, { 86, 249 }, { 87, 248 }, { 88, 65 }, { 89, 256 },
	{ 90, 297 }, { 91, 313 }, { 92, 439 }, { 94, 272 }, { 100, 104 },
	{ 101, 333 }, { 102, 16 }, { 103, 106 }, { 104, 317 }, { 150, 259 },
	{ 152, 620 }, { 200, 36 }, { 201, 142 }, { 202, 24 }, { 203, 25 },
	{ 204, 103 }, { 205, 226 }, { 206, 106 }, { 207, 97 }, { 208, 97 },
	{ 209, 136 }, { 210, 97 }, { 211, 136 }, { 212, 847 }, { 250, 12 },
	{ 251, 9 }, { 252, 12 }, { 253, 14 }, { 300, 274 }, { 301, 262 },
	{ 302, 320 }, { 303, 130 }, { 304, 258 }, { 305, 127 }, { 306, 125 },
	{ 307, 82 }, { 308, 104 }, { 309, 159 }, { 310, 249 }, { 311, 88 },
	{ 312, 84 }, { 313, 249 }, { 314, 121 }, { 315, 42 }, { 316, 74 },
	{ 317, 137 }, { 318, 126 }, { 319, 280 }, { 320, 123 }, { 321, 260 },
	{ 322, 147 }, { 400, 137 }, { 401, 104 }, { 403, 126 }, { 404, 159 },
	{ 405, 262 }, { 406, 82 }, { 407, 84 }, { 408, 88 }, { 409, 42 },
	{ 410, 49 }, { 411, 74 }, { 412, 128 }, { 413, 147 }, { 414, 130 },
	{ 415, 130 }, { 416, 121 }, { 417, 123 }, { 418, 129 }, { 419, 125 },
	{ 420, 127 }, { 421, 123 }, { 422, 147 }, { 423, 139 }, { 450, 274 },
	{ 451, 41 }, { 452, 36 }, { 453, 119 }, { 454, 119 }, { 455, 143 },
	{ 456, 72 }, { 500, 243 }, { 501, 103 }, { 502, 295 }, { 600, 44 },
	{ 601, 24 }, { 602, 16 }, { 603, 24 }, { 604, 19 }, { 605, 19 },
	{ 606, 30 }, { 607, 22 }, { 608, 22 }, { 609, 44 }, { 610, 24 },
	{ 611, 16 }, { 900, 241 }, { 901, 239 }, { 902, 286 }, { 903, 229 },
	{ 904, 207 }, { 905, 278 }, { 906, 217 }, { 907, 249 }, { 908, 234 },
	{ 909, 236 }, { 910, 219 }, { 911, 306 }, { 912, 347 }, { 913, 272 },
	{ 914, 311 }, { 915, 263 }, { 916, 228 }, { 917, 247 }, { 918, 296 },
	{ 950, 247 }, { 951, 313 }, { 952, 222 }, { 953, 265 }, { 954, 318 },
	{ 955, 275 }, { 956, 230 }, { 957, 276 }, { 980, 314 }, { 981, 237 },
	{ 982, 230 },
};

int MusicManager::GetTrackDuration(int trackId) {
	for (const auto& entry : TRACK_DURATIONS_SEC) {
		if (entry.first == trackId) {
			return entry.second > 0 ? entry.second * 60 : 0; // seconds -> frames @60fps; 0 = unknown
		}
	}
	return 0; // unknown -> caller falls back
}

std::string MusicManager::GetSongTimeString() const {
	int totalSec = m_songPlaybackFrames / 60;
	int mins = totalSec / 60;
	int secs = totalSec % 60;
	std::ostringstream oss;
	oss << std::setfill('0') << std::setw(2) << mins << ":"
		<< std::setfill('0') << std::setw(2) << secs;
	return oss.str();
}

void MusicManager::BuildTrackList() {
    // BBCF Central Fiction BGM tracks
    // Track IDs match the numeric prefix of BGM filenames:
    // ...\Steam\steamapps\common\BlazBlue Centralfiction\data\Sound\BGM\{ID}_name.pac
    m_tracks.push_back({ 0,  "Rebellion II (Ragna)", "btl" });
    m_tracks.push_back({ 1,  "Lust SIN II (Jin)", "btl" });
    m_tracks.push_back({ 2,  "Bullet Dance II (Noel)", "btl" });
    m_tracks.push_back({ 3,  "Queen of rose II (Rachel)", "btl" });
    m_tracks.push_back({ 4,  "Catus Carnival II (Taokaka)", "btl" });
    m_tracks.push_back({ 5,  "MOTOR HEAD II (Tager)", "btl" });
    m_tracks.push_back({ 6,  "Oriental Flower II (Litchi)", "btl" });
    m_tracks.push_back({ 7,  "Thin RED Line II (Arakune)", "btl" });
    m_tracks.push_back({ 8,  "REPPUU II (Bang)", "btl" });
    m_tracks.push_back({ 9,  "Marionette Purple II (Carl)", "btl" });
    m_tracks.push_back({ 10, "SUSANOOH II (Hakumen)", "btl" });
    m_tracks.push_back({ 11, "Awakening The Chaos II (Nu-13)", "btl" });
    m_tracks.push_back({ 12, "Condemnation Wings II (Tsubaki)", "btl" });
    m_tracks.push_back({ 13, "Gluttony Fang II (Hazama)", "btl" });
    m_tracks.push_back({ 14, "Sword of Doom II (Mu-12)", "btl" });
    m_tracks.push_back({ 15, "Alexandrite II (Makoto)", "btl" });
    m_tracks.push_back({ 16, "Howling Moon II (Valkenhayn)", "btl" });
    m_tracks.push_back({ 17, "Active Angel II (Platinum)", "btl" });
    m_tracks.push_back({ 18, "Plastic Night II (Relius)", "btl" });
    m_tracks.push_back({ 19, "Justice Sword (Izayoi)", "btl" });
    m_tracks.push_back({ 20, "BUSIN (Amane)", "btl" });
    m_tracks.push_back({ 21, "Spirit of Fire (Bullet)", "btl" });
    m_tracks.push_back({ 22, "The Tyrant (Azrael)", "btl" });
    m_tracks.push_back({ 23, "Black aggression (Kagura)", "btl" });
    m_tracks.push_back({ 24, "Science Fiction (Kokonoe)", "btl" });
    m_tracks.push_back({ 25, "Silent Scream (Terumi)", "btl" });
    m_tracks.push_back({ 26, "Crystal Forest (Celica)", "btl" });
    m_tracks.push_back({ 27, "Awakening The Chaos (Nu-13 / Lambda-11)", "btl" });
    m_tracks.push_back({ 28, "in the shadows (Hibiki)", "btl" });
    m_tracks.push_back({ 29, "Walpurgisnacht (Nine)", "btl" });
    m_tracks.push_back({ 30, "Jaeger (Naoto)", "btl" });
    m_tracks.push_back({ 31, "YOMOTSUHIRASAKA (Izanami)", "btl" });
    m_tracks.push_back({ 32, "MUST DIE (Susano'o)", "btl" });
    m_tracks.push_back({ 33, "conciliation (Es)", "btl" });
    m_tracks.push_back({ 34, "VARIABLE HEART (Mai)", "btl" });
    m_tracks.push_back({ 35, "STAND UNRIVALED (Jubei)", "btl" });
    m_tracks.push_back({ 50, "Under Heaven Destruction II (Ragna vs Jin)", "vs" });
    m_tracks.push_back({ 51, "Imperial Code II (Noel vs Jin)", "vs" });
    m_tracks.push_back({ 52, "White Requiem II (Rachel vs Ragna)", "vs" });
    m_tracks.push_back({ 53, "Nightmare Fiction II (Ragna vs Hazama)", "vs" });
    m_tracks.push_back({ 54, "Memory of Tears II (Tsubaki vs Noel)", "vs" });
    m_tracks.push_back({ 55, "Childish killer II (Tsubaki vs Jin)", "vs" });
    m_tracks.push_back({ 56, "Weak Executioner II (Arakune vs Litchi)", "vs" });
    m_tracks.push_back({ 57, "X-matic II (Relius vs Carl)", "vs" });
    m_tracks.push_back({ 58, "Dissonance (Relius vs Valkenhayn)", "vs" });
    m_tracks.push_back({ 59, "in JUSTICE (Hakumen vs Izayoi)", "vs" });
    m_tracks.push_back({ 60, "Black & White (Ragna vs Hakumen)", "vs" });
    m_tracks.push_back({ 61, "Reincarnation (Nine vs Celica/Kokonoe/Jubei)", "vs" });
    m_tracks.push_back({ 62, "Bloodline (Ragna/Jin vs Izanami)", "vs" });
    m_tracks.push_back({ 63, "God of War (Hakumen vs Susano'o)", "vs" });
    m_tracks.push_back({ 80, "Endless Despair (Boss Hazama)", "boss" });
    m_tracks.push_back({ 81, "Black Onslaught (Boss Ragna)", "boss" });
    m_tracks.push_back({ 82, "Blood Pain (Mirror Match)", "boss" });
    m_tracks.push_back({ 83, "Bang! Bang! Bigbang! (Bang)", "boss" });
    m_tracks.push_back({ 84, "Bang! Bang! Bigbang! (Short)", "boss" });
    m_tracks.push_back({ 85, "Sector7 (Sector 7)", "boss" });
    m_tracks.push_back({ 86, "SIX-HEROES (Six Heroes)", "boss" });
    m_tracks.push_back({ 87, "OMAE NO TETTUI NI KUGI WO UTE (Bang)", "boss" });
    m_tracks.push_back({ 88, "OMAE NO TETTUI NI KUGI WO UTE (Short)", "boss" });
    m_tracks.push_back({ 89, "Boss Theme (Arcade Boss)", "boss" });
    m_tracks.push_back({ 90, "THE HIGHLANDER (Bullet)", "boss" });
    m_tracks.push_back({ 91, "THE WORLD END (Boss Izanami)", "boss" });
    m_tracks.push_back({ 92, "HAKAISHIN (Boss Susano'o)", "boss" });
    m_tracks.push_back({ 94, "END GAZER (Es vs Celica/Nine)", "boss" });
    m_tracks.push_back({ 100, "CENTRALFICTION (Arcade Opening)", "sys" });
    m_tracks.push_back({ 101, "Twilight tear (Credits)", "sys" });
    m_tracks.push_back({ 102, "Ending", "sys" });
    m_tracks.push_back({ 103, "TRUE-BLUE (Console Opening)", "sys" });
    m_tracks.push_back({ 104, "Staffroll 2", "sys" });
    m_tracks.push_back({ 150, "CS Opening", "sys" });
    m_tracks.push_back({ 152, "CS Credits", "sys" });
    m_tracks.push_back({ 200, "a drop (Title Screen)", "sys" });
    m_tracks.push_back({ 201, "Next force (Character Select)", "sys" });
    m_tracks.push_back({ 202, "CF interlude (Introduction)", "sys" });
    m_tracks.push_back({ 203, "CF continue (Continue Screen)", "sys" });
    m_tracks.push_back({ 204, "CF Field 1 (Main Menu)", "sys" });
    m_tracks.push_back({ 205, "Neo ABYSS (Abyss Mode)", "sys" });
    m_tracks.push_back({ 206, "CF Field 2 (Main Menu V2)", "sys" });
    m_tracks.push_back({ 207, "Online Lobby 1", "sys" });
    m_tracks.push_back({ 208, "Online Lobby 2", "sys" });
    m_tracks.push_back({ 209, "Online Lobby 3", "sys" });
    m_tracks.push_back({ 210, "Online Lobby 4", "sys" });
    m_tracks.push_back({ 211, "Online Lobby 5", "sys" });
    m_tracks.push_back({ 212, "Online Lobby 6", "sys" });
    m_tracks.push_back({ 250, "CF VS (Versus Screen)", "sys" });
    m_tracks.push_back({ 251, "Story Event", "sys" });
    m_tracks.push_back({ 252, "CF winner (Victory Screen)", "sys" });
    m_tracks.push_back({ 253, "CF gameover (Game Over Screen)", "sys" });
    m_tracks.push_back({ 900, "Rebellion (Ragna)", "old" });
    m_tracks.push_back({ 901, "Lust SIN (Jin)", "old" });
    m_tracks.push_back({ 902, "Bullet Dance (Noel)", "old" });
    m_tracks.push_back({ 903, "Queen of rose (Rachel)", "old" });
    m_tracks.push_back({ 904, "Catus Carnival (Taokaka)", "old" });
    m_tracks.push_back({ 905, "MOTOR HEAD (Tager)", "old" });
    m_tracks.push_back({ 906, "Oriental Flower (Litchi)", "old" });
    m_tracks.push_back({ 907, "Thin RED Line (Arakune)", "old" });
    m_tracks.push_back({ 908, "REPPUU (Bang)", "old" });
    m_tracks.push_back({ 909, "Marionette Purple (Carl)", "old" });
    m_tracks.push_back({ 910, "SUSANOOH (Hakumen)", "old" });
    m_tracks.push_back({ 911, "Awakening The Chaos (Nu-13)", "old" });
    m_tracks.push_back({ 912, "Condemnation Wings (Tsubaki)", "old" });
    m_tracks.push_back({ 913, "Gluttony Fang (Hazama)", "old" });
    m_tracks.push_back({ 914, "Sword of Doom (Mu-12)", "old" });
    m_tracks.push_back({ 915, "Alexandrite (Makoto)", "old" });
    m_tracks.push_back({ 916, "Howling Moon (Valkenhayn)", "old" });
    m_tracks.push_back({ 917, "Active Angel (Platinum)", "old" });
    m_tracks.push_back({ 918, "Plastic Night (Relius)", "old" });
    m_tracks.push_back({ 950, "Under Heaven Destruction (Ragna vs Jin)", "old" });
    m_tracks.push_back({ 951, "Imperial Code (Noel vs Jin)", "old" });
    m_tracks.push_back({ 952, "White Requiem (Rachel vs Ragna)", "old" });
    m_tracks.push_back({ 953, "Nightmare Fiction (Ragna vs Hazama)", "old" });
    m_tracks.push_back({ 954, "Memory of Tears (Tsubaki vs Noel)", "old" });
    m_tracks.push_back({ 955, "Childish killer (Tsubaki vs Jin)", "old" });
    m_tracks.push_back({ 956, "Weak Executioner (Arakune vs Litchi)", "old" });
    m_tracks.push_back({ 957, "X-matic (Relius vs Carl)", "old" });
    m_tracks.push_back({ 980, "Endless Despair (Boss Hazama)", "old" });
    m_tracks.push_back({ 981, "Black Onslaught (Boss Ragna)", "old" });
    m_tracks.push_back({ 982, "Blood Pain (Mirror Match)", "old" });

    m_tracks.push_back({ 300, "Story Mode OP", "story" });
    m_tracks.push_back({ 301, "Story: Everyday", "story" });
    m_tracks.push_back({ 302, "Childish killer II", "story" });
    m_tracks.push_back({ 303, "Story: Memory", "story" });
    m_tracks.push_back({ 304, "Bullet Dance", "story" });
    m_tracks.push_back({ 305, "Story: Unease", "story" });
    m_tracks.push_back({ 306, "Story: Countryside", "story" });
    m_tracks.push_back({ 307, "Story: Drama", "story" });
    m_tracks.push_back({ 308, "Story: Everyday B", "story" });
    m_tracks.push_back({ 309, "Story: Everyday E", "story" });
    m_tracks.push_back({ 310, "Thin RED Line", "story" });
    m_tracks.push_back({ 311, "Story: Hurry", "story" });
    m_tracks.push_back({ 312, "Story: Horror", "story" });
    m_tracks.push_back({ 313, "SIX-HEROES", "story" });
    m_tracks.push_back({ 314, "Story: God", "story" });
    m_tracks.push_back({ 315, "Story: Gothic", "story" });
    m_tracks.push_back({ 316, "Story: Techno", "story" });
    m_tracks.push_back({ 317, "Story: Everyday A", "story" });
    m_tracks.push_back({ 318, "Story: Everyday D", "story" });
    m_tracks.push_back({ 319, "BUSHIN", "story" });
    m_tracks.push_back({ 320, "Story: Japanese Style", "story" });
    m_tracks.push_back({ 321, "The Tyrant", "story" });
    m_tracks.push_back({ 322, "Story: Tension", "story" });
    m_tracks.push_back({ 400, "Story: Everyday A", "story" });
    m_tracks.push_back({ 401, "Story: Everyday B", "story" });
    m_tracks.push_back({ 403, "Story: Everyday D", "story" });
    m_tracks.push_back({ 404, "Story: Everyday E", "story" });
    m_tracks.push_back({ 405, "Story: Everyday F", "story" });
    m_tracks.push_back({ 406, "Story: Drama", "story" });
    m_tracks.push_back({ 407, "Story: Horror", "story" });
    m_tracks.push_back({ 408, "Story: Hurry", "story" });
    m_tracks.push_back({ 409, "Story: Gothic", "story" });
    m_tracks.push_back({ 410, "Story: Pop", "story" });
    m_tracks.push_back({ 411, "Story: Techno", "story" });
    m_tracks.push_back({ 412, "Story: Gag", "story" });
    m_tracks.push_back({ 413, "Story: Tension", "story" });
    m_tracks.push_back({ 414, "Story: Memory", "story" });
    m_tracks.push_back({ 415, "Story: Sprint", "story" });
    m_tracks.push_back({ 416, "Story: God", "story" });
    m_tracks.push_back({ 417, "Story: Mystery", "story" });
    m_tracks.push_back({ 418, "Story: Sad", "story" });
    m_tracks.push_back({ 419, "Story: Countryside", "story" });
    m_tracks.push_back({ 420, "Story: Unease", "story" });
    m_tracks.push_back({ 421, "Story: Japanese Style", "story" });
    m_tracks.push_back({ 422, "Story: Grassland", "story" });
    m_tracks.push_back({ 423, "Story: Dark", "story" });
    m_tracks.push_back({ 450, "Story Mode OP", "story" });
    m_tracks.push_back({ 451, "Story Mode OP", "story" });
    m_tracks.push_back({ 452, "Story: Mikado Appears", "story" });
    m_tracks.push_back({ 453, "Story: Noel's Poem", "story" });
    m_tracks.push_back({ 454, "Story: Tsubaki's Whisper", "story" });
    m_tracks.push_back({ 455, "Story: Mikado Highlander", "story" });
    m_tracks.push_back({ 456, "Story: Master Unit", "story" });
    m_tracks.push_back({ 500, "Pandora Tears", "story" });
    m_tracks.push_back({ 501, "Story: Truth", "story" });
    m_tracks.push_back({ 502, "Stardust Memory ~eternal days~", "story" });

    m_tracks.push_back({ 600, "Astral Finish (A)", "astral" });
    m_tracks.push_back({ 601, "Astral Finish (B)", "astral" });
    m_tracks.push_back({ 602, "Astral Finish (C)", "astral" });
    m_tracks.push_back({ 603, "Astral Finish (A)", "astral" });
    m_tracks.push_back({ 604, "Astral Finish (B)", "astral" });
    m_tracks.push_back({ 605, "Astral Finish (C)", "astral" });
    m_tracks.push_back({ 606, "Astral Finish (A)", "astral" });
    m_tracks.push_back({ 607, "Astral Finish (B)", "astral" });
    m_tracks.push_back({ 608, "Astral Finish (C)", "astral" });
    m_tracks.push_back({ 609, "Astral Finish (A)", "astral" });
    m_tracks.push_back({ 610, "Astral Finish (B)", "astral" });
    m_tracks.push_back({ 611, "Astral Finish (C)", "astral" });

    for (auto& track : m_tracks) {
        m_trackEnabled[track.id] = true;
    }
}

// Discover and register custom tracks. Runs the MP3 -> PAC conversion pipeline
// (idempotent: cached .pac files are reused) and appends each converted track to
// the track list under the "custom" category, which the Jukebox renders below
// "astral". Called once from Initialize(), after LoadPreferences() so the user's
// saved enabled/disabled state for previously-seen custom track IDs applies.
void MusicManager::DiscoverCustomTracks() {
    StartCustomMusicDiscovery();
}

void MusicManager::RegisterCustomTracks(const std::vector<CustomTrackInfo>& customTracks) {
    // Discovery can run more than once now (importing a file rescans), and it returns
    // every custom track each time - not just the new one. Drop what a previous run
    // registered first, or the same track is added twice, which shows up as duplicate
    // ids and an ImGui "conflicting ID" complaint over the track list.
    // Ranges, not ">= base": replacements live past kReplacementTrackIdBase and are owned by
    // SyncReplacementTracks. Dropping them here would delete the other feature's rows every
    // time a song was imported.
    m_customTrackRows.clear();
    s_customTrackFiles.erase(std::remove_if(s_customTrackFiles.begin(), s_customTrackFiles.end(),
        [](const std::pair<int, std::string>& entry) { return IsCustomTrackId(entry.first); }),
        s_customTrackFiles.end());

    if (customTracks.empty()) {
        LogMusic("MusicManager: No custom tracks found\n");
        m_customTrackCount = 0;
        RebuildDynamicTracks();
        return;
    }

    LogMusic("MusicManager: Registering %d custom track(s)\n", (int)customTracks.size());

    m_customTrackSource.clear();
    m_customTrackTagGain.clear();

    for (const auto& ct : customTracks) {
        // Add to the track list with "custom" category (appears below "astral")
        m_customTrackRows.push_back({ ct.id, ct.displayName, "custom" });
        m_customTrackSource[ct.id] = ct.sourceName;
        if (ct.hasTagGain)
            m_customTrackTagGain[ct.id] = ct.tagGainDb;

        // Register in the custom filename lookup table
        s_customTrackFiles.push_back({ ct.id, ct.pacFilename });

        // Enable by default unless the user's saved preferences already have an
        // entry for this id (LoadPreferences ran first).
        if (m_trackEnabled.find(ct.id) == m_trackEnabled.end()) {
            m_trackEnabled[ct.id] = true;
        }

        LogMusic("MusicManager: Registered custom track ID=%d name=\"%s\" pac=\"%s\"\n",
            ct.id, ct.displayName.c_str(), ct.pacFilename.c_str());
    }

    m_customTrackCount = (int)customTracks.size();
    RebuildDynamicTracks();
    LogMusic("MusicManager: Total tracks after custom: %d\n", (int)m_tracks.size());
}

void MusicManager::RebuildDynamicTracks() {
    m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
        [](const MusicTrack& track) { return track.id >= kCustomTrackIdBase; }), m_tracks.end());

    // Custom first, then replacements. The Jukebox groups the list by walking it and
    // starting a new header whenever the category changes, so each has to arrive as one
    // unbroken run.
    for (const auto& row : m_customTrackRows) {
        m_tracks.push_back(row);
    }
    for (const auto& row : m_replacementTrackRows) {
        m_tracks.push_back(row);
    }

    // Enable a newly-seen dynamic track unless the user's saved preferences already have an
    // opinion about it (LoadPreferences runs before any of this).
    for (const auto& row : m_customTrackRows) {
        if (m_trackEnabled.find(row.id) == m_trackEnabled.end()) m_trackEnabled[row.id] = true;
    }
    for (const auto& row : m_replacementTrackRows) {
        if (m_trackEnabled.find(row.id) == m_trackEnabled.end()) m_trackEnabled[row.id] = true;
    }

    // m_currentTrack points INTO m_tracks, and the push_backs above can reallocate it -
    // which leaves that pointer dangling for every reader (the Jukebox and the main menu
    // both dereference it every frame). Re-resolve it from the id, which is the field that
    // actually survives.
    m_currentTrack = nullptr;
    for (const auto& t : m_tracks) {
        if (t.id == m_currentTrackId) { m_currentTrack = &t; break; }
    }
}

void MusicManager::SyncReplacementTracks() {
    auto& replacements = BgmReplacementManager::GetInstance();
    if (!replacements.IsInitialized()) {
        return;
    }

    // The signature allocates nothing, which is what makes this safe to call every frame.
    const unsigned int signature = replacements.GetActiveSignature();
    if (m_replacementsSynced && signature == m_replacementSignature) {
        return;
    }
    m_replacementSignature = signature;
    m_replacementsSynced = true;

    m_replacementTrackRows.clear();
    s_replacementTrackFiles.clear();

    for (const auto& entry : replacements.GetActiveReplacements()) {
        // Derived from the table index, so a track keeps the same id across launches and its
        // enabled state in music_preferences.ini keeps applying to it.
        const int id = kReplacementTrackIdBase + entry.tableIndex;

        // Named after the song the user chose, because that is what they are looking for in
        // the list; the shipped track it stands in for is what disambiguates two rows built
        // from the same file.
        std::string name = entry.sourceName.empty() ? entry.baseName : entry.sourceName;
        if (!entry.trackName.empty()) {
            name += " (replaces " + entry.trackName + ")";
        }

        m_replacementTrackRows.push_back({ id, name, "replacements" });
        s_replacementTrackFiles.push_back({ id, entry.relPathNoExt, entry.baseName });
    }

    RebuildDynamicTracks();
    LogMusic("MusicManager: Published %d BGM replacement(s) as Jukebox tracks\n",
        (int)m_replacementTrackRows.size());
}

void MusicManager::StartCustomMusicDiscovery() {
    if (m_customMusicStarted.exchange(true)) return;
    m_customTracksDiscovered = true;
    m_customMusicLoading = true;
    m_customMusicCurrent = 0;
    m_customMusicTotal = 0;
    {
        std::lock_guard<std::mutex> lock(m_customMusicStatusMutex);
        m_customMusicStatus = "Scanning custom music";
    }
    LogMusic("MusicManager: Starting custom music discovery\n");
    m_customMusicFuture = std::async(std::launch::async, [this]() {
        return ConvertCustomMusicOnStartup([this](int current, int total, const std::string& status) {
            m_customMusicCurrent = current;
            m_customMusicTotal = total;
            std::lock_guard<std::mutex> lock(m_customMusicStatusMutex);
            m_customMusicStatus = status;
        },
        [this](const std::string& sourceName) {
            auto it = m_customTrackVolume.find(sourceName);
            return it == m_customTrackVolume.end() ? 0.0f : it->second;
        });
    });
}

std::string MusicManager::GetCustomTrackSourceName(int trackId) const {
    auto it = m_customTrackSource.find(trackId);
    return it == m_customTrackSource.end() ? std::string() : it->second;
}

bool MusicManager::GetCustomTrackTagGainDb(int trackId, float& outGainDb) const {
    auto it = m_customTrackTagGain.find(trackId);
    if (it == m_customTrackTagGain.end())
        return false;
    outGainDb = it->second;
    return true;
}

float MusicManager::GetCustomTrackVolumeDb(int trackId) const {
    const std::string source = GetCustomTrackSourceName(trackId);
    if (source.empty()) return 0.0f;
    auto it = m_customTrackVolume.find(source);
    return it == m_customTrackVolume.end() ? 0.0f : it->second;
}

void MusicManager::SetCustomTrackVolumeDb(int trackId, float volumeDb) {
    const std::string source = GetCustomTrackSourceName(trackId);
    if (source.empty()) return;
    if (GetCustomTrackVolumeDb(trackId) == volumeDb) return;

    m_customTrackVolume[source] = volumeDb;
    SavePreferences();

    // The volume lives inside the converted .pac, so the file has to be rebuilt. The
    // scan notices the recorded gain no longer matches and reconverts just this one.
    RescanCustomMusic();
}

void MusicManager::RescanCustomMusic() {
    if (m_customMusicLoading) return;
    m_customMusicStarted = false;
    StartCustomMusicDiscovery();
}

void MusicManager::PollCustomMusicDiscovery() {
    if (!m_customMusicLoading) return;

    // Update() and JukeboxWindow::Draw() can both reach this method. Serialize
    // completion so the future is consumed and custom tracks are registered once.
    std::lock_guard<std::mutex> completionLock(m_customMusicPollMutex);
    if (!m_customMusicLoading || !m_customMusicFuture.valid()) return;
    if (m_customMusicFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    std::vector<CustomTrackInfo> customTracks = m_customMusicFuture.get();
    RegisterCustomTracks(customTracks);
    m_customMusicLoading = false;
    {
        std::lock_guard<std::mutex> lock(m_customMusicStatusMutex);
        m_customMusicStatus = customTracks.empty()
            ? "No custom MP3 files found"
            : "Custom music ready: " + std::to_string(customTracks.size()) + " track(s)";
    }
}

float MusicManager::GetCustomMusicProgress() const {
    int total = m_customMusicTotal.load();
    if (total <= 0) return m_customMusicLoading ? 0.0f : 1.0f;
    float progress = (float)m_customMusicCurrent.load() / (float)total;
    return progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
}

std::string MusicManager::GetCustomMusicStatus() const {
    std::lock_guard<std::mutex> lock(m_customMusicStatusMutex);
    return m_customMusicStatus;
}

void MusicManager::Initialize() {
    if (m_initialized) return;

    LogMusic("MusicManager::Initialize\n");

    s_musicSelectX = g_gameVals.musicSelect_X;
    s_musicSelectY = g_gameVals.musicSelect_Y;

    if (s_musicSelectX && IsNativeGameTrackId(*s_musicSelectX)) {
        m_gameMusicId = *s_musicSelectX;
        m_currentTrackId = m_gameMusicId;

        for (const auto& track : m_tracks) {
            if (track.id == m_gameMusicId) {
                m_currentTrack = &track;
                break;
            }
        }

        LogMusic("MusicManager: Initial game music ID = %d\n", m_gameMusicId);
        if (m_currentTrack) {
            LogMusic("MusicManager: Initial track = \"%s\"\n", m_currentTrack->name.c_str());
        }
    } else {
        LogMusic("MusicManager: WARNING - musicSelect_X not available! Music rotation will not work.\n");
    }

    LoadPreferences();

    m_initialized = true;
    LogMusic("MusicManager initialized with %d tracks, enabled=%d\n", (int)m_tracks.size(), m_enabled);
}

void MusicManager::OnMatchInit() {
    const bool applyRematchPreference = m_rematchPending && IsVersusMode();
    const int lastPlaylistTrack = m_lastPlaylistTrackId;
    m_rematchPending = false;
    m_pendingRematchTrackId = -1;

    LogMusic("MusicManager: OnMatchInit - resetting BGM state (current=%d anchor=%d controlling=%d loaded=%d mode=%d rematch=%d lastPlaylist=%d)\n",
        m_currentTrackId, m_anchorTrackId, m_modControllingBgm ? 1 : 0, m_customBgmLoaded ? 1 : 0,
        g_gameVals.pGameMode ? *g_gameVals.pGameMode : -1, applyRematchPreference ? 1 : 0, lastPlaylistTrack);
    if (m_customBgmLoaded || m_modControllingBgm) {
        ClearBgmForSceneExit();
    }

    // Every match initially loads the Character Select track (the anchor).
    // Re-sync bookkeeping to that native track first; a VS/Online rematch
    // override, if requested, is queued below and applied only after the fight's
    // audio bank finishes initializing.
    m_customBgmLoaded = false;
    m_modControllingBgm = false;
    if (GetBgmFilename(m_anchorTrackId) != nullptr) {
        m_currentTrackId = m_anchorTrackId;
        m_gameMusicId = m_anchorTrackId;
        m_currentTrack = nullptr;
        for (const auto& t : m_tracks) {
            if (t.id == m_anchorTrackId) { m_currentTrack = &t; break; }
        }
    }

    if (applyRematchPreference) {
        if (m_rematchTrackMode == RematchTrackMode::ResumeLast) {
            if (lastPlaylistTrack >= 0 && GetBgmFilename(lastPlaylistTrack) && IsTrackEnabled(lastPlaylistTrack)) {
                m_pendingRematchTrackId = lastPlaylistTrack;
            }
        } else if (m_rematchTrackMode == RematchTrackMode::PlayNext) {
            const int startingTrack = lastPlaylistTrack >= 0 ? lastPlaylistTrack : m_anchorTrackId;
            if (GetBgmFilename(startingTrack)) {
                m_pendingRematchTrackId = SelectNextTrackAfter(startingTrack);
            }
        }
    }

    LogMusic("MusicManager: Rematch choice mode=%d queuedTrack=%d (anchor=%d)\n",
        (int)m_rematchTrackMode, m_pendingRematchTrackId, m_anchorTrackId);
}

void MusicManager::Update() {
    if (!m_initialized) return;

    PollCustomMusicDiscovery();

    if (m_pendingPlay) {
        m_pendingPlayRetries++;
        if (m_pendingPlayRetries > 300) {
            LogMusic("MusicManager: Pending play TIMEOUT for cue %s\n", m_pendingCueName.c_str());
            m_pendingPlay = false;
            m_pendingSoundObj = nullptr;
        } else {
            __try {
                HMODULE hMod = GetModuleHandleA("BBCF.exe");
                if (hMod) {
                    uintptr_t modBase = (uintptr_t)hMod;
                    void* audioSingleton = *(void**)(modBase + 0x8929C8);
                    if (audioSingleton) {
                        void* bgmPlayer = *(void**)((uintptr_t)audioSingleton + 0x2C);
                        if (bgmPlayer) {
                            void* vtable = *(void**)bgmPlayer;
                            typedef int (__thiscall *PlayBgmFuncType)(void* thisPtr, void* soundObj, const char* path);
                            PlayBgmFuncType playBgm = (PlayBgmFuncType)*(uintptr_t*)((uintptr_t)vtable + 0x30);
                            
                            int state = *(int*)((uintptr_t)bgmPlayer + 0x40);
                            if (state == 1) {
                                playBgm(bgmPlayer, m_pendingSoundObj, m_pendingCueName.c_str());
                            }
                            state = *(int*)((uintptr_t)bgmPlayer + 0x40);
                            if (state == 2) {
                                int success = playBgm(bgmPlayer, m_pendingSoundObj, m_pendingCueName.c_str());
                                if (success) {
                                    LogMusic("MusicManager: Pending play succeeded for cue %s on try %d!\n", 
                                        m_pendingCueName.c_str(), m_pendingPlayRetries);
                                    m_pendingPlay = false;
                                    m_pendingSoundObj = nullptr;
                                }
                            }
                        }
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                LogMusic("MusicManager: Exception in pending play retry! Code 0x%08X\n", GetExceptionCode());
                m_pendingPlay = false;
                m_pendingSoundObj = nullptr;
            }
        }
    }

    // Acquire hook addresses if not yet set
    if (!s_musicSelectX && g_gameVals.musicSelect_X) {
        s_musicSelectX = g_gameVals.musicSelect_X;
        s_musicSelectY = g_gameVals.musicSelect_Y;
        LogMusic("MusicManager: Acquired musicSelect_X at frame level\n");

        if (s_musicSelectX) {
            int startupId = *s_musicSelectX;
            if (IsNativeGameTrackId(startupId)) { // ignore non-track garbage values
                m_gameMusicId = startupId;
                m_currentTrackId = startupId;
                for (const auto& track : m_tracks) {
                    if (track.id == startupId) {
                        m_currentTrack = &track;
                        break;
                    }
                }
            }
        }
    }

    // Detect leaving the match scene (e.g. Training -> Character Select / Main
    // Menu) and soft-reset our custom BGM. A non-selectable leftover track
    // otherwise errors Character Select (red debug screen / lock-up). Runs even
    // if rotation was toggled off mid-match, so a loaded track still gets cleaned
    // up; gated on m_customBgmLoaded so it only fires when we actually took over.
    DetectSceneExitAndUnload();

    if (!m_enabled) return;
    if (!IsInMatch()) return;

    // --- "Return to Character Select?" confirm-dialog handling ---
    // While this dialog is up, restore the initially-selected (selectable) anchor
    // track so the game's exit-to-Character-Select validation reads a supported
    // track (this is what avoids the red debug Character Select screen). Rotation
    // is suspended while the dialog is up; if the user cancels, the interrupted
    // track is re-played.
    {
        bool dialogUp = CheckConfirmDialogUp();
        if (dialogUp && !m_confirmDialogActive) {
            if (m_customBgmLoaded) {
                m_preDialogTrackId = m_currentTrackId;
                LogMusic("MusicManager: Confirm dialog opened -> restoring anchor track %d (interrupting %d)\n",
                    m_anchorTrackId, m_preDialogTrackId);
                RestoreAnchorForSceneExit();
            }
            m_confirmDialogActive = true;
            m_dialogClosedTimer = 0;
        } else if (!dialogUp && m_confirmDialogActive) {
            // Dialog just closed: begin a debounce to distinguish cancel vs confirm.
            m_confirmDialogActive = false;
            m_dialogClosedTimer = 1;
        }

        if (m_dialogClosedTimer > 0) {
            bool stillInMatch = g_gameVals.pGameState && *g_gameVals.pGameState == GameState_InMatch;
            if (!stillInMatch) {
                m_dialogClosedTimer = 0;   // confirmed: we moved to another scene
            } else if (++m_dialogClosedTimer >= 45) { // ~0.75s still in match => cancelled
                if (m_preDialogTrackId >= 0 && m_preDialogTrackId != m_currentTrackId) {
                    LogMusic("MusicManager: Confirm dialog cancelled -> re-playing track %d\n", m_preDialogTrackId);
                    PlayTrack(m_preDialogTrackId);
                }
                m_preDialogTrackId = -1;
                m_dialogClosedTimer = 0;
            }
        }
    }

    // Log current state periodically
    static int logTimer = 0;
    logTimer++;
    if (logTimer % 600 == 0) {
        LogMusic("MusicManager: musicSelect_X=%p, val=%d, currentTrackId=%d\n",
            (void*)s_musicSelectX, s_musicSelectX ? *s_musicSelectX : -1, m_currentTrackId);
    }

    UpdateMusicState();

    SyncReplacementTracks();
    ApplyPendingRematchTrack();

    // Only auto-rotate when actively fighting and the confirm dialog isn't open.
    if (!m_confirmDialogActive && g_gameVals.pMatchState && *g_gameVals.pMatchState == MatchState_Fight) {
        ChangeMusicIfNeeded();
    }
}

// The confirm dialog's message id (format 'e' + 3 digits, e.g. "e144"/"e384") is
// only present in the game's render-phase UI buffer at this slot — it reads as
// zero from the logic phase (MusicManager::Update). So the render path calls
// PollDialogRenderPhase() each frame to latch whether the dialog is visible, and
// Update() consumes that via CheckConfirmDialogUp().
bool MusicManager::CheckConfirmDialogUp() {
    return m_dialogSeenInRender;
}

void MusicManager::PollDialogRenderPhase() {
    HMODULE hMod = GetModuleHandleA("BBCF.exe");
    if (!hMod) { m_dialogSeenInRender = false; return; }
    uintptr_t base = (uintptr_t)hMod;

    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    auto isMsgId = [&](const char* p) {
        return p[0] == 'e' && isDigit(p[1]) && isDigit(p[2]) && isDigit(p[3]);
    };

    bool seen = false;
    bool sig1 = false, sig2 = false; // diagnostic: which signal identified it
    int sig2Slot = -1;                 // diagnostic: which element slot matched
    __try {
        // Signal 1: the dialog's message id (e.g. "e144") at this slot. It's
        // transient, so also check the button-element colors below.
        const char* slot = (const char*)(base + 0x613900);
        if (isMsgId(slot)) {
            sig1 = true;
        } else {
            const char* region = (const char*)(base + 0x6138E0);
            for (int off = 0; off < 0x40; off++) {
                if (isMsgId(region + off)) { sig1 = true; break; }
            }
        }

        // Signal 2: the confirm dialog's button elements live in this UI array and
        // carry the colors 0x4effffff / 0x2effffff (highlighted / non-highlighted).
        // These specific colors appear only while this dialog is up (the pause
        // menu uses different element colors), so they identify the dialog itself.
        if (!sig1) {
            int slotIndex = 0;
            for (uintptr_t off = 0x613884; off <= 0x61398c; off += 0x18, slotIndex++) {
                unsigned int color = *(const unsigned int*)(base + off);
                if (color == 0x4effffffu || color == 0x2effffffu) {
                    sig2 = true;
                    sig2Slot = slotIndex;
                    break;
                }
            }
        }
        seen = sig1 || sig2;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        seen = false;
    }

    // The "return to character select?" confirm dialog lives in the pause menu,
    // which is only reachable during an active fight (MatchState_Fight). It can
    // NOT appear during the pre-fight intro (MatchState_NotStarted), the
    // round-sign / countdown (MatchState_RebelActionRoundSign) or match
    // initialization (MatchState_Initialization). However, the countdown overlay
    // briefly paints one of the UI slots scanned by Signal 2 with the dialog's
    // button colors, producing a single-frame false positive while MatchState==2
    // (observed in DEBUG.txt). That false latch then runs the close-debounce
    // window which blanks the jukebox display. Suppressing the detection in the
    // pre-fight states removes the false positive entirely and cannot affect the
    // real pause-menu dialog. If MatchState is unreadable, fall back to accepting
    // the detection so a transient read failure can never break the real dialog.
    if (seen) {
        int matchState = -1;
        __try {
            if (g_gameVals.pMatchState) matchState = *g_gameVals.pMatchState;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            matchState = -1;
        }
        if (matchState == MatchState_NotStarted ||
            matchState == MatchState_RebelActionRoundSign ||
            matchState == MatchState_Initialization) {
            seen = false;
        }
    }

    // Hysteresis: expose the detection only after the (gated) signal has been
    // continuously present for a short run. The real dialog stays on screen
    // until the user answers it, so a 50 ms latch delay is imperceptible and
    // can't make a real dialog miss its anchor restore in time; but transient
    // single-frame UI flashes (like the countdown overlay hit above) never
    // latch the "dialog is up" state at all.
    const bool gatedSeen = seen;
    {
        auto now = std::chrono::steady_clock::now();
        if (seen) {
            if (!m_dialogSeenRunActive) {
                m_dialogSeenRunActive = true;
                m_dialogSeenRunStart = now;
            }
            seen = (now - m_dialogSeenRunStart) >= std::chrono::milliseconds(50);
        } else {
            m_dialogSeenRunActive = false;
        }
    }

    m_dialogSeenInRender = seen;

    // Diagnostic: trace the dialog state machine (which signal fired, the
    // gated/exposed state, and the latch/debounce transitions). Kept because
    // this detector is heuristic and this area has proven fragile.
    {
        static bool s_lastGated = false;
        static bool s_lastSeen = false;
        static bool s_lastActive = false;
        static bool s_lastDebounce = false;
        bool debounce = (m_dialogClosedTimer > 0);
        if (gatedSeen != s_lastGated || seen != s_lastSeen ||
            m_confirmDialogActive != s_lastActive || debounce != s_lastDebounce) {
            int gs = -1, ms = -1;
            __try {
                if (g_gameVals.pGameState) gs = *g_gameVals.pGameState;
                if (g_gameVals.pMatchState) ms = *g_gameVals.pMatchState;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            LogMusic("MusicManager: DialogState gated=%d seen=%d (sig1=%d sig2=%d slot=%d) active=%d debounce=%d(timer=%d) GameState=%d MatchState=%d track=%d\n",
                gatedSeen ? 1 : 0, seen ? 1 : 0, sig1 ? 1 : 0, sig2 ? 1 : 0, sig2Slot,
                m_confirmDialogActive ? 1 : 0, debounce ? 1 : 0, m_dialogClosedTimer,
                gs, ms, m_currentTrackId);
            s_lastGated = gatedSeen;
            s_lastSeen = seen;
            s_lastActive = m_confirmDialogActive;
            s_lastDebounce = debounce;
        }
    }

    // Update wall-clock timers in the render loop so they keep ticking even when paused.
    //
    // Re-arm the wall-clock anchor ONLY while truly outside the match scene
    // (menus / character select / loading), so the next match starts clean. Do
    // NOT re-arm during the in-match post-dialog debounce (ShouldShowPlayback()
    // false while m_dialogClosedTimer > 0): that window is also entered when the
    // confirm-dialog detector false-positives on the versus/online intro /
    // round-countdown overlay, and re-arming there desyncs the timer from the
    // BGM that is still playing (the jukebox UI resets to 0:00 and rotation
    // fires late by the whole intro+countdown duration). While in the match,
    // m_songStartTime is owned by track detection (UpdateMusicState) and by
    // PlayTrackPhysically; leaving it untouched here keeps it correct.
    if (!IsInMatch()) {
        m_songStartTime = std::chrono::steady_clock::now();
        m_songPlaybackFrames = 0;
        m_framesSinceLastChange = 0;
    } else if (m_currentTrackId >= 0 && !m_confirmDialogActive && !(m_gameMusicId >= 600 && m_gameMusicId <= 611)) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_songStartTime).count();
        int elapsedFrames = (int)(elapsed * 60.0);
        m_songPlaybackFrames = elapsedFrames;
        m_framesSinceLastChange = elapsedFrames;
    }
}

bool MusicManager::IsInMatch() const {
	// "In a match" = the match SCENE is active (stage intro, the fight, and round
	// results), i.e. GameState_InMatch — covers Training / Challenge / local VS /
	// online. Using the scene (not just MatchState_Fight) means the song timer
	// starts when the stage BGM actually begins during the intro, not a few
	// seconds late at the fight countdown.
	return g_gameVals.pGameState && *g_gameVals.pGameState == GameState_InMatch;
}

bool MusicManager::ShouldShowPlayback() const {
	// Show playback info while in the match scene, but NOT while we're in the
	// post-dialog debounce (m_dialogClosedTimer > 0) — that window covers the
	// exit transition to Character Select / Main Menu, where the track is no
	// longer playing, so the UI should read "None" / 00:00 there.
	return IsInMatch() && m_dialogClosedTimer == 0;
}

void MusicManager::UpdateMusicState() {
	// Log match-state transitions and, the moment the match transitions to the
	// victory screen (WinLoseSign(5) -> VictoryScreen(7)), clear the mod's BGM
	// footprint WHILE THE AUDIO ENGINE IS STILL ALIVE. The game's own teardown
	// starts right after this transition, and by the time the victory-screen
	// entry hook (OnMatchRematch) fires, Bank[13] is already mid-teardown:
	// destroying our direct-XACT state there (foreign banks + cue arrays over
	// native structures) halts the game's audio clock, and the main thread then
	// waits on that clock forever -> black screen instead of the match summary.
	// Clearing early (the same proven cleanup as the Character-Select exit)
	// leaves the teardown nothing foreign to choke on; the summary's scene init
	// then rebuilds Bank[13] natively and reloads the originally-selected song.
	{
		int ms = (g_gameVals.pMatchState) ? *g_gameVals.pMatchState : -1;
		if (ms != m_lastMatchState) {
			LogMusic("MusicManager: MatchState %d -> %d (currentTrack=%d, modControlling=%d, customLoaded=%d)\n",
				m_lastMatchState, ms, m_currentTrackId, m_modControllingBgm ? 1 : 0, m_customBgmLoaded ? 1 : 0);
			if (ms == MatchState_VictoryScreen) {
				m_rematchPending = IsVersusMode();
				LogMusic("MusicManager: Victory screen; VS/Online rematch pending=%d lastPlaylist=%d\n",
					m_rematchPending ? 1 : 0, m_lastPlaylistTrackId);
				if (m_customBgmLoaded || m_modControllingBgm) {
					LogMusic("MusicManager: Match end (-> VictoryScreen) with custom BGM active: clearing footprint before teardown\n");
					ClearBgmForSceneExit();
				}
			}
			m_lastMatchState = ms;
		}
	}

	// Read the ACTUAL playing BGM ID from the audio engine, not the menu cursor
	int gameMusicId = -1;

	HMODULE hMod = GetModuleHandleA("BBCF.exe");
	if (hMod) {
		__try {
			gameMusicId = *(int*)((uintptr_t)hMod + AUDIO_MGR_RVA + BGM_ID_OFFSET);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return;
		}
	}

	if (gameMusicId < 0) return;

	// Yield control to the game's native Astral Finish BGM if detected (IDs 600-611)
	if (gameMusicId >= 600 && gameMusicId <= 611) {
		if (m_customBgmLoaded || m_modControllingBgm) {
			LogMusic("MusicManager: Astral Finish BGM %d detected. Yielding control.\n", gameMusicId);
			ClearBgmForSceneExit();
		}
		return;
	}

	// audioMgr+0x1690 doesn't always hold a BGM track id: during matches and in
	// some menus the game reuses it for unrelated values (observed: 96, 112 —
	// neither exists as a track). Only accept values that map to a real BGM
	// file, so a bogus read can't become the "anchor" we present to the game
	// on scene exit (presenting a nonexistent BGM id to Character Select is
	// exactly what makes its validation blow up).
	bool validTrackId = IsNativeGameTrackId(gameMusicId);
	if (!validTrackId && gameMusicId != m_currentTrackId) {
		static int s_lastIgnoredBgmId = -1;
		if (gameMusicId != s_lastIgnoredBgmId) {
			LogMusic("MusicManager: Ignoring non-track BGM id %d from audioMgr+0x1690\n", gameMusicId);
			s_lastIgnoredBgmId = gameMusicId;
		}
	}

	// Detect if the game changed the music. Only while the mod is NOT in control:
	// once we're rotating (m_modControllingBgm), m_currentTrackId is authoritative
	// (set by PlayTrack) and audioMgr+0x1690 holds our supported "anchor" id, so we
	// must not let it override the real track.
	if (validTrackId && !m_modControllingBgm && gameMusicId != m_currentTrackId) {
		m_gameMusicId = gameMusicId;
		m_currentTrackId = gameMusicId;

		// The game loaded this track itself (e.g. on entering training). It's a
		// normally-selectable track, so remember it as the safe "anchor" we present
		// back to the game while we play non-selectable tracks via XACT.
		m_anchorTrackId = gameMusicId;

		// Restart our playback timer and discover the track's true length from disk
		// so rotation still advances at end-of-song.
		m_framesSinceLastChange = 0;
		m_songPlaybackFrames = 0;
		m_songStartTime = std::chrono::steady_clock::now();
		m_currentTrackDurationFrames = GetTrackDurationFramesFromPac(gameMusicId);

		m_currentTrack = nullptr;
		for (const auto& track : m_tracks) {
			if (track.id == gameMusicId) {
				m_currentTrack = &track;
				break;
			}
		}

		if (m_currentTrack) {
			int durSec = m_currentTrackDurationFrames / 60;
			LogMusic("MusicManager: Audio engine playing ID %d = \"%s\" (duration %d frames ~%02d:%02d)\n",
				gameMusicId, m_currentTrack->name.c_str(), m_currentTrackDurationFrames, durSec / 60, durSec % 60);
		}
	}
}

int MusicManager::GetRotationThresholdFrames() const {
	// Advance at the END OF THE SONG, using the live duration read from the wave bank of
	// whichever file is actually playing. What to do when that is missing depends on
	// whether the track has been replaced, which is what the two branches below decide.
	constexpr int MIN_PLAUSIBLE = 60; // 1s guard against a corrupt/short parse
	if (m_currentTrackDurationFrames > MIN_PLAUSIBLE) {
		return m_currentTrackDurationFrames;
	}
	// Nothing live to go on. The precomputed table was generated from the game's own audio,
	// so it only answers for a shipped track whose shipped audio is what is playing. Two
	// cases where it is not:
	//   - a replacement the Jukebox is playing as its own track: not in the table at all
	//   - a shipped track the GAME loaded while a replacement was live: the audio is the
	//     replacement's, so the table's number would cut it short - the original bug
	const char* bgmName = GetBgmFilename(m_currentTrackId);
	const bool gameIsPlayingAReplacement = !m_modControllingBgm && bgmName != nullptr &&
		BgmReplacementManager::GetInstance().HasActiveReplacement(bgmName, true);
	if (!IsReplacementTrackId(m_currentTrackId) && !gameIsPlayingAReplacement) {
		// Shipped track: the table is generated from the game's own audio and covers every
		// one of them, so the constant past it is only a safety net.
		int precomputed = GetTrackDuration(m_currentTrackId);
		if (precomputed > MIN_PLAUSIBLE) {
			return precomputed;
		}
		return MIN_FRAMES_BETWEEN_CHANGES;
	}

	// A replacement whose length could not be read: both sources above describe a file
	// that is not playing, and the short default would machine-gun through the playlist.
	// 0 says "length unknown" - ChangeMusicIfNeeded holds the song on it, and the Jukebox's
	// own "is there a total to show" check already treats it as no total.
	// Warned once per track, because this runs every frame from ChangeMusicIfNeeded and
	// again from the Jukebox window.
	static int s_lastHeldTrackId = -1;
	if (s_lastHeldTrackId != m_currentTrackId) {
		s_lastHeldTrackId = m_currentTrackId;
		LogMusic("MusicManager: WARNING - no duration for replaced track %d; rotation held\n",
			m_currentTrackId);
	}
	return 0;
}

void MusicManager::ChangeMusicIfNeeded() {
	// Advance at the end of the current song (its true duration). A threshold of 0 means
	// that length is unknown, in which case the song is held rather than advanced on a guess.
	const int threshold = GetRotationThresholdFrames();

	if (threshold <= 0 || m_framesSinceLastChange < threshold) {
		return;
	}

	// Repeat Single: restart the current track instead of advancing.
	if (m_repeatSingle) {
		int current = m_currentTrackId;
		if (current >= 0) {
			m_currentTrackId = -1;          // bypass the "already playing" guard
			PlayTrack(current);             // replay the same track
		}
		return;
	}

	// Select the next track
	int nextTrackId = SelectNextTrack();
	if (nextTrackId < 0) return;

	PlayTrack(nextTrackId);
}

int MusicManager::SelectNextTrack() {
    return SelectNextTrackAfter(m_currentTrackId);
}

int MusicManager::SelectNextTrackAfter(int trackId) {
    std::vector<MusicTrack> enabledTracks = GetEnabledTracks();
    if (enabledTracks.empty()) return -1;

    // Find current index in enabled tracks
    int currentIndex = -1;
    for (size_t i = 0; i < enabledTracks.size(); i++) {
        if (enabledTracks[i].id == trackId) {
            currentIndex = (int)i;
            break;
        }
    }

    if (m_rotationMode == MusicRotationMode::Sequential) {
        // Play in order, wrapping back to the start at the end of the playlist —
        // the music never stops on its own (Repeat Single covers looping one
        // track; there's no "end of playlist" state).
        int nextIndex = currentIndex + 1;
        if (nextIndex >= (int)enabledTracks.size()) {
            nextIndex = 0;
        }
        m_sequentialIndex = nextIndex + 1;
        return enabledTracks[nextIndex].id;

    } else {
        // Shuffle (the old "Random" mode is folded in here): play enabled tracks
        // in a shuffled order without repeating until all have played, then
        // reshuffle and carry on.
        bool shuffledPlaylistMatches =
            (int)m_shuffledPlaylist.size() == (int)enabledTracks.size();
        if (shuffledPlaylistMatches) {
            for (const auto& enabledTrack : enabledTracks) {
                if (std::find(m_shuffledPlaylist.begin(), m_shuffledPlaylist.end(), enabledTrack.id) == m_shuffledPlaylist.end()) {
                    shuffledPlaylistMatches = false;
                    break;
                }
            }
        }
        if (m_shuffledPlaylist.empty() || !shuffledPlaylistMatches ||
            m_shuffleIndex >= (int)m_shuffledPlaylist.size()) {
            ShufflePlaylist();
            // Start just after the current track in the shuffled order so we don't
            // immediately replay it.
            for (size_t i = 0; i < m_shuffledPlaylist.size(); i++) {
                if (m_shuffledPlaylist[i] == trackId) {
                    m_shuffleIndex = (int)i + 1;
                    if (m_shuffleIndex >= (int)m_shuffledPlaylist.size()) {
                        m_shuffleIndex = 0;
                    }
                    break;
                }
            }
        }

        int nextTrackId = m_shuffledPlaylist[m_shuffleIndex];
        m_shuffleIndex++;
        return nextTrackId;
    }

    return -1;
}

bool MusicManager::IsVersusMode() const {
    if (!g_gameVals.pGameMode) return false;
    int mode = -1;
    __try {
        mode = *g_gameVals.pGameMode;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return mode == GameMode_Versus || mode == GameMode_Online;
}

void MusicManager::ApplyPendingRematchTrack() {
    if (m_pendingRematchTrackId < 0 || !g_gameVals.pMatchState) {
        return;
    }

    const int matchState = *g_gameVals.pMatchState;
    if (matchState != MatchState_NotStarted &&
        matchState != MatchState_RebelActionRoundSign &&
        matchState != MatchState_Fight) {
        return;
    }

    int trackId = m_pendingRematchTrackId;
    m_pendingRematchTrackId = -1;
    if (!IsVersusMode() || !GetBgmFilename(trackId) || !IsTrackEnabled(trackId)) {
        LogMusic("MusicManager: Dropping invalid/out-of-mode rematch track %d\n", trackId);
        return;
    }

    LogMusic("MusicManager: Applying VS/Online rematch track %d (preference=%d)\n",
        trackId, (int)m_rematchTrackMode);
    PlayTrack(trackId);
}

static int CallPlaySoundObject(uintptr_t playSoundObjAddr, void* playController, void* soundObj, const char* path, int* voiceHandleOut) {
	PlaySoundObjectFuncType playSoundObj = (PlaySoundObjectFuncType)playSoundObjAddr;
	// Build a 256-byte (0x100) fixed char buffer matching the game's internal string type.
	// The game's StringInit copies bytes into the object base, zero-fills the rest to 0x100.
	char pathBuf[0x100];
	memset(pathBuf, 0, sizeof(pathBuf));
	strncpy(pathBuf, path, 0xFF);
	pathBuf[0xFF] = '\0';
	return playSoundObj(playController, soundObj, pathBuf, 0, 0, 0, voiceHandleOut);
}

// SEH exception filter that logs the crash address for diagnostics.
static int FilterException(unsigned int code, struct _EXCEPTION_POINTERS* ep) {
	if (ep && ep->ExceptionRecord) {
		LogMusic("MusicManager: CRASH at address %p (code 0x%08X)\n", ep->ExceptionRecord->ExceptionAddress, code);
	} else {
		LogMusic("MusicManager: CRASH with code 0x%08X\n", code);
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

// ============================================================================
// Track-length discovery — parse the XACT wave bank (.xwb / "WBND")
// ============================================================================
//
// Each BGM .pac is an FPAC container holding an .xsb (sound bank) and an .xwb
// (wave bank). The wave bank stores the true length of the audio, so we can
// advance to the next track at the END OF THE SONG instead of on a fixed timer.
//
// Verified XACT 2.x WBND layout (from BBCF BGM files, see tools/parse_wbnd.py):
//   +0x00 "WBND" signature, +0x04 version, +0x08 header version
//   +0x0C.. five (offset,length) segment pairs:
//       seg0=BankData, seg1=EntryMetaData, seg2=SeekTables,
//       seg3=EntryNames, seg4=EntryWaveData
//   BankData: +0x00 dwFlags, +0x04 dwEntryCount, +0x08 szBankName[64],
//             +0x48 entryMetaDataElementSize, ...
//   Each EntryMetaData entry (24 bytes):
//       +0x00 d0 = flags (low 4 bits) | Duration (high 28 bits, in SAMPLES)
//       +0x04 d1 = WAVEBANKMINIWAVEFORMAT (sampleRate = bits 5..22)
//       +0x08 PlayRegion {offset,length}
//       +0x10 LoopRegion {start,total}   (BBCF BGM = 0,0 -> does not loop)
//
// BBCF BGM audio is WMA, so PCM byte-count math is invalid; the duration comes
// from the Duration field divided by the per-entry sample rate (which varies:
// some tracks are 44100 Hz, others 48000 Hz). Returns the length in game frames
// (60 fps), or 0 on any failure so the caller falls back to the fixed interval.
static int ParseWbndDurationFrames(const char* xwb, int xwbSize) {
	if (!xwb || xwbSize < 0x34) return 0;
	if (memcmp(xwb, "WBND", 4) != 0) return 0;

	auto rd = [&](int off) -> unsigned int {
		if (off < 0 || off + 4 > xwbSize) return 0;
		return *(const unsigned int*)(xwb + off);
	};

	unsigned int bankDataOff = rd(0x0C); // seg0 offset
	unsigned int entryMdOff  = rd(0x14); // seg1 offset
	if (bankDataOff == 0 || bankDataOff + 8 > (unsigned int)xwbSize) return 0;
	if (entryMdOff  == 0 || entryMdOff + 8 > (unsigned int)xwbSize) return 0;

	unsigned int entryCount = rd((int)bankDataOff + 4);
	if (entryCount < 1) return 0;

	unsigned int d0 = rd((int)entryMdOff);     // flags | Duration(samples)
	unsigned int d1 = rd((int)entryMdOff + 4); // miniwaveformat
	unsigned int durationSamples = d0 >> 4;          // high 28 bits
	unsigned int sampleRate = (d1 >> 5) & 0x3FFFF;   // bits 5..22
	if (durationSamples == 0 || sampleRate == 0) return 0;

	unsigned long long frames = (unsigned long long)durationSamples * 60ULL / sampleRate;
	if (frames > 0x7FFFFFFFULL) return 0;
	return (int)frames;
}

// Case-insensitive compare of a name's last 4 chars against a lowercase ".ext".
static bool EndsWithExt(const char* name, int nameLen, const char* dotExt) {
	if (nameLen < 4) return false;
	const char* e = name + nameLen - 4;
	for (int i = 0; i < 4; i++) {
		char c = e[i];
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		if (c != dotExt[i]) return false;
	}
	return true;
}

// ============================================================================
// FPAC container parse — locate the .xsb and .xwb sub-files
// ============================================================================
//
// Each BGM .pac is an FPAC archive. The OLD code assumed fixed offsets
// (.xsb size @+0x38, .xwb offset/size @+0x54/+0x58, data @+0x60) that only hold
// for SHORT filenames ("002_btl_no"). Files with longer names ("050_btl_rgvsjn",
// "950_btl_rgvsjn_old") shift the file table, so those reads returned garbage ->
// "vs" tracks failed to play and "old" tracks CRASHED (CreateWaveBank handed a
// ~2GB size). This walks the table properly for ANY filename length.
//
// Verified FPAC layout (see tools/validate_durations.py):
//   +0x00 "FPAC"; +0x04 dataStart; +0x08 totalSize; +0x0C fileCount; +0x14 nameField
//   stride = (dataStart - 0x20) / fileCount ; file table starts at +0x20
//   entry[i] @ 0x20 + i*stride:
//       name[nameField]  (null-terminated, padded)
//       index  u32 @ entry + nameField + 0
//       offset u32 @ entry + nameField + 4   (relative to dataStart)
//       size   u32 @ entry + nameField + 8
//   sub-file data @ dataStart + offset
//
// Validates that the table (names + meta fields) lies within bufSize and returns
// the sub-files' absolute offsets (from buf start) and sizes. It does NOT require
// the sub-file DATA to be in the buffer — the caller checks that before use (so
// this works both on a fully-loaded file and on just the header read from disk).
static bool ParseFpacTable(const char* buf, int bufSize,
                           int* xsbOff, int* xsbSize, int* xwbOff, int* xwbSize) {
	if (!buf || bufSize < 0x20) return false;
	if (memcmp(buf, "FPAC", 4) != 0) return false;

	auto rd = [&](int off) -> unsigned int {
		if (off < 0 || off + 4 > bufSize) return 0;
		return *(const unsigned int*)(buf + off);
	};

	unsigned int dataStart = rd(0x04);
	unsigned int fileCount = rd(0x0C);
	unsigned int nameField = rd(0x14);
	if (dataStart < 0x20 || dataStart > (unsigned int)bufSize) return false;
	if (fileCount < 1 || fileCount > 8) return false;
	if (nameField < 4 || nameField > 256) return false;
	if (dataStart < 0x20 + fileCount) return false;
	unsigned int stride = (dataStart - 0x20) / fileCount;
	if (stride < nameField + 12) return false;

	bool gotXsb = false, gotXwb = false;
	for (unsigned int i = 0; i < fileCount; i++) {
		int e = (int)(0x20 + i * stride);
		if (e + (int)nameField + 12 > bufSize) return false;

		// Read the (null-terminated, padded) name within the nameField region.
		const char* nm = buf + e;
		int nl = 0;
		while (nl < (int)nameField && nm[nl] != '\0') nl++;

		unsigned int off = rd(e + (int)nameField + 4);
		unsigned int size = rd(e + (int)nameField + 8);
		if (size == 0) return false;
		// Absolute data offset, with overflow guard.
		if (off > 0x7FFFFFFFu || dataStart + off > 0x7FFFFFFFu) return false;
		int aoff = (int)(dataStart + off);

		if (EndsWithExt(nm, nl, ".xsb")) { *xsbOff = aoff; *xsbSize = (int)size; gotXsb = true; }
		else if (EndsWithExt(nm, nl, ".xwb")) { *xwbOff = aoff; *xwbSize = (int)size; gotXwb = true; }
	}
	return gotXsb && gotXwb;
}

// Read the true length (frames @60fps) of a track straight from its .pac file on
// disk. Used when the GAME loads a track itself (e.g. on match start/reset) so
// rotation still advances at end-of-song. Only the FPAC header/table and the WBND
// header are read (a few hundred bytes), not the whole audio file. Returns 0 on
// any failure.
// The path, relative to data/Sound/BGM and without the extension, of the file the GAME will
// have opened for this track: the replacement when one is live, otherwise the shipped file.
// This is a question about the game's own loads only - the Jukebox always plays what its
// track id names. Written into a caller-supplied buffer rather than returned as a
// std::string because the caller uses __try, which MSVC will not accept in a function
// holding an unwindable local (C2712).
static void ResolveBgmLoadName(const char* bgmName, bool mustBeWhatTheGameLoaded,
	char* out, size_t outSize) {
	const std::string replacement = BgmReplacementManager::GetInstance()
		.GetActiveReplacementPlayPath(bgmName, mustBeWhatTheGameLoaded);
	strcpy_s(out, outSize, replacement.empty() ? bgmName : replacement.c_str());
}

static int GetTrackDurationFramesFromPac(int trackId) {
	const char* bgmName = MusicManager::GetBgmFilename(trackId);
	if (!bgmName) return 0;

	// Rotation advances at this number, so for a replaced track it has to come from the
	// file the game actually opened. Reading the shipped .pac instead is what cut a longer
	// replacement off at the original song's length.
	char loadName[MAX_PATH];
	ResolveBgmLoadName(bgmName, true, loadName, sizeof(loadName));

	// Anchored to the game folder: CreateFileA resolves a relative path against the working
	// directory, which is not reliably the install folder (see PlayTrackPhysically).
	char path[MAX_PATH];
	sprintf_s(path, "%s\\data\\Sound\\BGM\\%s.pac", GetGameDirectory().c_str(), loadName);

	HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return 0;

	int frames = 0;
	__try {
		DWORD fileSize = GetFileSize(hFile, NULL);
		// Read enough for the FPAC header + file table (dataStart <= 0x80 for all
		// known files; 0x100 leaves margin). ParseFpacTable bounds-checks the table.
		char hdr[0x100];
		DWORD got = 0;
		int hdrSize = (fileSize < sizeof(hdr)) ? (int)fileSize : (int)sizeof(hdr);
		if (ReadFile(hFile, hdr, hdrSize, &got, NULL) && got >= 0x20) {
			int xsbOff, xsbSize, xwbOff, xwbSize;
			if (ParseFpacTable(hdr, (int)got, &xsbOff, &xsbSize, &xwbOff, &xwbSize) &&
				xwbOff > 0 && xwbSize >= 0x34 &&
				(DWORD)xwbOff + (DWORD)xwbSize <= fileSize) {
				// Read just the WBND header (the entry metadata we need is within
				// the first ~0xAC bytes of the wave bank).
				char xwb[0x100];
				int chunk = (xwbSize < (int)sizeof(xwb)) ? xwbSize : (int)sizeof(xwb);
				if (SetFilePointer(hFile, xwbOff, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
					ReadFile(hFile, xwb, chunk, &got, NULL) && got >= 0x34) {
					frames = ParseWbndDurationFrames(xwb, (int)got);
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		frames = 0;
	}
	CloseHandle(hFile);
	return frames;
}

// ============================================================================
// PlayTrackPhysically — Mid-match BGM change via direct XACT manipulation
// ============================================================================
//
// WORKING PIPELINE (discovered 2026-07-19 after 10+ patches of reverse engineering):
//
//   STEP 1: Stop current BGM
//     - Deactivate audioManager BGM slot 0 flags
//     - Call CSoundBank_XACT::Stop (vtable[0x1C]) on Bank[13] source[0]
//     - Call CSoundBank_XACT::Clear (RVA 0x051890) to release XACT SB/WB handles
//     - Re-init cues array via InitCues (RVA 0x0510D0) and zero-fill it
//     - Null out playController slot referencing old BGM sound object
//     - Null out soundSlotMgr slot 0x3F to bypass RegisterBgm's free()
//
//   STEP 2: Load new BGM from disk
//     - RegisterBgm (RVA 0x7C930) to allocate slot buffer of file size
//     - Win32 ReadFile to copy .pac data directly into the slot buffer
//     - Parse FPAC header: .xsb at +0x60, .xwb at +0x190
//     - Register Wave Bank via CSoundBank_XACT wrapper (RVA 0x050DD0)
//       which calls IXACTEngine::CreateInMemoryWaveBank (COM vtable[0x28])
//     - Register Sound Bank via CSoundBank_XACT wrapper (RVA 0x050D40)
//       which calls IXACTEngine::CreateSoundBank (COM vtable[0x24])
//
//   STEP 3: Play new BGM
//     - Call CSoundBank_XACT::Play (RVA 0x0515B0) with cue name string
//     - CRITICAL: 4th argument (param3) must be a valid pointer to a
//       zero-initialized struct, NOT null. The function dereferences
//       param3+0x04, +0x08, +0x0C internally. Passing 0 crashes at
//       RVA 0x051361.
//     - Play iterates registered IXACTSoundBank COM objects (bank13+0x08
//       array), calls IXACTSoundBank::GetCueIndex (COM vtable[0]) with
//       the cue name, then calls internal RVA 0x051340 to create a
//       CSoundCue_XACT and start XACT playback.
//
//   STEP 4: Synchronize game state
//     - Write trackId to audioManager+0x1690
//     - Update musicSelect_X/Y pointers for UI sync
//
// Key discoveries that made this work:
//   - The game's XACT engine (xactengine2_10.dll) is version 2.x, NOT 3.x
//   - IXACTSoundBank COM vtable[0] = GetCueIndex (returns XACTINDEX in ax)
//   - IXACTSoundBank COM vtable[5] = Play (ret 0xC, 3 stack args)
//   - CSoundBank_XACT at bank13+0x08 stores registered IXACTSoundBank* array
//   - Bank[13] of CSoundEngine_XACT singleton is the BGM bank
//   - CSoundEngine_XACT singleton at modBase + 0x623630 (accessor RVA 0x00E1A0)
// ============================================================================

// Bank[13] layout (CSoundBank_XACT):
//   +0x08: IXACTSoundBank* array (fixed 16 entries), count at +0x48
//   +0x4C: wave-bank handle array (fixed 16 entries), count at +0x8C
//   +0x90: cue array pointer (0x98-byte CSoundCue_XACT elements)
// InitCues (RVA 0x0510D0) = Clear (vtable[0x08], RVA 0x051890) + fresh cue
// array allocation; Clear releases all SBs (vtable[0x18]) and WB handles
// (engine refcount table) and zeroes the counts. So after InitCues the bank
// is empty and ready for exactly one custom SB/WB pair (registered below).
bool MusicManager::PlayTrackPhysically(uintptr_t modBase, int trackId, const char* bgmName, int* outDurationFrames, int presentedId, const char* cueOverride) {
	if (outDurationFrames) *outDurationFrames = 0;
	uintptr_t audioMgrAddr = modBase + AUDIO_MGR_RVA;
	uintptr_t playControllerAddr = modBase + PLAY_CONTROLLER_RVA;
	uintptr_t soundSlotMgrAddr = modBase + SOUND_SLOT_MGR_RVA;
	uintptr_t soundEngineAddr = modBase + SOUND_ENGINE_RVA;

	// Slot 0x3F is the game's active-BGM slot (the milestone's proven approach).
	// Use a SCRATCH slot (0x3E) for the mod's track and leave slot 0x3F (the
	// game's active-BGM slot) holding the game's original track. Character Select
	// validates slot 0x3F, so keeping the original there avoids the red debug
	// screen that appeared whenever the mod overwrote it with a different track.
	constexpr int BGM_SLOT_INDEX = 0x3E;

	// Build the path strings
	char physicalPath[260];
	char logicalPath[260];
	// bgmName is used verbatim: a native track id means the SHIPPED file, deliberately, so
	// that selecting a replaced track in the Jukebox still plays the original. A replacement
	// is reached by picking its own row in the list, whose id resolves to the replacement's
	// path here. Either way the loader takes a subdirectory happily - that is how PreviewPac
	// auditions one.
	//
	// Anchored to the game folder: this one is opened with CreateFileA, so a moved working
	// directory would make it miss. The logical path below stays relative - that one is
	// handed to the game, which resolves it its own way.
	sprintf_s(physicalPath, "%s\\data\\Sound\\BGM\\%s.pac", GetGameDirectory().c_str(), bgmName);
	sprintf_s(logicalPath, "data/sound/BGM/%s.pac", bgmName);
	LogMusic("MusicManager: Loading BGM file: %s (logical: %s)\n", physicalPath, logicalPath);

	// --- STEP 1: Stop the current BGM audio and clear playController references ---
	__try {
		// Deactivate BGM Slot 0 in audioManager to prevent race conditions
		*(int*)(audioMgrAddr + 0x118 + 0x00) = 0;   // active = 0
		*(int*)(audioMgrAddr + 0x118 + 0x04) = 0;   // state = 0
		*(int*)(audioMgrAddr + 0x118 + 0x10) = -1;  // voice = -1
		LogMusic("MusicManager: Deactivated BGM Slot 0\n");

		// Stop the BGM audio in XACT Bank[13]
		void** bankArray = *(void***)(soundEngineAddr + 0x04);
		int bankCount = *(int*)(soundEngineAddr + 0x10);
		if (bankArray && bankCount > 13) {
			void* bank13 = bankArray[13];
			if (bank13) {
				int stopIndex = 0;
				typedef void (__thiscall *BankStopWrapperType)(void* bank, int* idx);
				BankStopWrapperType stopWrapper = (BankStopWrapperType)(modBase + BANK_STOP_RVA);
				stopWrapper(bank13, &stopIndex);
				LogMusic("MusicManager: Stopped BGM source[0] via Bank[13]->vtable[0x1C]\n");

				// Re-init the cue array. Verified by disassembly: InitCues (RVA
				// 0x0510D0) calls CSoundBank_XACT::vtable[0x08] = Clear (RVA
				// 0x051890) FIRST: it destroys the playing cues, releases every
				// registered sound bank (SB objects via their vtable[0x18], WB
				// handles via the engine's refcount table) and zeroes the bank
				// arrays, THEN allocates the fresh cue array the registration
				// functions need. So this one call resets Bank[13] to empty the
				// same way the game's own scene transitions do (no bank objects
				// leak across rotations).
				//
				// Releasing the previous custom wave bank here is also what makes
				// it safe for RegisterBgm (STEP 2) to free the previous slot
				// buffer: that wave bank was the buffer's only remaining user,
				// so recycling it ends the ~3 MB-per-rotation buffer leak that
				// degraded long sessions (heap growth -> freezes/crashes).
				typedef void (__thiscall *BankInitCuesFuncType)(void* bank, int cueCount);
				BankInitCuesFuncType initCues = (BankInitCuesFuncType)(modBase + 0x0510D0); // Method 1
				initCues(bank13, 1);

				// Zero-initialize the cue array structure past the vtable pointer to avoid dereferencing garbage on play failures
				void* cues_ptr = *(void**)((char*)bank13 + 0x90);
				if (cues_ptr) {
					memset((char*)cues_ptr + 4, 0, 0x98 - 4);
				}

				// MOD-LOADED TRACKS ONLY: belt-and-suspenders reset of the registered
				// sound/wave bank count fields and slot arrays so the registration
				// below lands deterministically in slot 0. InitCues already zeroes
				// these (see above), so for native tracks this is a no-op — it is
				// deliberately gated to the dynamic id ranges (custom Jukebox songs and
				// published BGM replacements, both of which are a .pac of ours loaded
				// out of a subdirectory) to guarantee the native rotation path is
				// byte-for-byte unchanged. A preview passes id -1 and keeps the native
				// path it has always taken.
				if (trackId >= kCustomTrackIdBase) {
					*(int*)((char*)bank13 + 0x48) = 0;   // sound bank count
					*(int*)((char*)bank13 + 0x8C) = 0;   // wave bank count
					memset((char*)bank13 + 0x08, 0, 16 * sizeof(void*)); // SB pointer array
					memset((char*)bank13 + 0x4C, 0, 16 * sizeof(void*)); // WB handle array
				}

				LogMusic("MusicManager: Cleared Bank[13] and re-initialized cues\n");
			}
		}

		// Find and clear any playController slots referencing the current BGM
		// soundObj — must happen BEFORE RegisterBgm (STEP 2) frees that buffer.
		void* currentBgmObj = nullptr;
		__try {
			currentBgmObj = *(void**)(soundSlotMgrAddr + BGM_SLOT_INDEX * 8);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			LogMusic("MusicManager: Failed to read BGM sound object from slot 0x%X\n", BGM_SLOT_INDEX);
		}

		if (currentBgmObj) {
			__try {
				void** slotsPtr = *(void***)(playControllerAddr + 0);
				int maxSlots = *(int*)(playControllerAddr + 4);
				if (slotsPtr) {
					for (int i = 0; i < maxSlots; i++) {
						uintptr_t slotAddr = (uintptr_t)slotsPtr + i * 0x124;
						void* slotSoundObj = *(void**)(slotAddr + 0x00);
						if (slotSoundObj == currentBgmObj) {
							// Found active slot pointing to BGM sound object!
							// Clear status to 0 (inactive) and nullify pointer to prevent crashes
							*(int*)(slotAddr + 0x118) = 0;
							*(void**)(slotAddr + 0x00) = nullptr;
							LogMusic("MusicManager: Cleared playController slot %d referencing BGM soundObj %p\n", i, currentBgmObj);
						}
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				LogMusic("MusicManager: Exception while clearing playController slots\n");
			}

			// NOTE: the slot is deliberately LEFT in place. RegisterBgm (STEP 2)
			// frees the previous buffer itself — safe now, because the Clear
			// above released the wave bank that referenced it (this is the same
			// stop -> release -> free ordering the game's native scene cleanup
			// uses). The old "null the slot to bypass the free" trick leaked
			// ~3 MB per rotation.
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		LogMusic("MusicManager: Stop exception 0x%08X (continuing)\n", GetExceptionCode());
	}

	// --- STEP 2: Read physical BGM file synchronously and allocate slot buffer ---
	HANDLE hFile = CreateFileA(physicalPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		LogMusic("MusicManager: FAIL - Cannot open physical BGM file: %s\n", physicalPath);
		return false;
	}

	DWORD fileSize = GetFileSize(hFile, NULL);
	if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
		LogMusic("MusicManager: FAIL - Invalid file size for: %s\n", physicalPath);
		CloseHandle(hFile);
		return false;
	}

	// Register BGM to allocate the slot buffer of fileSize (arg3 is fileSize)
	RegisterFuncType registerBgm = (RegisterFuncType)(modBase + REGISTER_RVA);
	int regResult = 0;
	__try {
		regResult = registerBgm((void*)soundSlotMgrAddr, BGM_SLOT_INDEX, (void*)fileSize);
		LogMusic("MusicManager: RegisterBgm allocated slot=0x%X size=%d, result=%d\n", BGM_SLOT_INDEX, fileSize, regResult);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		LogMusic("MusicManager: RegisterBgm CRASHED! Exception 0x%08X\n", GetExceptionCode());
		CloseHandle(hFile);
		return false;
	}

	GetSoundObjFuncType getSoundObj = (GetSoundObjFuncType)(modBase + GET_SOUND_OBJ_RVA);
	void* soundObj = getSoundObj((void*)soundSlotMgrAddr, BGM_SLOT_INDEX);
	if (!soundObj) {
		LogMusic("MusicManager: FAIL - GetSoundObjectPtr returned null\n");
		CloseHandle(hFile);
		return false;
	}

	// Read file contents synchronously into the allocated slot buffer
	DWORD bytesRead = 0;
	BOOL readSuccess = ReadFile(hFile, soundObj, fileSize, &bytesRead, NULL);
	CloseHandle(hFile);

	if (!readSuccess || bytesRead != fileSize) {
		LogMusic("MusicManager: FAIL - ReadFile failed (read %d of %d bytes)\n", bytesRead, fileSize);
		return false;
	}
	LogMusic("MusicManager: Successfully read %d BGM bytes directly into slot buffer %p\n", bytesRead, soundObj);

	// Parse the FPAC container to locate the .xsb (Sound Bank) and .xwb (Wave
	// Bank) streams. Walks the file table so it works for ANY filename length
	// (short "btl" names AND long "vs"/"old" names — the old fixed offsets broke
	// those and crashed on "old" tracks), and bounds-checks the data so a
	// malformed .pac fails gracefully instead of crashing.
	int xsbOff = 0, xsbSize = 0, xwbOff = 0, xwbSize = 0;
	if (!ParseFpacTable((const char*)soundObj, (int)fileSize, &xsbOff, &xsbSize, &xwbOff, &xwbSize)) {
		LogMusic("MusicManager: FAIL - Could not parse FPAC file table for %s\n", bgmName);
		return false;
	}
	if (xsbOff < 0 || xsbSize <= 0 || (DWORD)xsbOff + (DWORD)xsbSize > fileSize ||
		xwbOff < 0 || xwbSize <= 0 || (DWORD)xwbOff + (DWORD)xwbSize > fileSize) {
		LogMusic("MusicManager: FAIL - FPAC sub-file out of bounds (xsb %d+%d, xwb %d+%d, fileSize %u)\n",
			xsbOff, xsbSize, xwbOff, xwbSize, (unsigned)fileSize);
		return false;
	}

	char* soundObjBytes = (char*)soundObj;
	void* xsbData = soundObjBytes + xsbOff;
	void* xwbData = soundObjBytes + xwbOff;

	LogMusic("MusicManager: Parsed FPAC: .xsb at offset 0x%X (size=%d), .xwb at offset 0x%X (size=%d)\n",
		xsbOff, xsbSize, xwbOff, xwbSize);

	// Discover the true track length from the loaded wave bank so rotation can
	// advance at end-of-song instead of on a fixed timer.
	if (outDurationFrames) {
		*outDurationFrames = ParseWbndDurationFrames((const char*)xwbData, xwbSize);
		int durSec = *outDurationFrames / 60;
		LogMusic("MusicManager: Parsed BGM duration from wave bank: %d frames (~%02d:%02d)\n",
			*outDurationFrames, durSec / 60, durSec % 60);
	}

	// Register the new Wave Bank and Sound Bank into CSoundBank_XACT (Bank[13])
	// Wave Bank MUST be registered first — the Sound Bank references it.
	// These wrapper functions call IXACTEngine::CreateInMemoryWaveBank (COM vtable[0x28])
	// and IXACTEngine::CreateSoundBank (COM vtable[0x24]) respectively.
	__try {
		void** bankArray = *(void***)(soundEngineAddr + 0x04);
		int bankCount = *(int*)(soundEngineAddr + 0x10);
		if (bankArray && bankCount > 13) {
			void* bank13 = bankArray[13];
			if (bank13) {
				// vtable[0x10] = CreateWaveBank wrapper (RVA 0x050DD0)
				// Calls IXACTEngine::CreateInMemoryWaveBank, stores WB handle at bank13+0x4C[n]
				typedef int (__thiscall *CreateWaveBankFuncType)(void* bank, void* data, int size);
				CreateWaveBankFuncType createWB = (CreateWaveBankFuncType)(modBase + 0x050DD0);
				int wbResult = createWB(bank13, xwbData, xwbSize);
				LogMusic("MusicManager: Registered Wave Bank, HRESULT = 0x%08X\n", wbResult);

				// vtable[0x0C] = CreateSoundBank wrapper (RVA 0x050D40)
				// Calls IXACTEngine::CreateSoundBank, stores SB handle at bank13+0x08[n]
				typedef int (__thiscall *CreateSoundBankFuncType)(void* bank, void* data, int size);
				CreateSoundBankFuncType createSB = (CreateSoundBankFuncType)(modBase + 0x050D40);
				int sbResult = createSB(bank13, xsbData, xsbSize);
				LogMusic("MusicManager: Registered Sound Bank, HRESULT = 0x%08X\n", sbResult);
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		LogMusic("MusicManager: CRASH during XACT bank registration! Exception 0x%08X\n", GetExceptionCode());
		return false;
	}

	// --- STEP 3: Play BGM via CSoundBank_XACT::Play (vtable[0x14], RVA 0x0515B0) ---
	// This wrapper iterates all registered IXACTSoundBank COM objects in bank13,
	// calls IXACTSoundBank::GetCueIndex (COM vtable[0]) to resolve the cue name,
	// then calls internal function RVA 0x051340 to create a CSoundCue_XACT and
	// start XACT playback through the per-frame CSoundBank_XACT::Run tick.
	__try {
		void** bankArray = *(void***)(soundEngineAddr + 0x04);
		int bankCount = *(int*)(soundEngineAddr + 0x10);
		if (bankArray && bankCount > 13) {
			void* bank13 = bankArray[13];
			if (bank13) {
				// Get the registered Sound Bank pointer at bank13 + 8
				void* registeredSB = *(void**)((char*)bank13 + 8);
				if (!registeredSB) {
					LogMusic("MusicManager: FAIL - registeredSB pointer is null! Cannot play BGM\n");
					return false;
				}

				// CRITICAL: param3 must be a valid pointer, NOT null/zero.
				// CSoundBank_XACT::Play dereferences param3->fields at +0x04, +0x08, +0x0C
				// internally (at RVA 0x051361). Passing 0 causes access violation 0xC0000005.
				// A zero-initialized struct satisfies all reads safely.
				int dummyParams[4] = { 0, 0, 0, 0 };
				int playResult = -1;
				typedef void (__thiscall *BankPlayFuncType)(void* bank, int* resultOut, const char* cueName, void* param3);
				BankPlayFuncType playCue = (BankPlayFuncType)(modBase + 0x0515B0);
				// Jukebox custom tracks use the complete native 000_btl_rg XACT bank
				// template; only their outer .pac filename is custom. Replacement
				// previews supply their real cue explicitly, while native tracks use
				// their normal filename.
				// Previews supply their real cue explicitly. Otherwise the id decides:
				// a replacement's cue is the original base filename, not its own path,
				// and a custom track's is the native template's.
				const char* resolvedCue = MusicManager::GetBgmCueName(trackId);
				const char* playCueName = cueOverride
					? cueOverride
					: (resolvedCue ? resolvedCue : bgmName);
				playCue(bank13, &playResult, playCueName, dummyParams);
				LogMusic("MusicManager: Direct CSoundBank_XACT::Play(\"%s\") returned %d\n", playCueName, playResult);
			}
		}
	}
	__except (FilterException(GetExceptionCode(), GetExceptionInformation())) {
		return false;
	}

	// --- STEP 4: Synchronize game state ---
	// IMPORTANT: present a SUPPORTED "anchor" track id to the game (presentedId),
	// NOT the real trackId. Non-selectable tracks (vs/old/sys/...) left in
	// audioMgr+0x1690 / musicSelect_X make the game error (red debug screen) when
	// it next processes the BGM — e.g. on exiting Training to Character Select.
	// The real track still plays via the XACT manipulation above; this only keeps
	// the game-facing state valid.
	__try {
		*(int*)(audioMgrAddr + 0x1690) = presentedId;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		LogMusic("MusicManager: Failed to write presentedId to audioManager + 0x1690\n");
	}

	// Sync game menu cursor state (also the supported anchor, never a raw vs/old id)
	if (MusicManager::s_musicSelectX) *MusicManager::s_musicSelectX = presentedId;
	if (MusicManager::s_musicSelectY) *MusicManager::s_musicSelectY = 0;

	// Restore audioManager BGM slot 0 to its native (active) state.
	// We deactivated it in STEP 1 to prevent race conditions during loading/registration.
	// The game's training mode position reset checks if this slot is active.
	__try {
		if (m_audioSlot0Captured) {
			*(int*)(audioMgrAddr + 0x118 + 0x00) = m_origSlot0Active;
			*(int*)(audioMgrAddr + 0x118 + 0x04) = m_origSlot0State;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		LogMusic("MusicManager: Failed to restore Slot 0 active status\n");
	}

	m_songStartTime = std::chrono::steady_clock::now();
	LogMusic("MusicManager: PlayTrackPhysically completed\n");
	return true;
}

void MusicManager::PlayTrack(int trackId, bool force) {
	LogMusic("MusicManager: PlayTrack(%d)\n", trackId);

	HMODULE hMod = GetModuleHandleA("BBCF.exe");
	if (!hMod) {
		LogMusic("MusicManager: PlayTrack(%d) - FAIL: BBCF.exe module not found\n", trackId);
		return;
	}

	uintptr_t modBase = (uintptr_t)hMod;

	// Skip if this is already the current track (compare against our tracked
	// track, not audioMgr+0x1690 which holds the selectable "anchor" id) - unless the
	// caller needs it reloaded because the file behind it has been rewritten.
	if (m_currentTrackId == trackId && !force) {
		LogMusic("MusicManager: Track %d already playing, skipping\n", trackId);
		return;
	}

	// Determine BGM filename
	const char* bgmName = GetBgmFilename(trackId);
	if (!bgmName) {
		LogMusic("MusicManager: PlayTrack(%d) - FAIL: Unknown BGM filename\n", trackId);
		return;
	}

	LogMusic("MusicManager: Changing BGM from %d to %d (%s)\n", m_currentTrackId, trackId, bgmName);

	// Capture the native audioManager BGM slot 0 state ONCE, before we ever
	// deactivate it (PlayTrackPhysically STEP 1 does), so scene exit can restore
	// it and the game's native Character Select BGM init finds an active slot.
	if (!m_audioSlot0Captured) {
		__try {
			uintptr_t audioMgrAddr = modBase + AUDIO_MGR_RVA;
			m_origSlot0Active = *(int*)(audioMgrAddr + 0x118 + 0x00);
			m_origSlot0State  = *(int*)(audioMgrAddr + 0x118 + 0x04);
			m_audioSlot0Captured = true;
			LogMusic("MusicManager: Captured native audioMgr slot0 (active=%d, state=%d)\n",
				m_origSlot0Active, m_origSlot0State);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	if (!PlayTrackPhysically(modBase, trackId, bgmName, &m_currentTrackDurationFrames, m_anchorTrackId)) {
		LogMusic("MusicManager: PlayTrackPhysically failed\n");
		return;
	}

	LogMusic("MusicManager: Successfully changed BGM to track %d (%s)\n", trackId, bgmName);

	m_currentTrackId = trackId;
	m_framesSinceLastChange = 0;
	m_songPlaybackFrames = 0;
	m_modControllingBgm = true; // the mod is now the authority on the current track
	m_customBgmLoaded = true;   // we've taken over BGM; needs soft-reset on scene exit
	if (IsVersusMode()) {
		m_lastPlaylistTrackId = trackId;
	}

	m_currentTrack = nullptr;
	for (const auto& t : m_tracks) {
		if (t.id == trackId) { m_currentTrack = &t; break; }
	}

	if (m_currentTrack) {
		LogMusic("MusicManager: Track = \"%s\"\n", m_currentTrack->name.c_str());
	}
}

bool MusicManager::PreviewPac(const std::string& relPathNoExt, const std::string& cueName) {
	if (!IsInMatch()) {
		LogMusic("MusicManager: PreviewPac(%s) ignored - only works inside a match\n",
			relPathNoExt.c_str());
		return false;
	}

	HMODULE hMod = GetModuleHandleA("BBCF.exe");
	if (!hMod) return false;
	const uintptr_t modBase = (uintptr_t)hMod;

	if (!m_audioSlot0Captured) {
		__try {
			const uintptr_t audioMgrAddr = modBase + AUDIO_MGR_RVA;
			m_origSlot0Active = *(int*)(audioMgrAddr + 0x118 + 0x00);
			m_origSlot0State  = *(int*)(audioMgrAddr + 0x118 + 0x04);
			m_audioSlot0Captured = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	LogMusic("MusicManager: PreviewPac path=\"%s\" cue=\"%s\"\n",
		relPathNoExt.c_str(), cueName.c_str());

	int durationFrames = 0;
	if (!PlayTrackPhysically(modBase, -1, relPathNoExt.c_str(), &durationFrames,
			m_anchorTrackId, cueName.c_str())) {
		LogMusic("MusicManager: PreviewPac FAILED for \"%s\"\n", relPathNoExt.c_str());
		return false;
	}

	// The mod is now driving BGM, so the scene-exit cleanup has to know - otherwise
	// leaving the match with a preview loaded errors Character Select.
	m_currentTrackDurationFrames = durationFrames;
	m_currentTrackId = -1;
	m_currentTrack = nullptr;
	m_modControllingBgm = true;
	m_customBgmLoaded = true;
	m_framesSinceLastChange = 0;
	m_songPlaybackFrames = 0;
	m_songStartTime = std::chrono::steady_clock::now();
	return true;
}

void MusicManager::PlayNextTrack() {
    // Pick the next track according to the current rotation mode.
    int nextTrackId = SelectNextTrack();
    if (nextTrackId < 0) {
        LogMusic("MusicManager: PlayNextTrack - no enabled tracks\n");
        return;
    }
    PlayTrack(nextTrackId);
}

void MusicManager::ShufflePlaylist() {
    m_shuffledPlaylist.clear();

    std::vector<MusicTrack> enabledTracks = GetEnabledTracks();
    for (const auto& track : enabledTracks) {
        m_shuffledPlaylist.push_back(track.id);
    }

    static std::mt19937 rng(static_cast<unsigned int>(std::time(nullptr)));
    std::shuffle(m_shuffledPlaylist.begin(), m_shuffledPlaylist.end(), rng);
    m_shuffleIndex = 0;
}

std::vector<MusicTrack> MusicManager::GetEnabledTracks() const {
    std::vector<MusicTrack> enabled;
    for (const auto& track : m_tracks) {
        auto it = m_trackEnabled.find(track.id);
        if (it != m_trackEnabled.end() && it->second) {
            enabled.push_back(track);
        }
    }
    return enabled;
}

void MusicManager::ToggleTrackEnabled(int trackId) {
    auto it = m_trackEnabled.find(trackId);
    if (it != m_trackEnabled.end()) {
        it->second = !it->second;
        SavePreferences();
    }
}

bool MusicManager::IsTrackEnabled(int trackId) const {
    auto it = m_trackEnabled.find(trackId);
    if (it != m_trackEnabled.end()) {
        return it->second;
    }
    return true;
}

void MusicManager::SetCategoryEnabled(const std::string& category, bool enabled) {
    for (const auto& track : m_tracks) {
        if (track.category == category) {
            m_trackEnabled[track.id] = enabled;
        }
    }
    SavePreferences();
}

// Returns 1 = all enabled, 0 = all disabled, -1 = mixed (or no tracks in category).
int MusicManager::GetCategoryEnabledState(const std::string& category) const {
    bool anyOn = false, anyOff = false;
    for (const auto& track : m_tracks) {
        if (track.category == category) {
            if (IsTrackEnabled(track.id)) anyOn = true; else anyOff = true;
        }
    }
    if (anyOn && !anyOff) return 1;
    if (anyOff && !anyOn) return 0;
    return -1;
}

// Called every frame. When the game transitions out of the match scene (e.g.
// Training -> Character Select / Main Menu) while a custom BGM is loaded,
// soft-reset the audio state so the game loads its normal scene BGM cleanly.
void MusicManager::DetectSceneExitAndUnload() {
    int gameState = -1;
    __try {
        if (g_gameVals.pGameState) gameState = *g_gameVals.pGameState;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        gameState = -1;
    }
    if (gameState < 0) {
        return; // state unreadable this frame; keep last known, retry next frame
    }

    if (gameState == GameState_CharacterSelectionScreen ||
        gameState == GameState_MainMenu || gameState == GameState_Lobby) {
        m_rematchPending = false;
        m_pendingRematchTrackId = -1;
        m_lastPlaylistTrackId = -1;
    }

    if (m_lastGameState == GameState_InMatch && gameState != GameState_InMatch && m_customBgmLoaded) {
        LogMusic("MusicManager: Leaving match scene (GameState %d -> %d); soft-resetting custom BGM\n",
            m_lastGameState, gameState);
        UnloadCustomBgm();
    }
    m_lastGameState = gameState;
}

// Soft-reset: stop + clear the BGM bank, null the BGM slot, and reset the
// track-id / music-select cursors to a safe, selectable default. Mirrors the
// proven-safe stop/clear sequence in PlayTrackPhysically STEP 1. Does NOT
// register or play anything — the game's own scene loader takes over from here.
void MusicManager::UnloadCustomBgm() {
    if (!m_customBgmLoaded) return; // nothing we took over; safe no-op

    HMODULE hMod = GetModuleHandleA("BBCF.exe");
    if (hMod) {
        uintptr_t modBase = (uintptr_t)hMod;
        uintptr_t audioMgrAddr = modBase + AUDIO_MGR_RVA;
        uintptr_t soundSlotMgrAddr = modBase + SOUND_SLOT_MGR_RVA;
        uintptr_t soundEngineAddr = modBase + SOUND_ENGINE_RVA;
        constexpr int BGM_SLOT_INDEX = 0x3E; // scratch slot the mod played from

        __try {
            void** bankArray = *(void***)(soundEngineAddr + 0x04);
            int bankCount = *(int*)(soundEngineAddr + 0x10);
            if (bankArray && bankCount > 13) {
                void* bank13 = bankArray[13];
                if (bank13) {
                    int stopIndex = 0;
                    typedef void (__thiscall* BankStopWrapperType)(void* bank, int* idx);
                    ((BankStopWrapperType)(modBase + BANK_STOP_RVA))(bank13, &stopIndex);
                    typedef void (__thiscall* BankClearFuncType)(void* bank);
                    ((BankClearFuncType)(modBase + 0x051890))(bank13);
                }
            }
            // Null the SCRATCH slot so the game's next RegisterBgm/free() is clean.
            // (Slot 0x3F, the game's active-BGM slot, is intentionally left holding
            // the selectable anchor track.)
            *(void**)(soundSlotMgrAddr + BGM_SLOT_INDEX * 8) = nullptr;
            *(int*)(soundSlotMgrAddr + BGM_SLOT_INDEX * 8 + 4) = 0;
            // Present the initially-selected (selectable) anchor track to the game.
            *(int*)(audioMgrAddr + 0x1690) = m_anchorTrackId;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LogMusic("MusicManager: Exception during UnloadCustomBgm (continuing)\n");
        }
    }

    // Reset music-select cursors to the initially-selected anchor track.
    if (s_musicSelectX) *s_musicSelectX = m_anchorTrackId;
    if (s_musicSelectY) *s_musicSelectY = 0;

    // Reset internal rotation state so re-entering the match re-arms cleanly.
    m_customBgmLoaded = false;
    m_modControllingBgm = false;
    m_currentTrackId = m_anchorTrackId;
    m_currentTrack = nullptr;
    for (const auto& t : m_tracks) {
        if (t.id == m_anchorTrackId) { m_currentTrack = &t; break; }
    }
    m_currentTrackDurationFrames = 0;
    m_framesSinceLastChange = 0;
    m_songPlaybackFrames = 0;
    LogMusic("MusicManager: Custom BGM unloaded; restored anchor track %d for scene change\n", m_anchorTrackId);
}

void MusicManager::RestoreAnchorForSceneExit() {
    if (!m_customBgmLoaded) return; // nothing we took over; safe no-op

    HMODULE hMod = GetModuleHandleA("BBCF.exe");
    if (!hMod) return;
    uintptr_t modBase = (uintptr_t)hMod;

    const char* anchorName = GetBgmFilename(m_anchorTrackId);
    if (!anchorName) {
        // No known anchor track; fall back to a plain unload.
        UnloadCustomBgm();
        return;
    }

    // Re-load the anchor (initially-selected) track through the normal pipeline.
    // This stops/clears the non-selectable track and puts a valid, selectable
    // track into the BGM slot / Bank[13] / audioMgr+0x1690 / musicSelect_X, so
    // the game's Character Select transition reads a supported track and shows
    // the original song pre-selected (as if the playlist never cycled).
    int anchorDuration = 0;
    bool restored = PlayTrackPhysically(modBase, m_anchorTrackId, anchorName, &anchorDuration, m_anchorTrackId);
    if (restored) {
        LogMusic("MusicManager: Restored anchor track %d (%s) for scene exit\n",
            m_anchorTrackId, anchorName);
    } else {
        // The audible restore failed, but the bookkeeping MUST still fall back to
        // the anchor: the control flags are cleared unconditionally below, and if
        // m_currentTrackId kept pointing at the interrupted playlist track, every
        // later cleanup (all guarded on those flags) would no-op and the stale
        // "last played" track would leak into the next match's jukebox display.
        LogMusic("MusicManager: Anchor restore FAILED; resetting bookkeeping to anchor %d anyway\n",
            m_anchorTrackId);
    }
    m_currentTrackId = m_anchorTrackId;
    m_currentTrackDurationFrames = restored ? anchorDuration : 0;
    m_gameMusicId = m_anchorTrackId;
    m_currentTrack = nullptr;
    for (const auto& t : m_tracks) {
        if (t.id == m_anchorTrackId) { m_currentTrack = &t; break; }
    }

    // The mod is no longer playing a custom (non-selectable) track.
    m_customBgmLoaded = false;
    m_modControllingBgm = false;
    m_framesSinceLastChange = 0;
    m_songPlaybackFrames = 0;
}

void MusicManager::RestoreNativeBgmForMatchEnd() {
    // Only when the mod actually took over BGM; the non-deviated (native) case
    // is left untouched so match end behaves exactly like vanilla.
    if (!m_customBgmLoaded && !m_modControllingBgm) return;

    // Backup cleanup for match-end flows that reach the victory screen WITHOUT
    // the MatchState -> VictoryScreen transition cleanup in UpdateMusicState
    // (that early cleanup is the primary fix for the match-summary black
    // screen). Runs the same proven cleanup as the Character Select exit:
    // Clear Bank[13] while the engine is still reachable (its bank ops no-op
    // if the engine is already mid-teardown), restore audioMgr slot 0 to its
    // native state, and present the selectable anchor everywhere.
    //
    // The scratch slot is orphaned FIRST so the scene-exit cleanup can't free
    // the wave buffer out from under a live XACT wave bank (in-memory wave
    // banks reference their data; freeing it underneath a live bank is what
    // killed the audio clock and black-screened the summary). One buffer is
    // leaked per such scene exit — acceptable; rotations recycle theirs.
    HMODULE hMod = GetModuleHandleA("BBCF.exe");
    if (!hMod) return;
    uintptr_t soundSlotMgrAddr = (uintptr_t)hMod + SOUND_SLOT_MGR_RVA;
    __try {
        *(void**)(soundSlotMgrAddr + 0x3E * 8) = nullptr;
        *(int*)(soundSlotMgrAddr + 0x3E * 8 + 4) = 0;
        LogMusic("MusicManager: Match end: orphaned scratch slot 0x3E buffer (protects XACT wave bank)\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogMusic("MusicManager: Exception orphaning scratch slot (continuing)\n");
    }

    ClearBgmForSceneExit();
}

void MusicManager::ClearBgmForSceneExit() {
    if (!m_customBgmLoaded && !m_modControllingBgm) {
        return;
    }

    HMODULE hMod = GetModuleHandleA("BBCF.exe");
    if (!hMod) return;
    uintptr_t modBase = (uintptr_t)hMod;
    uintptr_t audioMgrAddr = modBase + AUDIO_MGR_RVA;
    uintptr_t soundSlotMgrAddr = modBase + SOUND_SLOT_MGR_RVA;
    uintptr_t soundEngineAddr = modBase + SOUND_ENGINE_RVA;
    constexpr int SCRATCH_SLOT = 0x3E;

    // Stop + clear Bank[13] so it is EMPTY when the game's Character Select
    // XACT-init / BGM loader runs, letting the game rebuild the bank natively
    // instead of tripping over banks we registered via direct COM.
    __try {
        void** bankArray = *(void***)(soundEngineAddr + 0x04);
        int bankCount = *(int*)(soundEngineAddr + 0x10);
        if (bankArray && bankCount > 13) {
            void* bank13 = bankArray[13];
            if (bank13) {
                int stopIndex = 0;
                typedef void (__thiscall* BankStopWrapperType)(void* bank, int* idx);
                ((BankStopWrapperType)(modBase + BANK_STOP_RVA))(bank13, &stopIndex);
                typedef void (__thiscall* BankClearFuncType)(void* bank);
                ((BankClearFuncType)(modBase + 0x051890))(bank13);
            }
        }
        // Null the scratch slot; slot 0x3F (the game's active slot) is left
        // holding the selectable anchor for the game's validation.
        *(void**)(soundSlotMgrAddr + SCRATCH_SLOT * 8) = nullptr;
        *(int*)(soundSlotMgrAddr + SCRATCH_SLOT * 8 + 4) = 0;
        // Present the selectable anchor id to the game-facing state.
        *(int*)(audioMgrAddr + 0x1690) = m_anchorTrackId;
        // Restore audioManager BGM slot 0 to its native (active) state. The mod
        // deactivated it on every play; the game's native Character Select BGM
        // init needs it active to (re)load the original track. voice = -1 so the
        // native reload assigns a fresh cue (the old one was destroyed).
        if (m_audioSlot0Captured) {
            *(int*)(audioMgrAddr + 0x118 + 0x00) = m_origSlot0Active;
            *(int*)(audioMgrAddr + 0x118 + 0x04) = m_origSlot0State;
            *(int*)(audioMgrAddr + 0x118 + 0x10) = -1;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogMusic("MusicManager: Exception during ClearBgmForSceneExit (continuing)\n");
    }

    if (s_musicSelectX) *s_musicSelectX = m_anchorTrackId;
    if (s_musicSelectY) *s_musicSelectY = 0;

    m_customBgmLoaded = false;
    m_modControllingBgm = false;
    m_currentTrackId = m_anchorTrackId;
    m_currentTrackDurationFrames = 0;
    m_framesSinceLastChange = 0;
    m_songPlaybackFrames = 0;
    LogMusic("MusicManager: ClearBgmForSceneExit: Bank[13] cleared for native char-select reload (anchor=%d)\n",
        m_anchorTrackId);
}

void MusicManager::SavePreferences() {
    // BBCF_IM/ only exists once some other feature has written into it, and on a
    // fresh install nothing has yet - without this the ofstream below silently
    // fails and every Jukebox setting is lost on restart.
    CreateDirectoryA(GamePath("BBCF_IM").c_str(), NULL); // no-op if it already exists

    std::ofstream file(GamePath("BBCF_IM/music_preferences.ini"));
    if (!file.is_open()) {
        LOG(2, "MusicManager: Could not open music_preferences.ini for writing\n");
        return;
    }

    file << "[MusicTracks]\n";
    for (const auto& track : m_tracks) {
        auto it = m_trackEnabled.find(track.id);
        bool enabled = (it != m_trackEnabled.end()) ? it->second : true;
        file << track.id << "=" << (enabled ? "1" : "0") << "\n";
    }

    file << "\n[CustomTrackVolume]\n";
    file << "; Per-song volume for custom Jukebox tracks, in decibels, by file name.\n";
    for (const auto& entry : m_customTrackVolume) {
        if (entry.second != 0.0f)
            file << entry.first << "=" << entry.second << "\n";
    }

    file << "\n[Settings]\n";
    file << "Enabled=" << (m_enabled ? "1" : "0") << "\n";

    int modeInt = 0;
    if (m_rotationMode == MusicRotationMode::Sequential) modeInt = 1;
    else if (m_rotationMode == MusicRotationMode::Shuffle) modeInt = 2;
    file << "RotationMode=" << modeInt << "\n";
	file << "RepeatSingle=" << (m_repeatSingle ? "1" : "0") << "\n";
	file << "RematchTrackMode=" << (int)m_rematchTrackMode << "\n";

	file.close();
    LOG(2, "MusicManager: Saved preferences\n");
}

void MusicManager::LoadPreferences() {
    std::ifstream file(GamePath("BBCF_IM/music_preferences.ini"));
    if (!file.is_open()) {
        LOG(2, "MusicManager: No preferences file found, using defaults\n");
        return;
    }

    std::string line;
    std::string section;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;

        std::string key = line.substr(0, eqPos);
        std::string value = line.substr(eqPos + 1);

        if (section == "CustomTrackVolume") {
            if (!key.empty() && key[0] != ';')
                m_customTrackVolume[key] = (float)atof(value.c_str());
        } else if (section == "MusicTracks") {
            int trackId = std::stoi(key);
            bool enabled = (value == "1");
            m_trackEnabled[trackId] = enabled;
        } else if (section == "Settings") {
            if (key == "Enabled") {
                m_enabled = (value == "1");
            } else if (key == "RotationMode") {
                int modeInt = std::stoi(value);
                if (modeInt == 1) m_rotationMode = MusicRotationMode::Sequential;
                else m_rotationMode = MusicRotationMode::Shuffle; // 2 (and legacy 0 "Random")
            } else if (key == "RepeatSingle") {
				m_repeatSingle = (value == "1");
			} else if (key == "RematchTrackMode") {
				int rematchMode = std::stoi(value);
				if (rematchMode >= (int)RematchTrackMode::CharacterSelect &&
					rematchMode <= (int)RematchTrackMode::PlayNext) {
					m_rematchTrackMode = (RematchTrackMode)rematchMode;
				}
			}
		}
    }

    file.close();
    LOG(2, "MusicManager: Loaded preferences (enabled=%d, mode=%d)\n", m_enabled, (int)m_rotationMode);
}

void MusicManager::ResetPreferences() {
    m_trackEnabled.clear();
    for (auto& track : m_tracks) {
        m_trackEnabled[track.id] = true;
    }
    // Matches the shipped default; see m_enabled's declaration for why rotation is off.
    m_enabled = false;
	m_rotationMode = MusicRotationMode::Sequential;
	m_repeatSingle = false;
	m_rematchTrackMode = RematchTrackMode::CharacterSelect;
	SavePreferences();
    LOG(2, "MusicManager: Preferences reset - all tracks enabled\n");
}

