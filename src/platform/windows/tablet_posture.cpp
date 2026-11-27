/**
 * @file src/platform/windows/tablet_posture.cpp
 * @brief Windows backend of the host laptop/slate state (see src/tablet_posture.h).
 *
 * Windows computes "tablet posture" in InputHost (DevicePostureHelpers::CalculateDevicePostureMode)
 * and the shell reads it to dock the touch keyboard and switch to the tablet taskbar. It is a
 * tablet when the device is convertible, the laptop/slate indicator says slate, it is not docked, and
 * the display configuration is a valid tablet one. This backend supplies those inputs for a host
 * whose display is a virtual one:
 *
 *  - Microsoft's inbox "GPIO Laptop or Slate Indicator Driver" (msgpiowin32.inf, hardware id
 *    PNP0C60) provides the indicator. The device is created on first use as a root-enumerated device,
 *    exactly like the virtual gamepad root device. Writing one byte to its interface sets the state:
 *    1 = laptop, 0 = slate. The kernel then updates ConvertibleSlateMode and the system metric live.
 *  - PriorityControl\ConvertibilityEnabled and PriorityControl\CSMIgnoreDisplayConfig are Windows'
 *    own OEM declarations ("this device is a convertible", "do not judge the display configuration").
 *    They are set only while the host is a slate and restored to what they were afterwards. A journal
 *    file lets a restarted host undo them after a crash.
 */
// standard includes
#include <array>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

// platform includes
#include <winsock2.h>
#include <windows.h>

#include <setupapi.h>

// local includes
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/tablet_posture_policy.h"

using namespace std::literals;

namespace tablet_posture::backend {
  bool apply(state_e state);

  void recover(bool resync);
}  // namespace tablet_posture::backend

namespace tablet_posture::backend {
  namespace {
    // GUID_GPIOBUTTONS_LAPTOPSLATE_INTERFACE
    const GUID laptop_slate_interface = {0x317fc439, 0x3f77, 0x41c8, {0xb0, 0x9e, 0x08, 0xad, 0x63, 0x27, 0x2a, 0xa3}};
    // GUID_DEVCLASS_HIDCLASS
    const GUID hid_class = {0x745a17a0, 0x74d3, 0x11d0, {0xb6, 0xfe, 0x00, 0xa0, 0xc9, 0x0f, 0x57, 0xda}};

    constexpr auto indicator_hardware_id = L"PNP0C60";
    constexpr auto indicator_inf = L"C:\\Windows\\INF\\msgpiowin32.inf";
    constexpr auto priority_control = L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl";
    constexpr auto convertibility_value = L"ConvertibilityEnabled";
    constexpr auto ignore_display_value = L"CSMIgnoreDisplayConfig";

    constexpr DWORD install_flag_force = 0x00000001;  // INSTALLFLAG_FORCE

    constexpr unsigned char laptop_byte = 1;
    constexpr unsigned char slate_byte = 0;

    std::filesystem::path journal_path() {
      return platf::appdata() / "tablet_posture.journal";
    }

    /// Path of the laptop/slate indicator interface, or nullopt when no such device exists.
    std::optional<std::wstring> indicator_path() {
      HDEVINFO set = SetupDiGetClassDevsW(&laptop_slate_interface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
      if (set == INVALID_HANDLE_VALUE) {
        return std::nullopt;
      }
      std::optional<std::wstring> path;
      SP_DEVICE_INTERFACE_DATA data {};
      data.cbSize = sizeof(data);
      if (SetupDiEnumDeviceInterfaces(set, nullptr, &laptop_slate_interface, 0, &data)) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &required, nullptr);
        std::string buffer(required, '\0');
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &data, detail, required, nullptr, nullptr)) {
          path = std::wstring(detail->DevicePath);
        }
      }
      SetupDiDestroyDeviceInfoList(set);
      return path;
    }

    /// Create the root-enumerated indicator device and bind the inbox driver to it.
    bool create_indicator() {
      HDEVINFO set = SetupDiCreateDeviceInfoList(&hid_class, nullptr);
      if (set == INVALID_HANDLE_VALUE) {
        return false;
      }
      SP_DEVINFO_DATA device_info {};
      device_info.cbSize = sizeof(device_info);
      bool ok = SetupDiCreateDeviceInfoW(set, L"HIDClass", &hid_class, L"Vibepollo laptop/slate indicator", nullptr, DICD_GENERATE_ID, &device_info);
      if (ok) {
        std::array<wchar_t, 16> ids {};
        wcscpy(ids.data(), indicator_hardware_id);  // double-NUL terminated multi-string
        ok = SetupDiSetDeviceRegistryPropertyW(set, &device_info, SPDRP_HARDWAREID, reinterpret_cast<const BYTE *>(ids.data()), static_cast<DWORD>(ids.size() * sizeof(wchar_t)));
      }
      if (ok) {
        ok = SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &device_info);
      }
      SetupDiDestroyDeviceInfoList(set);
      if (!ok) {
        BOOST_LOG(warning) << "Tablet posture: could not register the indicator device, error "sv << GetLastError();
        return false;
      }
      // newdev.dll is loaded on demand: device creation happens once per machine, and this keeps the
      // host free of a link-time dependency on it.
      using update_driver_fn = BOOL(WINAPI *)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);
      HMODULE newdev = LoadLibraryW(L"newdev.dll");
      const auto update_driver = newdev ? reinterpret_cast<update_driver_fn>(reinterpret_cast<void *>(GetProcAddress(newdev, "UpdateDriverForPlugAndPlayDevicesW"))) : nullptr;
      if (!update_driver) {
        BOOST_LOG(warning) << "Tablet posture: newdev.dll is unavailable, error "sv << GetLastError();
        if (newdev) {
          FreeLibrary(newdev);
        }
        return false;
      }
      BOOL reboot = FALSE;
      const BOOL bound = update_driver(nullptr, indicator_hardware_id, indicator_inf, install_flag_force, &reboot);
      const DWORD bind_error = bound ? ERROR_SUCCESS : GetLastError();
      FreeLibrary(newdev);
      if (!bound) {
        BOOST_LOG(warning) << "Tablet posture: could not bind the inbox laptop/slate driver, error "sv << bind_error;
        return false;
      }
      BOOST_LOG(info) << "Tablet posture: created the laptop/slate indicator device"sv;
      return true;
    }

    /// Value of a PriorityControl DWORD, or nullopt when absent.
    std::optional<DWORD> read_control_value(const wchar_t *name) {
      DWORD data = 0;
      DWORD size = sizeof(data);
      if (RegGetValueW(HKEY_LOCAL_MACHINE, priority_control, name, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      return data;
    }

    /// One write of the state byte to the indicator interface.
    bool write_indicator_once(const std::wstring &path, unsigned char value) {
      HANDLE device = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (device == INVALID_HANDLE_VALUE) {
        BOOST_LOG(warning) << "Tablet posture: could not open the indicator, error "sv << GetLastError();
        return false;
      }
      DWORD written = 0;
      const BOOL ok = WriteFile(device, &value, 1, &written, nullptr);
      const DWORD write_error = ok ? ERROR_SUCCESS : GetLastError();
      CloseHandle(device);
      if (!ok || written != 1) {
        BOOST_LOG(warning) << "Tablet posture: indicator write failed, error "sv << write_error;
        return false;
      }
      return true;
    }

    /// True once the kernel reports the written state in ConvertibleSlateMode (it mirrors the byte: 1 laptop, 0 slate).
    bool wait_for_state(unsigned char value) {
      for (int attempt = 0; attempt < 20; ++attempt) {
        if (read_control_value(L"ConvertibleSlateMode") == std::optional<DWORD> {value}) {
          return true;
        }
        Sleep(100);
      }
      return false;
    }

    bool write_indicator(unsigned char value) {
      auto path = indicator_path();
      if (!path && create_indicator()) {
        for (int attempt = 0; attempt < 20 && !path; ++attempt) {
          Sleep(250);
          path = indicator_path();
        }
        Sleep(1000);  // let the new device finish starting before its first write
      }
      if (!path) {
        BOOST_LOG(warning) << "Tablet posture: no laptop/slate indicator device available"sv;
        return false;
      }
      for (int attempt = 0; attempt < 3; ++attempt) {
        if (write_indicator_once(*path, value) && wait_for_state(value)) {
          return true;
        }
        Sleep(500);
      }
      BOOST_LOG(warning) << "Tablet posture: Windows did not report the requested laptop/slate state"sv;
      return false;
    }

    bool write_control_value(const wchar_t *name, std::optional<DWORD> value) {
      HKEY key = nullptr;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, priority_control, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
      }
      LONG status = ERROR_SUCCESS;
      if (value) {
        const DWORD data = *value;
        status = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&data), sizeof(data));
      } else {
        status = RegDeleteValueW(key, name);
        if (status == ERROR_FILE_NOT_FOUND) {
          status = ERROR_SUCCESS;
        }
      }
      RegCloseKey(key);
      return status == ERROR_SUCCESS;
    }

    std::string journal_text(std::optional<DWORD> convertibility, std::optional<DWORD> ignore_display) {
      const auto field = [](const char *name, const std::optional<DWORD> &value) {
        return std::string(name) + "=" + (value ? std::to_string(*value) : std::string("absent")) + "\n";
      };
      return field("convertibility", convertibility) + field("ignore_display", ignore_display);
    }

    std::optional<std::optional<DWORD>> parse_journal_field(const std::string &text, const std::string &name) {
      const auto at = text.find(name + "=");
      if (at == std::string::npos) {
        return std::nullopt;
      }
      const auto start = at + name.size() + 1;
      const auto end = text.find('\n', start);
      const auto value = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (value == "absent") {
        return std::optional<DWORD> {};
      }
      try {
        return std::optional<DWORD> {static_cast<DWORD>(std::stoul(value))};
      } catch (...) {
        return std::nullopt;
      }
    }

    /// Declare the host a convertible with a tablet-capable display. Remembers the previous values once.
    bool declare() {
      std::error_code ec;
      if (!std::filesystem::exists(journal_path(), ec)) {
        std::ofstream(journal_path(), std::ios::trunc) << journal_text(read_control_value(convertibility_value), read_control_value(ignore_display_value));
      }
      const bool a = write_control_value(convertibility_value, 1);
      const bool b = write_control_value(ignore_display_value, 1);
      if (!a || !b) {
        BOOST_LOG(warning) << "Tablet posture: could not write the convertible declaration (the host service needs administrator rights)"sv;
      }
      return a && b;
    }

    /// Put the declaration values back to what they were before the host changed them.
    void undeclare() {
      std::error_code ec;
      if (!std::filesystem::exists(journal_path(), ec)) {
        return;
      }
      std::ifstream in(journal_path());
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      const auto convertibility = parse_journal_field(text, "convertibility");
      const auto ignore_display = parse_journal_field(text, "ignore_display");
      bool ok = true;
      if (convertibility) {
        ok = write_control_value(convertibility_value, *convertibility) && ok;
      }
      if (ignore_display) {
        ok = write_control_value(ignore_display_value, *ignore_display) && ok;
      }
      if (ok) {
        std::filesystem::remove(journal_path(), ec);
      } else {
        BOOST_LOG(warning) << "Tablet posture: could not restore the convertible declaration; it stays journaled for the next start"sv;
      }
    }
  }  // namespace

  bool apply(state_e state) {
    if (state == state_e::slate) {
      if (!declare()) {
        return false;
      }
      return write_indicator(slate_byte);
    }
    const bool ok = write_indicator(laptop_byte);
    undeclare();
    return ok;
  }

  void recover(bool resync) {
    std::error_code ec;
    if (std::filesystem::exists(journal_path(), ec)) {
      BOOST_LOG(warning) << "Tablet posture: restoring the state left by a previous host run"sv;
      if (indicator_path()) {
        write_indicator(laptop_byte);
      }
      undeclare();
      return;
    }
    // The inbox indicator driver starts in slate after every boot. Nothing declares the host a
    // convertible at this point, so Windows is not in tablet posture, but the state should still
    // read laptop until a touch client asks for slate.
    if (resync && indicator_path() && read_control_value(L"ConvertibleSlateMode") == std::optional<DWORD> {slate_byte}) {
      if (write_indicator(laptop_byte)) {
        BOOST_LOG(info) << "Tablet posture: indicator reset to laptop at start"sv;
      }
    }
  }
}  // namespace tablet_posture::backend
