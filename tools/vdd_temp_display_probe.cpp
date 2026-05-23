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

  std::wstring widen_ascii(const std::string &value) {
    std::wstring result;
    result.reserve(value.size());
    for (const char ch : value) {
      result.push_back(static_cast<unsigned char>(ch));
    }
    return result;
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
    bool matched_hdr = false;
    bool matched_10bit = false;
  };

  struct AdvancedColorState {
    bool queried = false;
    bool supported = false;
    bool active = false;
    bool limited_by_policy = false;
    bool hdr_supported = false;
    bool hdr_enabled = false;
    bool wcg_supported = false;
    bool wcg_enabled = false;
    DISPLAYCONFIG_COLOR_ENCODING color_encoding = DISPLAYCONFIG_COLOR_ENCODING_RGB;
    UINT32 bits_per_color_channel = 0;
    UINT32 active_color_mode = 0;
  };

  struct DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2_LOCAL {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    union {
      struct {
        UINT32 advancedColorSupported : 1;
        UINT32 advancedColorActive : 1;
        UINT32 reserved1 : 1;
        UINT32 advancedColorLimitedByPolicy : 1;
        UINT32 highDynamicRangeSupported : 1;
        UINT32 highDynamicRangeUserEnabled : 1;
        UINT32 wideColorSupported : 1;
        UINT32 wideColorUserEnabled : 1;
        UINT32 reserved : 24;
      };
      UINT32 value;
    };
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode;
  };

  struct DISPLAYCONFIG_SET_HDR_STATE_LOCAL {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    union {
      struct {
        UINT32 enableHdr : 1;
        UINT32 reserved : 31;
      };
      UINT32 value;
    };
  };

  const char *active_color_mode_name(UINT32 mode) {
    switch (mode) {
      case 0:
        return "SDR";
      case 1:
        return "WCG";
      case 2:
        return "HDR";
      default:
        return "unknown";
    }
  }

  std::optional<AdvancedColorState> query_advanced_color(const LUID &adapter_id, UINT32 target_id) {
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2_LOCAL info {};
    info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    info.header.size = sizeof(info);
    info.header.adapterId = adapter_id;
    info.header.id = target_id;
    const auto error = DisplayConfigGetDeviceInfo(&info.header);
    if (error == ERROR_SUCCESS) {
      AdvancedColorState state {};
      state.queried = true;
      state.supported = info.advancedColorSupported != 0;
      state.active = info.advancedColorActive != 0;
      state.limited_by_policy = info.advancedColorLimitedByPolicy != 0;
      state.hdr_supported = info.highDynamicRangeSupported != 0;
      state.hdr_enabled = info.highDynamicRangeUserEnabled != 0;
      state.wcg_supported = info.wideColorSupported != 0;
      state.wcg_enabled = info.wideColorUserEnabled != 0;
      state.color_encoding = info.colorEncoding;
      state.bits_per_color_channel = info.bitsPerColorChannel;
      state.active_color_mode = info.activeColorMode;
      return state;
    }

    std::cerr << "DisplayConfigGetDeviceInfo(GET_ADVANCED_COLOR_INFO_2) failed for target "
              << luid_string(adapter_id) << '/' << target_id << ": " << winerr(error) << '\n';

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO fallback {};
    fallback.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    fallback.header.size = sizeof(fallback);
    fallback.header.adapterId = adapter_id;
    fallback.header.id = target_id;
    const auto fallback_error = DisplayConfigGetDeviceInfo(&fallback.header);
    if (fallback_error != ERROR_SUCCESS) {
      std::cerr << "DisplayConfigGetDeviceInfo(GET_ADVANCED_COLOR_INFO) failed for target "
                << luid_string(adapter_id) << '/' << target_id << ": " << winerr(fallback_error) << '\n';
      return std::nullopt;
    }

    AdvancedColorState state {};
    state.queried = true;
    state.supported = fallback.advancedColorSupported != 0;
    state.active = fallback.advancedColorEnabled != 0;
    state.limited_by_policy = fallback.advancedColorForceDisabled != 0;
    state.color_encoding = fallback.colorEncoding;
    state.bits_per_color_channel = fallback.bitsPerColorChannel;
    return state;
  }

  bool set_hdr_state(const LUID &adapter_id, UINT32 target_id, bool enabled) {
    DISPLAYCONFIG_SET_HDR_STATE_LOCAL state {};
    state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE;
    state.header.size = sizeof(state);
    state.header.adapterId = adapter_id;
    state.header.id = target_id;
    state.enableHdr = enabled ? 1u : 0u;
    const auto error = DisplayConfigSetDeviceInfo(&state.header);
    if (error != ERROR_SUCCESS) {
      std::cerr << "DisplayConfigSetDeviceInfo(SET_HDR_STATE=" << (enabled ? "on" : "off")
                << ") failed for target " << luid_string(adapter_id) << '/' << target_id
                << ": " << winerr(error) << '\n';
      return false;
    }
    return true;
  }

  bool set_advanced_color(const LUID &adapter_id, UINT32 target_id, bool enabled) {
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE state {};
    state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    state.header.size = sizeof(state);
    state.header.adapterId = adapter_id;
    state.header.id = target_id;
    state.enableAdvancedColor = enabled ? 1u : 0u;
    const auto error = DisplayConfigSetDeviceInfo(&state.header);
    if (error != ERROR_SUCCESS) {
      std::cerr << "DisplayConfigSetDeviceInfo(SET_ADVANCED_COLOR_STATE=" << (enabled ? "on" : "off")
                << ") failed for target " << luid_string(adapter_id) << '/' << target_id
                << ": " << winerr(error) << '\n';
      return false;
    }
    return true;
  }

  ActivePathResult print_active_paths(
    const LUID *target_adapter,
    UINT32 target_id,
    UINT32 requested_width,
    UINT32 requested_height,
    UINT32 requested_hz,
    bool enable_hdr,
    bool require_hdr,
    bool require_10bit,
    const std::wstring &required_friendly_name
  ) {
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
      bool path_has_hdr = !require_hdr;
      bool path_has_10bit = !require_10bit;
      bool path_has_required_name = required_friendly_name.empty();
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
        path_has_required_name = required_friendly_name.empty() ||
                                 _wcsicmp(tname->monitorFriendlyDeviceName, required_friendly_name.c_str()) == 0;
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

      const bool candidate = (has_target ? is_match : path_has_requested_size) && path_has_required_name;
      if (candidate) {
        if (enable_hdr) {
          (void) set_hdr_state(path.targetInfo.adapterId, path.targetInfo.id, true);
          (void) set_advanced_color(path.targetInfo.adapterId, path.targetInfo.id, true);
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (auto advanced_color = query_advanced_color(path.targetInfo.adapterId, path.targetInfo.id)) {
          std::cout << " advancedColorSupported=" << (advanced_color->supported ? "yes" : "no")
                    << " advancedColorActive=" << (advanced_color->active ? "yes" : "no")
                    << " advancedColorLimitedByPolicy=" << (advanced_color->limited_by_policy ? "yes" : "no")
                    << " highDynamicRangeSupported=" << (advanced_color->hdr_supported ? "yes" : "no")
                    << " highDynamicRangeUserEnabled=" << (advanced_color->hdr_enabled ? "yes" : "no")
                    << " wideColorSupported=" << (advanced_color->wcg_supported ? "yes" : "no")
                    << " wideColorUserEnabled=" << (advanced_color->wcg_enabled ? "yes" : "no")
                    << " activeColorMode=" << active_color_mode_name(advanced_color->active_color_mode)
                    << " colorEncoding=" << static_cast<unsigned int>(advanced_color->color_encoding)
                    << " bitsPerColorChannel=" << advanced_color->bits_per_color_channel;
          path_has_hdr = advanced_color->supported &&
                         advanced_color->active &&
                         !advanced_color->limited_by_policy &&
                         advanced_color->hdr_supported &&
                         advanced_color->hdr_enabled &&
                         advanced_color->active_color_mode == 2;
          path_has_10bit = advanced_color->bits_per_color_channel >= 10;
        }
      }

      std::cout << '\n';
      result.matched = result.matched || candidate;
      result.matched_hdr = result.matched_hdr || (candidate && path_has_hdr);
      result.matched_10bit = result.matched_10bit || (candidate && path_has_10bit);
      if (candidate && path_has_requested_size && path_has_requested_refresh && path_has_hdr && path_has_10bit) {
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
    bool enable_hdr = false;
    bool require_hdr = false;
    bool require_10bit = false;
    std::wstring required_friendly_name;
    for (int i = 5; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--enable-hdr") {
        enable_hdr = true;
      } else if (arg == "--require-hdr") {
        require_hdr = true;
      } else if (arg == "--require-10bit") {
        require_10bit = true;
      } else if (arg == "--require-hdr10") {
        enable_hdr = true;
        require_hdr = true;
        require_10bit = true;
      } else if (arg == "--match-friendly" && i + 1 < argc) {
        required_friendly_name = widen_ascii(argv[++i]);
      }
    }
    const auto result = print_active_paths(nullptr, 0, width, height, hz, enable_hdr, require_hdr, require_10bit, required_friendly_name);
    if (!result.matched_requested_mode) {
      std::cerr << "FAIL: no active CCD path at requested " << width << 'x' << height << '@' << hz << "Hz";
      if (require_hdr) {
        std::cerr << " with HDR";
      }
      if (require_10bit) {
        std::cerr << " and 10-bit";
      }
      std::cerr << ". matched=" << result.matched
                << " hdr=" << result.matched_hdr
                << " 10bit=" << result.matched_10bit << '\n';
      return 7;
    }
    std::cout << "PASS: active CCD path found at requested " << width << 'x' << height << '@' << hz << "Hz";
    if (require_hdr) {
      std::cout << " with HDR";
    }
    if (require_10bit) {
      std::cout << " and 10-bit";
    }
    std::cout << ".\n";
    return 0;
  }

  const UINT32 width = argc > 1 ? static_cast<UINT32>(std::stoul(argv[1])) : 3840u;
  const UINT32 height = argc > 2 ? static_cast<UINT32>(std::stoul(argv[2])) : 2160u;
  const UINT32 hz = argc > 3 ? static_cast<UINT32>(std::stoul(argv[3])) : 240u;
  const UINT32 refresh_millihz = hz * 1000u;
  bool skip_ccd = false;
  bool enable_hdr = false;
  bool require_hdr = false;
  bool require_10bit = false;
  bool feed_lease = false;
  DWORD hold_ms = 0;
  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--skip-ccd") {
      skip_ccd = true;
    } else if (arg == "--enable-hdr") {
      enable_hdr = true;
    } else if (arg == "--require-hdr") {
      require_hdr = true;
    } else if (arg == "--require-10bit") {
      require_10bit = true;
    } else if (arg == "--require-hdr10") {
      enable_hdr = true;
      require_hdr = true;
      require_10bit = true;
    } else if (arg == "--feed-lease") {
      feed_lease = true;
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
      final_result = print_active_paths(&created.OsAdapterLuid, created.TargetId, width, height, hz, enable_hdr, require_hdr, require_10bit, {});
      if (final_result.matched_requested_mode) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } while (std::chrono::steady_clock::now() < deadline);
  }

  if (hold_ms > 0) {
    std::cout << "Holding temporary display for " << hold_ms << "ms.\n";
    if (feed_lease) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(hold_ms);
      unsigned int feed_count = 0;
      while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        const auto sleep_until = std::min(now + std::chrono::seconds(10), deadline);
        std::this_thread::sleep_until(sleep_until);
        if (std::chrono::steady_clock::now() >= deadline) {
          break;
        }

        VDD_LEASE_REQUEST feed {};
        feed.ApiNamespace = GUID_VDD_CONTROL_API_NAMESPACE;
        feed.LeaseId = lease_id;
        feed.RequestedTimeoutMs = 30000;
        if (!ioctl(handle, IOCTL_VDD_FEED_LEASE, &feed, sizeof(feed), nullptr, 0)) {
          std::cerr << "FAIL: lease feed failed after " << feed_count << " successful feeds.\n";
          CloseHandle(handle);
          return 8;
        }

        VDD_QUERY_LEASE_RESULT query {};
        query.ApiNamespace = GUID_VDD_CONTROL_API_NAMESPACE;
        if (ioctl(handle, IOCTL_VDD_QUERY_LEASE, &feed, sizeof(feed), &query, sizeof(query))) {
          std::cout << "Lease feed " << ++feed_count
                    << ": exists=" << query.LeaseExists
                    << " remainingMs=" << query.RemainingMs
                    << " temporaryDisplays=" << query.TemporaryDisplayCount << '\n';
        } else {
          std::cout << "Lease feed " << ++feed_count << ": query failed after successful feed.\n";
        }
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    }
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
    std::cerr << "FAIL: created target appeared but not at requested " << width << 'x' << height;
    if (require_hdr) {
      std::cerr << " with HDR";
    }
    if (require_10bit) {
      std::cerr << " and 10-bit";
    }
    std::cerr << ".\n";
    return 6;
  }

  std::cout << "PASS: created target appeared at requested " << width << 'x' << height;
  if (require_hdr) {
    std::cout << " with HDR";
  }
  if (require_10bit) {
    std::cout << " and 10-bit";
  }
  std::cout << ".\n";
  return 0;
}
