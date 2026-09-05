#pragma once

#include "Updater/UpdateCoordinator.h"

// The status line / progress bar / error message block that reports what an update or a
// release install is doing.
//
// It exists in one place because it is drawn in two: the "update available" prompt
// (UpdateNotifierWindow) and the all-releases browser (ReleaseCheckerWindow), which installs
// in place rather than handing the job over to the prompt. The two had grown separate copies
// that disagreed on details -- one wrapped the error text and the other did not, one showed
// the status line only while busy -- so a message that read fine in one window was clipped in
// the other.
//
// Both windows reserve room for this block at the bottom of a scrolling child, so the height
// has to be known before the block is drawn: EstimateHeight answers the same question Draw
// will answer, from the same snapshot.
namespace UpdateProgressWidget
{
	// True while an install is actually running, i.e. the states that show a progress bar.
	bool IsBusy(const Updater::UpdateUiSnapshot& snapshot);

	// Height Draw() will occupy, for the caller's bottom reserve. wrapWidth must be the
	// width the block will actually be drawn at, since the text wraps.
	float EstimateHeight(const Updater::UpdateUiSnapshot& snapshot, float wrapWidth,
		bool includeAutoApplyReason);

	// includeAutoApplyReason adds the "why this can't auto-update" line. The prompt shows it
	// because it is explaining a missing Update button; the releases browser already says the
	// same thing per release, so it leaves it out.
	void Draw(const Updater::UpdateUiSnapshot& snapshot, bool includeAutoApplyReason);
}
