#pragma once

#include "Game/Scr/ScrStateEntry.h"

#include <string>

// Turns a raw script state name into something a player recognises.
//
// The names the script carries are internal: NmlAtk5A, NmlAtkAIR2C, CmnActUkemiLandF. The
// dummy-action move list showed those verbatim, which made picking a move out of six hundred
// entries guesswork. The prefixes are mechanical, so most of them decode exactly:
//
//     NmlAtk5C          -> 5C
//     NmlAtkAIR2D       -> j.2D
//     NmlAtk5D_2nd      -> 5D (2nd)
//     NmlAtkBackThrow   -> Back Throw
//     NmlAtkExcite      -> Exceed Accel
//     CmnActUkemiLandF  -> Tech (forward)
//     CmnActBurstBegin  -> Burst
//
// Verified against the NmlAtk* name space of every character's script (2026-09-08): the
// direction+button and AIR forms cover all but a handful of entries, and the rest are
// spelled-out words that only need the CamelCase splitting up.
//
// A character's own specials have arbitrary internal names, so those get the CamelCase
// treatment and nothing more. The raw name is always kept for a tooltip: anybody labbing
// off this needs to be able to see what the script actually calls the state.
namespace ScrStateNames
{
	enum class Category
	{
		Normal,   // NmlAtk* - the direction+button moves, throws, Exceed Accel
		Common,   // CmnAct* - wakeups, techs, bursts, Overdrive, guard states
		Special,  // everything else, i.e. the character's own named moves
	};

	Category Categorize(const std::string& rawName);

	// Display form. Falls back to the raw name rather than to something empty.
	std::string Display(const std::string& rawName);

	// Short label for a category, for the filter tabs.
	const char* CategoryLabel(Category category);

	// Case-insensitive substring test used by the filter box, matching against both the
	// display name and the raw one - "ukemi" should find a tech even though the display
	// name does not contain it.
	bool Matches(const std::string& rawName, const std::string& needle);
}

// Names an invulnerability/guard-point combination for a tooltip. Lives here rather than in
// a window because more than one of them shows a frame breakdown.
std::string interpret_frame_invuln_enum(FrameInvuln value);
