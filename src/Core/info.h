#pragma once
#define MOD_VERSION	"v8.4"
#define MOD_VERSION_NUM	MOD_VERSION " Oceanya Edition"
#define MOD_WINDOW_TITLE "BBCF IM"

// Minimum installed version that can autoupdate directly to this release.
// Bump this when the zip format changes in a way that older IsAllowedEntryPath
// validators would reject (e.g. new file types at the zip root). Users below
// this version are automatically offered the nearest compatible intermediate
// release instead, then chain-update on the next session.
// Leave at "v3.110" when the zip format is unchanged.
#define MOD_MINIMUM_FROM_VERSION "v7.2"

#define MOD_FORCE_DISABLE_UPDATE_CHECK 0

// Links
#define MOD_LINK_DISCORD L"https://discord.gg/j2mCX9s"
#define MOD_LINK_FORUM L"https://steamcommunity.com/app/586140/discussions/0/3195868146163014015/"
#define MOD_LINK_GITHUB L"https://github.com/HaiKamDesu/BBCF-Improvement-Mod/releases"
#define MOD_LINK_API_GITHUB_RELEASE L"https://api.github.com/repos/HaiKamDesu/BBCF-Improvement-Mod/releases/latest"
#define REPLAY_DB_FRONTEND L"http://89.167.76.6:2000/"
 