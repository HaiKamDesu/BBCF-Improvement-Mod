#pragma once

// ============================================================================
//  BRANDING LAYOUT - THIS IS THE FILE TO EDIT
// ============================================================================
//
// Every number that positions or sizes the mod menu's branding lives in one
// struct, with its defaults in BrandingLayout.cpp. Nothing else in the codebase
// hardcodes a branding measurement.
//
// Two ways to change it:
//
//   1. Live, in game. Open the DEBUG window (footer, needs a debug build or
//      EnableInDevelopmentFeatures=1 in settings.ini) and use the "Branding
//      layout" section. Every field below is a slider that updates the menu as
//      you drag it. When it looks right, press "Copy defaults to clipboard" and
//      paste the result over kDefaults in BrandingLayout.cpp.
//
//   2. By hand. Edit kDefaults in BrandingLayout.cpp and rebuild.
//
// ============================================================================

#include "imgui.h"

namespace Branding
{
	struct Layout
	{
		// --- Title bar -------------------------------------------------------
		// Extra pixels of vertical padding on the title bar, on top of the
		// theme's own. The bar is the ceiling on how tall the wordmarks can be,
		// so raise this first if they still feel small once their own height
		// fractions are near 1.0.
		float titleBarExtraPadding;

		// Nudge for the "BBCF IM v8.4" text, in pixels from where it would
		// otherwise sit (left edge, vertically centred in the bar).
		float titleTextOffsetX;
		float titleTextOffsetY;

		// Wordmark heights, as a fraction of the title bar's height. 1.0 means
		// the artwork exactly fills the bar top to bottom. These act on the
		// artwork's INKED area - the transparent margin in the source PNGs is
		// already cropped out - so 1.0 really does mean full height.
		float oceanyaHeight;
		float laboratoriesHeight;

		// Per-wordmark vertical nudge in pixels, for when the two do not sit on
		// a line that looks right together.
		float oceanyaOffsetY;
		float laboratoriesOffsetY;

		// Horizontal gaps, in pixels, between the four pieces of the title.
		float gapAfterVersion;
		float gapAfterOceanya;
		float gapAfterLaboratories;

		// The word after the wordmarks. Turn off if the lockup reads better
		// without it.
		bool showEditionSuffix;

		// --- Background watermark --------------------------------------------
		// Opacity of the big "O", 0 (invisible) to 1 (solid).
		float watermarkOpacity;

		// 1.0 fits the mark inside the window; below 1.0 shrinks it, above 1.0
		// lets it overflow and be clipped by the window edges.
		float watermarkScale;

		// Pixel nudge from the centre of the window.
		float watermarkOffsetX;
		float watermarkOffsetY;

		// Tint. White keeps the artwork as-authored; the source art is white, so
		// this colour comes through as-is and can be set to anything.
		ImVec4 watermarkTint;
	};

	// Live values. Mutable on purpose: the tuner writes straight into this.
	Layout& GetLayout();

	// The tuner UI, drawn by the DEBUG window.
	void DrawLayoutTuner();
}
