#include "EmbeddedResources.h"

#include <Windows.h>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

bool LoadEmbeddedResource(const wchar_t* resourceName, std::string& out)
{
	if (!resourceName)
		return false;

	HMODULE module = reinterpret_cast<HMODULE>(&__ImageBase);

	HRSRC resource = FindResourceW(module, resourceName, RT_RCDATA);
	if (!resource)
	{
		// The .rc quotes the names, and some toolchains keep the quotes in the name table.
		std::wstring quoted = L"\"";
		quoted += resourceName;
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

	out.assign(static_cast<const char*>(data), static_cast<const char*>(data) + size);
	return true;
}
