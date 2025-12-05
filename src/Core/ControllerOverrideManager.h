#pragma once

#include <Windows.h>
#include <dinput.h>

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <array>
#include <unordered_map>
#include <unordered_set>

struct ControllerDeviceInfo
{
        GUID guid = GUID_NULL;
        std::string name;
        bool isKeyboard = false;
        bool isWinmmDevice = false;
        UINT winmmId = static_cast<UINT>(-1);
        bool hasVendorProductIds = false;
        USHORT vendorId = 0;
        USHORT productId = 0;
};

struct KeyboardDeviceInfo
{
        HANDLE deviceHandle = nullptr;
        std::string baseName;
        std::string displayName;
        std::string deviceId;
        std::string canonicalId;
        bool ignored = false;
        bool connected = true;
};

std::string GuidToString(const GUID& guid);

class ControllerOverrideManager
{
public:
        static ControllerOverrideManager& GetInstance();

        void SetOverrideEnabled(bool enabled);
        bool IsOverrideEnabled() const;

        void SetAutoRefreshEnabled(bool enabled);
        bool IsAutoRefreshEnabled() const;

        void SetKeyboardControllerSeparated(bool enabled);
        bool IsKeyboardControllerSeparated() const { return m_keyboardControllerSeparated; }

        void SetPlayerSelection(int playerIndex, const GUID& guid);
        GUID GetPlayerSelection(int playerIndex) const;

        const std::vector<ControllerDeviceInfo>& GetDevices() const;

        void SetMultipleKeyboardOverrideEnabled(bool enabled);
        bool IsMultipleKeyboardOverrideEnabled() const { return m_multipleKeyboardOverrideEnabled; }

        const std::vector<KeyboardDeviceInfo>& GetKeyboardDevices() const;
        const std::vector<KeyboardDeviceInfo>& GetAllKeyboardDevices() const;
        HANDLE GetPrimaryKeyboardHandle() const;
        void SetPrimaryKeyboardHandle(HANDLE deviceHandle);
        void IgnoreKeyboard(const KeyboardDeviceInfo& info);
        void UnignoreKeyboard(const std::string& canonicalId);
        void RenameKeyboard(const KeyboardDeviceInfo& info, const std::string& newName);

        std::string GetKeyboardLabelForId(const std::string& canonicalId) const;
        std::vector<KeyboardDeviceInfo> GetIgnoredKeyboardSnapshot() const;

        bool GetFilteredKeyboardState(BYTE* keyStateOut);

        bool IsSteamInputLikelyActive() const { return m_steamInputLikely; }

        bool RefreshDevices();
        void RefreshDevicesAndReinitializeGame();
        void TickAutoRefresh();

        void HandleWindowMessage(UINT msg, WPARAM wParam, LPARAM lParam);

        void ApplyOrdering(std::vector<DIDEVICEINSTANCEA>& devices) const;
        void ApplyOrdering(std::vector<DIDEVICEINSTANCEW>& devices) const;

        void RegisterCreatedDevice(IDirectInputDevice8A* device);
        void RegisterCreatedDevice(IDirectInputDevice8W* device);

        void DebugDumpTrackedDevices();

        bool IsDeviceAllowed(const GUID& guid) const;

        void OpenControllerControlPanel() const;
        bool OpenDeviceProperties(const GUID& guid) const;

        static std::string WideToUtf8(const std::wstring& value);

private:
        ControllerOverrideManager();

        template <typename T>
        void ApplyOrderingImpl(std::vector<T>& devices) const;

        void EnsureSelectionsValid();
        void EnsurePrimaryKeyboardValid();
        bool CollectDevices();
        bool RefreshKeyboardDevices();
        void ApplyKeyboardPreferences(std::vector<KeyboardDeviceInfo>& devices);
        void PersistKeyboardIgnores();
        void PersistKeyboardRenames();
        void LoadKeyboardPreferences();
        bool TryEnumerateDevicesA(std::vector<ControllerDeviceInfo>& outDevices);
        bool TryEnumerateDevicesW(std::vector<ControllerDeviceInfo>& outDevices);
        void TryEnumerateWinmmDevices(std::vector<ControllerDeviceInfo>& outDevices) const;
        static GUID CreateWinmmGuid(UINT winmmId);
        static bool NamesEqualIgnoreCase(const std::string& lhs, const std::string& rhs);

        void BounceTrackedDevices();
        void SendDeviceChangeBroadcast() const;
        void ReinitializeGameInputs();
        void ProcessPendingDeviceChange();

        void ProcessRawInput(HRAWINPUT rawInput);
        void HandleRawInputDeviceChange(HANDLE deviceHandle, bool arrived);
        void EnsureRawKeyboardRegistration();

        std::vector<ControllerDeviceInfo> m_devices;
        GUID m_playerSelections[2];
        bool m_overrideEnabled = false;
        bool m_autoRefreshEnabled = true;
        bool m_keyboardControllerSeparated = false;
        bool m_multipleKeyboardOverrideEnabled = false;
        ULONGLONG m_lastRefresh = 0;
        size_t m_lastDeviceHash = 0;
        bool m_steamInputLikely = false;
        std::atomic<bool> m_deviceChangeQueued{ false };

        std::vector<IDirectInputDevice8A*> m_trackedDevicesA;
        std::vector<IDirectInputDevice8W*> m_trackedDevicesW;
        std::vector<KeyboardDeviceInfo> m_allKeyboardDevices;
        std::vector<KeyboardDeviceInfo> m_keyboardDevices;
        std::unordered_map<HANDLE, std::array<BYTE, 256>> m_keyboardStates;
        HANDLE m_primaryKeyboardHandle = nullptr;
        std::string m_primaryKeyboardDeviceId;
        std::unordered_set<std::string> m_ignoredKeyboardIds;
        std::unordered_map<std::string, std::string> m_keyboardRenames;
        std::unordered_map<std::string, std::string> m_knownKeyboardNames;
        bool m_rawKeyboardRegistered = false;
        mutable std::mutex m_deviceMutex;
        mutable std::mutex m_keyboardMutex;
};
