"""Compile launch fallback and output enumeration with deterministic Windows APIs.

The production selection/validation and final headless-fallback blocks run
together, with both production driver predicates. No display or driver changes
are performed. Optional --source-dir supports before/after regression checks.
"""

import argparse
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source-dir", type=pathlib.Path, default=ROOT / "src")
parser.add_argument("--compiler", default="g++")
args = parser.parse_args()


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


launch = (args.source_dir / "nvhttp.cpp").read_text()
display = (args.source_dir / "display_device.cpp").read_text()
validation_start = launch.index("      const auto requested_output_name = config::get_active_output_name();")
validation_end = launch.index("      auto apply_virtual_display_request =", validation_start)
validation = launch[validation_start:validation_end]
fallback_end = launch.index("      apply_framegen_refresh_policy(request_virtual_display);", validation_end)
fallback_start = launch.rindex("      if (!request_virtual_display", validation_end, fallback_end)
fallback = function(launch[fallback_start:], "if (")

display_helpers = "\n".join(function(display, signature) for signature in (
    "bool output_exists(", "bool output_is_active(",
))
output_state_declaration = ""
if "output_state_e get_output_state(" in display:
    display_helpers += "\n" + function(display, "output_state_e get_output_state(")
    header = (args.source_dir / "display_device.h").read_text()
    start = header.index("enum class output_state_e")
    output_state_declaration = header[start:header.index("};", start) + 2]

driver_helpers = ""
for backend in ("SUNSHINE", "SUDOVDA"):
    source = (args.source_dir / f"platform/windows/virtual_display_{backend.lower()}.cpp").read_text()
    driver_helpers += f"\nnamespace VDISPLAY_{backend} {{\n"
    driver_helpers += "struct active_physical_snapshot_t { bool enumeration_available {}; std::vector<std::string> display_names; };\n"
    driver_helpers += "bool is_virtual_display_device(const display_device::EnumeratedDevice &d) { return d.virtual_display; }\n"
    driver_helpers += "bool isVirtualDisplayDriverInstalled() { return driver_installed; }\n"
    driver_helpers += "bool isSudaVDADriverInstalled() { return driver_installed; }\n"
    driver_helpers += function(source, "active_physical_snapshot_t active_physical_displays(") + "\n}\n"
    driver_helpers += function(source, f"bool VDISPLAY_{backend}::has_active_physical_display(") + "\n"
    driver_helpers += function(source, f"bool VDISPLAY_{backend}::should_auto_enable_virtual_display(") + "\n"

program = r'''
#include <algorithm>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#define BOOST_LOG(level) std::ostringstream()
namespace boost {
  bool iequals(const std::string &a, const std::string &b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
  }
}
namespace display_device {
  enum class DeviceEnumerationDetail { Minimal };
  enum class QueryType { All };
  enum class DisplayRecoveryBehavior { Skip };
  struct DisplayRecoveryBehaviorGuard { explicit DisplayRecoveryBehaviorGuard(DisplayRecoveryBehavior) {} };
''' + output_state_declaration + r'''
  struct EnumeratedDevice {
    std::string m_device_id, m_display_name, m_friendly_name;
    bool virtual_display = false;
    struct Info { bool m_primary = false; };
    std::optional<Info> m_info;
  };
  using EnumeratedDeviceList = std::vector<EnumeratedDevice>;
  struct PathAndModeData {};
  EnumeratedDeviceList devices, physical_devices;
  int query_count = 0, fail_query = 0, throw_query = 0;
  bool physical_enumeration_available = true;
  class WinApiLayer {
   public:
    virtual ~WinApiLayer() = default;
    virtual std::optional<PathAndModeData> queryDisplayConfig(QueryType) const {
      ++query_count;
      if (query_count == throw_query) throw std::runtime_error("enumeration failed");
      if (query_count == fail_query) return std::nullopt;
      return PathAndModeData {};
    }
  };
  struct WinDisplayDevice {
    std::shared_ptr<WinApiLayer> api;
    explicit WinDisplayDevice(std::shared_ptr<WinApiLayer> value): api(std::move(value)) {}
    EnumeratedDeviceList enumAvailableDevices(DeviceEnumerationDetail) {
      if (!api->queryDisplayConfig(QueryType::All)) return {};
      return devices;
    }
  };
}
bool driver_installed = true, helper_available = true;
int helper_queries = 0, headless_queries = 0;
bool display_helper_session_available() { ++helper_queries; return helper_available; }
namespace platf {
  bool automatic_adapter = true, adapter_query_available = true;
  std::vector<std::string> adapter_outputs;
  bool configured_capture_adapter_has_output(const std::vector<std::string> &names) {
    if (names.empty()) return false;
    if (automatic_adapter || !adapter_query_available) return true;
    return std::any_of(names.begin(), names.end(), [](const auto &name) {
      return std::find(adapter_outputs.begin(), adapter_outputs.end(), name) != adapter_outputs.end();
    });
  }
  namespace display_helper {
    struct Coordinator {
      static Coordinator &instance() { static Coordinator value; return value; }
      std::optional<display_device::EnumeratedDeviceList> enumerate_devices(display_device::DeviceEnumerationDetail) {
        if (!display_device::physical_enumeration_available) return std::nullopt;
        return display_device::physical_devices;
      }
    };
  }
}
namespace VDISPLAY_SUNSHINE { bool has_active_physical_display(); bool should_auto_enable_virtual_display(); }
namespace VDISPLAY_SUDOVDA { bool has_active_physical_display(); bool should_auto_enable_virtual_display(); }
bool use_sunshine = true;
namespace VDISPLAY {
  bool should_auto_enable_virtual_display() {
    ++headless_queries;
    return use_sunshine ? VDISPLAY_SUNSHINE::should_auto_enable_virtual_display() : VDISPLAY_SUDOVDA::should_auto_enable_virtual_display();
  }
}
namespace config {
  std::string output;
  std::string get_active_output_name() { return output; }
}
namespace display_device {
''' + display_helpers + "\n}\n" + driver_helpers + r'''
bool prepare(bool has_app_output_override = false, bool request_virtual_display = false) {
  bool no_active_sessions = true;
''' + validation + fallback + r'''
  return request_virtual_display;
}
int main() {
  using namespace display_device;
  const EnumeratedDevice vdd {"{vdd}", R"(\\.\DISPLAY7)", "Existing VDD", true, EnumeratedDevice::Info {true}};
  const EnumeratedDevice physical {"{physical}", R"(\\.\DISPLAY1)", "Physical", false, EnumeratedDevice::Info {true}};
  auto inactive = vdd; inactive.m_display_name.clear(); inactive.m_info.reset();
  auto secondary = vdd; secondary.m_info->m_primary = false;
  auto reset = [&](EnumeratedDeviceList outputs, std::string selection) {
    devices = outputs; physical_devices = outputs; config::output = selection;
    query_count = fail_query = throw_query = helper_queries = headless_queries = 0;
    driver_installed = helper_available = physical_enumeration_available = true;
    platf::automatic_adapter = platf::adapter_query_available = true; platf::adapter_outputs.clear();
  };
  int failures = 0, cases = 0;
  auto check = [&](const char *name, bool result, bool expected) {
    ++cases;
    if (result != expected) { ++failures; std::cerr << (use_sunshine ? "Sunshine" : "SudoVDA") << ": " << name << " expected virtual=" << expected << " got " << result << '\n'; }
  };
  for (bool sunshine : {true, false}) {
    use_sunshine = sunshine;
    for (bool owned_override : {false, true}) {
      for (auto name : {"{vdd}", R"(\\.\display7)", "existing vdd"}) {
        reset({vdd}, name);
        check("valid explicit VDD", prepare(owned_override), false);
      }
      reset({vdd}, ""); check("existing primary VDD", prepare(owned_override), false);
      reset({vdd, physical}, "{vdd}"); check("mixed physical and virtual", prepare(owned_override), false);
      reset({physical, vdd}, "{physical}"); check("explicit physical", prepare(owned_override), false);
      reset({physical, vdd}, ""); check("primary in mixed topology", prepare(owned_override), false);
    }
    reset({}, ""); check("empty headless topology", prepare(), true);
    reset({inactive}, ""); check("inactive is not a primary", prepare(), true);
    reset({secondary}, ""); check("secondary is not primary", prepare(), true);
    reset({vdd}, "{missing}"); check("unrelated VDD does not validate a selection", prepare(), true);
    reset({physical}, "{missing}"); check("missing configured target retains fallback", prepare(), true);
    reset({physical}, "{missing}"); check("app owns invalid output with physical fallback", prepare(true), false);
    reset({vdd}, "{missing}"); check("missing app target retains headless fallback", prepare(true), true);
    reset({inactive}, "{vdd}"); check("inactive configured output can activate", prepare(), false);
    reset({inactive}, "{vdd}"); helper_available = false; check("inactive configured output without helper", prepare(), true);
    reset({inactive}, "{vdd}"); helper_available = false; check("inactive app target without helper retains headless fallback", prepare(true), true);
    reset({vdd}, "{vdd}"); physical_devices.clear(); check("later enumeration loses valid selected output", prepare(), false);
    reset({vdd}, "{vdd}"); fail_query = 2; physical_devices.clear(); check("single selected-output snapshot avoids second query", prepare(), false);
    reset({vdd}, "{vdd}"); platf::automatic_adapter = false; check("selected output on wrong adapter retains fallback", prepare(), true);
    reset({physical}, ""); platf::automatic_adapter = false; check("primary on wrong adapter retains fallback", prepare(), true);
    reset({physical, vdd}, "{vdd}"); platf::automatic_adapter = false; platf::adapter_outputs = {vdd.m_display_name};
    check("virtual selection on configured adapter with physical on another", prepare(), false);
    reset({vdd}, "{vdd}"); platf::automatic_adapter = false; platf::adapter_query_available = false;
    check("unavailable adapter query stays fail-open", prepare(), false);
    reset({vdd}, "{vdd}"); fail_query = 1; check("failed Windows query preserves explicit target", prepare(), false);
    reset({vdd}, ""); fail_query = 1; check("failed Windows query preserves primary selection", prepare(), false);
    reset({vdd}, "{vdd}"); throw_query = 1; physical_devices.clear(); check("thrown enumeration preserves explicit target", prepare(), false);
    reset({}, ""); physical_enumeration_available = false; check("unavailable physical detection stays fail-open", prepare(), false);
    reset({}, ""); driver_installed = false; check("uninstalled driver is not enabled", prepare(), false);
    reset({vdd}, "{vdd}"); check("explicit virtual mode still wins", prepare(false, true), true);
    reset({vdd}, "{vdd}"); check("selection initially available", prepare(), false);
    devices.clear(); physical_devices.clear(); check("selection revalidated next launch", prepare(), true);
  }
  std::cout << cases << " selected-output fallback cases, " << failures << " failures\n";
  return failures != 0;
}
'''

with tempfile.TemporaryDirectory(prefix="selected-output-fallback-") as temporary:
    directory = pathlib.Path(temporary)
    source = directory / "test.cpp"
    source.write_text(program)
    binary = directory / "test"
    subprocess.run([args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-Wno-missing-field-initializers", "-D_WIN32",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
