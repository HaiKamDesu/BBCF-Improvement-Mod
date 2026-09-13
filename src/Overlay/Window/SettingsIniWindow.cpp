#include "SettingsIniWindow.h"

#include "Core/HotkeyManager.h"
#include "Core/Localization.h"
#include "Core/logger.h"
#include "Overlay/imgui_utils.h"
#include "Overlay/Widget/HotkeyBindWidget.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace {
	struct SettingMetadata
	{
		const char* iniKey;
		const char* displayName;
		const char* category;
		const char* tooltip;
	};

	static const SettingMetadata kSettingMetadata[] = {
		{ "LobbyLinkHotkeysEnabled", "Room link hotkeys", "Hotkeys", "Master switch for the two room-link shortcuts below. Turn it off if those key combinations get in the way of something else." },
		{ "ShowHpNumbers", "Show HP numbers in training", "Overlays", "Draws the exact remaining HP over each health bar while you are in training, the way Abyss mode already does. Training only - it stays off everywhere else." },
		{ "RememberLobbyAvatar", "Remember my lobby avatar", "Lobby", "Puts your lobby avatar, its colour and both accessory slots back on when you connect to network mode. The game only saves the accessories its own equip menu offers, so without this the hidden ones are gone every time you relaunch. Anything you change afterwards - on the Online page or in the game\'s equip menu - becomes what gets remembered." },
		{ "LobbyAvatarIcon", "Remembered avatar", "Lobby", "The avatar icon that will be put back on when you connect to network mode. Set by the mod when you change your avatar; -1 means nothing has been remembered yet. Use the sliders on the Online page rather than editing this." },
		{ "LobbyAvatarColor", "Remembered avatar colour", "Lobby", "The avatar colour that will be put back on when you connect to network mode. Set by the mod when you change your avatar; -1 means nothing has been remembered yet. Use the sliders on the Online page rather than editing this." },
		{ "LobbyAvatarAccessory1", "Remembered accessory 1", "Lobby", "The first accessory slot that will be put back on when you connect to network mode. Set by the mod when you change your avatar; -1 means nothing has been remembered yet. Use the sliders on the Online page rather than editing this." },
		{ "LobbyAvatarAccessory2", "Remembered accessory 2", "Lobby", "The second accessory slot that will be put back on when you connect to network mode. Set by the mod when you change your avatar; -1 means nothing has been remembered yet. Use the sliders on the Online page rather than editing this." },
		{ "DinputDllWrapper", "Extra input DLL to load", "System", "For people who already use another input tool (a stick remapper, a wrapper) that also installs itself as dinput8.dll. Put its filename here so both it and the mod load. Leave it on \"none\" unless someone told you otherwise - a wrong name here can stop the game starting." },
		{ "RenderingWidth", "Custom render width", "Graphics", "How wide the game renders internally when Viewport mode is set to use a custom size. Set it below 1280x768 to trade image quality for framerate on a weak PC. Ignored unless the viewport mode uses it." },
		{ "RenderingHeight", "Custom render height", "Graphics", "How tall the game renders internally when Viewport mode is set to use a custom size. Set it below 1280x768 to trade image quality for framerate on a weak PC. Ignored unless the viewport mode uses it." },
		{ "Viewport", "Viewport mode", "Graphics", "How the picture is sized inside the window. Leave it on the default unless the game looks stretched or you are deliberately rendering at a lower resolution for performance." },
		{ "AntiAliasing", "Anti-aliasing", "Graphics", "Smooths jagged edges, at some cost to framerate. -1 leaves the game to decide, which is what almost everyone wants. Higher numbers mean more smoothing; your graphics card may ignore values it does not support." },
		{ "V-sync", "V-sync", "Graphics", "Locks the game's drawing to your monitor's refresh rate. On: no screen tearing. Off: slightly less input lag. Fighting game players usually leave it off." },
		{ "MenuSize", "Menu size", "Interface", "How large the mod's own menus and text are. Turn it up if the menus are hard to read on a big screen or at high resolution." },
		{ "Notifications", "Notifications", "Interface", "The small messages the mod pops up in the corner (palette loaded, update available, room link copied). Turn off if you find them distracting." },
		{ "CheckUpdates", "Check for updates", "Interface", "Checks once at startup whether a newer version of the mod has been released, and tells you if so. It never installs anything without asking." },
		{ "ShowHudWhenReplayPaused", "Keep the input display up when a replay is paused", "Interface", "Pausing a replay normally makes the game hide its input display - the button columns down both sides and the two stick-and-button panels - which is exactly when you most want to read them. Turn this on to leave them on screen. Only affects Replay Theater, and pausing still pauses." },
		{ "GenerateDebugLogs", "Generate debug logs", "Debug", "Writes a detailed DEBUG.txt log next to the game. Leave this on: if anything ever goes wrong, that file is what lets the mod's developers work out why. It costs almost nothing." },
		{ "CrashDumpDetail", "Crash dump detail", "Debug", "How much of the game's memory a crash dump captures. Medium is the right answer almost always - it keeps the registers, the stack and the objects the crash touched, and produces a file small enough to actually send. Full captures all of the game's memory, which is hundreds of megabytes of textures nobody will read; only pick it if a developer asks you to." },
		{ "CrashReportsToKeep", "Crash reports to keep", "Debug", "Deletes the oldest crash reports so BBCF_IM\\CrashReports cannot grow forever. Set to 0 to keep every report, which is how that folder has previously reached several gigabytes." },
		{ "DCodeAutoRecover", "Fix stuck player profiles", "Debug", "Sometimes the game gets permanently stuck fetching another player's profile, and their D-Code and stats never appear for the rest of the session. This notices that and nudges the game into trying again. Leave it on." },
		{ "SpectatorSyncHooksEnabled", "Spectator desync fix", "Debug", "While spectating, briefly repeats the players' last known inputs when new ones have not arrived yet, instead of letting the game guess with empty ones - guessing permanently desyncs the match you are watching. Only affects spectating. Requires a restart." },
		{ "EntityHitboxProbeEnabled", "Probe hitbox activity (testing)", "Debug", "TESTING ONLY. Asks the game itself, every frame, which on-screen objects currently have an active hitbox, and records the answer in DEBUG.txt. It exists to work out why a few moves' hitboxes never show up in the overlay. There is no reason to turn this on unless a developer asked you to." },
		{ "DCodeForceFailureOnce", "Break profile fetch (testing)", "Debug", "TESTING ONLY. Deliberately breaks the first profile fetch after launch so the fix above can be verified. There is no reason to turn this on unless a developer asked you to." },
		{ "DebugLogSessionHistory", "Keep old debug logs", "Debug", "How many previous sessions' debug logs to keep in the BBCF_IM folder. Useful when a problem happened a few sessions ago and you only noticed later. 0 keeps only the most recent one." },
		{ "DCodeTusGateAutoClear", "Keep saving ranked progress", "Ranked", "Now and then the game quietly decides it cannot save your online profile any more. When that happens your D-Code stops showing up and any ranked or net-colour progress you earn afterwards is lost when you close the game. This turns saving back on. Leave it on." },
		{ "ShowRankedProgress", "Show ranked progress", "Ranked", "Shows a small overlay with your ranked stats while you are in ranked." },
		{ "ShowSquareColorProgress", "Show net colour progress", "Ranked", "Shows how close you are to the next net colour (the coloured square next to your name)." },
		{ "ShowRankedPrediction", "Show ranked prediction", "Ranked", "Before a ranked match, shows how much rank you stand to gain or lose." },
		{ "RankedProgressShowMatches", "Show match count", "Ranked", "Include your total number of matches in the ranked overlay." },
		{ "RankedProgressShowWins", "Show wins", "Ranked", "Include your win count in the ranked overlay." },
		{ "RankedProgressShowLosses", "Show losses", "Ranked", "Include your loss count in the ranked overlay." },
		{ "RankedProgressShowWinrate", "Show winrate", "Ranked", "Include your win percentage in the ranked overlay." },
		{ "RankedProgressShowCharacterLeaderboardPlacement", "Show character leaderboard place", "Ranked", "Include your position on your character's leaderboard in the ranked overlay." },
		{ "RankedProgressShowGlobalLeaderboardPlacement", "Show global leaderboard place", "Ranked", "Include your position on the overall leaderboard in the ranked overlay." },
		{ "RankedAutomationHarnessEnabled", "Ranked automation (developer tool)", "Ranked Debug", "A tool that drives the ranked menus by itself for testing. Not something you need; leave it off." },
		{ "RankedAutomationHarnessAutorun", "Start automation automatically", "Ranked Debug", "Starts the ranked automation tool by itself once it can. Developer tool; leave it off." },
		{ "RankedAutomationHarnessQuitOnFinish", "Close game after automation", "Ranked Debug", "Closes the game when the ranked automation tool finishes its run. Developer tool; leave it off." },
		{ "URT_RE_TraceEnabled", "Replay takeover logging", "Replay Takeover Debug", "Writes very detailed logs about replay takeover. Only useful if a developer is investigating a replay takeover problem for you; it makes large files." },
		{ "URT_RE_TraceLevel", "Replay takeover log detail", "Replay Takeover Debug", "How much detail the replay takeover log records. Higher means more. Only matters when the log above is on." },
		{ "URT_RE_MaxFileMB", "Replay takeover log size limit", "Replay Takeover Debug", "How large the replay takeover log may grow (in megabytes) before it is rotated. Only matters when the log above is on." },
		{ "URT_RE_MaxBackups", "Replay takeover logs kept", "Replay Takeover Debug", "How many old replay takeover logs to keep. Only matters when the log above is on." },
		{ "URT_RE_AllowSizeMismatchProbe", "Allow mismatched replay data (unsafe)", "Replay Takeover Debug", "Developer option that lets replay takeover use data that does not look right. Leave it off; turning it on can misbehave." },
		{ "URT_RE_AllowUnsafeProbeLoad", "Allow unsafe replay load (unsafe)", "Replay Takeover Debug", "Developer option that skips replay takeover's safety checks. Leave it off; turning it on can crash the game." },
		{ "EnableInDevelopmentFeatures", "Show unfinished features", "System", "Reveals features that are still being worked on. They may be incomplete, ugly, or broken. Fine to explore, just do not rely on them." },
		{ "RankedSlotWriteTrace", "Trace ranked LP writes (slow)", "Ranked Debug", "Developer tool that watches the memory holding your ranked points to find what writes it. It makes the game run extremely slowly and can hang it during screen transitions. Leave it off unless a developer asked you to turn it on." },
		{ "EnableRankedListConnectionFilter", "Filter ranked list by connection", "Ranked list", "Lets you hide players in the ranked list whose connection to you is poor." },
		{ "ShowRankedListFilterWindow", "Show ranked list filter window", "Ranked list", "Shows the small filter window next to the ranked player list." },
		{ "ShowRankedListHiddenPopup", "Show hidden players popup", "Ranked list", "Remembers whether the \"hidden players\" popup was left open, so it comes back next time." },
		{ "RankedListSortMode", "Ranked list sort mode", "Ranked list", "How the ranked player list is ordered." },
		{ "RankedListNetworkFilter", "Minimum connection quality", "Ranked list", "Hides ranked players whose connection rating to you is below this level. 0 shows everyone. Players whose rating has not been measured yet stay visible until it is." },
		{ "HideUnmetRequirementRooms", "Hide rooms you cannot join", "Ranked list", "Hides rooms that demand a better connection than yours, so everything left in the list is actually joinable." },
		{ "LoadForeignPalettesToggleDefault", "Show other players' palettes by default", "Palettes", "Whether custom colours made by other players are shown when you start the game. You can still flip this per session from the palette menu." },
		{ "AllowPaletteDownloads", "Let others save your palette", "Palettes", "Whether other players can save the custom colours they see you using. \"Not chosen yet\" means the mod will ask you once in-game." },
		{ "CustomPalettesInCharSelect", "Show custom colours on character select", "Palettes", "Makes the colour preview on the character select screen use your custom palettes, so it matches what you will actually see in the match. The mod writes a modified copy of one of the game's data files into its own folder to do this - nothing the game shipped is changed. Turn it off if you would rather it did not write that file." },
		{ "SwapControllerPos", "Swap player 1 and 2 controllers", "Controller", "Swaps which physical controller counts as player 1 and which as player 2. Currently forced off at startup because it can crash the game; turn it on again during a session if you need it." },
		{ "EnableControllerHooks", "Controller tools", "Controller", "Powers the mod's controller features (assignment, keyboard separation). Turn it off only if you suspect them of causing trouble." },
		{ "ForceEnableControllerSettingHooks", "Force controller tools on Linux/Steam Deck", "Controller", "The mod switches the controller tools off automatically under Wine/Proton because they misbehave there. This forces them back on, and overrides the platform check on its own - you do not also need to turn Controller tools back on. Unsupported: expect problems, and turn it off again before reporting a bug." },
		{ "WineControllerPromptAnswered", "Wine/Proton controller prompt answered", "Controller", "Whether you have already answered the prompt that appears under Wine/Proton about the controller tools. Set it back to off to be asked again the next time you start the game." },
		{ "PrimaryKeyboardDeviceId", "Main keyboard", "Controller", "Which physical keyboard counts as the main one when you have more than one plugged in. Set this from the controller menu rather than by hand." },
		{ "IgnoredKeyboardIds", "Ignored keyboards", "Controller", "Keyboards the mod should pretend do not exist - handy for a wireless dongle or a laptop's built-in keyboard. Set this from the controller menu rather than by hand." },
		{ "KeyboardRenameMap", "Keyboard names", "Controller", "The friendly names you gave your keyboards. Set from the controller menu; not meant to be edited here." },
		{ "KeyboardMappings", "Keyboard assignments", "Controller", "Which keyboard is assigned to which player for local two-player-on-keyboards. Set from the controller menu; not meant to be edited here." },
		{ "AutomaticallyUpdateControllers", "Detect controllers automatically", "Controller detection", "Re-checks your controllers by itself when you plug one in, unplug one, or Steam changes something. Leave it on unless it is causing hitches." },
		{ "AutoRefreshSteamGraceMs", "Controller detection: Steam delay", "Controller detection", "How long to wait (in milliseconds) after Steam reports a controller change before reacting. Raise it if controllers get detected wrongly right after Steam does something. Only matters with automatic detection on." },
		{ "AutoRefreshFollowupDelayMs", "Controller detection: second check delay", "Controller detection", "How long to wait (in milliseconds) before double-checking controllers after the first check. Only matters with automatic detection on." },
		{ "AutoRefreshTimerIntervalMs", "Controller detection: check interval", "Controller detection", "How often (in milliseconds) to look for controller changes. Lower reacts faster but does slightly more work each second." },
		{ "UploadReplayData", "Share replays with the database", "Replay Database", "Whether your finished matches are uploaded to the community replay database. \"Ask\" means the mod asks you once in-game." },
		{ "UploadReplayDataHost", "Replay database address", "Replay Database", "Which server replays are sent to. Do not change this unless you are running your own replay server." },
		{ "UploadReplayDataEndpoint", "Replay database upload path", "Replay Database", "The upload path on the replay server. Do not change this unless you are running your own replay server." },
		{ "UploadReplayDataPort", "Replay database port", "Replay Database", "The port on the replay server. Do not change this unless you are running your own replay server." },
		{ "UploadReplayDataUseTls", "Use a secure connection for replays", "Replay Database", "Turn this on only if your replay server address is a domain name that requires HTTPS. The default server does not; turning it on for that one breaks uploads." },
		{ "autoArchive", "Auto-archive replays", "Replay Database", "Automatically files away your saved replays. Note that this can silently produce no files at all depending on how the game is set up." },
		{ "FrameHistoryWidth", "Input display: cell width", "Input history", "How wide each square is in the input history display." },
		{ "FrameHistoryHeight", "Input display: cell height", "Input history", "How tall each square is in the input history display." },
		{ "FrameHistorySpacing", "Input display: spacing", "Input history", "How much gap there is between squares in the input history display." },
		{ "FrameHistoryAutoReset", "Input display: clear automatically", "Input history", "Clears the input history by itself between attempts, so each try starts from a clean slate." },
		{ "FrameHistoryEnabled", "Show input history", "Input history", "Shows a strip of your recent inputs on screen, one square per frame." },
		{ "FrameHistoryCountEmptyFrames", "Input display: count idle frames", "Input history", "Also counts frames where you pressed nothing. Off makes the display more compact; on makes the timing between inputs literal." },
		{ "Language", "Language", "Interface", "The language the mod's own menus are shown in. This does not change the game's language." },
		{ "TrainingInputDelay", "Input delay (frames)", "Training", "Holds your own inputs back by this many frames in training mode, so you can practise at the delay online runs at. BBCF online always adds 2 frames of input delay whatever your ping, so 2 is the setting that matches a net match. 0 turns it off, 10 is the most it will take. The dummy is never delayed." },
		{ "UnlimitedPlaybackLoopSetupSeconds", "Loop: setup time", "Playback loop", "How many seconds of breathing room you get at the start of each loop, before playback begins, so you can get into position." },
		{ "UnlimitedPlaybackLoopEndingSeconds", "Loop: wind-down time", "Playback loop", "How many seconds to wait at the end of each loop before it restarts." },
		{ "UnlimitedPlaybackLoopRestartLabState", "Loop: reload training state", "Playback loop", "Reloads your saved training state every time the loop restarts, so positions and health are identical each repetition." },
		{ "UnlimitedPlaybackLoopRestartMode", "Loop: restart position", "Playback loop", "Where the characters are put when the loop restarts - middle of the stage, a corner, or your own saved snapshot." },
		{ "D3D9IatFallbackHook", "Overlay rescue mode", "Graphics", "For the rare PC where the mod loads but no mod window ever appears, usually a dual-graphics laptop where NVIDIA's software gets in the way first. Automatic fixes it on affected PCs and does nothing on all the others - leave it there. Requires a restart." },
		{ "PlatinumVoiceChoice", "Platinum voice choice", "Audio", "Picks whether YOUR Platinum speaks as Sena or Luna instead of the game rolling for it. Offline only (training, versus CPU, replays, local versus) - it is switched off in online matches while a reported desync is investigated. Only affects the Platinum you control." },
		{ "LogFrameStalls", "Log frame stalls", "Debug", "Records every frame that took longer than the threshold below, with a breakdown of how much of the delay was the mod's own code and how much was the game, your graphics driver or Windows. Writes to BBCF_IM\\FrameStallIncidents.log, one file per launch. Leave it off unless someone is looking into stuttering on your PC. Requires a restart." },
		{ "FrameStallThresholdMs", "Frame stall threshold", "Debug", "How slow a frame has to be, in milliseconds, before it is worth logging as a stall. 33 is about two missed frames at 60fps. Only used while frame stall logging is on." },
		{ "OverlayAbTestSeconds", "Overlay cost A/B test", "Debug", "Turns the mod's overlay drawing off and on again every this many seconds and logs the framerate for each, so a report can show whether the overlay itself is costing you frames. 0 is off, which is what you want unless you are measuring something. Needs frame stall logging on to be any use." },
		{ "ReplayLaunchParamPollMs", "Replay link check interval", "Replay Database", "How often, in milliseconds, the mod asks Steam whether it handed the game a replay to open. That only changes when you launch from a replay link, so the default is already far more often than needed. 0 checks every single frame and exists only for testing." },
		{ "ReplayTakeoverSetupSeconds", "Takeover setup time", "Replay Takeover", "How long the game freezes right after you take over a replay, so you can get your hands into position before the recorded side starts moving. 0 starts immediately. The takeover dialog sets this too." },
		{ "TakeoverInputSlot", "Replay takeover controller", "Replay Takeover", "Which side you control when you take over a replay. Pick the one you actually play on - Player 1 also covers keyboard." },
		{ "MusicWaveBankFormat", "Custom music file format", "Audio", "How converted custom music is stored. Auto is almost always right: it writes compact WMA on Windows and switches to PCM under Wine/Proton, where Windows supplies no audio encoder and WMA cannot be made at all. PCM files play identically but take about ten times the disk space. Changing this rebuilds every converted track." },
	};

	// The draft being edited, so a hotkey's bind widget can warn about a collision with
	// another hotkey the user has changed but not saved yet. Set by BuildRows; only ever one
	// Settings modal exists.
	static settingsIni_t* g_draft = nullptr;

	// Hotkey rows take their display name and tooltip from hotkeys.def rather than the table
	// above, so the action list stays in one place.
	static std::string HotkeyConflictWarning(HotkeyManager::Action action, const HotkeyBinding& binding)
	{
		if (!binding.IsBound())
			return "";

		if (g_draft)
		{
#define HOTKEY(_id, _var, _ini, _default, _display, _tooltip) \
			if (!HotkeyManager::CanShareBinding(action, HotkeyManager::Hotkey_##_id) && \
				HotkeyManager::BindingsEqual(binding, HotkeyManager::BindingFromString(g_draft->_var))) \
				return std::string("Already used by \"") + _display + "\". Pressing it will do both.";
#include "Core/hotkeys.def"
#undef HOTKEY
		}

		if (HotkeyManager::IsControllerBinding(binding))
			return "Controller button: this also works during a match, so pick one you never "
				"press while playing.";

		return "";
	}

	static bool DrawHotkeyValueWidget(HotkeyManager::Action action, std::string& bindingText)
	{
		HotkeyBinding binding = HotkeyManager::BindingFromString(bindingText);
		const std::string warning = HotkeyConflictWarning(action, binding);

		if (!ImGuiHotkey::BindWidget(HotkeyManager::IniKey(action), binding,
			HotkeyManager::DefaultBindingString(action), warning.c_str()))
			return false;

		bindingText = HotkeyManager::BindingToString(binding);
		return true;
	}

	static const SettingMetadata* GetSettingMetadata(const char* iniKey)
	{
		for (const SettingMetadata& metadata : kSettingMetadata)
			if (_stricmp(metadata.iniKey, iniKey) == 0)
				return &metadata;
		return nullptr;
	}

	// The settings used to be one long scroll of collapsing headers, which meant finding
	// anything depended on remembering roughly how far down it was. They are paged now, the
	// same way the mod menu is: a page is a place, a category is a heading inside it.
	//
	// A page whose visible rows all belong to one category draws no heading at all - a group
	// label that just repeats the page title is noise (Hotkeys, Training).
	enum SettingsPageId
	{
		SPage_General,
		SPage_Hotkeys,
		SPage_LookAndSound,
		SPage_Controllers,
		SPage_Overlays,
		SPage_Training,
		SPage_Online,
		SPage_Replays,
		SPage_Advanced,
		SPage_Count
	};

	struct SettingsPageInfo
	{
		const char* label;
		const char* blurb;   // one line under the page title, so a page explains itself
		const char* keywords;
	};

	static const SettingsPageInfo kSettingsPages[SPage_Count] = {
		{ "General",      "The mod's own menus: language, size, notifications, updates.",  "interface language menu size notification update general" },
		{ "Hotkeys",      "Every key and controller button the mod listens for.",          "hotkey keybind bind shortcut key button rebind" },
		{ "Look & Sound", "How the game is drawn, what it sounds like, and palettes.",     "graphics video resolution viewport vsync audio music palette colour color" },
		{ "Controllers",  "Which device is which, and how the mod notices them.",          "controller gamepad pad keyboard device detect steam" },
		{ "Overlays",     "Information the mod draws on top of the game.",                 "overlay hud hp numbers input history frame display" },
		{ "Training",     "Lab tools: recordings, loops and the dummy.",                   "training lab playback loop recording dummy practice" },
		{ "Online",       "Lobby, ranked and the ranked player list.",                     "online netplay lobby avatar ranked lp list room" },
		{ "Replays",      "The replay database and replay takeover.",                      "replay database upload takeover urt archive" },
		{ "Advanced",     "Logging, crash reports and developer switches. Mostly leave alone.", "advanced debug log crash dump developer system unsafe trace" },
	};

	struct SettingsCategoryInfo
	{
		const char* name;
		SettingsPageId page;
	};

	// Order inside a page is the order the groups are drawn in. Every category string used in
	// kSettingMetadata has to appear here or its settings become unreachable, so "Other" is
	// the catch-all that BuildRows assigns to anything without metadata.
	static const SettingsCategoryInfo kSettingsCategories[] = {
		{ "Interface",             SPage_General },

		{ "Hotkeys",               SPage_Hotkeys },

		{ "Graphics",              SPage_LookAndSound },
		{ "Audio",                 SPage_LookAndSound },
		{ "Palettes",              SPage_LookAndSound },

		{ "Controller",            SPage_Controllers },
		{ "Controller detection",  SPage_Controllers },

		{ "Overlays",              SPage_Overlays },
		{ "Input history",         SPage_Overlays },

		{ "Training",              SPage_Training },
		{ "Playback loop",         SPage_Training },

		{ "Lobby",                 SPage_Online },
		{ "Ranked",                SPage_Online },
		{ "Ranked list",           SPage_Online },

		{ "Replay Database",       SPage_Replays },
		{ "Replay Takeover",       SPage_Replays },

		{ "System",                SPage_Advanced },
		{ "Debug",                 SPage_Advanced },
		{ "Ranked Debug",          SPage_Advanced },
		{ "Replay Takeover Debug", SPage_Advanced },
		{ "Other",                 SPage_Advanced },
	};

	static SettingsPageId PageOfCategory(const char* category)
	{
		for (const SettingsCategoryInfo& info : kSettingsCategories)
			if (_stricmp(info.name, category) == 0)
				return info.page;
		return SPage_Advanced;
	}

	// Named-option metadata for int settings that should render as a dropdown instead of
	// a raw number box. Register a setting here (by ini key) to make it an enum in the
	// Settings window; the underlying storage is still a plain `int` (settings.def is
	// unchanged), only the UI widget and the reset/default label change.
	struct SettingEnumOption
	{
		int value;
		const char* label;
	};

	struct SettingEnumMetadata
	{
		const char* iniKey;
		const SettingEnumOption* options;
		int optionCount;
	};

	static const SettingEnumOption kD3D9IatFallbackHookOptions[] = {
		{ 0, "Off" },
		{ 1, "Automatic (recommended)" },
		{ 2, "Always" },
	};

	static const SettingEnumOption kPlatinumVoiceChoiceOptions[] = {
		{ 0, "Default (random)" },
		{ 1, "Luna" },
		{ 2, "Sena" },
	};

	// Order must match the enum comment in settings.def and ChooseBankFormat().
	static const SettingEnumOption kMusicWaveBankFormatOptions[] = {
		{ 0, "Auto (WMA if available, else PCM)" },
		{ 1, "Always WMA (smaller; Windows only)" },
		{ 2, "Always PCM (larger; works everywhere)" },
	};

	// Both of these are consent settings whose -1 means "the mod has not asked you yet", which
	// is impossible to guess from a number box.
	static const SettingEnumOption kTakeoverSlotOptions[] = {
		{ 0, "Player 1 controller (or keyboard)" },
		{ 1, "Player 2 controller" },
	};

	static const SettingEnumOption kAskNoYesOptions[] = {
		{ -1, "Ask me in-game" },
		{ 0, "No" },
		{ 1, "Yes" },
	};

	// Order must match the enum comment in settings.def and DumpFlagsForDetail().
	static const SettingEnumOption kCrashDumpDetailOptions[] = {
		{ 0, "Small (stacks only)" },
		{ 1, "Medium (recommended)" },
		{ 2, "Full memory (very large)" },
	};

	static const SettingEnumMetadata kSettingEnumMetadata[] = {
		{ "CrashDumpDetail", kCrashDumpDetailOptions, _countof(kCrashDumpDetailOptions) },
		{ "AllowPaletteDownloads", kAskNoYesOptions, _countof(kAskNoYesOptions) },
		{ "TakeoverInputSlot", kTakeoverSlotOptions, _countof(kTakeoverSlotOptions) },
		{ "UploadReplayData", kAskNoYesOptions, _countof(kAskNoYesOptions) },
		{ "PlatinumVoiceChoice", kPlatinumVoiceChoiceOptions, _countof(kPlatinumVoiceChoiceOptions) },
		{ "MusicWaveBankFormat", kMusicWaveBankFormatOptions, _countof(kMusicWaveBankFormatOptions) },
		{ "D3D9IatFallbackHook", kD3D9IatFallbackHookOptions, _countof(kD3D9IatFallbackHookOptions) },
	};

	static const SettingEnumMetadata* GetSettingEnumMetadata(const char* iniKey)
	{
		for (const SettingEnumMetadata& metadata : kSettingEnumMetadata)
			if (_stricmp(metadata.iniKey, iniKey) == 0)
				return &metadata;
		return nullptr;
	}

	static const char* SettingEnumLabelForValue(const SettingEnumMetadata& metadata, int value)
	{
		for (int i = 0; i < metadata.optionCount; ++i)
			if (metadata.options[i].value == value)
				return metadata.options[i].label;
		return "Unknown";
	}

	static bool DrawValueWidget(const char* id, bool& val)
	{
		return ImGui::Checkbox(id, &val);
	}

	// Plain int widget. Settings with enum metadata are routed to DrawEnumValueWidget
	// instead (see DrawValueWidgetForSetting) - this overload stays the fallback for
	// ordinary numeric int settings.
	static bool DrawValueWidget(const char* id, int& val)
	{
		ImGui::PushItemWidth(-1);
		const bool changed = ImGui::InputInt(id, &val);
		ImGui::PopItemWidth();
		return changed;
	}

	static bool DrawEnumValueWidget(const char* id, int& val, const SettingEnumMetadata& metadata)
	{
		bool changed = false;
		ImGui::PushItemWidth(-1);
		if (ImGui::BeginCombo(id, SettingEnumLabelForValue(metadata, val)))
		{
			for (int i = 0; i < metadata.optionCount; ++i)
			{
				const bool selected = metadata.options[i].value == val;
				if (ImGui::Selectable(metadata.options[i].label, selected))
				{
					val = metadata.options[i].value;
					changed = true;
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::PopItemWidth();
		return changed;
	}

	// Dispatch used by the settings.def X-macro: int settings with registered enum
	// metadata render as a combo box; everything else falls back to DrawValueWidget.
	template <typename T>
	static bool DrawValueWidgetForSetting(const char*, const char* id, T& val)
	{
		return DrawValueWidget(id, val);
	}

	static bool DrawValueWidgetForSetting(const char* iniKey, const char* id, int& val)
	{
		if (const SettingEnumMetadata* enumMetadata = GetSettingEnumMetadata(iniKey))
			return DrawEnumValueWidget(id, val, *enumMetadata);
		return DrawValueWidget(id, val);
	}

	// Defined below with the other value widgets; needed here for the non-hotkey fallback.
	static bool DrawValueWidget(const char* id, std::string& val);

	// The language used to be a combo box of its own in the mod menu. It is a setting like any
	// other, so it lives here now - but stored as a language code, which is not something to
	// type into a text box, hence its own widget.
	static bool DrawLanguageValueWidget(const char* id, std::string& val)
	{
		const auto& options = Localization::GetAvailableLanguages();

		std::string preview = val;
		for (const auto& option : options)
			if (option.code == val)
				preview = option.displayName;

		bool changed = false;
		ImGui::PushItemWidth(-1);
		if (ImGui::BeginCombo(id, preview.c_str()))
		{
			for (const auto& option : options)
			{
				std::string label = option.displayName;
				if (!option.complete)
					label += Messages.Language_incomplete_label();

				if (!option.complete)
					ImGui::BeginDisabled();

				const bool selected = option.code == val;
				if (ImGui::Selectable(label.c_str(), selected))
				{
					val = option.code;
					changed = true;
				}
				if (selected)
					ImGui::SetItemDefaultFocus();

				if (!option.complete)
					ImGui::EndDisabled();
			}
			ImGui::EndCombo();
		}
		ImGui::PopItemWidth();
		return changed;
	}

	static bool DrawValueWidgetForSetting(const char* iniKey, const char* id, std::string& val)
	{
		const HotkeyManager::Action action = HotkeyManager::ActionFromIniKey(iniKey);
		if (action != HotkeyManager::Hotkey_Count)
			return DrawHotkeyValueWidget(action, val);
		if (_stricmp(iniKey, "Language") == 0)
			return DrawLanguageValueWidget(id, val);
		return DrawValueWidget(id, val);
	}

	static bool DrawValueWidget(const char* id, float& val)
	{
		ImGui::PushItemWidth(-1);
		const bool changed = ImGui::InputFloat(id, &val, 0.0f, 0.0f, "%.4f");
		ImGui::PopItemWidth();
		return changed;
	}

	static bool DrawValueWidget(const char* id, std::string& val)
	{
		char buf[1024];
		strncpy_s(buf, sizeof(buf), val.c_str(), _TRUNCATE);
		ImGui::PushItemWidth(-1);
		const bool changed = ImGui::InputText(id, buf, sizeof(buf));
		ImGui::PopItemWidth();
		if (changed)
			val = buf;
		return changed;
	}

	static bool IsRestartRequired(const char* iniKey)
	{
		static const char* const kRestartKeys[] = {
			"RenderingWidth", "RenderingHeight", "Viewport", "AntiAliasing", "V-sync", "MenuSize",
			"DinputDllWrapper",
			"SwapControllerPos", "EnableControllerHooks", "ForceEnableControllerSettingHooks",
			"PrimaryKeyboardDeviceId", "IgnoredKeyboardIds", "KeyboardRenameMap", "KeyboardMappings",
			"SpectatorSyncHooksEnabled",
			"D3D9IatFallbackHook",
			"LogFrameStalls",
			nullptr
		};
		for (int i = 0; kRestartKeys[i]; ++i)
			if (_stricmp(iniKey, kRestartKeys[i]) == 0)
				return true;
		return false;
	}

	// Parse a settings.def default-value literal into each supported type.
	static void SettingAssignFromString(bool& val, const char* str) { val = atoi(str) != 0; }
	static void SettingAssignFromString(int& val, const char* str) { val = atoi(str); }
	static void SettingAssignFromString(float& val, const char* str) { val = (float)atof(str); }
	static void SettingAssignFromString(std::string& val, const char* str) { val = str; }

	static std::string SettingValueToString(bool val) { return val ? "1" : "0"; }
	static std::string SettingValueToString(int val) { return std::to_string(val); }
	static std::string SettingValueToString(float val)
	{
		std::ostringstream oss;
		oss << val;
		return oss.str();
	}
	static std::string SettingValueToString(const std::string& val) { return val; }

	static void RestartGame()
	{
		// Prefer relaunching through Steam. Spawning BBCF.exe directly starts it outside
		// the Steam wrapper, which loses the overlay and Cloud saves; under Proton it also
		// bypasses the launcher that set the prefix up in the first place.
		const HINSTANCE viaSteam = ShellExecuteW(
			nullptr, L"open", L"steam://rungameid/586140", nullptr, nullptr, SW_SHOWNORMAL);

		// ShellExecute reports failure as a value of 32 or less. That happens when the
		// steam:// handler is not registered, which is normal in a bare Wine prefix, so
		// fall back to launching the executable rather than leaving the game closed.
		if (reinterpret_cast<UINT_PTR>(viaSteam) <= 32)
		{
			wchar_t exePath[MAX_PATH] = {};
			GetModuleFileNameW(NULL, exePath, MAX_PATH);
			STARTUPINFOW si = { sizeof(si) };
			PROCESS_INFORMATION pi = {};
			if (CreateProcessW(exePath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
			{
				CloseHandle(pi.hProcess);
				CloseHandle(pi.hThread);
			}
		}
		ExitProcess(0);
	}
}

void SettingsIniWindow::DrawOpenButton()
{
	if (!ImGui::Button("Settings"))
		return;

	Open();
}

void SettingsIniWindow::Open(const char* initialFilter)
{
	BuildRows();

	if (initialFilter && *initialFilter)
	{
		// After BuildRows, which clears the filter.
		strncpy_s(m_settingsFilter.InputBuf, sizeof(m_settingsFilter.InputBuf), initialFilter, _TRUNCATE);
		m_settingsFilter.Build();
	}

	ImGui::OpenPopup("Settings##modal");
}

void SettingsIniWindow::BuildRows()
{
	m_settingsDraft = Settings::settingsIni;
	m_settingsFilter.Clear();
	m_settingRows.clear();
	m_needsRestart = false;
	g_draft = &m_settingsDraft;

	// A setting with no kSettingMetadata row still works, it just renders with its raw ini
	// key, no tooltip, and lands under "Other" - which is easy to ship without noticing.
	// Five settings had drifted in exactly that way. Say so in the log instead.
#define SETTING(_type, _var, _inistring, _defaultval) \
	const SettingMetadata* metadata_##_var = GetSettingMetadata(_inistring); \
	const HotkeyManager::Action hotkey_##_var = HotkeyManager::ActionFromIniKey(_inistring); \
	const bool isHotkey_##_var = hotkey_##_var != HotkeyManager::Hotkey_Count; \
	if (!isHotkey_##_var && metadata_##_var == nullptr) \
		LOG(1, "[Settings] '%s' has no kSettingMetadata row; it will render as its raw ini key under Other\n", _inistring); \
	m_settingRows.push_back({ \
		_inistring, \
		isHotkey_##_var ? HotkeyManager::DisplayName(hotkey_##_var) \
			: (metadata_##_var ? metadata_##_var->displayName : _inistring), \
		isHotkey_##_var ? "Hotkeys" \
			: (metadata_##_var ? metadata_##_var->category : "Other"), \
		isHotkey_##_var ? HotkeyManager::Tooltip(hotkey_##_var) \
			: (metadata_##_var ? metadata_##_var->tooltip : "No description available."), \
		_defaultval, \
		[this]() -> bool { \
			char buf[128] = "##"; \
			strncat_s(buf, sizeof(buf), _inistring, _TRUNCATE); \
			return DrawValueWidgetForSetting(_inistring, buf, m_settingsDraft._var); \
		}, \
		[this]() -> bool { return m_settingsDraft._var != Settings::settingsIni._var; }, \
		[this]() { SettingAssignFromString(m_settingsDraft._var, _defaultval); }, \
		IsRestartRequired(_inistring), \
		_stricmp(_inistring, "Language") == 0 \
	});
#include "Core/settings.def"
#undef SETTING
}

bool SettingsIniWindow::RowMatchesFilter(const SettingRow& row) const
{
	return m_settingsFilter.PassFilter(row.name.c_str())
		|| m_settingsFilter.PassFilter(row.displayName.c_str())
		|| m_settingsFilter.PassFilter(row.category.c_str())
		|| m_settingsFilter.PassFilter(row.tooltip.c_str());
}

int SettingsIniWindow::VisibleRowCount(const char* category) const
{
	int count = 0;
	for (const SettingRow& row : m_settingRows)
		if (_stricmp(row.category.c_str(), category) == 0 && RowMatchesFilter(row))
			++count;
	return count;
}

void SettingsIniWindow::DrawRow(SettingRow& row, bool& restartRowChanged)
{
	ImGui::PushID(row.name.c_str());
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted(row.displayName.c_str());
	if (ImGui::BeginPopupContextItem("##reset_ctx"))
	{
		char resetLabel[192];
		const SettingEnumMetadata* enumMetadata = GetSettingEnumMetadata(row.name.c_str());
		const char* defaultLabel = row.defaultValue.empty() ? "empty" : row.defaultValue.c_str();
		if (enumMetadata)
			defaultLabel = SettingEnumLabelForValue(*enumMetadata, atoi(row.defaultValue.c_str()));
		snprintf(resetLabel, sizeof(resetLabel), "Reset to default (%s)", defaultLabel);
		if (ImGui::MenuItem(resetLabel))
		{
			row.resetToDefault();
			if (row.isRestartRequired)
				restartRowChanged = true;
		}
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	ImGui::ShowHelpMarker(row.tooltip.c_str());
	ImGui::TextDisabled("%s", row.name.c_str());

	// There is no "Live"/"Restart" column any more: it labelled every row in the mod for the
	// sake of the handful that need a relaunch, and the Save button already turns into "Save
	// and Restart" when one of them is pending. What is left is the one case where the answer
	// is actually useful - this row, right now, is why it is asking.
	if (row.isRestartRequired && row.differsFromOriginal())
	{
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "needs restart");
	}

	ImGui::TableSetColumnIndex(1);
	if (row.draw() && row.isRestartRequired)
		restartRowChanged = true;

	ImGui::PopID();
}

void SettingsIniWindow::DrawCategoryTable(const char* category, bool& restartRowChanged)
{
	ImGui::PushID(category);

	if (_stricmp(category, "Hotkeys") == 0)
	{
		if (ImGui::Button("Reset all hotkeys to defaults"))
		{
#define HOTKEY(_id, _var, _ini, _default, _display, _tooltip) m_settingsDraft._var = _default;
#include "Core/hotkeys.def"
#undef HOTKEY
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Nothing is saved until you press Save at the bottom.");
	}

	// Tables rather than Columns: SetColumnWidth ran every frame, so dragging a separator was
	// undone before the next frame drew. Here the pixel widths are only the initial layout.
	const ImGuiTableFlags settingsTableFlags =
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_BordersInnerV |
		ImGuiTableFlags_BordersInnerH |
		ImGuiTableFlags_RowBg;

	if (!ImGui::BeginTable("##settings_cols", 2, settingsTableFlags))
	{
		ImGui::PopID();
		return;
	}

	ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, 285.0f);
	ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableHeadersRow();

	// Two passes so pinned rows lead their category regardless of settings.def order,
	// which exists to keep settings.ini stable and is nobody's idea of a reading order.
	for (int pass = 0; pass < 2; ++pass)
	for (SettingRow& row : m_settingRows)
	{
		if (row.pinnedFirst != (pass == 0))
			continue;
		if (_stricmp(row.category.c_str(), category) != 0 || !RowMatchesFilter(row))
			continue;

		DrawRow(row, restartRowChanged);
	}

	ImGui::EndTable();
	ImGui::Spacing();
	ImGui::PopID();
}

void SettingsIniWindow::DrawNav(float width, float height)
{
	ImGui::BeginChild("##settings_nav", ImVec2(width, height), true);

	ImGui::SeparatorText("Pages");

	for (int i = 0; i < SPage_Count; ++i)
	{
		// Pages are never dimmed or hidden by the search: a page is a place, and one that
		// disappears while you type is a place you can no longer get back to.
		if (ImGui::Selectable(kSettingsPages[i].label, m_currentPage == i))
		{
			m_currentPage = i;
			m_settingsFilter.Clear();
		}
	}

	ImGui::EndChild();
}

void SettingsIniWindow::DrawPageBody(bool& restartRowChanged)
{
	const SettingsPageInfo& page = kSettingsPages[m_currentPage];

	ImGui::TextUnformatted(page.label);
	ImGui::TextDisabled("%s", page.blurb);
	ImGui::Separator();
	ImGui::Spacing();

	// Counted first: a page whose rows all sit in one category gets no group heading, because
	// a heading that repeats the page title above it is pure decoration.
	int categoriesWithRows = 0;
	for (const SettingsCategoryInfo& info : kSettingsCategories)
		if (info.page == m_currentPage && VisibleRowCount(info.name) > 0)
			++categoriesWithRows;

	if (categoriesWithRows == 0)
	{
		ImGui::TextDisabled("Nothing on this page matches the search.");
		return;
	}

	for (const SettingsCategoryInfo& info : kSettingsCategories)
	{
		if (info.page != m_currentPage || VisibleRowCount(info.name) == 0)
			continue;

		if (categoriesWithRows > 1)
			ImGui::SeparatorText(info.name);

		DrawCategoryTable(info.name, restartRowChanged);
	}
}

void SettingsIniWindow::DrawSearchResults(bool& restartRowChanged)
{
	ImGui::TextUnformatted("Search results");
	ImGui::TextDisabled("Every page at once. Clear the box, or pick a page on the left, to go back.");
	ImGui::Separator();
	ImGui::Spacing();

	int shown = 0;
	for (const SettingsCategoryInfo& info : kSettingsCategories)
	{
		if (VisibleRowCount(info.name) == 0)
			continue;

		++shown;
		// "Page > Category", so a hit also teaches where the setting lives; next time the
		// user goes straight there instead of searching for it again.
		char heading[160];
		snprintf(heading, sizeof(heading), "%s  >  %s", kSettingsPages[info.page].label, info.name);
		ImGui::SeparatorText(heading);

		DrawCategoryTable(info.name, restartRowChanged);
	}

	if (shown == 0)
		ImGui::TextDisabled("No settings match the current search.");
}

void SettingsIniWindow::DrawModal()
{
	if (m_settingRows.empty())
		return;

	const float footerHeight = m_needsRestart ? 64.0f : 40.0f;

	ImGui::SetNextWindowSize(ImVec2(860, 640), ImGuiCond_Always);
	if (!ImGui::BeginPopupModal("Settings##modal", nullptr, ImGuiWindowFlags_NoResize))
		return;

	ImGui::TextUnformatted("Settings");
	ImGui::SameLine();
	ImGui::TextDisabled("Hover (?) for details. Right-click a setting name to reset it to its default.");
	m_settingsFilter.Draw("##settings_filter", -1.0f);
	if (!m_settingsFilter.IsActive())
	{
		// ImGuiTextFilter has no placeholder support, so the hint is painted over the empty
		// box - the same trick the mod menu's own search uses.
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 padding = ImGui::GetStyle().FramePadding;
		ImGui::GetWindowDrawList()->AddText(
			ImVec2(min.x + padding.x, min.y + padding.y),
			ImGui::GetColorU32(ImGuiCol_TextDisabled),
			"Search every setting by name, page or key...");
	}
	ImGui::Spacing();

	DrawNav(168.0f, -footerHeight);
	ImGui::SameLine();

	bool anyRestartRowChangedThisFrame = false;

	ImGui::BeginChild("##settings_scroll", ImVec2(0, -footerHeight), true);
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, style.FramePadding.y + 1.0f));

		if (m_settingsFilter.IsActive())
			DrawSearchResults(anyRestartRowChangedThisFrame);
		else
			DrawPageBody(anyRestartRowChangedThisFrame);

		ImGui::PopStyleVar();
	}
	ImGui::EndChild();

	if (anyRestartRowChangedThisFrame)
	{
		m_needsRestart = false;
		for (SettingRow& row : m_settingRows)
			if (row.isRestartRequired && row.differsFromOriginal())
			{
				m_needsRestart = true;
				break;
			}
	}

	if (m_needsRestart)
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
			"Some changes require a restart to take effect.");

	const float saveButtonWidth = m_needsRestart ? 150.0f : 120.0f;
	const float footerWidth = 120.0f + saveButtonWidth + 120.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
	ImGui::SetCursorPosX((std::max)(ImGui::GetStyle().WindowPadding.x,
		(ImGui::GetWindowWidth() - footerWidth) * 0.5f));

	if (ImGui::Button("Cancel", ImVec2(120, 0)))
		ImGui::CloseCurrentPopup();

	ImGui::SameLine();

	const char* saveLabel = m_needsRestart ? "Save and Restart" : "Save";
	if (ImGui::Button(saveLabel, ImVec2(saveButtonWidth, 0)))
	{
		const bool debugLogsWereEnabled = Settings::settingsIni.generateDebugLogs;
		const std::string languageBeforeSave = Settings::settingsIni.language;

#define SETTING(_type, _var, _inistring, _defaultval) \
		if (m_settingsDraft._var != Settings::settingsIni._var) { \
			Settings::changeSetting(_inistring, SettingValueToString(m_settingsDraft._var)); \
			Settings::settingsIni._var = m_settingsDraft._var; \
		}
#include "Core/settings.def"
#undef SETTING

		if (Settings::settingsIni.generateDebugLogs != debugLogsWereEnabled)
			SetLoggingEnabled(Settings::settingsIni.generateDebugLogs);

		// Every string the menus draw is looked up per frame, so reloading here is enough to
		// change the whole UI language without a restart.
		if (Settings::settingsIni.language != languageBeforeSave)
		{
			Localization::Reload(Settings::settingsIni.language);
			Settings::settingsIni.language = Localization::GetCurrentLanguage();
			Settings::changeSetting("Language", Settings::settingsIni.language);
		}

		// Push the settings that subsystems read through g_modVals (keybinds, frame history
		// sizing, palette/upload toggles). Without this they'd only take effect on the next
		// D3D device reset.
		Settings::applyRuntimeSettings();

		ImGui::CloseCurrentPopup();

		if (m_needsRestart)
			RestartGame();
	}

	ImGui::SameLine();

	if (ImGui::Button("Restart Game", ImVec2(120, 0)))
		RestartGame();

	ImGui::EndPopup();
}
