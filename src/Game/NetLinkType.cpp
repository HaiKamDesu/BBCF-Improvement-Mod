#include <winsock2.h>
#include <Windows.h>
#include <iphlpapi.h>

#include "NetLinkType.h"

#include <cstdlib>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

int NetLinkType::Detect(unsigned long remoteIpHostOrder)
{
	DWORD ifIndex = 0;
	const IPAddr dest = htonl(remoteIpHostOrder ? remoteIpHostOrder : 0x01010101ul);
	if (GetBestInterface(dest, &ifIndex) != NO_ERROR)
		return 0;

	int result = 0;
	ULONG size = 16 * 1024;
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		IP_ADAPTER_ADDRESSES* list = (IP_ADAPTER_ADDRESSES*)malloc(size);
		if (!list)
			break;
		const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
		const ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
		if (rc == ERROR_BUFFER_OVERFLOW)
		{
			free(list);
			continue;
		}
		if (rc == NO_ERROR)
		{
			for (IP_ADAPTER_ADDRESSES* a = list; a; a = a->Next)
			{
				if (a->IfIndex != ifIndex)
					continue;
				// 71 = IF_TYPE_IEEE80211 (Wi-Fi), 6 = IF_TYPE_ETHERNET_CSMACD (wired). Anything
				// else (mobile broadband, tunnels, VPN adapters, odd USB tethering) = unknown.
				if (a->IfType == 71)
					result = 2;
				else if (a->IfType == 6)
					result = 1;
				break;
			}
		}
		free(list);
		break;
	}
	return result;
}
