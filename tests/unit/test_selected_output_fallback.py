"""Compile launch fallback and output enumeration with deterministic Windows APIs.

The production selection/validation and final headless-fallback blocks run
together, with both production driver predicates, request-policy dispatch,
effective preparation mapping and topology policy. Windows APIs and request
transport are stubbed; this is not a full helper launch. --source-dir supports before/after
regression checks; --emit-only prepares the replay without compiling it.
"""

import argparse
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source-dir", type=pathlib.Path, default=ROOT / "src")
parser.add_argument("--compiler", default="g++")
parser.add_argument("--emit-only", type=pathlib.Path)
args = parser.parse_args()


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def enumeration(source, name):
    start = source.index(f"enum class {name}")
    return source[start:source.index("};", start) + 2]


launch = (args.source_dir / "nvhttp.cpp").read_text()
display = (args.source_dir / "display_device.cpp").read_text()
config_header = (args.source_dir / "config.h").read_text()
request_helpers = (args.source_dir / "platform/windows/display_helper_request_helpers.cpp").read_text()
request_builder = function(request_helpers, "std::optional<DisplayApplyRequest> build_request_from_session(")
request_policy_input = request_builder[request_builder.index("{") + 1:request_builder.index("    if (!policy.dispatch)")]
constructor_start = request_helpers.index("SessionDisplayConfigurationHelper::SessionDisplayConfigurationHelper(")
constructor_end = request_helpers.index("  std::optional<display_device::Resolution>", constructor_start)
constructor = request_helpers[constructor_start:constructor_end]
topology = (args.source_dir / "platform/windows/display_helper_v2/topology_policy.cpp").read_text()
preparation_mapping = function(display, "[[maybe_unused]] std::optional<SingleDisplayConfiguration::DevicePreparation> parse_device_prep_option(")
validation_start = launch.index("      const auto requested_output_name =")
apollo_output_ownership = "app_output_override ? *app_output_override" in launch[validation_start:validation_start + 200]
validation_end = launch.index("      if (is_input_only) {", validation_start) if apollo_output_ownership else launch.index("      auto apply_virtual_display_request =", validation_start)
validation = launch[validation_start:validation_end]
fallback_end = launch.index("apply_framegen_refresh_policy(request_virtual_display);", validation_end)
fallback_start = launch.rindex("if (!request_virtual_display", validation_end, fallback_end)
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

# Apollo retains its physical-output pin release inside the headless block.
# Model only that request transport; preserve and execute the actual pin logic.
apollo_request_transport = r"""
  std::optional<std::string> app_output_override = has_app_output_override ? std::optional {config::output} : std::nullopt;
  std::optional<std::string> pending_output_override = app_output_override;
  bool pinned_virtual_display_mode_disabled = has_app_output_override;
  auto disable_virtual_display_request = [&] { launch_session->virtual_display = false; };
""" if apollo_output_ownership else ""

program = r'''
#include <algorithm>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
''' + '#include "' + str(args.source_dir.resolve() / "platform/windows/display_helper_request_policy.h") + '"\n' + r'''
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
  struct SingleDisplayConfiguration {
    enum class DevicePreparation { VerifyOnly, EnsureActive, EnsurePrimary, EnsureOnlyDisplay };
  };
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
  bool is_virtual_display_output(const std::string &name) {
    return std::any_of(display_device::devices.begin(), display_device::devices.end(), [&](const auto &d) {
      return d.virtual_display && (boost::iequals(name, d.m_device_id) || boost::iequals(name, d.m_display_name));
    });
  }
  bool should_auto_enable_virtual_display() {
    ++headless_queries;
    return use_sunshine ? VDISPLAY_SUNSHINE::should_auto_enable_virtual_display() : VDISPLAY_SUDOVDA::should_auto_enable_virtual_display();
  }
}
namespace config {
  struct video_t {
''' + enumeration(config_header, "virtual_display_mode_e") + "\n" + enumeration(config_header, "virtual_display_layout_e") + r'''
    struct dd_t {
''' + enumeration(config_header, "config_option_e") + r'''
      config_option_e configuration_option = config_option_e::verify_only;
    } dd;
    virtual_display_mode_e virtual_display_mode = virtual_display_mode_e::disabled;
    virtual_display_layout_e virtual_display_layout = virtual_display_layout_e::exclusive;
    std::string output_name;
  };
  video_t video;
  std::string output;
  std::string get_active_output_name() { return output; }
  std::optional<std::string> runtime_output_name_override() { return output; }
  void set_runtime_output_name_override(const std::optional<std::string> &) {}
}
namespace rtsp_stream {
  struct launch_session_t {
    std::optional<config::video_t::dd_t::config_option_e> dd_config_option_override;
    std::optional<config::video_t::virtual_display_mode_e> virtual_display_mode_override;
    std::optional<config::video_t::virtual_display_layout_e> virtual_display_layout_override;
    std::optional<std::string> output_name_override;
    bool virtual_display = false, virtual_display_failed = false;
    std::string virtual_display_device_id;
  };
}
namespace display_device {
''' + display_helpers + "\n" + preparation_mapping + "\n}\n" + driver_helpers + r'''
namespace display_helper_integration::helpers {
''' + function(request_helpers, "bool output_name_targets_virtual(") + "\n" + function(request_helpers, "bool session_has_physical_output_override(") + r'''
  struct SessionDisplayConfigurationHelper {
    const config::video_t &video_config_;
    config::video_t effective_video_config_;
    const rtsp_stream::launch_session_t &session_;
    SessionDisplayConfigurationHelper(const config::video_t &, const rtsp_stream::launch_session_t &, bool = false);
  };
''' + constructor + r'''
  // Exercise the actual builder's dispatch input, actual helper constructor's
  // override resolution and actual physical preparation parser, without IPC.
  std::optional<display_device::SingleDisplayConfiguration::DevicePreparation> request_preparation(
      const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
''' + request_policy_input + r'''
    if (!policy.dispatch) return std::nullopt;
    SessionDisplayConfigurationHelper helper(video_config, session);
    return display_device::parse_device_prep_option(helper.effective_video_config_);
  }
}
namespace actual_topology_policy {
  using ActiveTopology = std::vector<std::vector<std::string>>;
  using display_device::SingleDisplayConfiguration;
''' + function(topology, "std::set<std::string> flatten_topology(") + "\n" + function(topology, "ActiveTopology compute_new_topology(") + r'''
}
rtsp_stream::launch_session_t launch_settings;
std::optional<display_device::SingleDisplayConfiguration::DevicePreparation> last_request_preparation;
bool prepare(bool has_app_output_override = false, bool request_virtual_display = false) {
  bool no_active_sessions = true;
  auto launch_session = std::make_shared<rtsp_stream::launch_session_t>(launch_settings);
  if (has_app_output_override) launch_session->output_name_override = config::output;
''' + apollo_request_transport + validation + fallback + r'''
  last_request_preparation = display_helper_integration::helpers::request_preparation(config::video, *launch_session);
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
    config::video = {}; launch_settings = {}; last_request_preparation.reset();
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
    reset({inactive}, "{vdd}"); config::video.dd.configuration_option = config::video_t::dd_t::config_option_e::ensure_active;
    check("inactive configured output can activate", prepare(), false);
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

    // U007: inactive physical selection with no active physical output. Test
    // default VerifyOnly, Disabled and every activating policy, inherited and
    // overridden, with/without helper access and an empty/nonempty topology.
    using Option = config::video_t::dd_t::config_option_e;
    const std::vector<Option> options {Option::disabled, Option::verify_only, Option::ensure_active, Option::ensure_primary, Option::ensure_only_display};
    std::vector<std::optional<Option>> overrides {std::nullopt};
    for (const auto option : options) overrides.push_back(option);
    auto inactive_physical = physical; inactive_physical.m_display_name.clear(); inactive_physical.m_info.reset();
    for (const auto global : options) for (const auto override : overrides) {
      reset({vdd}, "{vdd}");
      config::video.dd.configuration_option = global; launch_settings.dd_config_option_override = override;
      check("active output survives all preparation policies", prepare(), false);
      reset({vdd}, "{vdd}"); physical_devices.clear(); fail_query = 1;
      config::video.dd.configuration_option = global; launch_settings.dd_config_option_override = override;
      check("unknown output keeps fail-open semantics under every policy", prepare(), false);
      for (const bool owns_output : {false, true}) for (const bool has_helper : {false, true}) for (const bool has_peer : {false, true}) {
        reset(has_peer ? EnumeratedDeviceList {inactive_physical, vdd} : EnumeratedDeviceList {inactive_physical}, "{physical}");
        config::video.dd.configuration_option = global;
        launch_settings.dd_config_option_override = override;
        helper_available = has_helper;
        const bool fallback = prepare(owns_output);
        using namespace actual_topology_policy;
        const ActiveTopology initial = has_peer ? ActiveTopology {{"{vdd}"}} : ActiveTopology {};
        const auto request = last_request_preparation;
        const auto applied = request ? compute_new_topology(*request, false, "{physical}", {}, initial) : initial;
        const bool activated = has_helper && flatten_topology(applied).contains("{physical}");
        check("inactive target has activation or headless fallback", fallback || activated, true);
        const bool can_activate = request && *request != SingleDisplayConfiguration::DevicePreparation::VerifyOnly;
        check("fallback respects effective request preparation", fallback, !(has_helper && can_activate));
      }
    }
  }
  std::cout << cases << " selected-output fallback cases, " << failures << " failures\n";
  return failures != 0;
}
'''

if args.emit_only:
    args.emit_only.write_text(program)
    print(f"Prepared selected-output/request-policy replay at {args.emit_only}; no compiler or test executed")
    raise SystemExit(0)

with tempfile.TemporaryDirectory(prefix="selected-output-fallback-") as temporary:
    directory = pathlib.Path(temporary)
    source = directory / "test.cpp"
    source.write_text(program)
    binary = directory / "test"
    subprocess.run([args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-Wno-missing-field-initializers", "-D_WIN32",
                    "-I", str(args.source_dir.resolve().parent), str(source),
                    str(args.source_dir / "platform/windows/display_helper_request_policy.cpp"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
