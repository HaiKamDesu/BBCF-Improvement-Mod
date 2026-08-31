#pragma once

// The mod menu's artwork: the two wordmarks drawn in its title bar and the "O" watermark
// behind its content.
//
// The PNGs are embedded in the DLL as RCDATA (resource/resource.rc) rather than shipped
// loose, so there is nothing extra to install and nothing to go missing. They are decoded
// and uploaded to D3D9 on demand, and re-uploaded after a device reset - the textures live
// in D3DPOOL_DEFAULT because D3DPOOL_MANAGED is not allowed on the Ex device the game
// creates.

#include "imgui.h"

struct IDirect3DDevice9;

namespace Branding
{
	struct Logo
	{
		ImTextureID texture = ImTextureID_Invalid;

		// Full texture size, and the sub-rectangle of it that is actually inked. The source
		// PNGs carry a lot of transparent margin - Logo_Oceanya is only 75% ink vertically,
		// and Logo_O_Centered is not centred in its own canvas - so drawing them whole makes
		// the artwork look undersized and off-centre. Everything below works off the content
		// box; uv0/uv1 crop the margin away at draw time.
		int width = 0;
		int height = 0;
		int contentWidth = 0;
		int contentHeight = 0;
		ImVec2 uv0 = ImVec2(0.0f, 0.0f);
		ImVec2 uv1 = ImVec2(1.0f, 1.0f);

		bool IsValid() const { return texture != ImTextureID_Invalid && contentWidth > 0 && contentHeight > 0; }
		float Aspect() const { return contentHeight > 0 ? (float)contentWidth / (float)contentHeight : 1.0f; }
	};

	void Initialize(IDirect3DDevice9* device);
	void Shutdown();

	// Paired with the D3D9 device reset the overlay already handles for ImGui's own font
	// atlas; without these the logos survive as dangling handles across a resolution change.
	void InvalidateDeviceObjects();
	void CreateDeviceObjects();

	const Logo& Oceanya();
	const Logo& Laboratories();
	const Logo& OMark();
}
