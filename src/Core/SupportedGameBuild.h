#pragma once

// The one BBCF.exe this version of the mod is written against. Every hardcoded game address in
// src/ belongs to it.
//
// When porting to a new game build, change all three lines together. tools/package_release.ps1
// reads BBCF_SUPPORTED_GAME_BUILD_ID and BBCF_SUPPORTED_GAME_STEAM_BUILD into the release's
// update-manifest.json, which is how an installed mod that finds itself on that new build knows
// this release can be installed over it (GameBuildGuard.h, UnsupportedBuildFlow.h).
//
// The ID is the exe's PE header: TimeDateStamp-SizeOfImage-AddressOfEntryPoint, in hex.

#define BBCF_SUPPORTED_GAME_BUILD_ID "6442636E-0160C000-003A557A"
#define BBCF_SUPPORTED_GAME_STEAM_BUILD "11073075"
#define BBCF_SUPPORTED_GAME_EXE_SHA256 "9a94ac4003cc8dc77226cadc7496a0eb112f69ccc92e50ded27262ed80551f25"
