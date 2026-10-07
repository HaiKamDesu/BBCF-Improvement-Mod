// Windows device backend: XInput pads, DirectInput pads/sticks, and every keyboard separately
// through Raw Input. See InputBackend.h.
//
// The rules this file exists to keep, each one a past failure of the game or of the mod:
//   - Never walk the device tree on the game thread. EnumDevices has measured 100 ms on a
//     machine with many virtual devices (InputDevices.h); it runs on a worker here, and the
//     game thread only ever polls devices that are already open.
//   - XInput is polled by the user index the pad is actually on (the game polls its slot
//     number instead, which kills a lone pad on index 1).
//   - All four XInput indices, any number of DirectInput devices. Nothing is capped at two.
//   - Hot-plug is continuous: WM_DEVICECHANGE wakes the worker at once, and it also re-checks
//     on its own every few seconds in case that message never arrives.
//   - An empty or failing device reports disconnected, never "connected with no buttons".
//   - Keyboards only count while the game window has focus, and every key is released the
//     moment focus goes, so nothing stays held across an alt-tab.

#include "InputBackend.h"
#include "DeviceModels.h"

#include "Core/dllmain.h"
#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/XInputRuntime.h"

#include <dinput.h>
#include <Xinput.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace LocalInput
{
	namespace
	{
		constexpr DWORD kXInputEmptyRecheckMs = 1000;   // XInputGetState on an empty index is slow
		constexpr DWORD kEnumSafetyNetMs = 4000;        // worker re-enumerates even without a message
		constexpr LONG kAxisThreshold = 500;            // half-press on the normalized -1000..1000 range
		constexpr SHORT kStickThreshold = 16000;        // XInput sticks, ~half travel
		constexpr BYTE kTriggerThreshold = 100;

		const WORD kXInputButtonMasks[16] = {
			XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
			XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
			XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
			XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
			XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
			0, 0, // LT, RT
		};

		std::string Narrow(const wchar_t* text)
		{
			if (!text || !*text)
				return std::string();
			const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
			std::string out(size > 0 ? size - 1 : 0, '\0');
			if (size > 1)
				WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], size, nullptr, nullptr);
			return out;
		}

		std::string GuidString(const GUID& guid)
		{
			char buf[48];
			snprintf(buf, sizeof(buf), "%08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X",
				static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3,
				guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
				guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
			return buf;
		}

		uint32_t Fnv1a(const std::wstring& text)
		{
			uint32_t hash = 2166136261u;
			for (wchar_t c : text)
			{
				hash ^= static_cast<uint32_t>(towlower(c));
				hash *= 16777619u;
			}
			return hash;
		}

		HWND GameWindow()
		{
			return g_gameProc.hWndGameWindow;
		}

		// ---------------------------------------------------------------- XInput

		struct XPad
		{
			bool connected = false;
			bool everChecked = false;
			DWORD lastEmptyCheck = 0;
			uint8_t subType = 0;   // asked once per connection
			ControlSet controls;
			int16_t axes[6] = {};
		};

		// ---------------------------------------------------------------- DirectInput

		struct DIFound
		{
			GUID instance{};
			std::string name;
			uint16_t vendorId = 0, productId = 0;
			IDirectInputDevice8W* device = nullptr; // opened by the worker, handed over
		};

		struct DIPad
		{
			std::string id;
			std::string name;
			uint16_t vendorId = 0, productId = 0;
			IDirectInputDevice8W* device = nullptr;
			bool connected = false;
			bool axisPresent[kDInputAxisCount] = {};
			LONG axisMin[kDInputAxisCount] = {};
			LONG axisMax[kDInputAxisCount] = {};
			DWORD lastAcquireTry = 0;
			ControlSet controls;
			int16_t axes[kDInputAxisCount] = {};
			// Where each axis rests, taken from the first reading. Sticks rest in the middle,
			// but triggers and unused axes on DirectInput devices often rest at one end; without
			// this they would read as held forever.
			bool restCaptured = false;
			LONG rest[kDInputAxisCount] = {};
		};

		LONG AxisValue(const DIJOYSTATE2& js, int axis)
		{
			switch (axis)
			{
			case 0: return js.lX;
			case 1: return js.lY;
			case 2: return js.lZ;
			case 3: return js.lRx;
			case 4: return js.lRy;
			case 5: return js.lRz;
			case 6: return js.rglSlider[0];
			case 7: return js.rglSlider[1];
			default: return 0;
			}
		}

		int AxisIndex(DWORD ofs)
		{
			switch (ofs)
			{
			case DIJOFS_X: return 0;
			case DIJOFS_Y: return 1;
			case DIJOFS_Z: return 2;
			case DIJOFS_RX: return 3;
			case DIJOFS_RY: return 4;
			case DIJOFS_RZ: return 5;
			case DIJOFS_SLIDER(0): return 6;
			case DIJOFS_SLIDER(1): return 7;
			default: return -1;
			}
		}

		struct AxisSetup
		{
			IDirectInputDevice8W* device;
			DIPad* pad;
		};

		BOOL CALLBACK EnumAxes(LPCDIDEVICEOBJECTINSTANCEW object, LPVOID context)
		{
			AxisSetup* setup = static_cast<AxisSetup*>(context);
			const int axis = AxisIndex(object->dwOfs);
			if (axis < 0)
				return DIENUM_CONTINUE;
			DIPROPRANGE range = {};
			range.diph.dwSize = sizeof(range);
			range.diph.dwHeaderSize = sizeof(range.diph);
			range.diph.dwHow = DIPH_BYID;
			range.diph.dwObj = object->dwType;
			range.lMin = -1000;
			range.lMax = 1000;
			setup->device->SetProperty(DIPROP_RANGE, &range.diph);
			DIPROPRANGE actual = range;
			if (SUCCEEDED(setup->device->GetProperty(DIPROP_RANGE, &actual.diph)) && actual.lMax > actual.lMin)
			{
				setup->pad->axisPresent[axis] = true;
				setup->pad->axisMin[axis] = actual.lMin;
				setup->pad->axisMax[axis] = actual.lMax;
			}
			return DIENUM_CONTINUE;
		}

		// "IG_" in the HID path marks an XInput pad, which XInput already covers.
		bool IsXInputDevice(IDirectInputDevice8W* device)
		{
			DIPROPGUIDANDPATH path = {};
			path.diph.dwSize = sizeof(path);
			path.diph.dwHeaderSize = sizeof(path.diph);
			path.diph.dwHow = DIPH_DEVICE;
			if (FAILED(device->GetProperty(DIPROP_GUIDANDPATH, &path.diph)))
				return false;
			for (int i = 0; path.wszPath[i] && path.wszPath[i + 1] && i < MAX_PATH - 2; ++i)
			{
				if ((path.wszPath[i] == L'i' || path.wszPath[i] == L'I') &&
					(path.wszPath[i + 1] == L'g' || path.wszPath[i + 1] == L'G') && path.wszPath[i + 2] == L'_')
					return true;
			}
			return false;
		}

		// ---------------------------------------------------------------- Keyboards

		struct Keyboard
		{
			std::string id;
			std::string name;
			HANDLE handle = nullptr;   // raw input device handle, null for the virtual keyboard
			bool connected = true;
			ControlSet keys;
		};

		typedef BOOLEAN(__stdcall* HidD_GetProductStringFn)(HANDLE, PVOID, ULONG);

		std::string KeyboardProductName(const std::wstring& path)
		{
			static HMODULE hid = LoadLibraryW(L"hid.dll");
			static HidD_GetProductStringFn getProduct = hid
				? reinterpret_cast<HidD_GetProductStringFn>(GetProcAddress(hid, "HidD_GetProductString")) : nullptr;
			if (!getProduct || path.empty())
				return std::string();
			HANDLE file = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return std::string();
			wchar_t name[128] = {};
			const bool ok = getProduct(file, name, sizeof(name)) != FALSE;
			CloseHandle(file);
			return ok ? Narrow(name) : std::string();
		}

		std::wstring RawDeviceName(HANDLE handle)
		{
			UINT size = 0;
			if (GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, nullptr, &size) != 0 || size == 0)
				return std::wstring();
			std::wstring name(size, L'\0');
			if (GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, &name[0], &size) == static_cast<UINT>(-1))
				return std::wstring();
			while (!name.empty() && name.back() == L'\0')
				name.pop_back();
			return name;
		}

		class WindowsInputBackend final : public IInputBackend
		{
		public:
			const char* Name() const override { return "Windows (XInput + DirectInput + Raw Input)"; }

			// At process exit the worker has already been killed by the OS, and this runs from
			// static destruction under the loader lock: never join here, and never let a joinable
			// std::thread reach its destructor (that is std::terminate).
			~WindowsInputBackend() override
			{
				if (m_worker.joinable())
					m_worker.detach();
			}

			void Start() override
			{
				m_running = true;
				m_worker = std::thread([this] { WorkerLoop(); });
				EnsureRawKeyboard();
				LOG(1, "[LocalInput] Windows backend started\n");
			}

			void Stop() override
			{
				{
					std::lock_guard<std::mutex> lock(m_workerMutex);
					m_running = false;
				}
				m_workerWake.notify_all();
				if (m_worker.joinable())
					m_worker.join();
				std::lock_guard<std::mutex> lock(m_handoffMutex);
				for (DIPad& pad : m_diPads)
					ReleasePad(pad);
				m_diPads.clear();
				for (DIFound& found : m_pendingFound)
					if (found.device) found.device->Release();
				m_pendingFound.clear();
			}

			void Poll(std::vector<DeviceSnapshot>& out) override
			{
				out.clear();
				EnsureRawKeyboard();
				PollXInput(out);
				AdoptWorkerResults();
				PollDirectInput(out);
				ReportKeyboards(out);
			}

			bool OnWindowMessage(HWND, UINT msg, WPARAM wParam, LPARAM lParam) override
			{
				switch (msg)
				{
				case WM_INPUT:
					OnRawInput(reinterpret_cast<HRAWINPUT>(lParam));
					return false; // DefWindowProc must still see it
				case WM_INPUT_DEVICE_CHANGE:
					OnKeyboardDeviceChange(reinterpret_cast<HANDLE>(lParam), wParam == GIDC_ARRIVAL);
					WakeWorker();
					return false;
				case WM_DEVICECHANGE:
					if (m_autoDetect)
					{
						WakeWorker();
						m_xinputRecheckAll = true;
					}
					return false;
				default:
					return false;
				}
			}

			void SetAutoDetect(bool enabled) override
			{
				m_autoDetect = enabled;
				LOG(1, "[LocalInput] automatic controller detection %s\n", enabled ? "on" : "off");
			}

			void Refresh() override
			{
				LOG(1, "[LocalInput] refresh requested\n");
				m_xinputRecheckAll = true;
				ListKeyboards();
				WakeWorker();
			}

			bool SteamInputLikely() const override
			{
				return m_steamInput;
			}

			void OnFocusChanged(bool focused) override
			{
				m_focused = focused;
				if (!focused)
				{
					std::lock_guard<std::mutex> lock(m_keyboardMutex);
					for (auto& entry : m_keyboards)
						entry.second.keys.reset();
				}
			}

		private:
			// ---------------- XInput

			void PollXInput(std::vector<DeviceSnapshot>& out)
			{
				const DWORD now = GetTickCount();
				const bool recheckAll = m_xinputRecheckAll.exchange(false);
				for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index)
				{
					XPad& pad = m_xpads[index];
					// Empty indices are slow to ask; ask them once a second, staggered, unless a
					// device change just happened.
					const bool due = recheckAll || !pad.everChecked || pad.connected ||
						(m_autoDetect && (now - pad.lastEmptyCheck) >= kXInputEmptyRecheckMs + index * 50);
					if (due)
					{
						XINPUT_STATE state = {};
						const bool connected = XInputRuntime::GetState(index, &state) == ERROR_SUCCESS;
						pad.everChecked = true;
						if (!connected)
						{
							if (pad.connected)
								LOG(1, "[LocalInput] XInput pad %lu disconnected\n", index);
							pad.connected = false;
							pad.controls.reset();
							pad.lastEmptyCheck = now;
						}
						else
						{
							if (!pad.connected)
							{
								XINPUT_CAPABILITIES caps = {};
								pad.subType = XInputRuntime::GetCapabilities(index, &caps) == ERROR_SUCCESS ? caps.SubType : 0;
								LOG(1, "[LocalInput] XInput pad %lu connected (subtype %u)\n", index, pad.subType);
							}
							pad.connected = true;
							Decode(state.Gamepad, pad);
						}
					}

					if (!pad.everChecked)
						continue;
					DeviceSnapshot snap;
					snap.id = "XI:" + std::to_string(index);
					const bool stick = pad.subType == kXInputSubTypeArcadeStick || pad.subType == kXInputSubTypeArcadePad;
					snap.defaultName = (stick ? "Xbox arcade stick " : "Xbox controller ") + std::to_string(index + 1);
					snap.kind = DeviceKind::XInputPad;
					snap.subType = pad.subType;
					snap.connected = pad.connected;
					snap.controls = pad.controls;
					snap.axisCount = 6;
					std::memcpy(snap.axes, pad.axes, sizeof(pad.axes));
					out.push_back(snap);
				}
			}

			static void Decode(const XINPUT_GAMEPAD& g, XPad& pad)
			{
				pad.controls.reset();
				for (int i = 0; i < 14; ++i)
					pad.controls[i] = (g.wButtons & kXInputButtonMasks[i]) != 0;
				pad.controls[14] = g.bLeftTrigger > kTriggerThreshold;
				pad.controls[15] = g.bRightTrigger > kTriggerThreshold;
				pad.controls[16] = g.sThumbLY > kStickThreshold;
				pad.controls[17] = g.sThumbLY < -kStickThreshold;
				pad.controls[18] = g.sThumbLX < -kStickThreshold;
				pad.controls[19] = g.sThumbLX > kStickThreshold;
				pad.controls[20] = g.sThumbRY > kStickThreshold;
				pad.controls[21] = g.sThumbRY < -kStickThreshold;
				pad.controls[22] = g.sThumbRX < -kStickThreshold;
				pad.controls[23] = g.sThumbRX > kStickThreshold;
				pad.axes[0] = static_cast<int16_t>(g.sThumbLX * 1000 / 32767);
				pad.axes[1] = static_cast<int16_t>(-g.sThumbLY * 1000 / 32767);
				pad.axes[2] = static_cast<int16_t>(g.sThumbRX * 1000 / 32767);
				pad.axes[3] = static_cast<int16_t>(-g.sThumbRY * 1000 / 32767);
				pad.axes[4] = static_cast<int16_t>(g.bLeftTrigger * 1000 / 255);
				pad.axes[5] = static_cast<int16_t>(g.bRightTrigger * 1000 / 255);
			}

			// ---------------- DirectInput worker

			void WakeWorker()
			{
				{
					std::lock_guard<std::mutex> lock(m_workerMutex);
					m_wakeRequested = true;
				}
				m_workerWake.notify_all();
			}

			void WorkerLoop()
			{
				IDirectInput8W* dinput = nullptr;
				if (orig_DirectInput8Create)
				{
					orig_DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
						reinterpret_cast<LPVOID*>(&dinput), nullptr);
				}
				if (!dinput)
				{
					LOG(1, "[LocalInput] DirectInput unavailable; DirectInput pads will not be listed\n");
				}

				std::unique_lock<std::mutex> lock(m_workerMutex);
				while (m_running)
				{
					lock.unlock();
					if (dinput && GameWindow())
						Enumerate(dinput);
					lock.lock();
					if (m_autoDetect)
						m_workerWake.wait_for(lock, std::chrono::milliseconds(kEnumSafetyNetMs),
							[this] { return !m_running || m_wakeRequested; });
					else
						m_workerWake.wait(lock, [this] { return !m_running || m_wakeRequested; });
					m_wakeRequested = false;
				}
				lock.unlock();
				if (dinput)
					dinput->Release();
			}

			struct EnumContext
			{
				IDirectInput8W* dinput;
				std::vector<DIFound>* found;
				const std::vector<std::string>* known;
			};

			static BOOL CALLBACK EnumDevice(LPCDIDEVICEINSTANCEW instance, LPVOID context)
			{
				EnumContext* ctx = static_cast<EnumContext*>(context);
				DIFound found;
				found.instance = instance->guidInstance;
				found.name = Narrow(instance->tszProductName[0] ? instance->tszProductName : instance->tszInstanceName);
				// HID devices carry their USB ids in the product GUID: Data1 = PID << 16 | VID,
				// Data4 ending in "PIDVID".
				if (std::memcmp(&instance->guidProduct.Data4[2], "PIDVID", 6) == 0)
				{
					found.vendorId = LOWORD(instance->guidProduct.Data1);
					found.productId = HIWORD(instance->guidProduct.Data1);
				}
				const std::string id = "DI:" + GuidString(found.instance);
				// Already open on the game thread: only its presence matters.
				if (std::find(ctx->known->begin(), ctx->known->end(), id) == ctx->known->end())
				{
					IDirectInputDevice8W* device = nullptr;
					if (FAILED(ctx->dinput->CreateDevice(found.instance, &device, nullptr)) || !device)
						return DIENUM_CONTINUE;
					if (IsXInputDevice(device))
					{
						device->Release();
						return DIENUM_CONTINUE; // XInput covers it
					}
					found.device = device;
				}
				ctx->found->push_back(found);
				return DIENUM_CONTINUE;
			}

			// Steam Input exposes controllers as its own virtual devices (Valve's vendor id 28DE).
			void DetectSteamInput()
			{
				UINT count = 0;
				if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
					return;
				std::vector<RAWINPUTDEVICELIST> list(count);
				if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
					return;
				bool steam = false;
				for (UINT i = 0; i < count && !steam; ++i)
				{
					if (list[i].dwType != RIM_TYPEHID)
						continue;
					std::wstring name = RawDeviceName(list[i].hDevice);
					for (wchar_t& c : name)
						c = static_cast<wchar_t>(towupper(c));
					steam = name.find(L"VID_28DE") != std::wstring::npos;
				}
				if (steam != m_steamInput)
					LOG(1, "[LocalInput] Steam Input %s\n", steam ? "is presenting virtual controllers" : "is not active");
				m_steamInput = steam;
			}

			void Enumerate(IDirectInput8W* dinput)
			{
				DetectSteamInput();
				std::vector<std::string> known;
				{
					std::lock_guard<std::mutex> lock(m_handoffMutex);
					for (const DIPad& pad : m_diPads)
						if (pad.device)
							known.push_back(pad.id);
				}
				std::vector<DIFound> found;
				EnumContext ctx{ dinput, &found, &known };
				const DWORD start = GetTickCount();
				dinput->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumDevice, &ctx, DIEDFL_ATTACHEDONLY);
				const DWORD took = GetTickCount() - start;
				if (took > 50)
					LOG(1, "[LocalInput] DirectInput enumeration took %lu ms (worker thread, game unaffected)\n", took);

				// New devices are prepared here, off the game thread, so the game thread only
				// ever polls.
				for (DIFound& f : found)
				{
					if (!f.device)
						continue;
					if (FAILED(f.device->SetDataFormat(&c_dfDIJoystick2)))
					{
						f.device->Release();
						f.device = nullptr;
						continue;
					}
					f.device->SetCooperativeLevel(GameWindow(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
				}

				std::lock_guard<std::mutex> lock(m_handoffMutex);
				for (DIFound& old : m_pendingFound)
					if (old.device) old.device->Release();
				m_pendingFound = std::move(found);
				m_pendingGeneration++;
			}

			void ReleasePad(DIPad& pad)
			{
				if (pad.device)
				{
					pad.device->Unacquire();
					pad.device->Release();
					pad.device = nullptr;
				}
			}

			void AdoptWorkerResults()
			{
				std::lock_guard<std::mutex> lock(m_handoffMutex);
				if (m_pendingGeneration == m_adoptedGeneration)
					return;
				m_adoptedGeneration = m_pendingGeneration;

				std::vector<std::string> present;
				for (DIFound& f : m_pendingFound)
				{
					const std::string id = "DI:" + GuidString(f.instance);
					present.push_back(id);
					if (!f.device)
						continue; // already open
					auto it = std::find_if(m_diPads.begin(), m_diPads.end(), [&](const DIPad& p) { return p.id == id; });
					DIPad* pad = nullptr;
					if (it == m_diPads.end())
					{
						m_diPads.push_back(DIPad());
						pad = &m_diPads.back();
					}
					else if (it->device)
					{
						// Two enumerations overlapped and both opened it; keep the one in use.
						f.device->Release();
						f.device = nullptr;
						continue;
					}
					else
					{
						pad = &*it;
					}
					pad->restCaptured = false;
					pad->id = id;
					pad->name = f.name.empty() ? "Controller" : f.name;
					pad->vendorId = f.vendorId;
					pad->productId = f.productId;
					pad->device = f.device;
					f.device = nullptr;
					AxisSetup setup{ pad->device, pad };
					std::memset(pad->axisPresent, 0, sizeof(pad->axisPresent));
					pad->device->EnumObjects(EnumAxes, &setup, DIDFT_AXIS);
					pad->device->Acquire();
					pad->connected = true;
					LOG(1, "[LocalInput] DirectInput device connected: %s (%s, %04X:%04X)\n", pad->name.c_str(), id.c_str(), pad->vendorId, pad->productId);
				}
				for (DIPad& pad : m_diPads)
				{
					if (pad.device && std::find(present.begin(), present.end(), pad.id) == present.end())
					{
						LOG(1, "[LocalInput] DirectInput device disconnected: %s\n", pad.name.c_str());
						ReleasePad(pad);
						pad.connected = false;
						pad.controls.reset();
					}
				}
			}

			void PollDirectInput(std::vector<DeviceSnapshot>& out)
			{
				const DWORD now = GetTickCount();
				for (DIPad& pad : m_diPads)
				{
					if (pad.device)
					{
						HRESULT hr = pad.device->Poll();
						if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
						{
							if (now - pad.lastAcquireTry >= 250)
							{
								pad.lastAcquireTry = now;
								if (SUCCEEDED(pad.device->Acquire()))
									hr = pad.device->Poll();
							}
						}
						DIJOYSTATE2 js = {};
						if (SUCCEEDED(pad.device->GetDeviceState(sizeof(js), &js)))
						{
							pad.connected = true;
							Decode(js, pad);
						}
						else
						{
							// Most likely unplugged: report it now, let the worker confirm.
							if (pad.connected)
								WakeWorker();
							pad.connected = false;
							pad.controls.reset();
						}
					}

					DeviceSnapshot snap;
					snap.id = pad.id;
					snap.defaultName = pad.name;
					snap.kind = DeviceKind::DInputPad;
					snap.vendorId = pad.vendorId;
					snap.productId = pad.productId;
					snap.connected = pad.connected;
					snap.controls = pad.controls;
					snap.axisCount = kDInputAxisCount;
					std::memcpy(snap.axes, pad.axes, sizeof(pad.axes));
					out.push_back(snap);
				}
			}

			static void Decode(const DIJOYSTATE2& js, DIPad& pad)
			{
				pad.controls.reset();
				for (int i = 0; i < kDInputButtonCount; ++i)
					pad.controls[i] = (js.rgbButtons[i] & 0x80) != 0;
				for (int hat = 0; hat < kDInputPovHats; ++hat)
				{
					const DWORD pov = js.rgdwPOV[hat];
					if (LOWORD(pov) == 0xFFFF)
						continue;
					const int angle = static_cast<int>(pov % 36000);
					const int base = kDInputPovBase + hat * 4;
					pad.controls[base + 0] = angle >= 29250 || angle <= 6750;
					pad.controls[base + 1] = angle >= 2250 && angle <= 15750;
					pad.controls[base + 2] = angle >= 11250 && angle <= 24750;
					pad.controls[base + 3] = angle >= 20250 && angle <= 33750;
				}
				for (int axis = 0; axis < kDInputAxisCount; ++axis)
				{
					if (!pad.axisPresent[axis])
					{
						pad.axes[axis] = 0;
						continue;
					}
					const LONG raw = AxisValue(js, axis);
					const LONG range = pad.axisMax[axis] - pad.axisMin[axis];
					const LONG norm = range > 0 ? ((raw - pad.axisMin[axis]) * 2000 / range) - 1000 : 0;
					pad.axes[axis] = static_cast<int16_t>(std::max<LONG>(-1000, std::min<LONG>(1000, norm)));
					if (!pad.restCaptured)
					{
						// A resting axis is either centred or parked at an end. Anything in
						// between means it was being held at connect time: assume centred.
						const LONG magnitude = norm < 0 ? -norm : norm;
						pad.rest[axis] = (magnitude <= 300 || magnitude >= 850) ? norm : 0;
					}
					const LONG delta = norm - pad.rest[axis];
					pad.controls[kDInputAxisBase + axis * 2 + 0] = delta < -kAxisThreshold;
					pad.controls[kDInputAxisBase + axis * 2 + 1] = delta > kAxisThreshold;
				}
				pad.restCaptured = true;
			}

			// ---------------- Keyboards

			void EnsureRawKeyboard()
			{
				const HWND hwnd = GameWindow();
				if (!hwnd || hwnd == m_rawHwnd)
					return;
				RAWINPUTDEVICE rid = {};
				rid.usUsagePage = 0x01;
				rid.usUsage = 0x06;
				// No RIDEV_INPUTSINK: keys only arrive while the game is the foreground window.
				rid.dwFlags = RIDEV_DEVNOTIFY;
				rid.hwndTarget = hwnd;
				if (RegisterRawInputDevices(&rid, 1, sizeof(rid)))
				{
					m_rawHwnd = hwnd;
					ListKeyboards();
					LOG(1, "[LocalInput] raw keyboard input registered on the game window\n");
				}
				else
				{
					LOG(1, "[LocalInput] RegisterRawInputDevices failed (%lu)\n", GetLastError());
				}
			}

			Keyboard& KeyboardFor(HANDLE handle)
			{
				auto it = m_keyboards.find(handle);
				if (it != m_keyboards.end())
					return it->second;
				Keyboard kb;
				kb.handle = handle;
				if (handle == nullptr)
				{
					// SendInput, on-screen keyboards, AutoHotkey, Steam Input's keyboard
					// emulation: input injected by software carries no device handle.
					kb.id = "KB:virtual";
					kb.name = "Software keyboard (injected input)";
				}
				else
				{
					const std::wstring path = RawDeviceName(handle);
					char id[32];
					snprintf(id, sizeof(id), "KB:%08X", Fnv1a(path));
					kb.id = id;
					kb.name = KeyboardProductName(path);
					if (kb.name.empty())
						kb.name = "Keyboard";
				}
				return m_keyboards.emplace(handle, kb).first->second;
			}

			void ListKeyboards()
			{
				UINT count = 0;
				if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
					return;
				std::vector<RAWINPUTDEVICELIST> list(count);
				if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
					return;
				std::lock_guard<std::mutex> lock(m_keyboardMutex);
				for (UINT i = 0; i < count; ++i)
					if (list[i].dwType == RIM_TYPEKEYBOARD)
						KeyboardFor(list[i].hDevice).connected = true;
			}

			void OnKeyboardDeviceChange(HANDLE handle, bool arrived)
			{
				std::lock_guard<std::mutex> lock(m_keyboardMutex);
				Keyboard& kb = KeyboardFor(handle);
				kb.connected = arrived;
				if (!arrived)
					kb.keys.reset();
			}

			static int FixVirtualKey(const RAWKEYBOARD& k)
			{
				const bool e0 = (k.Flags & RI_KEY_E0) != 0;
				switch (k.VKey)
				{
				case VK_SHIFT: return k.MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
				case VK_CONTROL: return e0 ? VK_RCONTROL : VK_LCONTROL;
				case VK_MENU: return e0 ? VK_RMENU : VK_LMENU;
				default: return k.VKey;
				}
			}

			void OnRawInput(HRAWINPUT input)
			{
				RAWINPUT raw = {};
				UINT size = sizeof(raw);
				if (GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
					return;
				if (raw.header.dwType != RIM_TYPEKEYBOARD)
					return;
				const RAWKEYBOARD& k = raw.data.keyboard;
				if (k.VKey == 0 || k.VKey >= 255)
					return; // fake keys emitted around Pause / Print Screen
				const bool down = (k.Flags & RI_KEY_BREAK) == 0;
				const int vk = FixVirtualKey(k);
				std::lock_guard<std::mutex> lock(m_keyboardMutex);
				Keyboard& kb = KeyboardFor(raw.header.hDevice);
				kb.connected = true;
				if (!m_focused && down)
					return;
				kb.keys[vk] = down;
				// The generic modifier follows its sided keys, so a binding to either works.
				if (vk == VK_LSHIFT || vk == VK_RSHIFT)
					kb.keys[VK_SHIFT] = kb.keys[VK_LSHIFT] || kb.keys[VK_RSHIFT];
				if (vk == VK_LCONTROL || vk == VK_RCONTROL)
					kb.keys[VK_CONTROL] = kb.keys[VK_LCONTROL] || kb.keys[VK_RCONTROL];
				if (vk == VK_LMENU || vk == VK_RMENU)
					kb.keys[VK_MENU] = kb.keys[VK_LMENU] || kb.keys[VK_RMENU];
			}

			void ReportKeyboards(std::vector<DeviceSnapshot>& out)
			{
				std::lock_guard<std::mutex> lock(m_keyboardMutex);
				for (auto& entry : m_keyboards)
				{
					Keyboard& kb = entry.second;
					DeviceSnapshot snap;
					snap.id = kb.id;
					snap.defaultName = kb.name;
					snap.kind = DeviceKind::Keyboard;
					snap.connected = kb.connected;
					snap.controls = m_focused ? kb.keys : ControlSet();
					out.push_back(snap);
				}
			}

			// XInput
			XPad m_xpads[XUSER_MAX_COUNT];
			std::atomic<bool> m_xinputRecheckAll{ false };

			// DirectInput: worker enumerates and opens, game thread adopts and polls.
			std::thread m_worker;
			std::mutex m_workerMutex;
			std::condition_variable m_workerWake;
			bool m_running = false;
			bool m_wakeRequested = false;
			std::mutex m_handoffMutex;
			std::vector<DIFound> m_pendingFound;
			uint64_t m_pendingGeneration = 0;
			uint64_t m_adoptedGeneration = 0;
			std::vector<DIPad> m_diPads;

			// Keyboards
			std::mutex m_keyboardMutex;
			std::unordered_map<HANDLE, Keyboard> m_keyboards;
			HWND m_rawHwnd = nullptr;
			std::atomic<bool> m_focused{ true };
			std::atomic<bool> m_autoDetect{ true };
			std::atomic<bool> m_steamInput{ false };
		};
	}

	std::unique_ptr<IInputBackend> CreateWindowsInputBackend()
	{
		return std::unique_ptr<IInputBackend>(new WindowsInputBackend());
	}
}
