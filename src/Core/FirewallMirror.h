#pragma once

// Copies Windows Firewall rules that name one program so they name another, for 4 GB mode
// (LaaRedirect.h): rules are tied to an exe's path, and BBCF_IM_4GB.exe would otherwise start
// with none of the rules a player (or Steam's installer) gave BBCF.exe - a firewall prompt
// mid-match and blocked incoming connections until it is answered.
//
// Header-only because two programs use it: the mod counts what is missing (reading rules needs
// no admin rights) and the updater, run elevated, adds them (adding does).

#include <Windows.h>
#include <netfw.h>

#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace FirewallMirror
{
	inline std::wstring Expand(const wchar_t* path)
	{
		if (!path)
			return L"";
		wchar_t buffer[MAX_PATH * 2] = {};
		const DWORD length = ExpandEnvironmentStringsW(path, buffer, MAX_PATH * 2);
		return (length == 0 || length > MAX_PATH * 2) ? std::wstring(path) : std::wstring(buffer);
	}

	inline bool SamePath(const wchar_t* stored, const std::wstring& path)
	{
		return stored && _wcsicmp(Expand(stored).c_str(), path.c_str()) == 0;
	}

	inline std::wstring Text(BSTR value)
	{
		return value ? std::wstring(value, SysStringLen(value)) : std::wstring();
	}

	// What makes two rules the same rule, apart from the program they name.
	inline std::wstring Signature(INetFwRule* rule)
	{
		BSTR name = nullptr, local = nullptr, remote = nullptr;
		long protocol = 0, profiles = 0;
		NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_IN;
		NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
		rule->get_Name(&name);
		rule->get_Protocol(&protocol);
		rule->get_LocalPorts(&local);
		rule->get_RemotePorts(&remote);
		rule->get_Direction(&direction);
		rule->get_Action(&action);
		rule->get_Profiles(&profiles);
		std::wstring signature = Text(name) + L"|" + std::to_wstring(protocol) + L"|" + Text(local) + L"|" + Text(remote) +
			L"|" + std::to_wstring(direction) + L"|" + std::to_wstring(action) + L"|" + std::to_wstring(profiles);
		SysFreeString(name);
		SysFreeString(local);
		SysFreeString(remote);
		return signature;
	}

	inline INetFwRule* CloneFor(INetFwRule* source, const std::wstring& program)
	{
		INetFwRule* rule = nullptr;
		if (FAILED(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwRule), reinterpret_cast<void**>(&rule))))
			return nullptr;
		BSTR value = nullptr;
		long number = 0;
		VARIANT_BOOL flag = VARIANT_FALSE;
		NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_IN;
		NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
		// Order matters: ports can only be set once the protocol says TCP or UDP.
		if (SUCCEEDED(source->get_Name(&value))) { rule->put_Name(value); SysFreeString(value); value = nullptr; }
		if (SUCCEEDED(source->get_Description(&value))) { rule->put_Description(value); SysFreeString(value); value = nullptr; }
		if (SUCCEEDED(source->get_Grouping(&value)) && value) { rule->put_Grouping(value); SysFreeString(value); value = nullptr; }
		if (SUCCEEDED(source->get_Protocol(&number))) rule->put_Protocol(number);
		if (SUCCEEDED(source->get_LocalPorts(&value)) && value) { rule->put_LocalPorts(value); SysFreeString(value); value = nullptr; }
		if (SUCCEEDED(source->get_RemotePorts(&value)) && value) { rule->put_RemotePorts(value); SysFreeString(value); value = nullptr; }
		if (SUCCEEDED(source->get_LocalAddresses(&value)) && value) { rule->put_LocalAddresses(value); SysFreeString(value); value = nullptr; }
		if (SUCCEEDED(source->get_RemoteAddresses(&value)) && value) { rule->put_RemoteAddresses(value); SysFreeString(value); value = nullptr; }
		if (SUCCEEDED(source->get_Direction(&direction))) rule->put_Direction(direction);
		if (SUCCEEDED(source->get_Action(&action))) rule->put_Action(action);
		if (SUCCEEDED(source->get_Profiles(&number))) rule->put_Profiles(number);
		if (SUCCEEDED(source->get_InterfaceTypes(&value)) && value) { rule->put_InterfaceTypes(value); SysFreeString(value); value = nullptr; }
		if (SUCCEEDED(source->get_EdgeTraversal(&flag))) rule->put_EdgeTraversal(flag);
		if (SUCCEEDED(source->get_Enabled(&flag))) rule->put_Enabled(flag);
		BSTR app = SysAllocString(program.c_str());
		rule->put_ApplicationName(app);
		SysFreeString(app);
		return rule;
	}

	// The rules naming `from` that have no counterpart naming `to`. With apply, adds them (needs
	// admin rights). Returns how many were missing, or -1 if the firewall could not be read; each
	// one is described in *report. The caller initialises COM.
	inline int Mirror(const std::wstring& from, const std::wstring& to, bool apply, std::wstring* report, int* added = nullptr)
	{
		if (added)
			*added = 0;
		INetFwPolicy2* policy = nullptr;
		if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy))))
			return -1;
		INetFwRules* rules = nullptr;
		IUnknown* enumUnknown = nullptr;
		IEnumVARIANT* enumerator = nullptr;
		if (FAILED(policy->get_Rules(&rules)) || FAILED(rules->get__NewEnum(&enumUnknown)) ||
			FAILED(enumUnknown->QueryInterface(__uuidof(IEnumVARIANT), reinterpret_cast<void**>(&enumerator))))
		{
			if (enumUnknown) enumUnknown->Release();
			if (rules) rules->Release();
			policy->Release();
			return -1;
		}

		std::vector<INetFwRule*> sources;
		std::set<std::wstring> existing;
		VARIANT item;
		VariantInit(&item);
		ULONG fetched = 0;
		while (enumerator->Next(1, &item, &fetched) == S_OK && fetched == 1)
		{
			INetFwRule* rule = nullptr;
			if (item.vt == VT_DISPATCH && item.pdispVal &&
				SUCCEEDED(item.pdispVal->QueryInterface(__uuidof(INetFwRule), reinterpret_cast<void**>(&rule))))
			{
				BSTR app = nullptr;
				rule->get_ApplicationName(&app);
				if (SamePath(app, from))
					sources.push_back(rule), rule = nullptr;
				else if (SamePath(app, to))
					existing.insert(Signature(rule));
				SysFreeString(app);
				if (rule)
					rule->Release();
			}
			VariantClear(&item);
		}

		int missing = 0;
		for (INetFwRule* source : sources)
		{
			const std::wstring signature = Signature(source);
			if (existing.count(signature))
			{
				source->Release();
				continue;
			}
			++missing;
			bool ok = true;
			if (apply)
			{
				INetFwRule* copy = CloneFor(source, to);
				ok = copy && SUCCEEDED(rules->Add(copy));
				if (copy)
					copy->Release();
				if (ok && added)
					++*added;
			}
			if (report)
				*report += (apply ? (ok ? L"added: " : L"FAILED to add: ") : L"missing: ") + signature + L"\r\n";
			existing.insert(signature);
			source->Release();
		}
		enumerator->Release();
		enumUnknown->Release();
		rules->Release();
		policy->Release();
		return missing;
	}
}
