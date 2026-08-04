#include "SystemSpecsLogger.h"

#include "Core/logger.h"

#include <Windows.h>
#include <winioctl.h>
#include <intrin.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace
{
	std::string NarrowFromWide(const std::wstring& wide)
	{
		if (wide.empty())
		{
			return {};
		}
		const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (size <= 0)
		{
			return {};
		}
		std::string narrow(static_cast<size_t>(size) - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &narrow[0], size, nullptr, nullptr);
		return narrow;
	}

	bool ContainsCaseInsensitive(std::wstring haystack, const wchar_t* needle)
	{
		std::wstring upperNeedle = needle;
		std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::towupper);
		std::transform(upperNeedle.begin(), upperNeedle.end(), upperNeedle.begin(), ::towupper);
		return haystack.find(upperNeedle) != std::wstring::npos;
	}

	std::wstring ReadRegistryString(HKEY hKey, const wchar_t* name)
	{
		wchar_t buf[256] = {};
		DWORD size = sizeof(buf);
		DWORD type = 0;
		if (RegQueryValueExW(hKey, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS
			&& (type == REG_SZ || type == REG_EXPAND_SZ))
		{
			return buf;
		}
		return L"";
	}

	DWORD ReadRegistryDword(HKEY hKey, const wchar_t* name)
	{
		DWORD value = 0;
		DWORD size = sizeof(value);
		DWORD type = 0;
		if (RegQueryValueExW(hKey, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS
			&& type == REG_DWORD)
		{
			return value;
		}
		return 0;
	}

	void LogOsVersion()
	{
		HKEY hKey;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
			KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
		{
			LOG(1, "[SystemSpecs] OS: registry query failed\n");
			return;
		}

		const std::wstring productName = ReadRegistryString(hKey, L"ProductName");
		const std::wstring displayVersion = ReadRegistryString(hKey, L"DisplayVersion");
		const std::wstring buildNumber = ReadRegistryString(hKey, L"CurrentBuildNumber");
		const DWORD ubr = ReadRegistryDword(hKey, L"UBR");
		RegCloseKey(hKey);

		LOG(1, "[SystemSpecs] OS: %s %s (build %s.%u)\n",
			NarrowFromWide(productName.empty() ? L"Windows" : productName).c_str(),
			NarrowFromWide(displayVersion).c_str(),
			NarrowFromWide(buildNumber).c_str(),
			ubr);
	}

	void LogCpuAndRam()
	{
		int cpuInfo[4] = {};
		char brand[0x40] = {};
		__cpuid(cpuInfo, 0x80000000);
		if (static_cast<unsigned int>(cpuInfo[0]) >= 0x80000004)
		{
			__cpuid(reinterpret_cast<int*>(brand), 0x80000002);
			__cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003);
			__cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004);
		}
		else
		{
			strcpy_s(brand, "unknown CPU");
		}

		SYSTEM_INFO sysInfo = {};
		GetSystemInfo(&sysInfo);

		MEMORYSTATUSEX memStatus = {};
		memStatus.dwLength = sizeof(memStatus);
		GlobalMemoryStatusEx(&memStatus);
		const double totalRamGB = static_cast<double>(memStatus.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);

		LOG(1, "[SystemSpecs] CPU: %s (%u logical cores), RAM: %.1f GB\n",
			brand, sysInfo.dwNumberOfProcessors, totalRamGB);
	}

	void LogGpu(IDirect3DDevice9* device)
	{
		if (!device)
		{
			LOG(1, "[SystemSpecs] GPU: no device available\n");
			return;
		}

		IDirect3D9* pD3D9 = nullptr;
		if (FAILED(device->GetDirect3D(&pD3D9)) || !pD3D9)
		{
			LOG(1, "[SystemSpecs] GPU: GetDirect3D failed\n");
			return;
		}

		D3DADAPTER_IDENTIFIER9 identifier = {};
		const HRESULT hr = pD3D9->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &identifier);
		pD3D9->Release();

		if (FAILED(hr))
		{
			LOG(1, "[SystemSpecs] GPU: GetAdapterIdentifier failed (hr=0x%08X)\n", hr);
			return;
		}

		const LARGE_INTEGER driverVersion = identifier.DriverVersion;
		LOG(1, "[SystemSpecs] GPU: %s (driver %s, version %u.%u.%u.%u)\n",
			identifier.Description,
			identifier.Driver,
			HIWORD(driverVersion.HighPart), LOWORD(driverVersion.HighPart),
			HIWORD(driverVersion.LowPart), LOWORD(driverVersion.LowPart));
	}

	void LogDisk()
	{
		wchar_t exePath[MAX_PATH] = {};
		if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
		{
			LOG(1, "[SystemSpecs] Disk: could not resolve game executable path\n");
			return;
		}

		wchar_t driveRoot[4] = { exePath[0], L':', L'\\', L'\0' };

		ULARGE_INTEGER freeBytes = {};
		ULARGE_INTEGER totalBytes = {};
		const bool haveSpace = GetDiskFreeSpaceExW(driveRoot, &freeBytes, &totalBytes, nullptr) != 0;

		wchar_t volumePath[7] = { exePath[0], L':', L'\0' };
		HANDLE hDevice = CreateFileW(volumePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_EXISTING, 0, nullptr);

		const char* rotationalDesc = "unknown";
		if (hDevice != INVALID_HANDLE_VALUE)
		{
			STORAGE_PROPERTY_QUERY query = {};
			query.PropertyId = StorageDeviceSeekPenaltyProperty;
			query.QueryType = PropertyStandardQuery;

			DEVICE_SEEK_PENALTY_DESCRIPTOR desc = {};
			DWORD bytesReturned = 0;
			if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
				&desc, sizeof(desc), &bytesReturned, nullptr))
			{
				rotationalDesc = desc.IncursSeekPenalty ? "HDD (seek penalty)" : "SSD (no seek penalty)";
			}
			CloseHandle(hDevice);
		}

		if (haveSpace)
		{
			LOG(1, "[SystemSpecs] Disk (%ls): %s, %.1f/%.1f GB free\n",
				driveRoot, rotationalDesc,
				static_cast<double>(freeBytes.QuadPart) / (1024.0 * 1024.0 * 1024.0),
				static_cast<double>(totalBytes.QuadPart) / (1024.0 * 1024.0 * 1024.0));
		}
		else
		{
			LOG(1, "[SystemSpecs] Disk (%ls): %s, free space unavailable\n", driveRoot, rotationalDesc);
		}
	}

	void LogSecuritySoftware()
	{
		static const wchar_t* kKnownAvKeywords[] = {
			L"Defender", L"Avast", L"AVG", L"Norton", L"McAfee", L"Malwarebytes",
			L"BitDefender", L"Kaspersky", L"ESET", L"Sophos", L"Webroot",
			L"TrendMicro", L"Trend Micro", L"F-Secure", L"Panda", L"Avira",
			L"CrowdStrike", L"SentinelOne", L"Comodo", L"ZoneAlarm",
		};

		const SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
		if (!hSCM)
		{
			LOG(1, "[SystemSpecs] Security software: service manager unavailable\n");
			return;
		}

		DWORD bytesNeeded = 0;
		DWORD servicesReturned = 0;
		DWORD resumeHandle = 0;
		EnumServicesStatusExW(hSCM, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE,
			nullptr, 0, &bytesNeeded, &servicesReturned, &resumeHandle, nullptr);

		std::string found;
		if (bytesNeeded > 0)
		{
			std::vector<uint8_t> buffer(bytesNeeded);
			if (EnumServicesStatusExW(hSCM, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE,
				buffer.data(), static_cast<DWORD>(buffer.size()), &bytesNeeded, &servicesReturned,
				&resumeHandle, nullptr))
			{
				const auto* services = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
				for (DWORD i = 0; i < servicesReturned; ++i)
				{
					const wchar_t* displayName = services[i].lpDisplayName;
					if (!displayName)
					{
						continue;
					}
					for (const wchar_t* keyword : kKnownAvKeywords)
					{
						if (ContainsCaseInsensitive(displayName, keyword))
						{
							if (!found.empty())
							{
								found += "; ";
							}
							found += NarrowFromWide(displayName);
							break;
						}
					}
				}
			}
		}

		CloseServiceHandle(hSCM);

		LOG(1, "[SystemSpecs] Running security services: %s\n",
			found.empty() ? "none matched known list" : found.c_str());
	}
}

void LogSystemSpecs(IDirect3DDevice9* device)
{
	LogOsVersion();
	LogCpuAndRam();
	LogGpu(device);
	LogDisk();
	LogSecuritySoftware();
}
