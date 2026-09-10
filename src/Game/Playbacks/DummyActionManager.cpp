#include "DummyActionManager.h"

#include "Core/Localization.h"
#include "Core/utils.h"
#include "Game/Scr/ScrStateNames.h"
#include "Game/TasManager.h"
#include "Core/logger.h"

#include <cstdio>
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
	case UnlimitedPlaybackManager::Trigger_OnHit:     return "On Hit";
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
		return "when the dummy is hit - a burst goes out at once, inputs once hitstun ends";
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
	default: break;
	}
	return "";
}

DummyActionManager::Action& DummyActionManager::Get(TriggerType trigger)
{
	return m_actions[static_cast<size_t>(trigger)];
}

const DummyActionManager::Action& DummyActionManager::Get(TriggerType trigger) const
{
	return m_actions[static_cast<size_t>(trigger)];
}

bool DummyActionManager::HasAction(TriggerType trigger) const
{
	return Get(trigger).IsSet();
}

int DummyActionManager::AssignedCount() const
{
	int count = 0;
	for (const Action& action : m_actions)
	{
		if (action.IsSet()) { ++count; }
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

void DummyActionManager::Set(TriggerType trigger, const Action& action)
{
	Action& stored = Get(trigger) = action;

	// Names taken from the pointers at the moment of commit, which is the only moment they
	// are known to be good: they were just picked out of the current character's state list.
	//
	// Only when there ARE pointers. An action arriving from disk has names and no pointers
	// yet - deriving from an empty list wiped exactly the names that had just been read,
	// which is why a saved animation action came back as "no move picked" and would have
	// been written back out empty on the next save.
	if (stored.source == Source_Animation && !stored.animations.empty())
	{
		stored.animationNames.clear();
		stored.animationNames.reserve(stored.animations.size());
		for (const scrState* state : stored.animations)
		{
			stored.animationNames.push_back(state ? state->name : std::string());
		}
	}

	SyncTriggerEnable();
	Save();
}

void DummyActionManager::Clear(TriggerType trigger)
{
	Get(trigger) = Action{};
	// The library this trigger was drawing from goes with it. Without this, deleting a row
	// and adding it back handed you the previous one's fully loaded library, which reads as
	// the delete not having worked.
	UnlimitedPlaybackManager::Instance().ResetTriggerLibrary(trigger);
	SyncTriggerEnable();
	Save();
}

void DummyActionManager::ClearAll()
{
	for (Action& action : m_actions)
	{
		action = Action{};
	}
	for (TriggerType trigger : TriggerOrder())
	{
		UnlimitedPlaybackManager::Instance().ResetTriggerLibrary(trigger);
	}
	SyncTriggerEnable();
	Save();
}

bool DummyActionManager::IsRunnable(TriggerType trigger) const
{
	const Action& action = Get(trigger);
	switch (action.source)
	{
	case Source_Notation:  return !action.notationFrames.empty();
	case Source_File:      return !action.filePath.empty();
	case Source_Animation: return !action.animations.empty();
	case Source_CfSlot:    return action.cfSlot >= 1 && action.cfSlot <= 4;
	case Source_Burst:     return true;   // nothing to configure
	case Source_Library:
		// Either a file has been picked, or entries were built up in the editor without
		// being saved to one yet - both are a library this trigger can draw from.
		return !action.libraryPath.empty()
			|| UnlimitedPlaybackManager::Instance().TriggerLibraryHasEntries(trigger);
	default:
		break;
	}
	return false;
}

std::string DummyActionManager::Summary(TriggerType trigger) const
{
	const Action& action = Get(trigger);
	switch (action.source)
	{
	case Source_Notation:
		return action.notation.empty() ? L("not typed yet") : action.notation;

	case Source_File:
		return action.fileName.empty() ? L("no file picked") : action.fileName;

	case Source_CfSlot:
		return FormatText(L("slot %d").c_str(), action.cfSlot);

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
	{
		if (action.libraryPath.empty())
		{
			return L("no library picked");
		}
		const char* mode = "";
		switch (action.selectionMode)
		{
		case UnlimitedPlaybackManager::Selection_Sequential:          mode = "sequential"; break;
		case UnlimitedPlaybackManager::Selection_NonRepeatingRandom:  mode = "no repeats"; break;
		default:                                                      mode = "random"; break;
		}
		return action.libraryName + "  (" + L(mode) + ")";
	}

	default:
		break;
	}
	return "";
}

void DummyActionManager::SyncTriggerEnable()
{
	// TriggerConfig::enabled is the gate at the top of UnlimitedPlaybackManager's
	// TryFireTrigger, so it means "this trigger is armed" - for any source, not just the
	// library. An earlier cut of this set it to (source == Source_Library), which silently
	// disabled every notation, file and slot action ever assigned: the row looked armed and
	// nothing fired. Which source a trigger uses is settled later, by ResolveTriggerAction.
	UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();
	for (int i = 0; i < UnlimitedPlaybackManager::Trigger_Count; ++i)
	{
		const TriggerType trigger = static_cast<TriggerType>(i);
		playback.GetTrigger(trigger).enabled = IsRunnable(trigger);
	}
}

void DummyActionManager::InvalidateAnimations()
{
	for (Action& action : m_actions)
	{
		if (action.source != Source_Animation) { continue; }
		// Pointers only. The names stay, so the same moves can be found again for whoever
		// the dummy is now - and so a saved action survives a character swap.
		action.animations.clear();
	}
}

bool DummyActionManager::NeedsAnimationResolve(const void* ownerCharData) const
{
	if (m_resolvedForCharData == ownerCharData)
	{
		return false;
	}
	for (const Action& action : m_actions)
	{
		if (action.source == Source_Animation && !action.animationNames.empty())
		{
			return true;
		}
	}
	return false;
}

void DummyActionManager::ResolveAnimations(const std::vector<scrState*>& states,
	const void* ownerCharData)
{
	m_resolvedForCharData = ownerCharData;

	for (Action& action : m_actions)
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
				TriggerLabel(static_cast<TriggerType>(&action - m_actions.data())),
				static_cast<unsigned int>(resolved.size()),
				static_cast<unsigned int>(action.animationNames.size()));
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
}

void DummyActionManager::Save() const
{
	if (m_loading)
	{
		// Load() assigns through Set(), which saves. Writing the file back while reading it
		// would be pointless at best.
		return;
	}

	FILE* file = nullptr;
	if (fopen_s(&file, DummyActionsFilePath().c_str(), "w") != 0 || !file)
	{
		LOG(2, "[DummyActions] could not write '%s'\n", DummyActionsFilePath().c_str());
		return;
	}

	fprintf(file, "# BBCF Improvement Mod - training dummy actions.\n");
	fprintf(file, "# Rewritten whenever an action changes. Safe to delete.\n");

	const UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();
	for (TriggerType trigger : TriggerOrder())
	{
		const Action& action = Get(trigger);
		if (!action.IsSet()) { continue; }

		const UnlimitedPlaybackManager::TriggerConfig& config = playback.GetTrigger(trigger);
		fprintf(file, "\n[%d]\n", static_cast<int>(trigger));
		fprintf(file, "source=%d\n", static_cast<int>(action.source));
		fprintf(file, "delay=%d\n", config.delayFrames);
		fprintf(file, "cooldown=%d\n", config.cooldownFrames);
		fprintf(file, "mirror=%d\n", config.autoMirror ? 1 : 0);

		switch (action.source)
		{
		case Source_Notation:
			// Only the text: the frames are re-parsed on load, so a change to the parser
			// cannot leave a stale buffer behind.
			fprintf(file, "notation=%s\n", action.notation.c_str());
			break;
		case Source_File:
			fprintf(file, "file=%s\n", action.filePath.c_str());
			break;
		case Source_Library:
			fprintf(file, "library=%s\n", action.libraryPath.c_str());
			fprintf(file, "selection=%d\n", action.selectionMode);
			break;
		case Source_CfSlot:
			fprintf(file, "slot=%d\n", action.cfSlot);
			break;
		case Source_Animation:
		{
			fprintf(file, "moves=%s\n", JoinList(action.animationNames).c_str());
			std::vector<std::string> delays;
			for (size_t i = 0; i < action.animationNames.size(); ++i)
			{
				delays.push_back(std::to_string(
					i < action.animationDelays.size() ? action.animationDelays[i] : 0));
			}
			fprintf(file, "movedelays=%s\n", JoinList(delays).c_str());
			fprintf(file, "naoto=%d\n", action.naotoEnSpecials ? 1 : 0);
			break;
		}
		default:
			break;
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

	// Window sizes last, so the action sections stay first and readable.
	if (!m_uiSizes.empty())
	{
		fprintf(file, "\n[sizes]\n");
		for (const auto& entry : m_uiSizes)
		{
			fprintf(file, "%s=%.0fx%.0f\n",
				entry.first.c_str(), entry.second.first, entry.second.second);
		}
	}

	fclose(file);
}

void DummyActionManager::Load()
{
	FILE* file = nullptr;
	if (fopen_s(&file, DummyActionsFilePath().c_str(), "r") != 0 || !file)
	{
		return;   // nothing saved yet
	}

	m_loading = true;

	UnlimitedPlaybackManager& playback = UnlimitedPlaybackManager::Instance();
	int currentTrigger = -1;
	Action action;
	int delayFrames = 0;
	int cooldownFrames = 1;
	bool autoMirror = true;

	const auto commit = [&]() {
		if (currentTrigger < 0 || currentTrigger >= UnlimitedPlaybackManager::Trigger_Count
			|| !action.IsSet())
		{
			return;
		}
		const TriggerType trigger = static_cast<TriggerType>(currentTrigger);

		// Notation is re-parsed rather than stored as bytes.
		if (action.source == Source_Notation && !action.notation.empty())
		{
			std::vector<uint16_t> parsed;
			if (TasManager::TryParseCommand(action.notation, &parsed) && !parsed.empty())
			{
				action.notationFrames.clear();
				action.notationFrames.reserve((parsed.size() + 4) * 2);
				for (uint16_t frame : parsed)
				{
					action.notationFrames.push_back(static_cast<char>(frame & 0xFF));
					action.notationFrames.push_back(0);
				}
				// The same neutral tail the modal appends, so a button on the last frame is
				// not at the mercy of the buffer ending.
				for (int i = 0; i < 4; ++i)
				{
					action.notationFrames.push_back(static_cast<char>(5));
					action.notationFrames.push_back(0);
				}
			}
		}

		Set(trigger, action);
		UnlimitedPlaybackManager::TriggerConfig& config = playback.GetTrigger(trigger);
		config.delayFrames = delayFrames;
		config.cooldownFrames = cooldownFrames < 1 ? 1 : cooldownFrames;
		config.autoMirror = autoMirror;
	};

	char line[2048];
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
			continue;
		}

		if (text == "[loop]")
		{
			commit();
			currentTrigger = -3;   // the loop settings
			continue;
		}

		if (text[0] == '[')
		{
			commit();
			action = Action{};
			delayFrames = 0;
			cooldownFrames = 1;
			autoMirror = true;
			currentTrigger = atoi(text.c_str() + 1);
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

		if (currentTrigger == -2)
		{
			float width = 0.0f;
			float height = 0.0f;
			if (sscanf_s(value.c_str(), "%fx%f", &width, &height) == 2
				&& width > 0.0f && height > 0.0f)
			{
				m_uiSizes[key] = { width, height };
			}
			continue;
		}

		if (key == "source")        { action.source = static_cast<Source>(atoi(value.c_str())); }
		else if (key == "delay")    { delayFrames = atoi(value.c_str()); }
		else if (key == "cooldown") { cooldownFrames = atoi(value.c_str()); }
		else if (key == "mirror")   { autoMirror = atoi(value.c_str()) != 0; }
		else if (key == "notation") { action.notation = value; }
		else if (key == "file")     { action.filePath = value; action.fileName = value; }
		else if (key == "library")  { action.libraryPath = value; action.libraryName = value; }
		else if (key == "selection"){ action.selectionMode = atoi(value.c_str()); }
		else if (key == "slot")     { action.cfSlot = atoi(value.c_str()); }
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

	// Names are all that was saved for a file or library, so give the row something to show
	// rather than a full path.
	for (Action& stored : m_actions)
	{
		const auto leafOf = [](const std::string& path) {
			const size_t at = path.find_last_of("/\\");
			return at == std::string::npos ? path : path.substr(at + 1);
		};
		if (stored.source == Source_File && !stored.filePath.empty())
		{
			stored.fileName = leafOf(stored.filePath);
		}
		if (stored.source == Source_Library && !stored.libraryPath.empty())
		{
			std::string name = leafOf(stored.libraryPath);
			const size_t dot = name.find_last_of('.');
			stored.libraryName = dot == std::string::npos ? name : name.substr(0, dot);
		}
	}

	// Forces the next frame to match these names against whoever is loaded, since the parse
	// they would normally ride along with has usually already happened by now.
	m_resolvedForCharData = nullptr;

	SyncTriggerEnable();
	LOG(1, "[DummyActions] loaded %d action(s) from '%s'\n",
		AssignedCount(), DummyActionsFilePath().c_str());
	// Per action, so a round trip that drops something says which part of it went missing
	// instead of just looking wrong on the page.
	for (TriggerType trigger : TriggerOrder())
	{
		const Action& stored = Get(trigger);
		if (!stored.IsSet()) { continue; }
		LOG(1, "[DummyActions]   %s: source=%d runnable=%d summary='%s'\n",
			TriggerLabel(trigger), static_cast<int>(stored.source),
			IsRunnable(trigger) ? 1 : 0, Summary(trigger).c_str());
	}
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
