#include "LocalInputManager.h"

#include "GameKeyConfig.h"
#include "InputBackend.h"

#include "Core/logger.h"
#include "Core/RuntimePlatform.h"
#include "Core/Settings.h"
#include "Core/utils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace LocalInput
{
	namespace
	{
		constexpr ULONGLONG kMinGameTickMs = 4;   // controller updates within a frame share one poll
		constexpr ULONGLONG kMinUiTickMs = 30;    // when the game is not ticking controllers

		struct Profile
		{
			std::string name;
			bool pinned = false;
			Side pinnedSide = Side_P1;
			bool seen = false;           // keyboards: has ever been typed on
			bool ignored = false;        // hidden by the player
			bool hasMapping = false;     // the mod's own mapping, used instead of the game's
			Mapping mapping;
			bool hasBinds = false;       // set in the game's Button Settings with this device
			GameBinds binds;
		};
	}

	struct Manager::Device
	{
		DeviceSnapshot snap;
		bool sessionKnown = false;  // assigned at least once this session
		Side side = Side_P1;
		ULONGLONG lastInputMs = 0;
		uint32_t buttons = 0;       // battle word (what the UI shows)
		uint32_t menuWord = 0;
	};

	namespace
	{
		std::mutex g_mutex;
		std::unique_ptr<IInputBackend> g_backend;
		std::vector<DeviceSnapshot> g_snapshots;
		std::vector<std::string> g_order;   // first-seen order, which is the list order in the UI
		std::unordered_map<std::string, Manager::Device*> g_devicesById;
		std::vector<std::unique_ptr<Manager::Device>> g_devices;
		std::unordered_map<std::string, Profile> g_profiles;
		uint32_t g_words[2][2] = {};   // [side][0 menus, 1 battle]
		bool g_hasDevice[2] = {};

		std::string g_captureId;
		ControlSet g_captureBaseline;
		bool g_captureBaselineTaken = false;
		int g_capturedControl = -1;

		std::wstring ProfilePath()
		{
			return GamePathW(L"BBCF_IM\\controllers.ini");
		}

		const char kHex[] = "0123456789ABCDEF";

		std::string ToHex(const uint8_t* bytes, size_t count)
		{
			std::string out;
			for (size_t i = 0; i < count; ++i)
			{
				out += kHex[bytes[i] >> 4];
				out += kHex[bytes[i] & 15];
			}
			return out;
		}

		bool FromHex(const std::string& text, uint8_t* bytes, size_t count)
		{
			if (text.size() != count * 2)
				return false;
			for (size_t i = 0; i < count; ++i)
			{
				const int hi = isxdigit(static_cast<unsigned char>(text[i * 2])) ? std::stoi(text.substr(i * 2, 1), nullptr, 16) : -1;
				const int lo = isxdigit(static_cast<unsigned char>(text[i * 2 + 1])) ? std::stoi(text.substr(i * 2 + 1, 1), nullptr, 16) : -1;
				if (hi < 0 || lo < 0)
					return false;
				bytes[i] = static_cast<uint8_t>(hi * 16 + lo);
			}
			return true;
		}

		// The device's game Button Settings: its own, or what the game had at load.
		GameBinds EffectiveBinds(const Profile* profile, DeviceKind kind, Side side)
		{
			const GameKeyConfig::BindClass cls = GameKeyConfig::ClassOf(kind);
			if (profile && profile->hasBinds)
			{
				GameBinds binds = profile->binds;
				GameKeyConfig::Sanitize(binds, cls);
				return binds;
			}
			return GameKeyConfig::Inherited(cls, side);
		}

		uint32_t MapControls(const ControlSet& controls, const Mapping& mapping)
		{
			uint32_t word = 0;
			for (const auto& entry : mapping)
			{
				if (entry.first >= 0 && entry.first < kMaxControls && controls[entry.first])
					word |= entry.second;
			}
			return word;
		}
	}

	Manager& Manager::Get()
	{
		static Manager instance;
		return instance;
	}

	Mapping Manager::DefaultMapping(DeviceKind kind)
	{
		Mapping m;
		switch (kind)
		{
		case DeviceKind::Keyboard:
			// The game's own keyboard defaults (WASD, U I O J, K L P =, Enter, Backspace), plus
			// the arrows its hard-wired menu fallbacks accept.
			m['W'] = Btn_Up; m['S'] = Btn_Down; m['A'] = Btn_Left; m['D'] = Btn_Right;
			m[VK_UP] = Btn_Up; m[VK_DOWN] = Btn_Down; m[VK_LEFT] = Btn_Left; m[VK_RIGHT] = Btn_Right;
			m['U'] = Btn_A; m['I'] = Btn_B; m['O'] = Btn_C; m['J'] = Btn_D;
			m['L'] = Btn_Taunt; m[VK_OEM_PLUS] = Btn_Special; m['K'] = Btn_Fn1; m['P'] = Btn_Fn2;
			m[VK_RETURN] = Btn_Start; m[VK_BACK] = Btn_Reset;
			break;
		case DeviceKind::XInputPad:
			// The game's own pad defaults: X=A, Y=B, B=C, A=D (so A confirms and B backs out),
			// LB=Fn1, RB=Taunt, LT=Fn2, RT=Special.
			m[2] = Btn_A; m[3] = Btn_B; m[1] = Btn_C; m[0] = Btn_D;
			m[4] = Btn_Fn1; m[5] = Btn_Taunt; m[14] = Btn_Fn2; m[15] = Btn_Special;
			m[7] = Btn_Start; m[6] = Btn_Reset;
			m[10] = Btn_Up; m[11] = Btn_Down; m[12] = Btn_Left; m[13] = Btn_Right;
			m[16] = Btn_Up; m[17] = Btn_Down; m[18] = Btn_Left; m[19] = Btn_Right;
			break;
		case DeviceKind::DInputPad:
			// PlayStation-style order, which most sticks and generic pads follow:
			// 0 Square, 1 Cross, 2 Circle, 3 Triangle, 4 L1, 5 R1, 6 L2, 7 R2, 8 Share, 9 Options.
			m[0] = Btn_A; m[3] = Btn_B; m[2] = Btn_C; m[1] = Btn_D;
			m[4] = Btn_Fn1; m[5] = Btn_Taunt; m[6] = Btn_Fn2; m[7] = Btn_Special;
			m[9] = Btn_Start; m[8] = Btn_Reset;
			m[kDInputPovBase + 0] = Btn_Up; m[kDInputPovBase + 1] = Btn_Right;
			m[kDInputPovBase + 2] = Btn_Down; m[kDInputPovBase + 3] = Btn_Left;
			m[kDInputAxisBase + 0] = Btn_Left; m[kDInputAxisBase + 1] = Btn_Right;   // X axis
			m[kDInputAxisBase + 2] = Btn_Up; m[kDInputAxisBase + 3] = Btn_Down;      // Y axis
			break;
		}
		return m;
	}

	std::string Manager::ControlName(DeviceKind kind, int control)
	{
		char buf[64];
		switch (kind)
		{
		case DeviceKind::Keyboard:
		{
			UINT scan = MapVirtualKeyW(static_cast<UINT>(control), MAPVK_VK_TO_VSC);
			switch (control)
			{
			case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
			case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
			case VK_RCONTROL: case VK_RMENU:
				scan |= 0x100; // extended
				break;
			}
			char name[64] = {};
			if (scan && GetKeyNameTextA(static_cast<LONG>(scan << 16), name, sizeof(name)) > 0)
				return name;
			snprintf(buf, sizeof(buf), "Key %d", control);
			return buf;
		}
		case DeviceKind::XInputPad:
		{
			static const char* const names[kXInputControlCount] = {
				"A", "B", "X", "Y", "LB", "RB", "Back", "Start", "L3", "R3",
				"D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "LT", "RT",
				"Left stick Up", "Left stick Down", "Left stick Left", "Left stick Right",
				"Right stick Up", "Right stick Down", "Right stick Left", "Right stick Right",
			};
			return control >= 0 && control < kXInputControlCount ? names[control] : "?";
		}
		case DeviceKind::DInputPad:
		{
			if (control < kDInputButtonCount)
			{
				snprintf(buf, sizeof(buf), "Button %d", control + 1);
				return buf;
			}
			if (control >= kDInputPovBase && control < kDInputPovBase + kDInputPovHats * 4)
			{
				static const char* const dirs[4] = { "Up", "Right", "Down", "Left" };
				const int hat = (control - kDInputPovBase) / 4;
				if (hat == 0)
					snprintf(buf, sizeof(buf), "D-pad %s", dirs[(control - kDInputPovBase) % 4]);
				else
					snprintf(buf, sizeof(buf), "Hat %d %s", hat + 1, dirs[(control - kDInputPovBase) % 4]);
				return buf;
			}
			if (control >= kDInputAxisBase && control < kDInputAxisBase + kDInputAxisCount * 2)
			{
				static const char* const axes[kDInputAxisCount] = { "X", "Y", "Z", "Rx", "Ry", "Rz", "Slider 1", "Slider 2" };
				const int axis = (control - kDInputAxisBase) / 2;
				snprintf(buf, sizeof(buf), "%s axis %s", axes[axis], ((control - kDInputAxisBase) % 2) ? "+" : "-");
				return buf;
			}
			return "?";
		}
		}
		return "?";
	}

	// ---------------------------------------------------------------- profiles

	void Manager::LoadProfiles()
	{
		std::ifstream file(ProfilePath());
		if (!file)
			return;
		std::string line;
		Profile* current = nullptr;
		while (std::getline(file, line))
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
				line.pop_back();
			if (line.empty() || line[0] == ';')
				continue;
			if (line[0] == '[' && line.back() == ']')
			{
				current = &g_profiles[line.substr(1, line.size() - 2)];
				continue;
			}
			if (!current)
				continue;
			const size_t eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			const std::string key = line.substr(0, eq);
			const std::string value = line.substr(eq + 1);
			if (key == "name")
				current->name = value;
			else if (key == "side")
			{
				current->pinned = true;
				current->pinnedSide = value == "2" ? Side_P2 : Side_P1;
			}
			else if (key == "seen")
				current->seen = value == "1";
			else if (key == "hidden")
				current->ignored = value == "1";
			else if (key == "binds")
			{
				uint8_t bytes[80];
				if (FromHex(value, bytes, sizeof(bytes)))
				{
					current->hasBinds = true;
					std::memcpy(current->binds.setB, bytes, 40);
					std::memcpy(current->binds.setA, bytes + 40, 40);
				}
			}
			else if (key == "typeb")
				current->binds.typeB = value == "1" ? 1 : 0;
			else if (key == "menuset")
				current->binds.menuSet = value == "1" ? 1 : 0;
			else if (key == "map")
			{
				current->hasMapping = true;
				current->mapping.clear();
				std::stringstream items(value);
				std::string item;
				while (std::getline(items, item, ','))
				{
					const size_t colon = item.find(':');
					if (colon == std::string::npos)
						continue;
					const int control = atoi(item.substr(0, colon).c_str());
					const uint32_t buttons = static_cast<uint32_t>(strtoul(item.substr(colon + 1).c_str(), nullptr, 16));
					if (control >= 0 && control < kMaxControls && buttons)
						current->mapping[control] = buttons;
				}
			}
		}
	}

	void Manager::SaveProfiles() const
	{
		CreateDirectoryW(GamePathW(L"BBCF_IM").c_str(), nullptr);
		const std::wstring path = ProfilePath();
		const std::wstring temp = path + L".tmp";
		{
			std::ofstream file(temp, std::ios::trunc);
			if (!file)
				return;
			file << "; Controllers known to the mod's local input system. Edited from the mod menu.\n";
			for (const auto& entry : g_profiles)
			{
				const Profile& p = entry.second;
				if (p.name.empty() && !p.pinned && !p.seen && !p.hasMapping && !p.ignored && !p.hasBinds)
					continue;
				file << "[" << entry.first << "]\n";
				if (!p.name.empty())
					file << "name=" << p.name << "\n";
				if (p.pinned)
					file << "side=" << (p.pinnedSide == Side_P2 ? 2 : 1) << "\n";
				if (p.seen)
					file << "seen=1\n";
				if (p.ignored)
					file << "hidden=1\n";
				if (p.hasBinds)
				{
					uint8_t bytes[80];
					std::memcpy(bytes, p.binds.setB, 40);
					std::memcpy(bytes + 40, p.binds.setA, 40);
					file << "binds=" << ToHex(bytes, sizeof(bytes)) << "\n";
					file << "typeb=" << (p.binds.typeB ? 1 : 0) << "\n";
					file << "menuset=" << (p.binds.menuSet ? 1 : 0) << "\n";
				}
				if (p.hasMapping)
				{
					file << "map=";
					bool first = true;
					for (const auto& m : p.mapping)
					{
						char item[32];
						snprintf(item, sizeof(item), "%s%d:%X", first ? "" : ",", m.first, m.second);
						file << item;
						first = false;
					}
					file << "\n";
				}
			}
		}
		MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
	}

	// ---------------------------------------------------------------- lifecycle

	void Manager::Start()
	{
		if (m_active)
			return;
		if (!Settings::settingsIni.newControllerSystem || !IsControllerHooksRuntimeAllowed())
		{
			LOG(1, "[LocalInput] not started (setting off or controller hooks not allowed here)\n");
			return;
		}
		if (!InstallGameInputBridge())
			return;
		GameKeyConfig::Install();
		std::lock_guard<std::mutex> lock(g_mutex);
		LoadProfiles();
		g_backend = CreateWindowsInputBackend();
		g_backend->SetAutoDetect(Settings::settingsIni.autoUpdateControllers);
		g_backend->Start();
		m_active = true;
		LOG(1, "[LocalInput] started with backend: %s\n", g_backend->Name());
	}

	void Manager::Shutdown()
	{
		if (!m_active)
			return;
		std::lock_guard<std::mutex> lock(g_mutex);
		m_active = false;
		if (g_backend)
		{
			g_backend->Stop();
			g_backend.reset();
		}
	}

	void Manager::OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (!m_active || !g_backend)
			return;
		if (msg == WM_ACTIVATEAPP || msg == WM_KILLFOCUS || msg == WM_SETFOCUS || msg == WM_ACTIVATE)
		{
			bool focused = true;
			if (msg == WM_ACTIVATEAPP) focused = wParam != FALSE;
			else if (msg == WM_KILLFOCUS) focused = false;
			else if (msg == WM_SETFOCUS) focused = true;
			else focused = LOWORD(wParam) != WA_INACTIVE;
			static int lastFocused = -1;
			if (lastFocused != (focused ? 1 : 0))
			{
				lastFocused = focused ? 1 : 0;
				LOG(1, "[LocalInput] game window %s: keyboards %s, pads keep working\n",
					focused ? "focused" : "lost focus", focused ? "live again" : "released and ignored");
			}
			g_backend->OnFocusChanged(focused);
		}
		g_backend->OnWindowMessage(hwnd, msg, wParam, lParam);
	}

	// ---------------------------------------------------------------- per frame

	void Manager::Tick(bool fromGameHook)
	{
		if (!m_active)
			return;
		const ULONGLONG now = GetTickCount64();
		if (now - m_lastTickMs < (fromGameHook ? kMinGameTickMs : kMinUiTickMs))
			return;
		m_lastTickMs = now;

		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_backend)
			return;
		g_backend->Poll(g_snapshots);
		ApplySnapshots();
		if (fromGameHook)
			SyncGameKeyConfig();
		RecomputeWords();
	}

	Side Manager::ChooseSide(const Device& device) const
	{
		auto profile = g_profiles.find(device.snap.id);
		if (profile != g_profiles.end() && profile->second.pinned)
			return profile->second.pinnedSide;
		if (device.snap.kind == DeviceKind::Keyboard)
			return Side_P1;
		int pads[2] = {};
		for (const auto& other : g_devices)
		{
			if (other.get() == &device || !other->sessionKnown || !other->snap.connected ||
				other->snap.kind == DeviceKind::Keyboard)
				continue;
			auto otherProfile = g_profiles.find(other->snap.id);
			if (otherProfile != g_profiles.end() && otherProfile->second.ignored)
				continue; // hidden devices (vJoy...) do not take a side's pad spot
			pads[other->side]++;
		}
		if (pads[Side_P1] == 0)
			return Side_P1;
		if (pads[Side_P2] == 0)
			return Side_P2;
		return pads[Side_P2] < pads[Side_P1] ? Side_P2 : Side_P1;
	}

	void Manager::ApplySnapshots()
	{
		const ULONGLONG now = GetTickCount64();
		for (const DeviceSnapshot& snap : g_snapshots)
		{
			Device* device = nullptr;
			auto it = g_devicesById.find(snap.id);
			if (it == g_devicesById.end())
			{
				g_devices.push_back(std::unique_ptr<Device>(new Device()));
				device = g_devices.back().get();
				g_devicesById[snap.id] = device;
				g_order.push_back(snap.id);
			}
			else
			{
				device = it->second;
			}
			const bool wasConnected = device->snap.connected;
			device->snap = snap;

			// Sides are decided the first time a device shows up connected this session, and
			// kept from then on - an unplug/replug lands it back where it was.
			if (snap.connected && !device->sessionKnown)
			{
				device->side = ChooseSide(*device);
				device->sessionKnown = true;
				LOG(1, "[LocalInput] %s '%s' -> P%d\n", snap.id.c_str(), snap.defaultName.c_str(), device->side + 1);
			}
			else if (snap.connected != wasConnected && device->sessionKnown)
			{
				LOG(1, "[LocalInput] %s %s (stays on P%d)\n", snap.id.c_str(),
					snap.connected ? "reconnected" : "disconnected", device->side + 1);
			}

			if (snap.connected && snap.controls.any())
			{
				device->lastInputMs = now;
				if (snap.kind == DeviceKind::Keyboard)
				{
					Profile& profile = g_profiles[snap.id];
					if (!profile.seen)
					{
						profile.seen = true;
						SaveProfiles();
					}
				}
			}
		}
	}

	void Manager::RecomputeWords()
	{
		uint32_t words[2][2] = {};
		bool hasDevice[2] = {};
		const bool capturing = !g_captureId.empty();
		for (const auto& device : g_devices)
		{
			device->buttons = 0;
			if (!device->snap.connected || !device->sessionKnown)
				continue;
			auto profile = g_profiles.find(device->snap.id);
			if (profile != g_profiles.end() && profile->second.hasMapping)
			{
				// The mod's own mapping replaces the game's Button Settings for this device.
				device->buttons = MapControls(device->snap.controls, profile->second.mapping);
				device->menuWord = device->buttons;
			}
			else
			{
				const GameKeyConfig::BindClass cls = GameKeyConfig::ClassOf(device->snap.kind);
				const GameBinds binds = EffectiveBinds(profile != g_profiles.end() ? &profile->second : nullptr,
					device->snap.kind, device->side);
				const GameKeyConfig::HeldKeys held = GameKeyConfig::HeldGameKeys(device->snap.kind, device->snap.controls);
				device->buttons = GameKeyConfig::Word(cls, held, binds, true, device->side);
				device->menuWord = GameKeyConfig::Word(cls, held, binds, false, device->side);
			}

			// A keyboard that has never been typed on is most likely not a keyboard anyone
			// plays on (media keys, a mouse's macro keys), so it does not make a side "present".
			if (profile != g_profiles.end() && profile->second.ignored)
				continue; // hidden: shown apart, never reaches the game
			const bool counts = device->snap.kind != DeviceKind::Keyboard ||
				(profile != g_profiles.end() && (profile->second.seen || profile->second.pinned));
			if (counts)
				hasDevice[device->side] = true;

			if (capturing)
				continue; // rebinding: nothing reaches the game
			if (device->snap.kind == DeviceKind::Keyboard && m_overlayWantsKeyboard)
				continue; // typing into the overlay
			words[device->side][0] |= device->menuWord;
			words[device->side][1] |= device->buttons;
		}

		if (capturing)
		{
			auto it = g_devicesById.find(g_captureId);
			if (it != g_devicesById.end() && it->second->snap.connected)
			{
				const ControlSet& now = it->second->snap.controls;
				if (!g_captureBaselineTaken)
				{
					g_captureBaseline = now;
					g_captureBaselineTaken = true;
				}
				else if (g_capturedControl < 0)
				{
					for (int c = 0; c < kMaxControls; ++c)
					{
						if (now[c] && !g_captureBaseline[c])
						{
							g_capturedControl = c;
							break;
						}
					}
					g_captureBaseline &= now; // a control released and pressed again counts
				}
			}
		}

		for (int side = 0; side < 2; ++side)
			for (int set = 0; set < 2; ++set)
				g_words[side][set] = CleanDirections(words[side][set]);
		g_hasDevice[0] = hasDevice[0];
		g_hasDevice[1] = hasDevice[1];
	}

	uint32_t Manager::PlayerWord(Side side, bool battle) const
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_words[side][battle ? 1 : 0];
	}

	void Manager::SyncGameKeyConfig()
	{
		std::vector<GameKeyConfig::DeviceRef> refs;
		refs.reserve(g_devices.size());
		for (const auto& device : g_devices)
		{
			if (!device->sessionKnown)
				continue;
			auto profile = g_profiles.find(device->snap.id);
			const Profile* p = profile != g_profiles.end() ? &profile->second : nullptr;
			GameKeyConfig::DeviceRef ref;
			ref.id = device->snap.id;
			ref.kind = device->snap.kind;
			ref.side = device->side;
			ref.usable = device->snap.connected && !(p && p->ignored);
			ref.idle = device->snap.kind == DeviceKind::Keyboard && !(p && (p->seen || p->pinned));
			ref.lastInputMs = device->lastInputMs;
			ref.controls = device->snap.controls;
			ref.binds = EffectiveBinds(p, device->snap.kind, device->side);
			refs.push_back(ref);
		}
		std::vector<std::pair<std::string, GameBinds>> changed;
		GameKeyConfig::Frame(refs, changed);
		if (changed.empty())
			return;
		for (const auto& entry : changed)
		{
			Profile& profile = g_profiles[entry.first];
			profile.hasBinds = true;
			profile.binds = entry.second;
		}
		SaveProfiles();
	}

	bool Manager::PlayerHasDevice(Side side) const
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_hasDevice[side];
	}

	// ---------------------------------------------------------------- UI

	std::vector<DeviceView> Manager::Devices() const
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		std::vector<DeviceView> out;
		for (const std::string& id : g_order)
		{
			const Device* device = g_devicesById[id];
			if (!device->sessionKnown)
				continue; // never seen connected this session
			DeviceView view;
			view.id = id;
			view.defaultName = device->snap.defaultName;
			auto profile = g_profiles.find(id);
			view.name = (profile != g_profiles.end() && !profile->second.name.empty())
				? profile->second.name : device->snap.defaultName;
			view.kind = device->snap.kind;
			view.connected = device->snap.connected;
			view.side = device->side;
			view.pinned = profile != g_profiles.end() && profile->second.pinned;
			view.idle = device->snap.kind == DeviceKind::Keyboard &&
				!(profile != g_profiles.end() && (profile->second.seen || profile->second.pinned));
			view.ignored = profile != g_profiles.end() && profile->second.ignored;
			view.lastInputMs = device->lastInputMs;
			view.controls = device->snap.controls;
			view.buttons = device->buttons;
			view.axisCount = device->snap.axisCount;
			std::memcpy(view.axes, device->snap.axes, sizeof(view.axes));
			out.push_back(view);
		}
		return out;
	}

	void Manager::Assign(const std::string& id, Side side)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Profile& profile = g_profiles[id];
		profile.pinned = true;
		profile.pinnedSide = side;
		// Putting a device on a side is using it: a hidden device (dragged out of the hidden
		// list) is shown again, and an unused keyboard counts as used.
		profile.ignored = false;
		profile.seen = true;
		auto it = g_devicesById.find(id);
		if (it != g_devicesById.end())
			it->second->side = side;
		SaveProfiles();
		LOG(1, "[LocalInput] %s pinned to P%d\n", id.c_str(), side + 1);
	}

	void Manager::ResetAssignments()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (auto& entry : g_profiles)
			entry.second.pinned = false;
		for (auto& device : g_devices)
			device->sessionKnown = false;
		// Re-run the defaults in the order devices were first seen.
		for (const std::string& id : g_order)
		{
			Device* device = g_devicesById[id];
			if (!device->snap.connected)
				continue;
			device->side = ChooseSide(*device);
			device->sessionKnown = true;
		}
		SaveProfiles();
	}

	void Manager::Rename(const std::string& id, const std::string& name)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_profiles[id].name = name;
		SaveProfiles();
	}

	Mapping Manager::GetMapping(const std::string& id) const
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto profile = g_profiles.find(id);
		if (profile != g_profiles.end() && profile->second.hasMapping)
			return profile->second.mapping;
		auto it = g_devicesById.find(id);
		if (it == g_devicesById.end())
			return Mapping();
		const DeviceKind kind = it->second->snap.kind;
		return GameKeyConfig::EffectiveMapping(kind,
			EffectiveBinds(profile != g_profiles.end() ? &profile->second : nullptr, kind, it->second->side), it->second->side);
	}

	bool Manager::HasCustomMapping(const std::string& id) const
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto profile = g_profiles.find(id);
		return profile != g_profiles.end() && profile->second.hasMapping;
	}

	bool Manager::HasOwnGameBinds(const std::string& id) const
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto profile = g_profiles.find(id);
		return profile != g_profiles.end() && profile->second.hasBinds;
	}

	void Manager::SetMapping(const std::string& id, const Mapping& mapping)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Profile& profile = g_profiles[id];
		profile.hasMapping = true;
		profile.mapping = mapping;
		SaveProfiles();
	}

	void Manager::ResetMapping(const std::string& id)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Profile& profile = g_profiles[id];
		profile.hasMapping = false;
		profile.mapping.clear();
		SaveProfiles();
	}

	void Manager::BeginCapture(const std::string& id)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_captureId = id;
		g_captureBaselineTaken = false;
		g_capturedControl = -1;
	}

	void Manager::EndCapture()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_captureId.clear();
		g_capturedControl = -1;
	}

	bool Manager::PollCapture(int* control)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_capturedControl < 0)
			return false;
		*control = g_capturedControl;
		g_capturedControl = -1;
		g_captureBaselineTaken = false;
		return true;
	}

	void Manager::SetIgnored(const std::string& id, bool ignored)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Profile& profile = g_profiles[id];
		profile.ignored = ignored;
		if (!ignored)
			profile.seen = true; // showing an unused keyboard means someone plays on it
		SaveProfiles();
		LOG(1, "[LocalInput] %s %s\n", id.c_str(), ignored ? "hidden" : "shown");
	}

	void Manager::SetAutoDetect(bool enabled)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_backend)
			g_backend->SetAutoDetect(enabled);
	}

	bool Manager::AutoDetect() const
	{
		return Settings::settingsIni.autoUpdateControllers;
	}

	void Manager::Refresh()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_backend)
			g_backend->Refresh();
	}

	bool Manager::SteamInputLikely() const
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_backend && g_backend->SteamInputLikely();
	}
}
