"""Exercise production Windows color-profile calls with an injected API on Linux."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "src/platform/windows/virtual_display.cpp").read_text()


def function(name):
    start = source.index(name)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


program = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include "src/color_profile_policy.h"
using HRESULT = std::int32_t;
using LSTATUS = long;
constexpr HRESULT E_NOTIMPL = static_cast<HRESULT>(0x80004001u);
constexpr HRESULT DENIED = static_cast<HRESULT>(0x80070005u);
constexpr LSTATUS ERROR_SUCCESS = 0, ERROR_NOT_FOUND = 1168, ERROR_GEN_FAILURE = 31;
constexpr int FACILITY_WIN32 = 7;
constexpr bool TRUE = true;
#define SUCCEEDED(hr) ((hr) >= 0)
#define HRESULT_FACILITY(hr) ((static_cast<std::uint32_t>(hr) >> 16) & 0x1fff)
#define HRESULT_CODE(hr) (static_cast<std::uint32_t>(hr) & 0xffff)
#define BOOST_LOG(level) std::clog
struct advanced_color_profile_result_t {
  bool api_available = false, target_found = false, attempted = false, success = false;
  HRESULT association_status = E_NOTIMPL;
};
struct target_t { int target_adapter_id = 11, source_id = 22; };
bool target_found = true;
std::optional<target_t> advanced_color_target_for_monitor(const std::wstring &) {
  return target_found ? std::optional<target_t>(target_t {}) : std::nullopt;
}
int profile_scope(bool system_wide) { return system_wide ? 1 : 0; }
struct api_t {
  bool supported = true;
  HRESULT status = DENIED;
  mutable int calls = 0;
  bool available() const { return supported; }
  HRESULT add(int scope, const wchar_t *profile, int adapter, int source,
              bool set_default, bool advanced) const {
    assert(scope == 0 && std::wstring(profile) == L"client.icc");
    assert(adapter == 11 && source == 22 && set_default && advanced);
    ++calls;
    return status;
  }
} api;
api_t &advanced_color_api() { return api; }
''' + function("advanced_color_profile_result_t set_advanced_color_profile(") + "\n" + function("bool associate_hdr_profile(") + r'''
int main() {
  int registry_writes = 0;
  LSTATUS error = -1;
  auto legacy = [&](LSTATUS *status) { ++registry_writes; *status = 0; return true; };
  auto apply = [&] { return associate_hdr_profile(L"monitor", L"client.icc", false, legacy, &error); };
  assert(!apply()); // Access denied must not poison the registry or be treated as success.
  assert(error == 5 && registry_writes == 0 && api.calls == 1);
  api.status = 0;
  assert(apply());
  assert(error == 0 && registry_writes == 0 && api.calls == 2);
  target_found = false;
  assert(!apply()); // A transiently absent CCD target is not an older Windows API.
  assert(error == ERROR_NOT_FOUND && registry_writes == 0 && api.calls == 2);
  api.supported = false;
  assert(apply());
  assert(error == 0 && registry_writes == 1 && api.calls == 2);
}
'''

with tempfile.TemporaryDirectory(prefix="vibeshine-color-profile-") as work:
    cpp = Path(work) / "test.cpp"
    binary = Path(work) / "test"
    cpp.write_text(program)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root), str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


# Exercise the actual physical-route guard with failed/incomplete enumeration.
# Profile APIs are never called by this second fixture.
sunshine = (root / "src/platform/windows/virtual_display_sunshine.cpp").read_text()


def sunshine_function(name):
    start = sunshine.index(name)
    opening = sunshine.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (sunshine[end] == "{") - (sunshine[end] == "}")
        end += 1
    return sunshine[start:end]


guard_start = sunshine.index("        // Resolve known driver hardware IDs before optional enumeration:")
guard_end = sunshine.index("        bool success = false;", guard_start)
guard = sunshine[guard_start:guard_end]
predicates = "\n".join(sunshine_function(name) for name in [
    "bool equals_ci(const std::string &lhs, const std::string &rhs)",
    "bool contains_ci(const std::string &haystack, const std::string &needle)",
    "bool starts_with_ci(const std::string &value, const std::string &prefix)",
])
predicates += "\nnamespace VDISPLAY {\n" + sunshine_function("bool is_sunshine_virtual_display_identity(") + "\n}\n"
predicates += "using VDISPLAY::is_sunshine_virtual_display_identity;\n"
guard_program = r"""
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cwctype>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include "src/color_profile_policy.h"
struct Edid { std::string m_manufacturer_id, m_product_code; };
struct Device { std::string m_monitor_device_path, m_friendly_name; std::optional<Edid> m_edid; };
namespace display_device { enum class DeviceEnumerationDetail { Minimal }; }
std::optional<std::vector<Device>> enumeration;
int enumeration_calls = 0;
namespace platf {
std::wstring from_utf8(const std::string &s) { return {s.begin(), s.end()}; }
namespace display_helper {
struct Coordinator {
  static Coordinator &instance() { static Coordinator value; return value; }
  std::optional<std::vector<Device>> enumerate_devices(display_device::DeviceEnumerationDetail) {
    ++enumeration_calls;
    return enumeration;
  }
};
}
}
#define _wcsicmp fixture_wcsicmp
int _wcsicmp(const wchar_t *lhs, const wchar_t *rhs) {
  while (*lhs && std::towlower(*lhs) == std::towlower(*rhs)) { ++lhs; ++rhs; }
  return std::towlower(*lhs) - std::towlower(*rhs);
}
""" + predicates + r"""
bool association_reached = false;
void exact_guard(const std::wstring &path) {
  std::optional<std::wstring> device_name_w = path;
""" + guard + r"""
  association_reached = true;
}
void test(const char *name, const std::wstring &path,
          std::optional<std::vector<Device>> records, bool expected, bool enumerate) {
  enumeration = records;
  enumeration_calls = 0;
  association_reached = false;
  exact_guard(path);
  assert(association_reached == expected);
  assert((enumeration_calls != 0) == enumerate);
  std::cout << name << ": PASS\n";
}
int main() {
  const auto driver = LR"(\\?\DISPLAY#SDD4100#permanent#{id})";
  const auto driver_utf8 = R"(\\?\DISPLAY#SDD4100#permanent#{id})";
  test("null enumeration", driver, std::nullopt, false, false);
  test("missing identity", driver, std::vector<Device>{{driver_utf8, "", std::nullopt}}, false, false);
  test("known driver EDID", driver, std::vector<Device>{{driver_utf8, "", Edid{"SDD", "4100"}}}, false, false);
  test("mixed case default ID", LR"(\\?\display#sdd4aBc#permanent#{id})", std::nullopt, false, false);
  test("temporary default ID", LR"(\\?\DISPLAY#SDD5FFF#temporary#{id})", std::nullopt, false, false);
  test("physical null enumeration", LR"(\\?\DISPLAY#DEL1234#physical#{id})", std::nullopt, true, true);
  test("physical metadata", LR"(\\?\DISPLAY#DEL1234#physical#{id})",
       std::vector<Device>{{R"(\\?\DISPLAY#DEL1234#physical#{id})", "Physical", Edid{"DEL", "1234"}}}, true, true);
  test("SudoVDA override", LR"(\\?\DISPLAY#MTT1337#sudo#{id})", std::nullopt, true, true);
  test("custom driver metadata", LR"(\\?\DISPLAY#XYZ1234#custom#{id})",
       std::vector<Device>{{R"(\\?\DISPLAY#XYZ1234#custom#{id})", "", Edid{"SDD", "4100"}}}, false, true);
  test("custom physical metadata", LR"(\\?\DISPLAY#XYZ1234#custom#{id})",
       std::vector<Device>{{R"(\\?\DISPLAY#XYZ1234#custom#{id})", "", Edid{"XYZ", "1234"}}}, true, true);
  test("different SDD family", LR"(\\?\DISPLAY#SDD6100#custom#{id})", std::nullopt, true, true);
  test("malformed default ID", LR"(\\?\DISPLAY#SDD4ZZZ#invalid#{id})", std::nullopt, true, true);
}
"""

with tempfile.TemporaryDirectory(prefix="vibeshine-calibration-owner-") as work:
    cpp = Path(work) / "guard.cpp"
    binary = Path(work) / "guard"
    cpp.write_text(guard_program)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root), str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
