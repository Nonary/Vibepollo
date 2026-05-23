#define VDD_CONTROL_DEFINE_GUIDS

#include "third-party/vdd/vdd-control.h"

#include <SetupAPI.h>
#include <Windows.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

  std::string winerr(DWORD error = GetLastError()) {
    LPWSTR message = nullptr;
    const auto flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const auto size = FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::ostringstream out;
    out << error;
    if (size && message) {
      std::wstring wide(message, size);
      while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n')) {
        wide.pop_back();
      }
      const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
      std::string utf8(bytes, '\0');
      if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr, nullptr);
        out << " (" << utf8 << ')';
      }
    }
    if (message) {
      LocalFree(message);
    }
    return out.str();
  }

  bool same_luid(const LUID &lhs, const LUID &rhs) {
    return lhs.LowPart == rhs.LowPart && lhs.HighPart == rhs.HighPart;
  }

  std::string luid_string(const LUID &luid) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(8) << static_cast<uint32_t>(luid.HighPart)
        << ':' << std::setw(8) << luid.LowPart << std::dec;
    return out.str();
  }

  HANDLE open_vdd_control_device() {
    const HDEVINFO device_info_set = SetupDiGetClassDevsW(
      &GUID_DEVINTERFACE_VDD_CONTROL,
      nullptr,
      nullptr,
      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );
    if (device_info_set == INVALID_HANDLE_VALUE) {
      std::cerr << "SetupDiGetClassDevsW failed: " << winerr() << '\n';
      return INVALID_HANDLE_VALUE;
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA interface_data {};
    interface_data.cbSize = sizeof(interface_data);

    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(device_info_set, nullptr, &GUID_DEVINTERFACE_VDD_CONTROL, index, &interface_data); ++index) {
      DWORD detail_size = 0;
      (void) SetupDiGetDeviceInterfaceDetailW(device_info_set, &interface_data, nullptr, 0, &detail_size, nullptr);
      if (detail_size == 0) {
        continue;
      }

      std::vector<std::byte> detail_buffer(detail_size);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detail_buffer.data());
      detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
      if (!SetupDiGetDeviceInterfaceDetailW(device_info_set, &interface_data, detail, detail_size, &detail_size, nullptr)) {
        std::cerr << "SetupDiGetDeviceInterfaceDetailW failed: " << winerr() << '\n';
        continue;
      }

      std::wcout << L"VDD device path: " << detail->DevicePath << L'\n';
      handle = CreateFileW(
        detail->DevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
      );
      const DWORD create_error = GetLastError();
      if (handle != INVALID_HANDLE_VALUE) {
        break;
      }

      std::cerr << "CreateFileW failed: " << winerr(create_error) << '\n';
    }

    SetupDiDestroyDeviceInfoList(device_info_set);
    return handle;
  }

  bool ioctl(HANDLE handle, DWORD code, void *in_buffer, DWORD in_size, void *out_buffer, DWORD out_size) {
    DWORD bytes_returned = 0;
    if (DeviceIoControl(handle, code, in_buffer, in_size, out_buffer, out_size, &bytes_returned, nullptr)) {
      return true;
    }
    std::cerr << "DeviceIoControl 0x" << std::hex << code << std::dec << " failed: " << winerr() << '\n';
    return false;
  }

  template <typename TInput, typename TOutput>
  bool ioctl_buffered_inout(HANDLE handle, DWORD code, const TInput &input, TOutput &output) {
    static_assert(std::is_trivially_copyable_v<TInput>);
    static_assert(std::is_trivially_copyable_v<TOutput>);

    std::vector<std::byte> buffer(std::max(sizeof(TInput), sizeof(TOutput)));
    std::memcpy(buffer.data(), &input, sizeof(TInput));

    DWORD bytes_returned = 0;
    if (!DeviceIoControl(
          handle,
          code,
          buffer.data(),
          static_cast<DWORD>(sizeof(TInput)),
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &bytes_returned,
          nullptr
        )) {
      std::cerr << "DeviceIoControl(shared) 0x" << std::hex << code << std::dec << " failed: " << winerr() << '\n';
      return false;
    }

    std::memcpy(&output, buffer.data(), sizeof(TOutput));
    return true;
  }

  std::optional<DISPLAYCONFIG_TARGET_DEVICE_NAME> target_name(const LUID &adapter_id, UINT32 target_id) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME name {};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = adapter_id;
    name.header.id = target_id;
    const auto error = DisplayConfigGetDeviceInfo(&name.header);
    if (error != ERROR_SUCCESS) {
      return std::nullopt;
    }
    return name;
  }

  std::optional<DISPLAYCONFIG_SOURCE_DEVICE_NAME> source_name(const LUID &adapter_id, UINT32 source_id) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name {};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = adapter_id;
    name.header.id = source_id;
    const auto error = DisplayConfigGetDeviceInfo(&name.header);
    if (error != ERROR_SUCCESS) {
      return std::nullopt;
    }
    return name;
  }

  struct ActivePathResult {
    bool matched = false;
    bool matched_requested_mode = false;
  };

  ActivePathResult print_active_paths(const LUID *target_adapter, UINT32 target_id, UINT32 requested_width, UINT32 requested_height, UINT32 requested_hz) {
    ActivePathResult result {};

    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    LONG error = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    if (error != ERROR_SUCCESS) {
      std::cerr << "GetDisplayConfigBufferSizes failed: " << winerr(error) << '\n';
      return result;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    error = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
    if (error != ERROR_SUCCESS) {
      std::cerr << "QueryDisplayConfig failed: " << winerr(error) << '\n';
      return result;
    }

    paths.resize(path_count);
    modes.resize(mode_count);

    std::cout << "Active CCD paths: " << paths.size() << '\n';
    for (const auto &path : paths) {
      const bool has_target = target_adapter != nullptr;
      const bool is_match = has_target && same_luid(path.targetInfo.adapterId, *target_adapter) && path.targetInfo.id == target_id;
      bool path_has_requested_size = false;
      bool path_has_requested_refresh = requested_hz == 0;
      auto tname = target_name(path.targetInfo.adapterId, path.targetInfo.id);
      auto sname = source_name(path.sourceInfo.adapterId, path.sourceInfo.id);

      std::cout << (is_match ? "* " : "  ")
                << "source=" << luid_string(path.sourceInfo.adapterId) << '/' << path.sourceInfo.id
                << " target=" << luid_string(path.targetInfo.adapterId) << '/' << path.targetInfo.id;

      if (sname) {
        std::wcout << L" gdi=" << sname->viewGdiDeviceName;
      }
      if (tname) {
        std::wcout << L" monitor=" << tname->monitorDevicePath
                   << L" friendly=" << tname->monitorFriendlyDeviceName;
      }

      if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
          path.sourceInfo.modeInfoIdx < modes.size() &&
          modes[path.sourceInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        const auto &source_mode = modes[path.sourceInfo.modeInfoIdx].sourceMode;
        std::cout << " sourceMode=" << source_mode.width << 'x' << source_mode.height;
        path_has_requested_size = source_mode.width == requested_width && source_mode.height == requested_height;
      }

      if (path.targetInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
          path.targetInfo.modeInfoIdx < modes.size() &&
          modes[path.targetInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
        const auto &target_mode = modes[path.targetInfo.modeInfoIdx].targetMode.targetVideoSignalInfo;
        std::cout << " targetActive=" << target_mode.activeSize.cx << 'x' << target_mode.activeSize.cy
                  << " targetTotal=" << target_mode.totalSize.cx << 'x' << target_mode.totalSize.cy
                  << " vHz=";
        if (target_mode.vSyncFreq.Denominator != 0) {
          const double hz = static_cast<double>(target_mode.vSyncFreq.Numerator) / target_mode.vSyncFreq.Denominator;
          std::cout << hz;
          path_has_requested_refresh = requested_hz == 0 || std::fabs(hz - requested_hz) < 0.5;
        } else {
          std::cout << "unknown";
        }
      }

      std::cout << '\n';
      result.matched = result.matched || (has_target ? is_match : path_has_requested_size);
      if ((has_target ? is_match : true) && path_has_requested_size && path_has_requested_refresh) {
        result.matched_requested_mode = true;
      }
    }

    return result;
  }

}  // namespace

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--list-active") {
    const UINT32 width = argc > 2 ? static_cast<UINT32>(std::stoul(argv[2])) : 3840u;
    const UINT32 height = argc > 3 ? static_cast<UINT32>(std::stoul(argv[3])) : 2160u;
    const UINT32 hz = argc > 4 ? static_cast<UINT32>(std::stoul(argv[4])) : 240u;
    const auto result = print_active_paths(nullptr, 0, width, height, hz);
    if (!result.matched_requested_mode) {
      std::cerr << "FAIL: no active CCD path at requested " << width << 'x' << height << '@' << hz << "Hz.\n";
      return 7;
    }
    std::cout << "PASS: active CCD path found at requested " << width << 'x' << height << '@' << hz << "Hz.\n";
    return 0;
  }

  const UINT32 width = argc > 1 ? static_cast<UINT32>(std::stoul(argv[1])) : 3840u;
  const UINT32 height = argc > 2 ? static_cast<UINT32>(std::stoul(argv[2])) : 2160u;
  const UINT32 hz = argc > 3 ? static_cast<UINT32>(std::stoul(argv[3])) : 240u;
  const UINT32 refresh_millihz = hz * 1000u;
  bool skip_ccd = false;
  DWORD hold_ms = 0;
  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--skip-ccd") {
      skip_ccd = true;
    } else if (arg == "--hold-ms" && i + 1 < argc) {
      hold_ms = static_cast<DWORD>(std::stoul(argv[++i]));
    }
  }
  const uint64_t lease_id = (static_cast<uint64_t>(GetCurrentProcessId()) << 32u) ^ GetTickCount64();
  const uint64_t display_id = 0x53554E5600000000ull |
                              (static_cast<uint64_t>(width & 0xFFFFu) << 32u) |
                              (static_cast<uint64_t>(height & 0xFFFFu) << 16u) |
                              static_cast<uint64_t>(hz & 0xFFFFu);

  std::cout << "Requesting VDD temporary display "
            << width << 'x' << height << '@' << hz
            << "Hz lease=" << lease_id
            << " displayId=0x" << std::hex << display_id << std::dec << '\n';
  std::cout << "Struct sizes: create=" << sizeof(VDD_CREATE_TEMPORARY_DISPLAY)
            << " result=" << sizeof(VDD_CREATE_TEMPORARY_DISPLAY_RESULT)
            << " leaseDisplay=" << sizeof(VDD_LEASE_DISPLAY_REQUEST)
            << " protocol=" << sizeof(VDD_PROTOCOL_VERSION)
            << " GUID=" << sizeof(GUID)
            << " LUID=" << sizeof(LUID) << '\n';

  HANDLE handle = open_vdd_control_device();
  if (handle == INVALID_HANDLE_VALUE) {
    return 2;
  }

  VDD_PROTOCOL_VERSION version {};
  version.ApiNamespace = GUID_VDD_CONTROL_API_NAMESPACE;
  if (!ioctl(handle, IOCTL_VDD_GET_PROTOCOL_VERSION, nullptr, 0, &version, sizeof(version))) {
    CloseHandle(handle);
    return 3;
  }

  std::cout << "VDD protocol " << version.Major << '.' << version.Minor << '.' << version.Patch << '\n';

  VDD_CREATE_TEMPORARY_DISPLAY create {};
  create.ApiNamespace = GUID_VDD_CONTROL_API_NAMESPACE;
  create.LeaseId = lease_id;
  create.DisplayId = display_id;
  create.Width = width;
  create.Height = height;
  create.RefreshRateMilliHz = refresh_millihz;
  create.RequestedTimeoutMs = 30000;
  strcpy_s(create.DisplayName, "VDDProbe");

  VDD_CREATE_TEMPORARY_DISPLAY_RESULT created {};
  created.ApiNamespace = GUID_VDD_CONTROL_API_NAMESPACE;
  if (!ioctl_buffered_inout(handle, IOCTL_VDD_CREATE_TEMPORARY_DISPLAY, create, created)) {
    CloseHandle(handle);
    return 4;
  }

  std::cout << "Created: adapter=" << luid_string(created.OsAdapterLuid)
            << " target=" << created.TargetId
            << " connector=" << created.ConnectorIndex
            << " timeoutMs=" << created.EffectiveTimeoutMs << '\n';

  ActivePathResult final_result {};
  if (skip_ccd) {
    final_result.matched = true;
    final_result.matched_requested_mode = true;
    std::cout << "Skipping CCD validation in this process.\n";
  } else {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
      final_result = print_active_paths(&created.OsAdapterLuid, created.TargetId, width, height, hz);
      if (final_result.matched_requested_mode) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } while (std::chrono::steady_clock::now() < deadline);
  }

  if (hold_ms > 0) {
    std::cout << "Holding temporary display for " << hold_ms << "ms.\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
  }

  VDD_LEASE_DISPLAY_REQUEST remove {};
  remove.ApiNamespace = GUID_VDD_CONTROL_API_NAMESPACE;
  remove.LeaseId = lease_id;
  remove.DisplayId = display_id;
  if (!ioctl(handle, IOCTL_VDD_REMOVE_TEMPORARY_DISPLAY, &remove, sizeof(remove), nullptr, 0)) {
    std::cerr << "Remove failed; lease timeout should clean this display up.\n";
  } else {
    std::cout << "Removed temporary display.\n";
  }

  CloseHandle(handle);

  if (!final_result.matched) {
    std::cerr << "FAIL: created target never appeared in active CCD paths.\n";
    return 5;
  }
  if (!final_result.matched_requested_mode) {
    std::cerr << "FAIL: created target appeared but not at requested " << width << 'x' << height << ".\n";
    return 6;
  }

  std::cout << "PASS: created target appeared at requested " << width << 'x' << height << ".\n";
  return 0;
}
