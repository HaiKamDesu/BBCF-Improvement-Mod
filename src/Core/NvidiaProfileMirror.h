#pragma once

// Puts BBCF_IM_4GB.exe into the same NVIDIA driver profile as BBCF.exe (4 GB mode, LaaRedirect.h).
//
// The driver matches profiles by exe name. NVIDIA's own profile for the game ("BlazBlue
// Centralfiction") only turns on the dedicated GPU for Optimus laptops, which BBCF.exe's
// NvOptimusEnablement export - also in the copy - does anyway; but whatever a player set for the
// game in the NVIDIA Control Panel (frame cap, low latency, vsync, ...) lives in that profile too
// and would not reach a differently named exe. Adding the copy to the profile carries all of it.
//
// Through NVAPI's driver settings calls, which a normal user may make (the settings database is
// writable by everyone). Does nothing without an NVIDIA driver, or when the copy already belongs
// to a profile (a player's own choice). AMD and Intel offer no equivalent.

namespace NvidiaProfileMirror
{
	// Not from DllMain (loads nvapi.dll). Returns a one-line description of what it did, for the log.
	const char* Sync();
}
