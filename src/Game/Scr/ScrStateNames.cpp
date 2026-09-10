#include "ScrStateNames.h"

#include <algorithm>
#include <cctype>

namespace
{
	bool StartsWith(const std::string& s, const char* prefix)
	{
		const size_t n = std::string(prefix).size();
		return s.size() >= n && s.compare(0, n, prefix) == 0;
	}

	bool EndsWith(const std::string& s, const char* suffix)
	{
		const std::string suf(suffix);
		return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
	}

	std::string Lower(const std::string& s)
	{
		std::string out = s;
		std::transform(out.begin(), out.end(), out.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	// "GuardCrush" -> "Guard Crush", "AomukeSlideEnd" -> "Aomuke Slide End". Runs of capitals
	// are left alone so "OverDriveEnd" does not come out as "Over Drive End" letter by letter.
	std::string SplitCamelCase(const std::string& s)
	{
		std::string out;
		for (size_t i = 0; i < s.size(); ++i)
		{
			const char c = s[i];
			if (c == '_')
			{
				out += ' ';
				continue;
			}
			const bool boundary = i > 0
				&& std::isupper(static_cast<unsigned char>(c))
				&& (std::islower(static_cast<unsigned char>(s[i - 1]))
					|| std::isdigit(static_cast<unsigned char>(s[i - 1])));
			if (boundary)
			{
				out += ' ';
			}
			out += c;
		}
		return out;
	}

	bool IsButton(char c)
	{
		return c == 'A' || c == 'B' || c == 'C' || c == 'D';
	}

	bool IsNumpadDirection(char c)
	{
		return c >= '1' && c <= '9';
	}

	// Pulls the trailing "_2nd" / "_2" style qualifier off, so the direction+button test below
	// only has to deal with the move itself.
	std::string TakeQualifier(std::string& body)
	{
		if (EndsWith(body, "_2nd")) { body.resize(body.size() - 4); return " (2nd)"; }
		if (EndsWith(body, "_3rd")) { body.resize(body.size() - 4); return " (3rd)"; }
		if (EndsWith(body, "_2"))   { body.resize(body.size() - 2); return " (2)"; }
		if (EndsWith(body, "_3"))   { body.resize(body.size() - 2); return " (3)"; }
		if (EndsWith(body, "Stop")) { body.resize(body.size() - 4); return " (stop)"; }
		return "";
	}

	// "5C" -> "5C", "2D" -> "2D". Returns empty when the body is not a plain
	// direction+button pair, which is the signal to fall through to word splitting.
	std::string DirectionButton(const std::string& body)
	{
		if (body.size() == 2 && IsNumpadDirection(body[0]) && IsButton(body[1]))
		{
			return body;
		}
		// Doubled-up normals like "5AA" chain the same button.
		if (body.size() == 3 && IsNumpadDirection(body[0]) && IsButton(body[1]) && IsButton(body[2]))
		{
			return body;
		}
		return "";
	}

	// Air normals: AIR5C is j.C, but AIR2C keeps its direction as j.2C. The neutral 5 is
	// dropped because nobody writes j.5C.
	std::string AirNormal(const std::string& body)
	{
		if (!StartsWith(body, "AIR"))
		{
			return "";
		}
		const std::string rest = body.substr(3);
		const std::string plain = DirectionButton(rest);
		if (plain.empty())
		{
			return "";
		}
		if (plain[0] == '5')
		{
			return "j." + plain.substr(1);
		}
		return "j." + plain;
	}

	struct NamedState
	{
		const char* raw;
		const char* display;
	};

	// The NmlAtk entries that are words rather than inputs, and the CmnAct states worth
	// naming properly because they are the ones people actually assign as dummy actions.
	const NamedState kNormalWords[] = {
		{ "Throw",      "Throw" },
		{ "BackThrow",  "Back Throw" },
		{ "AirThrow",   "Air Throw" },
		{ "DeadAngle",  "Dead Angle" },
		{ "GuardCrush", "Guard Crush" },
		{ "Excite",     "Exceed Accel" },
	};

	const NamedState kCommonNames[] = {
		{ "CmnActUkemiLandN",        "Tech (neutral)" },
		{ "CmnActUkemiLandNLanding", "Tech (neutral, landing)" },
		{ "CmnActUkemiLandF",        "Tech (forward)" },
		{ "CmnActUkemiLandB",        "Tech (back)" },
		{ "CmnActUkemiStagger",      "Stagger recover" },
		{ "CmnActFDown2Stand",       "Wakeup (face down)" },
		{ "CmnActBDown2Stand",       "Wakeup (face up)" },
		{ "CmnActBurstBegin",        "Burst" },
		{ "CmnActAirBurstBegin",     "Burst (air)" },
		{ "CmnActOverDriveBegin",    "Overdrive" },
		{ "CmnActAirOverDriveBegin", "Overdrive (air)" },
		{ "CmnActFDash",             "Forward dash" },
		{ "CmnActBDash",             "Back dash" },
		{ "CmnActAirFDash",          "Air dash" },
		{ "CmnActAirBDash",          "Air back dash" },
		{ "CmnActLockReject",        "Throw escape" },
		{ "CmnActAirLockReject",     "Throw escape (air)" },
	};
}

ScrStateNames::Category ScrStateNames::Categorize(const std::string& rawName)
{
	if (StartsWith(rawName, "NmlAtk"))
	{
		return Category::Normal;
	}
	if (StartsWith(rawName, "CmnAct"))
	{
		return Category::Common;
	}
	return Category::Special;
}

const char* ScrStateNames::CategoryLabel(Category category)
{
	switch (category)
	{
	case Category::Normal:  return "Normals";
	case Category::Common:  return "Common";
	case Category::Special: return "Specials";
	}
	return "";
}

std::string ScrStateNames::Display(const std::string& rawName)
{
	if (rawName.empty())
	{
		return rawName;
	}

	if (StartsWith(rawName, "NmlAtk"))
	{
		std::string body = rawName.substr(6);
		const std::string qualifier = TakeQualifier(body);

		for (const NamedState& named : kNormalWords)
		{
			if (body == named.raw)
			{
				return named.display + qualifier;
			}
		}

		const std::string air = AirNormal(body);
		if (!air.empty())
		{
			return air + qualifier;
		}
		const std::string plain = DirectionButton(body);
		if (!plain.empty())
		{
			return plain + qualifier;
		}
		return SplitCamelCase(body) + qualifier;
	}

	if (StartsWith(rawName, "CmnAct"))
	{
		for (const NamedState& named : kCommonNames)
		{
			if (rawName == named.raw)
			{
				return named.display;
			}
		}
		return SplitCamelCase(rawName.substr(6));
	}

	return SplitCamelCase(rawName);
}

bool ScrStateNames::Matches(const std::string& rawName, const std::string& needle)
{
	if (needle.empty())
	{
		return true;
	}
	const std::string want = Lower(needle);
	return Lower(Display(rawName)).find(want) != std::string::npos
		|| Lower(rawName).find(want) != std::string::npos;
}

std::string interpret_frame_invuln_enum(FrameInvuln value) {
    switch (value) {
    case FrameInvuln::None:
        return "None";
    case FrameInvuln::Head:
        return "Head";
    case FrameInvuln::Body:
        return "Body";
    case FrameInvuln::Foot:
        return "Foot";
    case FrameInvuln::Throw:
        return "Throw";
    case FrameInvuln::HeadBody:
        return "HeadBody";
    case FrameInvuln::HeadFoot:
        return "HeadFoot";
    case FrameInvuln::HeadThrow:
        return "HeadThrow";
    case FrameInvuln::BodyFoot:
        return "BodyFoot";
    case FrameInvuln::BodyThrow:
        return "BodyThrow";
    case FrameInvuln::FootThrow:
        return "FootThrow";
    case FrameInvuln::HeadBodyFoot:
        return "HeadBodyFoot";
    case FrameInvuln::HeadBodyThrow:
        return "HeadBodyThrow";
    case FrameInvuln::HeadFootThrow:
        return "HeadFootThrow";
    case FrameInvuln::BodyFootThrow:
        return "BodyFootThrow";
    case FrameInvuln::All:
        return "All";
    default:
        return "Unknown";
    }
}
