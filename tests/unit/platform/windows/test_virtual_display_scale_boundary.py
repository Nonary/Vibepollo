#!/usr/bin/env python3
"""Run real Windows DPI/identity functions with deterministic OS boundaries.

Usage: python test_virtual_display_scale_boundary.py [C++ compiler]
No Windows SDK or service is required. Live driver/CCD activation still needs
Windows integration testing; these regressions exercise the actual source,
not a duplicate implementation of its DPI arithmetic or identity selection.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[4]
scale = (root / "src/platform/windows/virtual_display.cpp").read_text()
identity = (root / "src/platform/windows/virtual_display_sunshine.cpp").read_text()
header = (root / "src/platform/windows/virtual_display.h").read_text()


def function(source, name):
    match = re.search(r"^(?P<indent>  (?:  )?)[^\n]*\b" + name + r"\([\s\S]*?^(?P=indent)\}", source, re.M)
    if not match:
        raise RuntimeError("Missing source function: " + name)
    return match.group()


result_struct = re.search(r"  struct display_scale_result_t \{[\s\S]*?^  \};", header, re.M).group()
protocol = scale[scale.index("  constexpr std::array<std::uint32_t, 12> kWindowsScalePercentages"):scale.index("  std::optional<std::wstring> utf8_to_wide")]
harness = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
using LONG = std::int32_t;
using DISPLAYCONFIG_DEVICE_INFO_TYPE = std::int32_t;
constexpr LONG ERROR_SUCCESS=0, ERROR_INVALID_PARAMETER=87, ERROR_NOT_FOUND=1168,
  ERROR_INVALID_DATA=13, ERROR_NOT_SUPPORTED=50;
struct DISPLAYCONFIG_DEVICE_INFO_HEADER { DISPLAYCONFIG_DEVICE_INFO_TYPE type; std::uint32_t size; int adapterId; unsigned id; };
struct advanced_color_target_t { int source_adapter_id=1; unsigned source_id=2; };
std::ostringstream logs;
#define BOOST_LOG(level) logs
void Sleep(unsigned) {}
bool active = true;
bool is_virtual_display_monitor_path(const std::wstring &path) { return path.starts_with(L"virtual:"); }
std::optional<advanced_color_target_t> advanced_color_target_for_monitor(const std::wstring &) {
  return active ? std::optional(advanced_color_target_t{}) : std::nullopt;
}
// PROTOCOL
// RESULT
std::int32_t minimum=-2, current=1234568, maximum=5, written=0;
int writes=0;
LONG query_status=0, write_status=0;
LONG DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER *header) {
  auto &get = *reinterpret_cast<sunshine_displayconfig_get_dpi_scale_t *>(header);
  get.min_scale_relative=minimum; get.current_scale_relative=current; get.max_scale_relative=maximum;
  return query_status;
}
LONG DisplayConfigSetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER *header) {
  auto &set = *reinterpret_cast<sunshine_displayconfig_set_dpi_scale_t *>(header);
  written=set.scale_relative; ++writes;
  if (write_status == 0) current=written;
  return write_status;
}
// SETTER
struct Device { std::string m_device_id, m_monitor_device_path; bool is_virtual=true; };
std::optional<std::vector<Device>> devices;
bool equals_ci(const std::string &a, const std::string &b) {
  return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(), [](unsigned char x,unsigned char y) { return std::tolower(x)==std::tolower(y); });
}
bool is_virtual_display_device(const Device &device) { return device.is_virtual; }
namespace display_device { enum class DeviceEnumerationDetail { Minimal }; }
namespace platf {
  std::wstring from_utf8(const std::string &s) { return {s.begin(),s.end()}; }
  namespace display_helper {
    struct Coordinator {
      static Coordinator &instance() { static Coordinator value; return value; }
      auto enumerate_devices(display_device::DeviceEnumerationDetail) { return devices; }
    };
  }
}
// RESOLVER
void reset() { minimum=-2; current=1234568; maximum=5; written=0; writes=0; query_status=0; write_status=0; active=true; }
int main() {
  // Fresh monitor's unknown current value must not prevent the first write.
  reset(); auto result=set_display_scale_percent(L"virtual:one",175);
  assert(result.applied && result.previous_percent==0 && result.recommended_percent==150);
  assert(result.current_percent==175 && writes==1 && written==1);

  // Windows may return the sentinel again on the next session: write again.
  current=1234568; result=set_display_scale_percent(L"virtual:one",175);
  assert(result.applied && writes==2 && written==1);

  reset(); current=1; result=set_display_scale_percent(L"virtual:one",175);
  assert(result.applied && result.previous_percent==175 && writes==0);
  reset(); current=0; result=set_display_scale_percent(L"virtual:one",225);
  assert(result.applied && result.previous_percent==150 && written==3 && writes==1);

  // Unknown current values are observational only, including extreme values.
  for (auto unknown : {std::numeric_limits<std::int32_t>::max(),std::numeric_limits<std::int32_t>::min(),-3}) {
    reset(); current=unknown; result=set_display_scale_percent(L"virtual:one",175);
    assert(result.applied && result.previous_percent==0 && written==1 && writes==1);
  }
  // Invalid recommendations must still be rejected, never clamped.
  for (auto invalid : {1,-12,std::numeric_limits<std::int32_t>::min()}) {
    reset(); minimum=invalid; result=set_display_scale_percent(L"virtual:one",175);
    assert(!result.applied && result.status==ERROR_INVALID_DATA && writes==0);
  }
  reset(); result=set_display_scale_percent(L"virtual:one",350);
  assert(!result.applied && result.status==ERROR_NOT_SUPPORTED && writes==0);
  reset(); result=set_display_scale_percent(L"virtual:one",123);
  assert(!result.applied && result.status==ERROR_INVALID_PARAMETER && writes==0);
  reset(); result=set_display_scale_percent(L"physical:one",175);
  assert(!result.applied && !result.target_found && writes==0);
  reset(); active=false; result=set_display_scale_percent(L"virtual:one",175);
  assert(!result.applied && !result.target_found && writes==0);
  active=true; result=set_display_scale_percent(L"virtual:one",175);
  assert(result.applied && writes==1);
  reset(); query_status=5; result=set_display_scale_percent(L"virtual:one",175);
  assert(!result.applied && !result.queried && result.status==5 && writes==0);
  reset(); write_status=5; result=set_display_scale_percent(L"virtual:one",175);
  assert(!result.applied && result.status==5 && writes==1);

  // Resolve only the exact stable identity, even without any GDI name. Retry
  // against a fresh enumeration after publication instead of capturing a name.
  devices=std::vector<Device>{{"{other}","virtual:other"},{"{wanted}",""}};
  assert(!resolve_virtual_monitor_device_path_for_id("{wanted}"));
  devices->back().m_monitor_device_path="virtual:wanted";
  assert(resolve_virtual_monitor_device_path_for_id("{WANTED}")==L"virtual:wanted");
  assert(!resolve_virtual_monitor_device_path_for_id("{missing}"));
  assert(!resolve_virtual_monitor_device_path_for_id(""));
  devices->back().is_virtual=false;
  assert(!resolve_virtual_monitor_device_path_for_id("{wanted}"));
  devices=std::nullopt;
  assert(!resolve_virtual_monitor_device_path_for_id("{wanted}"));
  std::cout << "Windows DPI sentinel, bounds, write failures, session retries, and stable identity regressions passed.\n";
}
'''
harness = harness.replace("// PROTOCOL", protocol).replace("// RESULT", result_struct)
harness = harness.replace("// SETTER", function(scale, "set_display_scale_percent"))
harness = harness.replace("// RESOLVER", function(identity, "resolve_virtual_monitor_device_path_for_id"))
with tempfile.TemporaryDirectory(prefix="vdisplay-scale-") as directory:
    source = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    source.write_text(harness)
    subprocess.run([sys.argv[1] if len(sys.argv)>1 else "c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined", "-fno-sanitize-recover=undefined", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
