#pragma once

// Catalogues the entities the game puts on the field and whether the hitbox overlay can reach them.
//
// The overlay only draws an entity whose owner chain leads back to P1 or P2, and a box that was
// never drawn looks identical to a box that does not exist - which is exactly the kind of thing a
// screenshot of the overlay cannot settle.
//
// The first session this produced answered the open question it was built for: ownerEntity holds
// the root player rather than the immediate parent, so every entity sampled sat at depth 0,
// Litchi's staff included. Keeping the depth column is still worth it - it is the cheapest way to
// notice the day some character does not follow that rule.
//
// So every distinct (character, action, owner depth, box counts, verdict) combination seen during a
// session is written to DEBUG.txt once. Repeats are suppressed, so holding a Litchi staff on screen
// for a minute costs a handful of lines, not thousands. The output is a catalogue of what the
// overlay saw and what it did about it, which is enough to tell "the fix covers this" from "there
// is still something the fix does not reach" without anyone having to catch it live.
namespace EntityDiagnostics
{
	// Samples the entity list. Safe to call every rendered frame; it is a no-op outside a match.
	void Update();
}
