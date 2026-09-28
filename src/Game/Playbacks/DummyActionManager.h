#pragma once

#include "Game/Playbacks/UnlimitedPlaybackManager.h"
#include "Game/Scr/ScrStateEntry.h"

#include <array>
#include <map>
#include <utility>
#include <string>
#include <vector>

// What the training dummy has been told to do, and when.
//
// This replaces two systems that grew up separately and could not be combined:
//
//   - The old "dummy actions" panel, which could force one of the dummy's own script states
//     on four hard-coded triggers, and nothing else.
//   - Unlimited Playback, whose library could fire a recorded playback on any of seven
//     triggers - but only ONE trigger at a time, because picking a trigger type in its
//     settings cleared every other one.
//
// The model here is a trigger-first table: every trigger independently holds a LIST of
// actions, and an action names WHERE its inputs come from. So a wakeup DP, an on-hit burst
// and a looping blockstring can all be armed at once, which neither system allowed.
//
// A trigger with more than one action picks between them when it fires - at random, in
// order, or at random without repeats - exactly the way a Playback Library used to pick
// between its entries. That is why there is no library source any more: a trigger's action
// list IS the library, without the detour through a separate file, and every kind of action
// (notation, a file, an animation, a captured recording...) can sit in it side by side. Each
// action and each trigger has its own on/off switch, so a set can be built once and pieces
// of it turned off without losing them.
//
// Old files that named a library are still read: the library's entries are unpacked into
// captured-recording actions on that trigger, so nothing the user built is lost.
//
// See docs/DummyActionsRework.md for the whole design.
class DummyActionManager
{
public:
	using TriggerType = UnlimitedPlaybackManager::TriggerType;

	// Where an action's inputs come from. The order is the order of the "Add" menu.
	enum Source
	{
		Source_None = -1,
		Source_Notation = 0,  // typed numpad notation, parsed by the TAS tool's own parser
		Source_Library,       // LEGACY - a named Playback Library. Read from old files and
		                      // unpacked into Source_Recording actions; never created anew.
		Source_File,          // one playback file
		Source_Animation,     // one of the dummy's own script states
		Source_CfSlot,        // one of the game's four recording slots, read live
		Source_Burst,         // burst out of it, On Hitstun (Trigger_OnHit) only
		Source_Recording,     // a copy of a recording slot taken when the action was added
		Source_Count,
	};

	struct Action
	{
		// Off means it stays in the list but is never picked. Only meaningful when the
		// trigger holds more than one action; a lone action is always on.
		bool enabled = true;

		Source source = Source_None;

		// Source_Notation. Kept as text so the modal can show what was typed; the parse is
		// redone on assignment and the result cached in frames.
		std::string notation;
		std::vector<char> notationFrames;

		// Source_File. Path for playing it, name for the row summary.
		std::string filePath;
		std::string fileName;

		// Source_Animation. One move and the frames to wait, once the trigger has fired and
		// picked this action, before forcing it. Several moves on one trigger are several
		// actions, picked between like any other list; a file from when an action could hold
		// a random set of its own is split into one action per move on load. Kept as vectors
		// so that load can read the old layout, but the picker only ever writes one entry.
		std::vector<scrState*> animations;
		std::vector<int> animationDelays;
		// The same moves by name. A scrState* points into one character's script memory, so
		// it cannot survive a character swap, let alone a restart - the names are what is
		// saved to disk and what the pointers are rebuilt from after every parse.
		std::vector<std::string> animationNames;
		// Who the dummy was when the move was picked, by name. Move names are shared across
		// the cast ("NmlAtk5B" is everyone's 5B), so a name matching is not proof it is the
		// move that was meant - this is what lets a row or an import say "picked on Kokonoe".
		// Only a warning: a lab can hold moves for several characters on purpose. Empty for
		// actions saved before it was recorded.
		std::string animationCharacter;

		// Source_Library (legacy, load-time only).
		std::string libraryPath;
		std::string libraryName;
		int selectionMode = UnlimitedPlaybackManager::Selection_Random;

		// Source_CfSlot, 1-4. Also which slot a Source_Recording was captured from.
		int cfSlot = 1;

		// Source_Recording. The slot's raw two-bytes-per-frame buffer and its facing, taken
		// at the moment the action was added, so re-recording the slot does not change it.
		// Saved to disk as hex, so a lab of captured recordings is one file.
		std::vector<char> recordingFrames;
		bool recordingFacingLeft = false;
		std::string recordingName;

		// Source_Animation. Naoto's EN specials need a flag held down for his own script
		// states to be reachable at all, so it belongs with the animation payload rather
		// than as a loose toggle on a panel.
		bool naotoEnSpecials = false;

		bool IsSet() const { return source != Source_None; }
	};

	// A trigger's action list and how it picks from it.
	struct Trigger
	{
		// Off means nothing in it fires, whatever the actions say. Separate from the per
		// action switch so a whole setup can be parked without unticking every row.
		bool enabled = true;
		// One of UnlimitedPlaybackManager::Selection_*. Only consulted with more than one
		// enabled action.
		int selectionMode = UnlimitedPlaybackManager::Selection_Random;
		std::vector<Action> actions;
	};

	static DummyActionManager& Instance();

	// Triggers, in the order the rows and the "+ Add Action" menu list them.
	static const std::vector<TriggerType>& TriggerOrder();
	static const char* TriggerLabel(TriggerType trigger);
	// What the trigger fires on, for the row's tooltip. These are the conditions the
	// execution code actually tests, not loose descriptions.
	static const char* TriggerWhen(TriggerType trigger);
	static const char* SourceLabel(Source source);

	// Which sources a given trigger will accept. Two rules, both about the trigger's own
	// nature rather than about the UI:
	//
	//   - Burst is an answer to being hit, so it means nothing anywhere but On Hitstun
	//     (Trigger_OnHit) - by On Hit (Trigger_OnHitRecovery) the combo is already over.
	//   - Animation forces one of the dummy's script states outright, ignoring whether the
	//     game would let that state start. On Hit and On Block the dummy is in hitstun or
	//     blockstun, and forcing a move out of either is the dummy breaking the game's own
	//     rules - the only honest way to act out of those is inputs, which is what every
	//     other source is. So those two triggers do not offer it.
	static bool SourceAllowedForTrigger(Source source, TriggerType trigger);

	Trigger& Get(TriggerType trigger);
	const Trigger& Get(TriggerType trigger) const;

	// Whether the trigger holds anything at all. A trigger with no actions is not shown
	// and does not exist as far as the user is concerned.
	bool HasAction(TriggerType trigger) const;
	// Triggers with at least one action.
	int AssignedCount() const;
	// True while at least one trigger is still empty.
	bool HasFreeTrigger() const;

	// Appends a fully configured action to a trigger and returns its index. The UI edits a
	// draft and calls this once, so a row appears only when it is actually set up and
	// cancelling changes nothing.
	size_t Add(TriggerType trigger, const Action& action);
	// Overwrites one action in place, keeping its position and enable flag.
	void Replace(TriggerType trigger, size_t index, const Action& action);
	// Removes one action. Removing the last one removes the trigger with it.
	void Remove(TriggerType trigger, size_t index);
	// Removes the trigger and every action in it.
	void Clear(TriggerType trigger);
	void ClearAll();

	void SetTriggerEnabled(TriggerType trigger, bool enabled);
	void SetActionEnabled(TriggerType trigger, size_t index, bool enabled);
	void SetActionsEnabled(TriggerType trigger, const std::vector<size_t>& indices, bool enabled);
	void SetAllActionsEnabled(TriggerType trigger, bool enabled);
	void SetSelectionMode(TriggerType trigger, int mode);
	// The list operations the Playback Library's rows had: several at once, reordered by
	// insertion point (the same algorithm as UnlimitedPlaybackManager::MoveEntries), and
	// a captured recording renamed.
	void RemoveActions(TriggerType trigger, std::vector<size_t> indices);
	bool MoveActions(TriggerType trigger, const std::vector<size_t>& fromIndicesSorted,
		size_t insertionIndex, size_t* outInsertedAt = nullptr);
	void SetRecordingName(TriggerType trigger, size_t index, const std::string& name);
	// Unpacks a Playback Library file (*.upl) into captured-recording actions on the
	// trigger, on or off as each entry was. Returns how many were added, -1 if the file
	// could not be read. Also what an old single-library action is migrated through.
	int AddLibraryFile(TriggerType trigger, const std::string& path, int* outSelectionMode = nullptr);

	// One line for a row: the notation typed, the animation picked, the file named, and
	// so on.
	static std::string ActionSummary(const Action& action);
	// For a trigger: its single action's summary, or how many it holds.
	std::string Summary(TriggerType trigger) const;

	// Whether an action is configured enough to actually do something. One can sit
	// half-configured if the user opens a modal and cancels out of it.
	static bool IsActionRunnable(const Action& action);
	// Whether a trigger, as a whole, will do something when it fires: it is on, and it
	// holds at least one action that is on and runnable.
	bool IsRunnable(TriggerType trigger) const;

	// Picks the action a firing should use, per the trigger's selection mode, over the
	// actions that are on and runnable. Null when there is nothing to pick. Advances the
	// sequential index / no-repeat pool, so call it once per firing.
	const Action* Pick(TriggerType trigger);

	// Keeps UnlimitedPlaybackManager's per-trigger enable flags in step with this table.
	// That flag is the gate its TryFireTrigger checks first, so it has to mean "armed" for
	// every source. Cheap, and called every frame rather than only on edit: a modal that
	// finishes configuring an action changes the answer, and relying on the UI to say so
	// is how a feature ends up only working while its menu is open.
	void SyncTriggerEnable();

	// Drops animation pointers when the dummy's script is re-parsed. They point into that
	// character's script memory, so they cannot outlive it. The names are kept.
	void InvalidateAnimations();

	// Whether the animation pointers were last matched against this dummy. False for the
	// frame or so between the dummy changing and the re-match, when they still point into
	// the previous character's script.
	bool AnimationsResolvedFor(const void* charData) const { return charData && m_resolvedForCharData == charData; }

	// Every character an animation action was picked on, once each, in trigger order.
	std::vector<std::string> AnimationCharacters() const;

	// Rebuilds those pointers from the saved names against a parsed state list. A move the
	// current character does not have is dropped, along with its delay.
	//
	// ownerCharData is whose script the list belongs to; it is remembered so this is done
	// once per character rather than every frame, including when it legitimately resolves
	// to nothing because that character has no such move.
	void ResolveAnimations(const std::vector<scrState*>& states, const void* ownerCharData);

	// True when some animation action is holding names that have not been matched against
	// this character yet. The reason it is asked every frame rather than only on a character
	// swap: actions loaded from disk arrive AFTER the script was parsed, so there is no swap
	// left to hang the work on and the names would sit unresolved forever.
	bool NeedsAnimationResolve(const void* ownerCharData) const;

	// The table on disk, so a lab survives a restart. Text, one section per trigger holding
	// the trigger's own settings, then one section per action.
	void Save() const;
	void Load();
	// Loads once, the first time it is safe to touch the disk.
	void EnsureLoaded();

	// The same file format written to / read from an arbitrary path, for sharing a lab.
	// Importing replaces everything here. Neither touches the window sizes.
	//
	// The training file carries more than the dummy: extras is a flat key=value list the
	// caller owns (positions, wake-up timing, input delay), written under a [training]
	// section and handed back on import. This class does not interpret them.
	//
	// An export is self-contained: an action that reads a playback file or a CF slot on
	// this machine is written as a captured recording of what that file or slot holds right
	// now, so the recipient plays exactly what the author did and nothing points at a path
	// or a slot of theirs. The live table is not changed. One that cannot be read (a file
	// that is gone, an empty slot) is left out and named in the report.
	using Extras = std::vector<std::pair<std::string, std::string>>;
	struct ExportReport
	{
		int embeddedFiles = 0;
		int frozenSlots = 0;
		// "On Wakeup: slot 2 is empty", one per action left out.
		std::vector<std::string> skipped;
	};
	bool ExportTo(const std::string& path, const Extras* extras = nullptr, ExportReport* report = nullptr) const;
	bool ImportFrom(const std::string& path, Extras* extrasOut = nullptr);

	// Everything back to how a fresh install has it: no triggers, default firing settings,
	// default loop settings. Saved.
	void ResetToDefaults();

	// Window sizes the user has settled on, saved alongside the actions. Kept here rather
	// than in the panel because this is the thing that owns the file - ImGui only persists
	// window sizes through an ini the mod does not write.
	void SetUiSize(const std::string& key, float width, float height);
	bool GetUiSize(const std::string& key, float* width, float* height) const;

private:
	DummyActionManager() = default;

	using TriggerTable = std::array<Trigger, UnlimitedPlaybackManager::Trigger_Count>;
	bool WriteTo(const std::string& path, const TriggerTable& triggers, bool includeUiSizes, const Extras* extras) const;
	bool ReadFrom(const std::string& path, bool includeUiSizes, Extras* extrasOut);
	// Wipes the table and the per-trigger firing settings that live in
	// UnlimitedPlaybackManager, without saving. What Load and Import start from.
	void ResetInMemory();

	TriggerTable m_triggers;
	// Picking bookkeeping, per trigger: where a sequential pick is up to, and which actions
	// a no-repeat pick has not yet used this round.
	std::array<size_t, UnlimitedPlaybackManager::Trigger_Count> m_sequentialIndex = {};
	std::array<std::vector<size_t>, UnlimitedPlaybackManager::Trigger_Count> m_nonRepeatPools = {};
	bool m_loaded = false;
	// Whose parsed script the animation pointers were last matched against.
	const void* m_resolvedForCharData = nullptr;
	std::map<std::string, std::pair<float, float>> m_uiSizes;
	// Set while Load() is running, so the writes it makes do not each save the file again.
	bool m_loading = false;
};
