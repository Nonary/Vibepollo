"""Compile and exercise production configured-adapter decision functions.

The functions are extracted verbatim from misc.cpp and virtual_display.cpp.
Only Windows/DXGI and driver calls at their boundaries are deterministic stubs.
The display_base.cpp capture diagnostic strings remain weak source guards because
its native DXGI initialization path cannot be isolated in this portable harness.
"""
import pathlib
import subprocess
import sys
import tempfile


root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[4]
compiler = sys.argv[2] if len(sys.argv) > 2 else "g++"


def extract_function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    in_string = None
    escaped = False
    line_comment = False
    block_comment = False
    index = brace
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if line_comment:
            if char == "\n":
                line_comment = False
        elif block_comment:
            if char == "*" and following == "/":
                block_comment = False
                index += 1
        elif in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == in_string:
                in_string = None
        elif char == "/" and following == "/":
            line_comment = True
            index += 1
        elif char == "/" and following == "*":
            block_comment = True
            index += 1
        elif char in ('"', "'"):
            in_string = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
        index += 1
    raise ValueError(f"Unterminated function: {signature}")


misc = (root / "src/platform/windows/misc.cpp").read_text()
display = (root / "src/platform/windows/virtual_display.cpp").read_text()
sunshine = (root / "src/platform/windows/virtual_display_sunshine.cpp").read_text()
sudovda = (root / "src/platform/windows/virtual_display_sudovda.cpp").read_text()
display_base = (root / "src/platform/windows/display_base.cpp").read_text()
capture_function = extract_function(misc, "bool configured_capture_adapter_has_output(")
render_function = extract_function(display, "bool applyConfiguredRenderAdapterPreference(")
sunshine_wrapper = extract_function(sunshine, "bool apply_configured_render_adapter_preference(")
sudovda_wrapper = extract_function(sudovda, "bool apply_configured_render_adapter_preference(")
sunshine_detection = extract_function(sunshine, "bool VDISPLAY_SUNSHINE::has_active_physical_display() {")
sudovda_detection = extract_function(sudovda, "bool VDISPLAY_SUDOVDA::has_active_physical_display() {")

program = r'''
#include <cassert>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

struct captured_log_t {
  std::string severity;
  std::string message;
};
inline std::vector<captured_log_t> captured_logs;
struct test_log {
  std::string severity;
  std::ostringstream message;
  explicit test_log(std::string_view level) : severity(level) {}
  ~test_log() { captured_logs.push_back({severity, message.str()}); }
  template<class T>
  test_log &operator<<(const T &value) {
    message << value;
    return *this;
  }
};
#define BOOST_LOG(level) test_log {#level}

void assert_error_log_contains(std::string_view fragment) {
  assert(std::any_of(
    captured_logs.begin(),
    captured_logs.end(),
    [&](const auto &entry) {
      return entry.severity == "error" && entry.message.find(fragment) != std::string::npos;
    }
  ));
}

struct LUID { std::uint32_t LowPart = 0; std::int32_t HighPart = 0; };
enum class adapter_resolution_status_e { automatic, resolved, not_found, unknown, ambiguous };
struct adapter_resolution_t {
  adapter_resolution_status_e status = adapter_resolution_status_e::unknown;
  std::optional<LUID> luid;
  std::string description;
  std::string pnp_id;
  std::uint64_t dedicated_video_memory = 0;
  std::uint64_t shared_system_memory = 0;
  explicit operator bool() const { return status == adapter_resolution_status_e::resolved && luid.has_value(); }
};
namespace config {
  struct video_t { std::string adapter_name; std::string adapter_pnp_id; };
  inline video_t video;
}
namespace platf {
  enum class adapter_output_match_e { match, no_match, unknown };
  inline adapter_resolution_t capture_resolution;
  inline adapter_output_match_e output_match = adapter_output_match_e::unknown;
  inline unsigned int resolve_calls = 0;
  inline adapter_resolution_t preferred_resolution;
  inline unsigned int resolve_preferred_calls = 0;
  inline adapter_resolution_t resolve_adapter(std::string_view, std::string_view) {
    ++resolve_calls;
    return capture_resolution;
  }
  inline adapter_output_match_e adapter_drives_any_output(const LUID &, const std::vector<std::string> &) {
    return output_match;
  }
  inline std::string_view adapter_resolution_status_name(adapter_resolution_status_e) { return "test-status"; }
  inline std::wstring from_utf8(const std::string &text) { return std::wstring(text.begin(), text.end()); }
  inline adapter_resolution_t resolve_preferred_render_adapter(std::string_view, std::string_view) {
    ++resolve_preferred_calls;
    return preferred_resolution;
  }
}

namespace platf {
''' + capture_function + r'''
}

namespace VDISPLAY {
  inline bool driver_apply_result = true;
  inline unsigned int apply_calls = 0;
  inline unsigned int fallback_calls = 0;
  inline LUID applied_luid;
  inline bool setRenderAdapterByLuid(const LUID &luid, const std::wstring &, std::uint64_t, std::uint64_t) {
    ++apply_calls;
    applied_luid = luid;
    return driver_apply_result;
  }
  inline bool setRenderAdapterWithMostDedicatedMemory() { ++fallback_calls; return true; }
  inline std::string_view adapter_resolution_status_name(adapter_resolution_status_e) { return "test-status"; }
''' + render_function + r'''
}

namespace VDISPLAY_SUNSHINE {
  struct physical_displays_t { bool enumeration_available = true; std::vector<std::string> display_names; };
  inline physical_displays_t physical;
  physical_displays_t active_physical_displays() { return physical; }
  bool has_active_physical_display();
''' + sunshine_wrapper + r'''
}
namespace VDISPLAY_SUDOVDA {
  using physical_displays_t = VDISPLAY_SUNSHINE::physical_displays_t;
  inline physical_displays_t physical;
  physical_displays_t active_physical_displays() { return physical; }
  bool has_active_physical_display();
''' + sudovda_wrapper + r'''
}
''' + sunshine_detection + "\n" + sudovda_detection + r'''

int main() {
  using platf::adapter_output_match_e;
  const std::vector<std::string> active_displays {"\\\\.\\DISPLAY1"};
  config::video.adapter_name = "Capture GPU";
  platf::capture_resolution = {adapter_resolution_status_e::resolved, LUID {1, 0}, "Capture GPU", "PCI\\GPU1"};

  platf::output_match = adapter_output_match_e::match;
  assert(platf::configured_capture_adapter_has_output(active_displays));
  platf::output_match = adapter_output_match_e::no_match; // An active display on another GPU is irrelevant.
  assert(!platf::configured_capture_adapter_has_output(active_displays));
  // Exercise both actual caller functions too: dropping their configured-GPU
  // filter must not silently restore adapter-agnostic physical detection.
  VDISPLAY_SUNSHINE::physical = {true, active_displays};
  VDISPLAY_SUDOVDA::physical = {true, active_displays};
  for (auto detect : {VDISPLAY_SUNSHINE::has_active_physical_display,
                      VDISPLAY_SUDOVDA::has_active_physical_display}) {
    assert(!detect());
    platf::output_match = adapter_output_match_e::match;
    assert(detect());
    platf::output_match = adapter_output_match_e::no_match;
  }
  VDISPLAY_SUNSHINE::physical = {true, {}};
  VDISPLAY_SUDOVDA::physical = {true, {}};
  assert(!VDISPLAY_SUNSHINE::has_active_physical_display());
  assert(!VDISPLAY_SUDOVDA::has_active_physical_display());
  VDISPLAY_SUNSHINE::physical.enumeration_available = false;
  VDISPLAY_SUDOVDA::physical.enumeration_available = false;
  assert(VDISPLAY_SUNSHINE::has_active_physical_display());
  assert(VDISPLAY_SUDOVDA::has_active_physical_display());
  platf::output_match = adapter_output_match_e::unknown; // Preserve legacy fail-open behavior on API failure.
  assert(platf::configured_capture_adapter_has_output(active_displays));
  platf::capture_resolution.status = adapter_resolution_status_e::not_found;
  platf::capture_resolution.luid.reset();
  assert(platf::configured_capture_adapter_has_output(active_displays)); // Stale identity also fails open.

  assert(!platf::configured_capture_adapter_has_output({}));
  const auto previous_resolve_calls = platf::resolve_calls;
  config::video.adapter_name.clear();
  assert(platf::configured_capture_adapter_has_output(active_displays)); // No preference retains legacy behavior.
  assert(platf::resolve_calls == previous_resolve_calls);

  config::video.adapter_name = "Capture GPU";
  platf::preferred_resolution = {
    adapter_resolution_status_e::resolved, LUID {17, -3}, "Capture GPU", "PCI\\GPU1", 100, 200
  };
  assert(VDISPLAY::applyConfiguredRenderAdapterPreference("test"));
  assert(VDISPLAY::applied_luid.LowPart == 17 && VDISPLAY::applied_luid.HighPart == -3);
  assert(VDISPLAY::apply_calls == 1 && VDISPLAY::fallback_calls == 0);
  assert(VDISPLAY_SUNSHINE::apply_configured_render_adapter_preference("sunshine wrapper"));
  assert(VDISPLAY_SUDOVDA::apply_configured_render_adapter_preference("sudovda wrapper"));
  assert(VDISPLAY::apply_calls == 3 && VDISPLAY::fallback_calls == 0);

  VDISPLAY::driver_apply_result = false;
  captured_logs.clear();
  assert(!VDISPLAY::applyConfiguredRenderAdapterPreference("driver rejection"));
  assert(!VDISPLAY_SUNSHINE::apply_configured_render_adapter_preference("sunshine rejection"));
  assert(!VDISPLAY_SUDOVDA::apply_configured_render_adapter_preference("sudovda rejection"));
  assert(VDISPLAY::apply_calls == 6 && VDISPLAY::fallback_calls == 0);
  assert_error_log_contains("during driver rejection. No fallback adapter will be used.");
  assert_error_log_contains("during sunshine rejection. No fallback adapter will be used.");
  assert_error_log_contains("during sudovda rejection. No fallback adapter will be used.");

  captured_logs.clear();
  platf::preferred_resolution.status = adapter_resolution_status_e::unknown;
  platf::preferred_resolution.luid.reset();
  assert(!VDISPLAY::applyConfiguredRenderAdapterPreference("unresolved preference"));
  assert(!VDISPLAY_SUNSHINE::apply_configured_render_adapter_preference("sunshine unresolved"));
  assert(!VDISPLAY_SUDOVDA::apply_configured_render_adapter_preference("sudovda unresolved"));
  assert(VDISPLAY::apply_calls == 6 && VDISPLAY::fallback_calls == 0);
  assert_error_log_contains("could not be resolved for unresolved preference");
  assert_error_log_contains("could not be resolved for sunshine unresolved");
  assert_error_log_contains("could not be resolved for sudovda unresolved");
  assert_error_log_contains("No fallback adapter will be used.");
}
'''

with tempfile.TemporaryDirectory(prefix="configured-adapter-boundary-") as temporary:
    source = pathlib.Path(temporary) / "test.cpp"
    binary = pathlib.Path(temporary) / "test"
    source.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Native display_base_t::init() failure wording is not behavior-validated here:
# exercising those branches requires DXGI factory, adapter, and output objects,
# which this portable boundary harness deliberately does not emulate.
assert "does not match any GPU." in display_base
assert "has no display attached to the desktop." in display_base
