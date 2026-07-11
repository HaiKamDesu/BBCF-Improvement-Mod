#pragma once

// Diagnostic instrumentation for the D-Code display freeze / ranked progress
// rollback bug. See docs/Research/DCodeNetworkStallBug.md for the RE writeup
// this is built from. Gated behind Settings::settingsIni.enableInDevelopmentFeatures;
// does not read or write any game state, only observes it.
namespace NetworkStallDiagnostics
{
	void OnUpdate();
}
