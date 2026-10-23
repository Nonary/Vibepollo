#include "src/platform/windows/virtual_display.h"
#include "src/virtual_display_scale.h"

#include <cstring>

namespace {
  bool starts_with_wide_ascii_ci(const std::wstring_view value, const std::wstring_view prefix) {
    if (value.size() < prefix.size()) {
      return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
      const auto fold = [](const wchar_t ch) {
        return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch;
      };
      if (fold(value[i]) != fold(prefix[i])) {
        return false;
      }
    }
    return true;
  }

  bool contains_wide_ascii_ci(const std::wstring_view haystack, const std::wstring_view needle) {
    if (needle.empty()) {
      return true;
    }
    if (haystack.size() < needle.size()) {
      return false;
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
      if (starts_with_wide_ascii_ci(haystack.substr(i), needle)) {
        return true;
      }
    }
    return false;
  }
}  // namespace

namespace VDISPLAY {
  bool is_virtual_display_monitor_path(const std::wstring &monitor_device_path) {
    if (monitor_device_path.empty()) {
      return false;
    }

    const std::wstring_view path = monitor_device_path;
    if (contains_wide_ascii_ci(path, L"SunshineVirtualDisplay") ||
        contains_wide_ascii_ci(path, L"Sunshine Virtual Display") ||
        contains_wide_ascii_ci(path, L"SUDOVDA") ||
        contains_wide_ascii_ci(path, L"SUDOMAKER")) {
      return true;
    }

    // Windows monitor paths normally expose the device hardware ID as
    // DISPLAY#<hardware-id>#.... SDD4/SDD5 are Sunshine's synthetic EDIDs;
    // SMK is SudoVDA's synthetic manufacturer ID.
    constexpr std::wstring_view kDisplayPrefix = L"DISPLAY#";
    const auto display_prefix = path.find(kDisplayPrefix);
    if (display_prefix == std::wstring_view::npos) {
      return false;
    }
    const auto hardware_id_begin = display_prefix + kDisplayPrefix.size();
    const auto hardware_id_end = path.find(L'#', hardware_id_begin);
    if (hardware_id_end == std::wstring_view::npos) {
      return false;
    }
    const auto hardware_id = path.substr(hardware_id_begin, hardware_id_end - hardware_id_begin);
    if (starts_with_wide_ascii_ci(hardware_id, L"SMK")) {
      return true;
    }
    if (!starts_with_wide_ascii_ci(hardware_id, L"SDD")) {
      return false;
    }
    const auto product_code = hardware_id.substr(3);
    return starts_with_wide_ascii_ci(product_code, L"4") ||
           starts_with_wide_ascii_ci(product_code, L"5");
  }

  std::uint32_t effective_virtual_display_scale_percent(
    const int configured_scale_percent,
    const std::uint32_t width,
    const std::uint32_t height
  ) {
    return virtual_display_scale::effective_percent(configured_scale_percent, width, height);
  }

  std::uint64_t client_uuid_to_virtual_display_id(const GUID &client_guid) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(&client_guid);
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < sizeof(GUID); ++i) {
      hash ^= static_cast<std::uint64_t>(bytes[i]);
      hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
  }

  uuid_util::uuid_t virtualDisplayUuidFromStableId(const std::string &stable_id) {
    if (!stable_id.empty()) {
      try {
        return uuid_util::uuid_t::parse(stable_id);
      } catch (...) {
      }
    }

    uuid_util::uuid_t uuid {};
    constexpr std::uint64_t k_fnv_offset = 14695981039346656037ull;
    constexpr std::uint64_t k_fnv_prime = 1099511628211ull;

    const auto hash_with_domain = [&](const std::string_view domain) {
      std::uint64_t hash = k_fnv_offset;
      for (const auto ch : domain) {
        hash ^= static_cast<std::uint8_t>(ch);
        hash *= k_fnv_prime;
      }
      for (const auto ch : stable_id) {
        hash ^= static_cast<std::uint8_t>(ch);
        hash *= k_fnv_prime;
      }
      return hash;
    };

    uuid.b64[0] = hash_with_domain("sunshine-virtual-display-a:");
    uuid.b64[1] = hash_with_domain("sunshine-virtual-display-b:");
    uuid.b8[6] = static_cast<std::uint8_t>((uuid.b8[6] & 0x0f) | 0x50);
    uuid.b8[8] = static_cast<std::uint8_t>((uuid.b8[8] & 0x3f) | 0x80);
    if (uuid.b64[0] == 0 && uuid.b64[1] == 0) {
      uuid.b8[15] = 1;
    }
    return uuid;
  }

  uuid_util::uuid_t persistentVirtualDisplayUuid() {
    // The shared encoder-probe display needs a stable identity that cannot be
    // inherited from a paired client's persisted state.
    return virtualDisplayUuidFromStableId(std::string {policy::ensure_display_stable_id});
  }

  GUID sharedVirtualDisplayGuid() {
    const auto uuid = persistentVirtualDisplayUuid();
    GUID guid {};
    std::memcpy(&guid, uuid.b8, sizeof(guid));
    return guid;
  }

}  // namespace VDISPLAY
