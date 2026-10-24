#pragma once

// Wi-Fi or wired, for the PC & connection stats (PerformanceMonitor). Kept in its own file
// because it needs <winsock2.h> before <Windows.h>, which the rest of the mod includes first.
namespace NetLinkType
{
	// Blocking (a routing-table lookup plus GetAdaptersAddresses, a few ms); call it off the
	// game thread. remoteIpHostOrder = the peer's address if known (direct session), else 0
	// for the adapter that carries the default route. No packet is sent.
	// Returns 0 unknown, 1 wired, 2 Wi-Fi. Nothing else (adapter names, MACs, GUIDs, SSIDs,
	// addresses) is kept, returned or logged.
	int Detect(unsigned long remoteIpHostOrder);
}
