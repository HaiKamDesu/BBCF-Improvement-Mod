#pragma once

// The connection icons (setting ConnectionIcons): during online matches, for each player a PC
// icon and a connection icon (Wi-Fi, ethernet cable, or unknown link), coloured from the same
// ratings the PC & connection stats window uses; a grey question mark for a player whose
// numbers cannot exist (no mod, or a build too old to share them). No background, so they sit
// on the game's HUD; each is its own small window, dragged anywhere and remembered in
// menus.ini. Dragging the corner of any one resizes all four (one shared size).
//
// Shown during online matches (as a player or a spectator, including the stats window's
// offline fake-players test) unless the game HUD is hidden, and whenever the mod menu is open,
// so they can be arranged outside a match.

// Once per frame from WindowManager::Render, after the regular windows.
void DrawConnectionIconsStandalone(bool modMenuOpen);

// The on/off and lock checkboxes, drawn in the PC & connection stats window.
void DrawConnectionIconsControls();
