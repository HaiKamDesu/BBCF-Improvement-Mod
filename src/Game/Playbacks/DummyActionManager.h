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
//     settings cleared every other one ("Selects the single trigger type that can fire
//     library playback").
//
// The model here is a trigger-first table: every trigger independently holds at most one
// action, and an action names WHERE its inputs come from. So a wakeup DP, an on-hit burst
// and a looping blockstring can all be armed at once, which neither system allowed.
//
// A library is a named collection of recorded playbacks on disk - which is what an Unlimited
// Playback "profile" already is, since SaveProfile embeds every entry's playback bytes in the
// file. Each trigger names the library it draws from, so On Wakeup and On Block Gap can pull
// from entirely different sets, and the same library can serve several triggers.
//
// That makes PlaybackEntry::triggerEnabled obsolete: which entries a trigger may use is now
// decided by which library is assigned, not by a flag on every entry. Old library files still
// parse, and the field is read and discarded.
//
// See docs/DummyActionsRework.md for the whole design.
class DummyActionManager
{
public:
	using TriggerType = UnlimitedPlaybackManager::TriggerType;

	// Where a trigger's inputs come from. The order is the order of the "Change..." menu.
	enum Source
	{
		Source_None = -1,
		Source_Notation = 0,  // typed numpad notation, parsed by the TAS tool's own parser
		Source_Library,       // a named Playback Library, drawn from per its selection mode
		Source_File,          // one playback file
		Source_Animation,     // one or more of the dummy's own script states
		Source_CfSlot,        // one of the game's four recording slots
		Source_Burst,         // burst out of it, On Hit only
		Source_Count,
	};

	struct Action
	{
		Source source = Source_None;

		// Source_Notation. Kept as text so the modal can show what was typed; the parse is
		// redone on assignment and the result cached in frames.
		std::string notation;
		std::vector<char> notationFrames;

		// Source_File. Path for playing it, name for the row summary.
		std::string filePath;
		std::string fileName;

		// Source_Animation. A pool, matching what the old panel could do: more than one and
		// the dummy picks at random. Delays are per entry.
		std::vector<scrState*> animations;
		std::vector<int> animationDelays;
		// The same moves by name. A scrState* points into one character's script memory, so
		// it cannot survive a character swap, let alone a restart - the names are what is
		// saved to disk and what the pointers are rebuilt from after every parse.
		std::vector<std::string> animationNames;

		// Source_Library. Which library this trigger draws from, and how it picks when the
		// library holds more than one entry. The mode lives here rather than in the library
		// because a library is a reusable collection: two triggers can share one and still
		// pick from it differently.
		std::string libraryPath;
		std::string libraryName;
		int selectionMode = UnlimitedPlaybackManager::Selection_Random;

		// Source_CfSlot, 1-4.
		int cfSlot = 1;

		// Source_Animation. Naoto's EN specials need a flag held down for his own script
		// states to be reachable at all, so it belongs with the animation payload rather
		// than as a loose toggle on a panel.
		bool naotoEnSpecials = false;

		bool IsSet() const { return source != Source_None; }
	};

	static DummyActionManager& Instance();

	// Triggers, in the order the rows and the "+ Add Action" menu list them.
	static const std::vector<TriggerType>& TriggerOrder();
	static const char* TriggerLabel(TriggerType trigger);
	// What the trigger fires on, for the row's tooltip. These are the conditions the
	// execution code actually tests, not loose descriptions.
	static const char* TriggerWhen(TriggerType trigger);
	static const char* SourceLabel(Source source);

	Action& Get(TriggerType trigger);
	const Action& Get(TriggerType trigger) const;

	bool HasAction(TriggerType trigger) const;
	int AssignedCount() const;
	// True while at least one trigger is still free, i.e. while "+ Add Action" has anything
	// left to offer.
	bool HasFreeTrigger() const;

	// Commits a fully configured action onto a trigger. The UI edits a draft and calls this
	// once, so a row appears only when it is actually set up and cancelling changes nothing.
	void Set(TriggerType trigger, const Action& action);
	void Clear(TriggerType trigger);
	void ClearAll();

	// One line for the row: the notation typed, the animation picked, the library named, and
	// so on.
	std::string Summary(TriggerType trigger) const;

	// Whether the action is configured enough to actually do something. A trigger can sit
	// half-configured if the user opens a modal and cancels out of it.
	bool IsRunnable(TriggerType trigger) const;

	// Keeps UnlimitedPlaybackManager's per-trigger enable flags in step with this table.
	// That flag is the gate its TryFireTrigger checks first, so it has to mean "armed" for
	// every source, not only the library. Cheap, and called every frame rather than only on
	// edit: a modal that finishes configuring an action changes the answer, and relying on
	// the UI to say so is how a feature ends up only working while its menu is open.
	void SyncTriggerEnable();

	// Drops animation pointers when the dummy's script is re-parsed. They point into that
	// character's script memory, so they cannot outlive it. The names are kept.
	void InvalidateAnimations();

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

	// The table on disk, so a lab survives a restart. Text, one section per trigger, holding
	// the action and the trigger's own timing settings.
	void Save() const;
	void Load();
	// Loads once, the first time it is safe to touch the disk.
	void EnsureLoaded();

	// Window sizes the user has settled on, saved alongside the actions. Kept here rather
	// than in the panel because this is the thing that owns the file - ImGui only persists
	// window sizes through an ini the mod does not write.
	void SetUiSize(const std::string& key, float width, float height);
	bool GetUiSize(const std::string& key, float* width, float* height) const;

private:
	DummyActionManager() = default;

	std::array<Action, UnlimitedPlaybackManager::Trigger_Count> m_actions;
	bool m_loaded = false;
	// Whose parsed script the animation pointers were last matched against.
	const void* m_resolvedForCharData = nullptr;
	std::map<std::string, std::pair<float, float>> m_uiSizes;
	// Set while Load() is running, so the writes it makes do not each save the file again.
	bool m_loading = false;
};
