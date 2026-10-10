#include "GameBuildGuard.h"

#include "SupportedGameBuild.h"
#include "logger.h"
#include "Updater/UnsupportedBuildFlow.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <Windows.h>

namespace
{
	// The linker rewrites the timestamp on every build, and the image size and entry point move
	// with any code change, so together they name the build. Characteristics and CheckSum are
	// left out on purpose: a large-address-aware patch changes both without moving a byte of code.
	char g_currentId[32] = "unreadable";
	bool g_supported = true;
	bool g_overridden = false;
	std::atomic<bool> g_overrideLogged{ false };

	bool ReadMainModuleId(char* out, size_t outSize)
	{
		const auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
		if (!base)
			return false;

		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;

		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE ||
			nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
			return false;

		sprintf_s(out, outSize, "%08lX-%08lX-%08lX",
			nt->FileHeader.TimeDateStamp,
			nt->OptionalHeader.SizeOfImage,
			nt->OptionalHeader.AddressOfEntryPoint);
		return true;
	}

	bool OverrideRequested()
	{
		wchar_t value[8] = {};
		const DWORD len = GetEnvironmentVariableW(L"BBCF_IM_ALLOW_UNKNOWN_GAME_BUILD", value, 8);
		return len > 0 && len < 8 && value[0] == L'1';
	}

	DWORD WINAPI UnsupportedBuildThread(LPVOID)
	{
		Updater::RunUnsupportedBuildFlow(g_currentId);
		return 0;
	}
}

namespace GameBuildGuard
{
	void Evaluate()
	{
		const bool readable = ReadMainModuleId(g_currentId, sizeof(g_currentId));
		const bool matches = readable && std::strcmp(g_currentId, BBCF_SUPPORTED_GAME_BUILD_ID) == 0;

		g_overridden = !matches && OverrideRequested();
		g_supported = matches || g_overridden;
	}

	bool IsSupported()
	{
		return g_supported;
	}

	const char* CurrentBuildId()
	{
		return g_currentId;
	}

	void StartUnsupportedBuildFlow()
	{
		// CreateThread is allowed under the loader lock; the thread itself starts once DllMain
		// has returned.
		HANDLE thread = CreateThread(nullptr, 0, UnsupportedBuildThread, nullptr, 0, nullptr);
		if (thread)
			CloseHandle(thread);
	}

	void LogOverrideOnce()
	{
		if (!g_overridden || g_overrideLogged.exchange(true))
			return;

		ForceLog("[BuildGuard] Unknown game build %s (this mod supports %s, Steam build %s) allowed by "
			"BBCF_IM_ALLOW_UNKNOWN_GAME_BUILD; hooks will patch it anyway.\n",
			g_currentId, BBCF_SUPPORTED_GAME_BUILD_ID, BBCF_SUPPORTED_GAME_STEAM_BUILD);
	}
}
