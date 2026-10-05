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
