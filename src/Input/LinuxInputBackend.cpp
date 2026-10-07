// Placeholder for a native Linux device backend.
//
// Under Proton/Wine today the Windows backend runs (Wine emulates XInput, DirectInput and raw
// keyboard input). A native backend would read the host's devices directly - evdev
// (/dev/input/event*) or SDL's game controller API through a host-side helper - and fill the
// same DeviceSnapshots, so assignment, mapping and the UI work unchanged.
//
// What a real implementation must provide, mirroring WindowsInputBackend:
//   - stable device ids (evdev: the device's uniq/phys strings, or vendor:product:serial),
//   - hot-plug without polling the device tree on the game thread (udev monitor on a worker),
//   - keyboards listed separately so keyboard vs keyboard works,
//   - every key released when the game window loses focus.
//
// LocalInputManager::Start picks this backend only when it reports itself available.

#include "InputBackend.h"

namespace LocalInput
{
	namespace
	{
		class LinuxInputBackend final : public IInputBackend
		{
		public:
			const char* Name() const override { return "Linux (not implemented)"; }
			void Start() override {}
			void Stop() override {}
			void Poll(std::vector<DeviceSnapshot>& out) override { out.clear(); }
			bool OnWindowMessage(HWND, UINT, WPARAM, LPARAM) override { return false; }
			void OnFocusChanged(bool) override {}
			void SetAutoDetect(bool) override {}
			void Refresh() override {}
			bool SteamInputLikely() const override { return false; }
		};
	}

	std::unique_ptr<IInputBackend> CreateLinuxInputBackend()
	{
		return std::unique_ptr<IInputBackend>(new LinuxInputBackend());
	}
}
