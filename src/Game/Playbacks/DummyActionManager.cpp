#include "DummyActionManager.h"

#include "Core/Localization.h"
#include "Core/utils.h"
#include "Game/Scr/ScrStateNames.h"
#include "Game/TasManager.h"
#include "Core/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>

DummyActionManager& DummyActionManager::Instance()
{
	static DummyActionManager instance;
	return instance;
}

const std::vector<DummyActionManager::TriggerType>& DummyActionManager::TriggerOrder()
{
	// Ordered by how often people reach for them, not by the enum. On Block and Throw Tech
	// come last because they are the specialised pair.
	static const std::vector<TriggerType> order = {
		UnlimitedPlaybackManager::Trigger_Wakeup,
		UnlimitedPlaybackManager::Trigger_Gap,
		UnlimitedPlaybackManager::Trigger_OnHitRecovery,
		UnlimitedPlaybackManager::Trigger_OnHit,
		UnlimitedPlaybackManager::Trigger_KeyPress,
		UnlimitedPlaybackManager::Trigger_OnLoop,
		UnlimitedPlaybackManager::Trigger_OnBlock,
		UnlimitedPlaybackManager::Trigger_ThrowTech,
	};
	return order;
}

const char* DummyActionManager::TriggerLabel(TriggerType trigger)
{
	switch (trigger)
	{
	case UnlimitedPlaybackManager::Trigger_Wakeup:    return "On Wakeup";
	case UnlimitedPlaybackManager::Trigger_Gap:       return "On Block Gap";
	case UnlimitedPlaybackManager::Trigger_OnBlock:   return "On Block";
	case UnlimitedPlaybackManager::Trigger_OnHit:     return "On Hitstun";
	case UnlimitedPlaybackManager::Trigger_OnHitRecovery: return "On Hit";
	case UnlimitedPlaybackManager::Trigger_ThrowTech: return "On Throw Tech";
	case UnlimitedPlaybackManager::Trigger_KeyPress:  return "On Hotkey";
	case UnlimitedPlaybackManager::Trigger_OnLoop:    return "On Loop";
	default: break;
	}
	return "";
}

const char* DummyActionManager::TriggerWhen(TriggerType trigger)
{
	switch (trigger)
	{
	case UnlimitedPlaybackManager::Trigger_Wakeup:
		return "as the dummy gets up, and out of a stagger";
	case UnlimitedPlaybackManager::Trigger_Gap:
		return "the instant the dummy leaves blockstun";
	case UnlimitedPlaybackManager::Trigger_OnBlock:
		return "every time the dummy blocks something";
	case UnlimitedPlaybackManager::Trigger_OnHit:
		// Burst is handed to the game as the dummy is hit, so it breaks the combo;
		// anything made of inputs has to wait for hitstun to end to come out at all.
		return "the moment the dummy is hit, once per combo - a burst goes out at once, inputs once hitstun ends";
	case UnlimitedPlaybackManager::Trigger_OnHitRecovery:
		return "the instant the dummy stops being hit and can act again - not during a combo, and not after a knockdown";
	case UnlimitedPlaybackManager::Trigger_ThrowTech:
		return "after the dummy techs a throw";
	case UnlimitedPlaybackManager::Trigger_KeyPress:
		return "when you press the playback hotkey";
	case UnlimitedPlaybackManager::Trigger_OnLoop:
		return "over and over, resetting the lab between runs";
	default: break;
	}
	return "";
}

const char* DummyActionManager::SourceLabel(Source source)
{
	switch (source)
	{
	case Source_Notation:  return "From Input Notation";
	case Source_Library:   return "From Playback Library";
	case Source_File:      return "From File";
	case Source_Animation: return "From Animation";
	case Source_CfSlot:    return "From CF Recording Slot";
	case Source_Burst:     return "Burst";
	case Source_Recording: return "Recording";
	default: break;
	}
	return "";
}

bool DummyActionManager::SourceAllowedForTrigger(Source source, TriggerType trigger)
{
	if (source == Source_Library)
	{
		// Only ever read from old files, never offered.
		return false;
	}

	if (source == Source_Burst)
	{
		return trigger == UnlimitedPlaybackManager::Trigger_OnHit;
	}

	if (source == Source_Animation)
	{
		return trigger != UnlimitedPlaybackManager::Trigger_OnHit
			&& trigger != UnlimitedPlaybackManager::Trigger_OnBlock;
	}

	return true;
}

DummyActionManager::Trigger& DummyActionManager::Get(TriggerType trigger)
{
	return m_triggers[static_cast<size_t>(trigger)];
}

const DummyActionManager::Trigger& DummyActionManager::Get(TriggerType trigger) const
{
	return m_triggers[static_cast<size_t>(trigger)];
}

bool DummyActionManager::HasAction(TriggerType trigger) const
{
	return !Get(trigger).actions.empty();
}

int DummyActionManager::AssignedCount() const
{
	int count = 0;
	for (const Trigger& trigger : m_triggers)
	{
		if (!trigger.actions.empty()) { ++count; }
	}
	return count;
}

bool DummyActionManager::HasFreeTrigger() const
{
	for (TriggerType trigger : TriggerOrder())
	{
		if (!HasAction(trigger)) { return true; }
	}
	return false;
}

namespace
{
	// Names taken from the pointers at the moment of commit, which is the only moment they
	// are known to be good: they were just picked out of the current character's state list.
	//
	// Only when there ARE pointers. An action arriving from disk has names and no pointers
	// yet - deriving from an empty list wiped exactly the names that had just been read,
	// which is why a saved animation action came back as "no move picked" and would have
	// been written back out empty on the next save.
	void DeriveAnimationNames(DummyActionManager::Action& stored)
	{
		if (stored.source == DummyActionManager::Source_Animation && !stored.animations.empty())
		{
			stored.animationNames.clear();
			stored.animationNames.reserve(stored.animations.size());
			for (const scrState* state : stored.animations)
			{
				stored.animationNames.push_back(state ? state->name : std::string());
			}
		}
	}
}

size_t DummyActionManager::Add(TriggerType trigger, const Action& action)
{
	Trigger& slot = Get(trigger);
	slot.actions.push_back(action);
	DeriveAnimationNames(slot.actions.back());
	// The list changed shape, so a no-repeat round in progress is over.
	m_nonRepeatPools[trigger].clear();

	SyncTriggerEnable();
	Save();
	return slot.actions.size() - 1;
}

void DummyActionManager::Replace(TriggerType trigger, size_t index, const Action& action)
{
	Trigger& slot = Get(trigger);
	if (index >= slot.actions.size()) { return; }
	const bool wasEnabled = slot.actions[index].enabled;
	slot.actions[index] = action;
	slot.actions[index].enabled = wasEnabled;
	DeriveAnimationNames(slot.actions[index]);

	SyncTriggerEnable();
	Save();
}

void DummyActionManager::Remove(TriggerType trigger, size_t index)
{
	Trigger& slot = Get(trigger);
	if (index >= slot.actions.size()) { return; }
	slot.actions.erase(slot.actions.begin() + index);
	m_nonRepeatPools[trigger].clear();
	m_sequentialIndex[trigger] = 0;
	if (slot.actions.empty())
	{
		// The last action went, and a trigger with nothing in it is not a trigger. Its
		// switch and picking order go back to defaults so the next one starts clean.
		slot = Trigger{};
	}

	SyncTriggerEnable();
	Save();
}

void DummyActionManager::Clear(TriggerType trigger)
{
	Get(trigger) = Trigger{};
	m_nonRepeatPools[trigger].clear();
	m_sequentialIndex[trigger] = 0;
	SyncTriggerEnable();
	Save();
}

void DummyActionManager::ClearAll()
{
	for (Trigger& trigger : m_triggers)
	{
		trigger = Trigger{};
	}
	m_nonRepeatPools = {};
	m_sequentialIndex = {};
	SyncTriggerEnable();
	Save();
}

void DummyActionManager::SetTriggerEnabled(TriggerType trigger, bool enabled)
{
	Trigger& slot = Get(trigger);
	if (slot.enabled == enabled) { return; }
	slot.enabled = enabled;
	SyncTriggerEnable();
	Save();
}

void DummyActionManager::SetActionEnabled(TriggerType trigger, size_t index, bool enabled)
{
	Trigger& slot = Get(trigger);
	if (index >= slot.actions.size() || slot.actions[index].enabled == enabled) { return; }
	slot.actions[index].enabled = enabled;
	m_nonRepeatPools[trigger].clear();
	SyncTriggerEnable();
	Save();
}

void DummyActionManager::SetSelectionMode(TriggerType trigger, int mode)
{
	Trigger& slot = Get(trigger);
	if (slot.selectionMode == mode) { return; }
	slot.selectionMode = mode;
	m_nonRepeatPools[trigger].clear();
	m_sequentialIndex[trigger] = 0;
	Save();
}

void DummyActionManager::SetActionsEnabled(TriggerType trigger, const std::vector<size_t>& indices, bool enabled)
{
	Trigger& slot = Get(trigger);
	bool changed = false;
	for (size_t index : indices)
	{
		if (index < slot.actions.size() && slot.actions[index].enabled != enabled)
		{
			slot.actions[index].enabled = enabled;
			changed = true;
		}
	}
	if (!changed) { return; }
	m_nonRepeatPools[trigger].clear();
	SyncTriggerEnable();
	Save();
}

void DummyActionManager::SetAllActionsEnabled(TriggerType trigger, bool enabled)
{
	std::vector<size_t> all;
	for (size_t i = 0; i < Get(trigger).actions.size(); ++i) { all.push_back(i); }
	SetActionsEnabled(trigger, all, enabled);
}

void DummyActionManager::RemoveActions(TriggerType trigger, std::vector<size_t> indices)
{
	Trigger& slot = Get(trigger);
	std::sort(indices.begin(), indices.end());
	indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
	bool changed = false;
	for (auto it = indices.rbegin(); it != indices.rend(); ++it)
	{
		if (*it < slot.actions.size())
		{
			slot.actions.erase(slot.actions.begin() + static_cast<std::ptrdiff_t>(*it));
			changed = true;
		}
	}
	if (!changed) { return; }
	m_nonRepeatPools[trigger].clear();
	m_sequentialIndex[trigger] = 0;
	if (slot.actions.empty()) { slot = Trigger{}; }
	SyncTriggerEnable();
	Save();
}

bool DummyActionManager::MoveActions(TriggerType trigger, const std::vector<size_t>& fromIndicesSorted,
	size_t insertionIndex, size_t* outInsertedAt)
{
	Trigger& slot = Get(trigger);
	std::vector<Action>& actions = slot.actions;
	if (fromIndicesSorted.empty() || insertionIndex > actions.size()) { return false; }
	for (size_t idx : fromIndicesSorted)
	{
		if (idx >= actions.size()) { return false; }
	}
	std::vector<bool> isMoved(actions.size(), false);
	size_t removedBeforeInsertion = 0;
	for (size_t idx : fromIndicesSorted)
	{
		isMoved[idx] = true;
		if (idx < insertionIndex) { ++removedBeforeInsertion; }
	}
	std::vector<Action> moved;
	std::vector<Action> remaining;
	for (size_t i = 0; i < actions.size(); ++i)
	{
		(isMoved[i] ? moved : remaining).push_back(std::move(actions[i]));
	}
	size_t insertAt = insertionIndex - removedBeforeInsertion;
	if (insertAt > remaining.size()) { insertAt = remaining.size(); }
	remaining.insert(remaining.begin() + static_cast<std::ptrdiff_t>(insertAt),
		std::make_move_iterator(moved.begin()), std::make_move_iterator(moved.end()));
	actions = std::move(remaining);
	if (outInsertedAt) { *outInsertedAt = insertAt; }
	m_nonRepeatPools[trigger].clear();
	m_sequentialIndex[trigger] = 0;
	Save();
	return true;
}

void DummyActionManager::SetRecordingName(TriggerType trigger, size_t index, const std::string& name)
{
	Trigger& slot = Get(trigger);
	if (index >= slot.actions.size() || slot.actions[index].source != Source_Recording) { return; }
	slot.actions[index].recordingName = name;
	Save();
}

int DummyActionManager::AddLibraryFile(TriggerType trigger, const std::string& path, int* outSelectionMode)
{
	UnlimitedPlaybackManager::PlaybackLibrary library;
	if (path.empty()
		|| !UnlimitedPlaybackManager::Instance().ParseLibraryFile(path, true, &library, nullptr))
	{
		return -1;
	}
	int unpacked = 0;
	for (const UnlimitedPlaybackManager::PlaybackEntry& entry : library.entries)
	{
		const auto cached = library.cache.find(entry.id);
		if (cached == library.cache.end() || !cached->second.loaded) { continue; }
		Action recording;
		recording.source = Source_Recording;
		recording.enabled = entry.enabled;
		recording.recordingName = entry.name;
		recording.recordingFrames = cached->second.frames;
		recording.recordingFacingLeft = cached->second.facingLeft;
		Add(trigger, recording);
		++unpacked;
	}
	if (outSelectionMode) { *outSelectionMode = library.selectionMode; }
	LOG(1, "[DummyActions] unpacked library '%s' into %d recording(s) on %s\n",
		path.c_str(), unpacked, TriggerLabel(trigger));
	return unpacked;
}

bool DummyActionManager::IsActionRunnable(const Action& action)
{
	switch (action.source)
	{
	case Source_Notation:  return !action.notationFrames.empty();
	case Source_File:      return !action.filePath.empty();
	case Source_Animation: return !action.animations.empty();
	case Source_CfSlot:    return action.cfSlot >= 1 && action.cfSlot <= 4;
	case Source_Burst:     return true;   // nothing to configure
	case Source_Recording: return !action.recordingFrames.empty();
	default:
		break;
	}
	return false;
}

bool DummyActionManager::IsRunnable(TriggerType trigger) const
{
	const Trigger& slot = Get(trigger);
	if (!slot.enabled) { return false; }
	// A lone action is always on: its switch is not even shown.
	const bool single = slot.actions.size() == 1;
	for (const Action& action : slot.actions)
	{
		if ((single || action.enabled) && IsActionRunnable(action)) { return true; }
	}
	return false;
}

const DummyActionManager::Action* DummyActionManager::Pick(TriggerType trigger)
{
	Trigger& slot = Get(trigger);
	if (!slot.enabled) { return nullptr; }

	std::vector<size_t> candidates;
	const bool single = slot.actions.size() == 1;
	for (size_t i = 0; i < slot.actions.size(); ++i)
	{
		const Action& action = slot.actions[i];
		if ((single || action.enabled) && IsActionRunnable(action)) { candidates.push_back(i); }
	}
	if (candidates.empty()) { return nullptr; }
	if (candidates.size() == 1) { return &slot.actions[candidates[0]]; }

	static std::mt19937 rng(static_cast<unsigned int>(
		std::chrono::high_resolution_clock::now().time_since_epoch().count()));

	// The same three orders a Playback Library offered, which is what these lists replace.
	if (slot.selectionMode == UnlimitedPlaybackManager::Selection_Sequential)
	{
		const size_t pos = m_sequentialIndex[trigger] % candidates.size();
		m_sequentialIndex[trigger] = (pos + 1) % candidates.size();
		return &slot.actions[candidates[pos]];
	}

	if (slot.selectionMode == UnlimitedPlaybackManager::Selection_NonRepeatingRandom)
	{
		std::vector<size_t>& pool = m_nonRepeatPools[trigger];
		// Anything the pool still holds that is no longer a candidate (turned off since)
		// is dropped; an empty pool starts a fresh round.
		pool.erase(std::remove_if(pool.begin(), pool.end(), [&](size_t idx) {
			return std::find(candidates.begin(), candidates.end(), idx) == candidates.end();
		}), pool.end());
		if (pool.empty()) { pool = candidates; }
		std::uniform_int_distribution<size_t> dist(0, pool.size() - 1);
		const size_t at = dist(rng);
		const size_t picked = pool[at];
		pool.erase(pool.begin() + at);
		return &slot.actions[picked];
	}

	std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
	return &slot.actions[candidates[dist(rng)]];
}

std::string DummyActionManager::ActionSummary(const Action& action)
{
	switch (action.source)
	{
	case Source_Notation:
		return action.notation.empty() ? L("not typed yet") : action.notation;

	case Source_File:
		return action.fileName.empty() ? L("no file picked") : action.fileName;

	case Source_CfSlot:
		return FormatText(L("slot %d").c_str(), action.cfSlot);

	case Source_Recording:
	{
		const std::string name = action.recordingName.empty()
			? FormatText(L("slot %d").c_str(), action.cfSlot) : action.recordingName;
		return name + "  " + FormatText(L("(%d frames)").c_str(),
			static_cast<int>(action.recordingFrames.size() / 2));
	}

	case Source_Burst:
		return L("burst out of it");

	case Source_Animation:
	{
		if (action.animations.empty())
		{
			if (action.animationNames.empty())
			{
				return L("no move picked");
			}
			// Names are held but nothing matched. Show them anyway - saying "no move picked"
			// about an action that plainly has one is how a loaded action looks broken.
			std::string missing;
			for (size_t i = 0; i < action.animationNames.size(); ++i)
			{
				if (i) { missing += ", "; }
				missing += ScrStateNames::Display(action.animationNames[i]);
			}
			return missing + "  " + L("(this character has no such move)");
		}
		std::string summary;
		for (size_t i = 0; i < action.animations.size(); ++i)
		{
			if (i) { summary += ", "; }
			summary += ScrStateNames::Display(action.animations[i]->name);
			const int delay = i < action.animationDelays.size() ? action.animationDelays[i] : 0;
			if (delay) { summary += "+" + std::to_string(delay) + "f"; }
		}
		if (action.animations.size() > 1)
		{
			summary += "  ";
			summary += L("(random)");
		}
		return summary;
	}

	case Source_Library:
		return action.libraryName.empty() ? L("no library picked") : action.libraryName;

	default:
		break;
	}
	return "";
}

std::string DummyActionManager::Summary(TriggerType trigger) const
{
	const Trigger& slot = Get(trigger);
	if (slot.actions.empty()) { return ""; }
	if (slot.actions.size() == 1) { return ActionSummary(slot.actions.front()); }
	return FormatText(L("%d actions").c_str(), static_cast<int>(slot.actions.size()));
}

void DummyActionManager::SyncTriggerEnable()
{
	// TriggerConfig::enabled is the gate at the top of UnlimitedPlaybackManager's
	// TryFireTrigger, so it means "this trigger is armed" - for any source, not just the
	// library. An earlier cut of this set it to (source == Source_Library), which silently
	// disabled every notation, file and slot action ever assigned: the row looked armed and
	// nothing fired. Which action a trigger uses is settled later, by Pick.
	UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();
	for (int i = 0; i < UnlimitedPlaybackManager::Trigger_Count; ++i)
	{
		const TriggerType trigger = static_cast<TriggerType>(i);
		playback.GetTrigger(trigger).enabled = IsRunnable(trigger);
	}
}

void DummyActionManager::InvalidateAnimations()
{
	for (Trigger& trigger : m_triggers)
	{
		for (Action& action : trigger.actions)
		{
			if (action.source != Source_Animation) { continue; }
			// Pointers only. The names stay, so the same moves can be found again for
			// whoever the dummy is now - and so a saved action survives a character swap.
			action.animations.clear();
		}
	}
}

bool DummyActionManager::NeedsAnimationResolve(const void* ownerCharData) const
{
	if (m_resolvedForCharData == ownerCharData)
	{
		return false;
	}
	for (const Trigger& trigger : m_triggers)
	{
		for (const Action& action : trigger.actions)
		{
			if (action.source == Source_Animation && !action.animationNames.empty())
			{
				return true;
			}
		}
	}
	return false;
}

void DummyActionManager::ResolveAnimations(const std::vector<scrState*>& states,
	const void* ownerCharData)
{
	m_resolvedForCharData = ownerCharData;

	for (size_t t = 0; t < m_triggers.size(); ++t)
	{
		for (Action& action : m_triggers[t].actions)
		{
			if (action.source != Source_Animation || action.animationNames.empty())
			{
				continue;
			}

			std::vector<scrState*> resolved;
			std::vector<int> delays;
			for (size_t i = 0; i < action.animationNames.size(); ++i)
			{
				for (scrState* state : states)
				{
					if (state && state->name == action.animationNames[i])
					{
						resolved.push_back(state);
						delays.push_back(i < action.animationDelays.size()
							? action.animationDelays[i] : 0);
						break;
					}
				}
			}
			action.animations = resolved;
			action.animationDelays = delays;

			if (resolved.size() != action.animationNames.size())
			{
				LOG(1, "[DummyActions] '%s': matched %u of %u saved move(s) for this character\n",
					TriggerLabel(static_cast<TriggerType>(t)),
					static_cast<unsigned int>(resolved.size()),
					static_cast<unsigned int>(action.animationNames.size()));
			}
		}
	}
	SyncTriggerEnable();
}

namespace
{
	std::string DummyActionsFilePath()
	{
		return GamePath("BBCF_IM/dummy_actions.ini");
	}

	// Joined with '|' because a move name never contains one, and a blank entry has to
	// survive the round trip.
	std::string JoinList(const std::vector<std::string>& parts)
	{
		std::string out;
		for (size_t i = 0; i < parts.size(); ++i)
		{
			if (i) { out += "|"; }
			out += parts[i];
		}
		return out;
	}

	std::vector<std::string> SplitList(const std::string& text)
	{
		std::vector<std::string> parts;
		if (text.empty()) { return parts; }
		size_t start = 0;
		while (true)
		{
			const size_t at = text.find('|', start);
			parts.push_back(text.substr(start, at == std::string::npos ? at : at - start));
			if (at == std::string::npos) { break; }
			start = at + 1;
		}
		return parts;
	}

	std::string ToHex(const std::vector<char>& bytes)
	{
		static const char* digits = "0123456789abcdef";
		std::string out;
		out.reserve(bytes.size() * 2);
		for (char c : bytes)
		{
			const unsigned char b = static_cast<unsigned char>(c);
			out.push_back(digits[b >> 4]);
			out.push_back(digits[b & 0xF]);
		}
		return out;
	}

	std::vector<char> FromHex(const std::string& text)
	{
		std::vector<char> out;
		out.reserve(text.size() / 2);
		const auto nibble = [](char c) -> int {
			if (c >= '0' && c <= '9') { return c - '0'; }
			if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
			if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
			return -1;
		};
		for (size_t i = 0; i + 1 < text.size(); i += 2)
		{
			const int hi = nibble(text[i]);
			const int lo = nibble(text[i + 1]);
			if (hi < 0 || lo < 0) { break; }
			out.push_back(static_cast<char>((hi << 4) | lo));
		}
		return out;
	}

	std::string LeafOf(const std::string& path)
	{
		const size_t at = path.find_last_of("/\\");
		return at == std::string::npos ? path : path.substr(at + 1);
	}

	// Notation is re-parsed rather than stored as bytes, so a change to the parser cannot
	// leave a stale buffer behind.
	void ReparseNotation(DummyActionManager::Action& action)
	{
		if (action.source != DummyActionManager::Source_Notation || action.notation.empty())
		{
			return;
		}
		std::vector<uint16_t> parsed;
		if (!TasManager::TryParseCommand(action.notation, &parsed) || parsed.empty())
		{
			return;
		}
		action.notationFrames.clear();
		action.notationFrames.reserve((parsed.size() + 4) * 2);
		for (uint16_t frame : parsed)
		{
			action.notationFrames.push_back(static_cast<char>(frame & 0xFF));
			action.notationFrames.push_back(0);
		}
		// The same neutral tail the modal appends, so a button on the last frame is not at
		// the mercy of the buffer ending.
		for (int i = 0; i < 4; ++i)
		{
			action.notationFrames.push_back(static_cast<char>(5));
			action.notationFrames.push_back(0);
		}
	}
}

void DummyActionManager::Save() const
{
	if (m_loading)
	{
		// Load() assigns through Add(), which saves. Writing the file back while reading
		// it would be pointless at best.
		return;
	}
	WriteTo(DummyActionsFilePath(), true, nullptr);
}

bool DummyActionManager::ExportTo(const std::string& path, const Extras* extras) const
{
	return WriteTo(path, false, extras);
}

void DummyActionManager::ResetToDefaults()
{
	ResetInMemory();
	UnlimitedPlaybackManager& loop = UnlimitedPlaybackManager::Instance();
	loop.SetLoopSetupSeconds(0.0f);
	loop.SetLoopEndingSeconds(0.0f);
	loop.SetLoopRestartLabState(false);
	loop.SetLoopRestartMode(UnlimitedPlaybackManager::LoopReset_Middle);
	SyncTriggerEnable();
	Save();
}

bool DummyActionManager::WriteTo(const std::string& path, bool includeUiSizes, const Extras* extras) const
{
	FILE* file = nullptr;
	if (fopen_s(&file, path.c_str(), "w") != 0 || !file)
	{
		LOG(2, "[DummyActions] could not write '%s'\n", path.c_str());
		return false;
	}

	fprintf(file, "# BBCF Improvement Mod - training dummy actions.\n");
	fprintf(file, "# Rewritten whenever an action changes. Safe to delete.\n");
	fprintf(file, "# [N] is a trigger, [N.M] is action M of trigger N.\n");

	const UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();
	for (TriggerType trigger : TriggerOrder())
	{
		const Trigger& slot = Get(trigger);
		if (slot.actions.empty()) { continue; }

		const UnlimitedPlaybackManager::TriggerConfig& config = playback.GetTrigger(trigger);
		fprintf(file, "\n[%d]\n", static_cast<int>(trigger));
		fprintf(file, "enabled=%d\n", slot.enabled ? 1 : 0);
		fprintf(file, "selection=%d\n", slot.selectionMode);
		fprintf(file, "delay=%d\n", config.delayFrames);
		fprintf(file, "cooldown=%d\n", config.cooldownFrames);
		fprintf(file, "mirror=%d\n", config.autoMirror ? 1 : 0);

		for (size_t i = 0; i < slot.actions.size(); ++i)
		{
			const Action& action = slot.actions[i];
			fprintf(file, "\n[%d.%d]\n", static_cast<int>(trigger), static_cast<int>(i));
			fprintf(file, "enabled=%d\n", action.enabled ? 1 : 0);
			fprintf(file, "source=%d\n", static_cast<int>(action.source));

			switch (action.source)
			{
			case Source_Notation:
				fprintf(file, "notation=%s\n", action.notation.c_str());
				break;
			case Source_File:
				fprintf(file, "file=%s\n", action.filePath.c_str());
				break;
			case Source_CfSlot:
				fprintf(file, "slot=%d\n", action.cfSlot);
				break;
			case Source_Recording:
				fprintf(file, "slot=%d\n", action.cfSlot);
				fprintf(file, "name=%s\n", action.recordingName.c_str());
				fprintf(file, "facing=%d\n", action.recordingFacingLeft ? 1 : 0);
				fprintf(file, "frames=%s\n", ToHex(action.recordingFrames).c_str());
				break;
			case Source_Animation:
			{
				fprintf(file, "moves=%s\n", JoinList(action.animationNames).c_str());
				std::vector<std::string> delays;
				for (size_t d = 0; d < action.animationNames.size(); ++d)
				{
					delays.push_back(std::to_string(
						d < action.animationDelays.size() ? action.animationDelays[d] : 0));
				}
				fprintf(file, "movedelays=%s\n", JoinList(delays).c_str());
				fprintf(file, "naoto=%d\n", action.naotoEnSpecials ? 1 : 0);
				break;
			}
			default:
				break;
			}
		}
	}

	// The loop's own settings, which are on the manager rather than on a trigger but are
	// every bit as much part of a saved lab.
	{
		const UnlimitedPlaybackManager& loop = UnlimitedPlaybackManager::Instance();
		fprintf(file, "\n[loop]\n");
		fprintf(file, "setup=%.2f\n", loop.GetLoopSetupSeconds());
		fprintf(file, "ending=%.2f\n", loop.GetLoopEndingSeconds());
		fprintf(file, "restartlab=%d\n", loop.GetLoopRestartLabState() ? 1 : 0);
		fprintf(file, "restartmode=%d\n", loop.GetLoopRestartMode());
	}

	if (extras && !extras->empty())
	{
		fprintf(file, "\n[training]\n");
		for (const auto& entry : *extras)
		{
			fprintf(file, "%s=%s\n", entry.first.c_str(), entry.second.c_str());
		}
	}

	// Window sizes last, so the action sections stay first and readable.
	if (includeUiSizes && !m_uiSizes.empty())
	{
		fprintf(file, "\n[sizes]\n");
		for (const auto& entry : m_uiSizes)
		{
			fprintf(file, "%s=%.0fx%.0f\n",
				entry.first.c_str(), entry.second.first, entry.second.second);
		}
	}

	fclose(file);
	return true;
}

void DummyActionManager::ResetInMemory()
{
	UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();
	for (int i = 0; i < UnlimitedPlaybackManager::Trigger_Count; ++i)
	{
		m_triggers[i] = Trigger{};
		UnlimitedPlaybackManager::TriggerConfig& config = playback.GetTrigger(static_cast<TriggerType>(i));
		config.delayFrames = 0;
		config.cooldownFrames = 1;
		config.autoMirror = true;
		config.pendingFireFrame = -1;
	}
	m_nonRepeatPools = {};
	m_sequentialIndex = {};
	m_resolvedForCharData = nullptr;
}

void DummyActionManager::Load()
{
	ReadFrom(DummyActionsFilePath(), true, nullptr);
}

bool DummyActionManager::ImportFrom(const std::string& path, Extras* extrasOut)
{
	if (!ReadFrom(path, false, extrasOut))
	{
		return false;
	}
	// What was imported is now the lab, so it is what the next start restores.
	Save();
	return true;
}

bool DummyActionManager::ReadFrom(const std::string& path, bool includeUiSizes, Extras* extrasOut)
{
	FILE* file = nullptr;
	if (fopen_s(&file, path.c_str(), "r") != 0 || !file)
	{
		return false;   // nothing saved yet
	}

	m_loading = true;
	ResetInMemory();

	UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();

	// The section being read. A trigger section carries the trigger's own settings; an
	// action section carries one action. Old files had no action sections: the one action
	// sat in the trigger section itself, so "source=" starts an action wherever it is seen.
	int currentTrigger = -1;
	bool inActionSection = false;
	Action action;
	bool actionPending = false;

	const auto commit = [&]() {
		if (!actionPending) { return; }
		actionPending = false;
		if (currentTrigger < 0 || currentTrigger >= UnlimitedPlaybackManager::Trigger_Count
			|| !action.IsSet())
		{
			action = Action{};
			return;
		}
		const TriggerType trigger = static_cast<TriggerType>(currentTrigger);

		if (action.source == Source_Library)
		{
			// An old file naming a library: unpack it. Each entry becomes a captured
			// recording on this trigger, on or off as it was in the library, and the
			// library's picking order becomes the trigger's.
			if (AddLibraryFile(trigger, action.libraryPath) < 0)
			{
				LOG(1, "[DummyActions] dropping %s: its library '%s' could not be read\n",
					TriggerLabel(trigger), action.libraryPath.c_str());
			}
			else
			{
				Get(trigger).selectionMode = action.selectionMode;
			}
			action = Action{};
			return;
		}

		ReparseNotation(action);
		if (action.source == Source_File && !action.filePath.empty())
		{
			action.fileName = LeafOf(action.filePath);
		}
		Add(trigger, action);
		action = Action{};
	};

	char line[8192];
	while (fgets(line, sizeof(line), file))
	{
		std::string text(line);
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
		{
			text.pop_back();
		}
		if (text.empty() || text[0] == '#') { continue; }

		if (text == "[sizes]")
		{
			commit();
			currentTrigger = -2;   // the sizes section, not a trigger
			inActionSection = false;
			continue;
		}

		if (text == "[loop]")
		{
			commit();
			currentTrigger = -3;   // the loop settings
			inActionSection = false;
			continue;
		}

		if (text == "[training]")
		{
			commit();
			currentTrigger = -4;   // the caller's extras
			inActionSection = false;
			continue;
		}

		if (text[0] == '[')
		{
			commit();
			const int trigger = atoi(text.c_str() + 1);
			// "[N.M]" is an action of the trigger last declared as "[N]".
			inActionSection = text.find('.') != std::string::npos;
			currentTrigger = trigger;
			continue;
		}

		const size_t eq = text.find('=');
		if (eq == std::string::npos) { continue; }
		const std::string key = text.substr(0, eq);
		const std::string value = text.substr(eq + 1);

		if (currentTrigger == -3)
		{
			if (key == "setup")            { playback.SetLoopSetupSeconds((float)atof(value.c_str())); }
			else if (key == "ending")      { playback.SetLoopEndingSeconds((float)atof(value.c_str())); }
			else if (key == "restartlab")  { playback.SetLoopRestartLabState(atoi(value.c_str()) != 0); }
			else if (key == "restartmode") { playback.SetLoopRestartMode(atoi(value.c_str())); }
			continue;
		}

		if (currentTrigger == -4)
		{
			if (extrasOut) { extrasOut->push_back({ key, value }); }
			continue;
		}

		if (currentTrigger == -2)
		{
			if (!includeUiSizes) { continue; }
			float width = 0.0f;
			float height = 0.0f;
			if (sscanf_s(value.c_str(), "%fx%f", &width, &height) == 2
				&& width > 0.0f && height > 0.0f)
			{
				m_uiSizes[key] = { width, height };
			}
			continue;
		}

		if (currentTrigger < 0 || currentTrigger >= UnlimitedPlaybackManager::Trigger_Count)
		{
			continue;
		}
		const TriggerType trigger = static_cast<TriggerType>(currentTrigger);
		UnlimitedPlaybackManager::TriggerConfig& config = playback.GetTrigger(trigger);

		// Trigger-level keys. "enabled" is the trigger's own switch outside an action
		// section and the action's inside one.
		if (!inActionSection)
		{
			if (key == "delay")          { config.delayFrames = atoi(value.c_str()); continue; }
			if (key == "cooldown")       { config.cooldownFrames = (std::max)(1, atoi(value.c_str())); continue; }
			if (key == "mirror")         { config.autoMirror = atoi(value.c_str()) != 0; continue; }
			if (key == "enabled" && !actionPending) { Get(trigger).enabled = atoi(value.c_str()) != 0; continue; }
			// An old single-action file keeps "selection" with the library action, where
			// it is picked up by the migration above; a new file has it at trigger level.
			if (key == "selection" && !actionPending) { Get(trigger).selectionMode = atoi(value.c_str()); continue; }
		}

		if (key == "source")
		{
			// A second source= in one section starts another action; an enabled= that
			// arrived just before this one belongs to THIS action, so it is not committed
			// away.
			if (actionPending && action.IsSet()) { commit(); }
			action.source = static_cast<Source>(atoi(value.c_str()));
			actionPending = true;
			continue;
		}
		if (!actionPending && key == "enabled")
		{
			// An action section's switch comes before its source= line.
			action.enabled = atoi(value.c_str()) != 0;
			actionPending = true;
			continue;
		}
		if (key == "enabled")   { action.enabled = atoi(value.c_str()) != 0; }
		else if (key == "notation") { action.notation = value; }
		else if (key == "file")     { action.filePath = value; action.fileName = value; }
		else if (key == "library")  { action.libraryPath = value; action.libraryName = value; }
		else if (key == "selection"){ action.selectionMode = atoi(value.c_str()); }
		else if (key == "slot")     { action.cfSlot = atoi(value.c_str()); }
		else if (key == "name")     { action.recordingName = value; }
		else if (key == "facing")   { action.recordingFacingLeft = atoi(value.c_str()) != 0; }
		else if (key == "frames")   { action.recordingFrames = FromHex(value); }
		else if (key == "naoto")    { action.naotoEnSpecials = atoi(value.c_str()) != 0; }
		else if (key == "moves")    { action.animationNames = SplitList(value); }
		else if (key == "movedelays")
		{
			action.animationDelays.clear();
			for (const std::string& one : SplitList(value))
			{
				action.animationDelays.push_back(atoi(one.c_str()));
			}
		}
	}
	commit();
	fclose(file);

	m_loading = false;

	// A file written before Animation was taken off On Hit / On Block can still name one.
	// Drop it here rather than letting it sit as a row nothing will ever fire.
	for (size_t i = 0; i < m_triggers.size(); ++i)
	{
		const TriggerType trigger = static_cast<TriggerType>(i);
		std::vector<Action>& actions = m_triggers[i].actions;
		for (size_t a = 0; a < actions.size();)
		{
			if (SourceAllowedForTrigger(actions[a].source, trigger))
			{
				++a;
				continue;
			}
			LOG(1, "[DummyActions] dropping saved %s action on trigger %s - that trigger no longer accepts it\n",
				SourceLabel(actions[a].source), TriggerLabel(trigger));
			actions.erase(actions.begin() + a);
		}
		if (actions.empty()) { m_triggers[i] = Trigger{}; }
	}

	// Forces the next frame to match these names against whoever is loaded, since the parse
	// they would normally ride along with has usually already happened by now.
	m_resolvedForCharData = nullptr;

	SyncTriggerEnable();
	LOG(1, "[DummyActions] loaded %d trigger(s) from '%s'\n", AssignedCount(), path.c_str());
	// Per action, so a round trip that drops something says which part of it went missing
	// instead of just looking wrong on the page.
	for (TriggerType trigger : TriggerOrder())
	{
		const Trigger& slot = Get(trigger);
		if (slot.actions.empty()) { continue; }
		LOG(1, "[DummyActions]   %s: enabled=%d selection=%d actions=%u runnable=%d\n",
			TriggerLabel(trigger), slot.enabled ? 1 : 0, slot.selectionMode,
			static_cast<unsigned int>(slot.actions.size()), IsRunnable(trigger) ? 1 : 0);
		for (const Action& action : slot.actions)
		{
			LOG(1, "[DummyActions]     source=%d enabled=%d runnable=%d summary='%s'\n",
				static_cast<int>(action.source), action.enabled ? 1 : 0,
				IsActionRunnable(action) ? 1 : 0, ActionSummary(action).c_str());
		}
	}
	return true;
}

void DummyActionManager::SetUiSize(const std::string& key, float width, float height)
{
	if (width <= 0.0f || height <= 0.0f) { return; }
	const auto it = m_uiSizes.find(key);
	if (it != m_uiSizes.end()
		&& it->second.first == width && it->second.second == height)
	{
		return;
	}
	m_uiSizes[key] = { width, height };
	Save();
}

bool DummyActionManager::GetUiSize(const std::string& key, float* width, float* height) const
{
	const auto it = m_uiSizes.find(key);
	if (it == m_uiSizes.end() || !width || !height) { return false; }
	*width = it->second.first;
	*height = it->second.second;
	return true;
}

void DummyActionManager::EnsureLoaded()
{
	if (m_loaded) { return; }
	m_loaded = true;
	Load();
}
