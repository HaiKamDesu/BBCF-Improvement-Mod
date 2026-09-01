#pragma once
#include <string>

/* Reads one of the RCDATA blobs embedded in this DLL (see resource/resource.rc) into a
   string. Files the mod needs at runtime are compiled in rather than shipped loose, so a
   release is just the DLL plus the updater; the mod writes whatever else it needs itself.

   The resource name is the one written in the .rc, e.g. L"settings.ini". Returns false
   when the resource is missing or empty. */
bool LoadEmbeddedResource(const wchar_t* resourceName, std::string& out);
