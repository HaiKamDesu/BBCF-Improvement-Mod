#pragma once

// Test tool for running BBCF large-address-aware (4 GB of address space under WOW64).
//
// The flag alone proves little: a fresh process keeps allocating low for a long time, so code
// that treats a pointer as a signed int keeps working until the one session that crosses 2 GB.
// This makes every allocation cross it. When BBCF_IM_LAA_STRESS=1 is set and the exe carries
// the large-address-aware bit, every free range below 0x80000000 is reserved before the game's
// WinMain runs, and again every few seconds, so the game's heaps, textures, file buffers and
// thread stacks all land above 2 GB from then on.
//
// BBCF_IM_LAA_STRESS=3 instead boots normally, with HighMemoryReserve holding the upper half,
// and plugs the low half once 25 s in: a session running out, to test that the release happens
// and the game carries on above 2 GB. BBCF_IM_LAA_STRESS=4 is the same with the low-water watcher
// off, so only a failed allocation can trigger the release.
//
// Off unless the environment variable is set. Never ship a build that sets it.

namespace LaaStress
{
	// From DllMain: does nothing unless enabled and the exe is large-address-aware.
	void OnProcessAttach();

	// Once the log is open: writes what OnProcessAttach did.
	void LogSummary();

	// Modes 1 and 2: everything is meant to land above 2 GB from boot.
	bool PlugsLowHalfAtBoot();

	// Mode 4: HighMemoryReserve's watcher stays off.
	bool DisablesLowWaterWatcher();

	// From the render loop: plugs low ranges the game has freed since the last sweep (mode 1),
	// or the whole low half once (mode 3).
	void Tick();
}
