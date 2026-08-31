#include "Branding.h"

#include "Core/logger.h"

#include <Windows.h>
#include <d3d9.h>

#include <vector>

// The only translation unit that pulls in the stb implementation.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace Branding
{
	namespace
	{
		struct LogoSource
		{
			const wchar_t* resourceName;
			Logo logo;
			IDirect3DTexture9* texture = nullptr;
		};

		IDirect3DDevice9* g_device = nullptr;
		bool g_created = false;

		LogoSource g_logos[] = {
			{ L"Logo_Oceanya.png" },
			{ L"Logo_Laboratories.png" },
			{ L"Logo_O_Centered.png" },
		};

		enum { Logo_Oceanya = 0, Logo_Laboratories = 1, Logo_OMark = 2 };

		HMODULE OwnModule()
		{
			return reinterpret_cast<HMODULE>(&__ImageBase);
		}

		bool FindEmbeddedPng(const wchar_t* name, const unsigned char** outData, DWORD* outSize)
		{
			HMODULE module = OwnModule();

			HRSRC resource = FindResourceW(module, name, RT_RCDATA);
			if (!resource)
			{
				// The .rc quotes the names, and some toolchains keep the quotes in the name
				// table - the localization loader hit the same thing.
				std::wstring quoted = L"\"";
				quoted += name;
				quoted += L"\"";
				resource = FindResourceW(module, quoted.c_str(), RT_RCDATA);
			}

			if (!resource)
				return false;

			HGLOBAL handle = LoadResource(module, resource);
			if (!handle)
				return false;

			const DWORD size = SizeofResource(module, resource);
			const void* data = LockResource(handle);
			if (!data || size == 0)
				return false;

			*outData = static_cast<const unsigned char*>(data);
			*outSize = size;
			return true;
		}

		// Mirrors what imgui_impl_dx9 does for its font atlas: a dynamic texture in the
		// default pool, filled through LockRect, in BGRA order.
		IDirect3DTexture9* UploadRgba(const unsigned char* rgba, int width, int height)
		{
			if (!g_device)
				return nullptr;

			IDirect3DTexture9* texture = nullptr;
			if (g_device->CreateTexture(width, height, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
				D3DPOOL_DEFAULT, &texture, NULL) != D3D_OK)
			{
				return nullptr;
			}

			D3DLOCKED_RECT locked;
			if (texture->LockRect(0, &locked, NULL, 0) != D3D_OK)
			{
				texture->Release();
				return nullptr;
			}

			for (int y = 0; y < height; ++y)
			{
				const unsigned char* src = rgba + (size_t)y * width * 4;
				unsigned char* dst = static_cast<unsigned char*>(locked.pBits) + (size_t)y * locked.Pitch;
				for (int x = 0; x < width; ++x)
				{
					dst[0] = src[2]; // B
					dst[1] = src[1]; // G
					dst[2] = src[0]; // R
					dst[3] = src[3]; // A
					src += 4;
					dst += 4;
				}
			}

			texture->UnlockRect(0);
			return texture;
		}

		// The inked bounds of an RGBA image, so the transparent margin around the artwork can
		// be cropped with UVs instead of being scaled along with it.
		void MeasureContentBox(const unsigned char* rgba, int width, int height, Logo& logo)
		{
			const unsigned char kAlphaThreshold = 8;

			int minX = width;
			int minY = height;
			int maxX = -1;
			int maxY = -1;

			for (int y = 0; y < height; ++y)
			{
				const unsigned char* row = rgba + (size_t)y * width * 4;
				for (int x = 0; x < width; ++x)
				{
					if (row[x * 4 + 3] <= kAlphaThreshold)
						continue;

					if (x < minX) minX = x;
					if (x > maxX) maxX = x;
					if (y < minY) minY = y;
					if (y > maxY) maxY = y;
				}
			}

			if (maxX < minX || maxY < minY)
			{
				// Entirely transparent: fall back to the whole image rather than to nothing.
				logo.contentWidth = width;
				logo.contentHeight = height;
				logo.uv0 = ImVec2(0.0f, 0.0f);
				logo.uv1 = ImVec2(1.0f, 1.0f);
				return;
			}

			logo.contentWidth = maxX - minX + 1;
			logo.contentHeight = maxY - minY + 1;
			logo.uv0 = ImVec2((float)minX / (float)width, (float)minY / (float)height);
			logo.uv1 = ImVec2((float)(maxX + 1) / (float)width, (float)(maxY + 1) / (float)height);
		}

		void CreateOne(LogoSource& source)
		{
			if (source.texture)
				return;

			const unsigned char* data = nullptr;
			DWORD size = 0;
			if (!FindEmbeddedPng(source.resourceName, &data, &size))
			{
				LOG(2, "[Branding] Embedded artwork not found in the DLL resources.\n");
				return;
			}

			int width = 0;
			int height = 0;
			int channels = 0;
			stbi_uc* pixels = stbi_load_from_memory(data, (int)size, &width, &height, &channels, 4);
			if (!pixels)
			{
				LOG(2, "[Branding] Could not decode embedded artwork.\n");
				return;
			}

			source.texture = UploadRgba(pixels, width, height);

			if (source.texture)
			{
				// Pointer -> ImU64 on a 32-bit build: go through uintptr_t explicitly.
				source.logo.texture = (ImTextureID)(uintptr_t)source.texture;
				source.logo.width = width;
				source.logo.height = height;

				// Before the pixels are freed.
				MeasureContentBox(pixels, width, height, source.logo);
			}
			else
			{
				LOG(2, "[Branding] Could not upload artwork to the device.\n");
			}

			stbi_image_free(pixels);
		}

		void DestroyOne(LogoSource& source)
		{
			if (source.texture)
			{
				source.texture->Release();
				source.texture = nullptr;
			}
			source.logo = Logo();
		}
	}

	void Initialize(IDirect3DDevice9* device)
	{
		g_device = device;
		CreateDeviceObjects();
	}

	void Shutdown()
	{
		InvalidateDeviceObjects();
		g_device = nullptr;
	}

	void CreateDeviceObjects()
	{
		if (g_created || !g_device)
			return;

		for (LogoSource& source : g_logos)
			CreateOne(source);

		g_created = true;
	}

	void InvalidateDeviceObjects()
	{
		for (LogoSource& source : g_logos)
			DestroyOne(source);

		g_created = false;
	}

	const Logo& Oceanya() { return g_logos[Logo_Oceanya].logo; }
	const Logo& Laboratories() { return g_logos[Logo_Laboratories].logo; }
	const Logo& OMark() { return g_logos[Logo_OMark].logo; }
}
