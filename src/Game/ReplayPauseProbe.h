#pragma once

// Finds what a replay pause actually changes, by diffing memory instead of guessing.
//
// Six rounds of reasoning from the disassembly ruled out five candidate mechanisms and produced no
// positive lead, so this stops guessing which field to inspect and asks memory directly: take a
// snapshot while the replay runs, another a second later to learn which bytes churn on their own,
// and a third while it is paused. Report only the offsets that changed AND are not normal churn.
//
// The churn snapshot is what makes the result readable - a live replay rewrites a great deal of
// this region every frame, and without subtracting that the diff is thousands of offsets of noise.
namespace ReplayPauseProbe
{
	// Call once per rendered frame. Does nothing outside Replay Theater, and reports once per
	// session unless re-armed by leaving and re-entering.
	void Update(bool inReplayTheater, bool paused);

	// True while a replay is paused, read straight from the state the differ found. More direct
	// than inferring it from the world frame counter standing still, which hitstop also does.
	bool IsReplayPaused();
}
