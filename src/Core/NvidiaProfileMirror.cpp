#include "NvidiaProfileMirror.h"

#include <Windows.h>
#include <cwchar>
#include <memory>

namespace
{
	// nvapi_QueryInterface ids (public NVAPI SDK).
	constexpr unsigned kInitialize = 0x0150E828;
	constexpr unsigned kCreateSession = 0x0694D52E;
	constexpr unsigned kLoadSettings = 0x375DBD6B;
	constexpr unsigned kFindApplicationByName = 0xEEE566B2;
	constexpr unsigned kCreateApplication = 0x4347A9DE;
	constexpr unsigned kSaveSettings = 0xFCBC7E14;
	constexpr unsigned kDestroySession = 0xDAD9CFF8;
	constexpr int kExecutableNotFound = -166; // NVAPI_EXECUTABLE_NOT_FOUND

#pragma pack(push, 8)
	// NVDRS_APPLICATION_V3 (NvAPI_UnicodeString is 2048 NvU16).
	struct DrsApplication
	{
		unsigned version;
		unsigned isPredefined;
		wchar_t appName[2048];
		wchar_t userFriendlyName[2048];
		wchar_t launcher[2048];
		wchar_t fileInFolder[2048];
		unsigned flags;
	};
#pragma pack(pop)

	using QueryInterface_t = void*(__cdecl*)(unsigned);

	std::unique_ptr<DrsApplication> NewApplication()
	{
		auto app = std::make_unique<DrsApplication>();
		*app = {};
		app->version = sizeof(DrsApplication) | (3u << 16);
		return app;
	}
}

namespace NvidiaProfileMirror
{
	const char* Sync()
	{
		HMODULE nvapi = LoadLibraryW(L"nvapi.dll");
		if (!nvapi)
			return "no NVIDIA driver; nothing to do";
		auto query = reinterpret_cast<QueryInterface_t>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
		if (!query)
			return "nvapi.dll has no nvapi_QueryInterface; left alone";

		auto initialize = reinterpret_cast<int(__cdecl*)()>(query(kInitialize));
		auto createSession = reinterpret_cast<int(__cdecl*)(void**)>(query(kCreateSession));
		auto loadSettings = reinterpret_cast<int(__cdecl*)(void*)>(query(kLoadSettings));
		auto findApplication = reinterpret_cast<int(__cdecl*)(void*, const wchar_t*, void**, DrsApplication*)>(query(kFindApplicationByName));
		auto createApplication = reinterpret_cast<int(__cdecl*)(void*, void*, DrsApplication*)>(query(kCreateApplication));
		auto saveSettings = reinterpret_cast<int(__cdecl*)(void*)>(query(kSaveSettings));
		auto destroySession = reinterpret_cast<int(__cdecl*)(void*)>(query(kDestroySession));
		if (!initialize || !createSession || !loadSettings || !findApplication ||
			!createApplication || !saveSettings || !destroySession)
			return "NVAPI driver settings calls unavailable; left alone";
		if (initialize() != 0)
			return "NVAPI did not initialise; left alone";

		void* session = nullptr;
		if (createSession(&session) != 0 || !session)
			return "could not open the NVIDIA settings; left alone";
		const char* result = nullptr;
		void* gameProfile = nullptr;
		auto gameApp = NewApplication();
		auto copyApp = NewApplication();
		void* copyProfile = nullptr;
		if (loadSettings(session) != 0)
			result = "could not load the NVIDIA settings; left alone";
		else if (findApplication(session, L"bbcf.exe", &gameProfile, gameApp.get()) != 0 || !gameProfile)
			result = "BBCF.exe is in no NVIDIA profile; nothing to carry over";
		else
		{
			const int found = findApplication(session, L"bbcf_im_4gb.exe", &copyProfile, copyApp.get());
			if (found == 0)
				result = "BBCF_IM_4GB.exe already has an NVIDIA profile; left alone";
			else if (found != kExecutableNotFound)
				result = "could not look BBCF_IM_4GB.exe up in the NVIDIA settings; left alone";
			else
			{
				auto entry = NewApplication();
				wcscpy_s(entry->appName, L"bbcf_im_4gb.exe");
				wcscpy_s(entry->userFriendlyName, L"BlazBlue Centralfiction (4 GB mode)");
				if (createApplication(session, gameProfile, entry.get()) != 0)
					result = "could not add BBCF_IM_4GB.exe to BBCF's NVIDIA profile";
				else if (saveSettings(session) != 0)
					result = "added BBCF_IM_4GB.exe to BBCF's NVIDIA profile but could not save it";
				else
					result = "added BBCF_IM_4GB.exe to BBCF's NVIDIA profile";
			}
		}
		destroySession(session);
		return result;
	}
}
