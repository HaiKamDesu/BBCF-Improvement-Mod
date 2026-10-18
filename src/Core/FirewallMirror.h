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

	// A new rule like `source` but naming `program`. Setters Windows refuses are listed in
	// *problems (they explain an Add that then fails).
	inline INetFwRule* CloneFor(INetFwRule* source, const std::wstring& program, std::wstring* problems = nullptr)
	{
		INetFwRule* rule = nullptr;
		if (FAILED(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwRule), reinterpret_cast<void**>(&rule))))
			return nullptr;
		auto note = [problems](const wchar_t* what, HRESULT hr)
		{
			if (FAILED(hr) && problems)
			{
				wchar_t line[96] = {};
				swprintf_s(line, L" %ls=0x%08lX", what, static_cast<unsigned long>(hr));
				*problems += line;
			}
		};
		// Only values that are actually set are copied: an empty string where Windows expects
		// none is not the same thing. Order matters: ports need the protocol first.
		auto copyText = [&](HRESULT(STDMETHODCALLTYPE INetFwRule::*get)(BSTR*), HRESULT(STDMETHODCALLTYPE INetFwRule::*put)(BSTR), const wchar_t* what)
		{
			BSTR value = nullptr;
			if (SUCCEEDED((source->*get)(&value)) && value && SysStringLen(value) > 0)
				note(what, (rule->*put)(value));
			SysFreeString(value);
		};
		// A fresh, zeroed variable for every getter: get_Profiles ORs its bits into the value it is
		// handed instead of overwriting it, so reusing the protocol's variable turned UDP (17) and
		// all profiles (7) into an invalid 0x17, and Windows refused the rule (E_INVALIDARG).
		long protocol = 0, profiles = 0;
		VARIANT_BOOL edge = VARIANT_FALSE, enabled = VARIANT_FALSE;
		NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_IN;
		NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
		copyText(&INetFwRule::get_Name, &INetFwRule::put_Name, L"Name");
		copyText(&INetFwRule::get_Description, &INetFwRule::put_Description, L"Description");
		copyText(&INetFwRule::get_Grouping, &INetFwRule::put_Grouping, L"Grouping");
		BSTR app = SysAllocString(program.c_str());
		note(L"ApplicationName", rule->put_ApplicationName(app));
		SysFreeString(app);
		if (SUCCEEDED(source->get_Protocol(&protocol))) note(L"Protocol", rule->put_Protocol(protocol));
		copyText(&INetFwRule::get_LocalPorts, &INetFwRule::put_LocalPorts, L"LocalPorts");
		copyText(&INetFwRule::get_RemotePorts, &INetFwRule::put_RemotePorts, L"RemotePorts");
		copyText(&INetFwRule::get_LocalAddresses, &INetFwRule::put_LocalAddresses, L"LocalAddresses");
		copyText(&INetFwRule::get_RemoteAddresses, &INetFwRule::put_RemoteAddresses, L"RemoteAddresses");
		copyText(&INetFwRule::get_IcmpTypesAndCodes, &INetFwRule::put_IcmpTypesAndCodes, L"IcmpTypesAndCodes");
		if (SUCCEEDED(source->get_Direction(&direction))) note(L"Direction", rule->put_Direction(direction));
		if (SUCCEEDED(source->get_Action(&action))) note(L"Action", rule->put_Action(action));
		if (SUCCEEDED(source->get_Profiles(&profiles))) note(L"Profiles", rule->put_Profiles(profiles));
		copyText(&INetFwRule::get_InterfaceTypes, &INetFwRule::put_InterfaceTypes, L"InterfaceTypes");
		if (SUCCEEDED(source->get_EdgeTraversal(&edge))) note(L"EdgeTraversal", rule->put_EdgeTraversal(edge));
		if (SUCCEEDED(source->get_Enabled(&enabled))) note(L"Enabled", rule->put_Enabled(enabled));
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
			HRESULT hr = S_OK;
			std::wstring problems;
			if (apply)
			{
				// Once more on failure, from a fresh clone: a transient refusal must not cost the
				// player the rule.
				for (int attempt = 0; attempt < 2; ++attempt)
				{
					problems.clear();
					INetFwRule* copy = CloneFor(source, to, &problems);
					hr = copy ? rules->Add(copy) : E_FAIL;
					if (copy)
						copy->Release();
					if (SUCCEEDED(hr))
						break;
					Sleep(200);
				}
				ok = SUCCEEDED(hr);
				if (ok && added)
					++*added;
			}
			if (report)
			{
				wchar_t code[32] = {};
				if (!ok)
					swprintf_s(code, L" (hr=0x%08lX)", static_cast<unsigned long>(hr));
				*report += (apply ? (ok ? L"added: " : L"FAILED to add: ") : L"missing: ") + signature + code +
					(problems.empty() ? L"" : L" refused setters:" + problems) + L"\r\n";
			}
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
