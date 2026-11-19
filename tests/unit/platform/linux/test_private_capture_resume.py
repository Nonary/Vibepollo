"""Exercise the Linux resume publication and rollback with the production config functions."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
config = (root / "src/config.cpp").read_text()
nvhttp = (root / "src/nvhttp.cpp").read_text()
resume = nvhttp[nvhttp.rindex("  void resume(bool &host_audio"):]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    for end in range(brace + 1, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise ValueError(signature)


publication = re.search(r"    if \(!(?P<prepared>\w+)\.output_name\.empty\(\)\) \{\n"
                        r"[^#]*?\n    \}\n#endif", resume)
assert publication
early_clear = function(resume, "    if (no_active_sessions && allow_session_display_changes && !exclusive_retained_catalog_owner)")
program = r"""
#include <cassert>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
namespace config {
using runtime_output_override_lease_t = std::uint64_t;
std::shared_mutex g_output_override_mutex;
std::optional<std::string> g_exclusive_private_capture, g_runtime_output_name_override;
std::uint64_t g_next_runtime_output_override_lease {}, g_runtime_output_override_lease {};
"""
for signature in ("std::uint64_t set_runtime_output_name_override_impl(",
                  "void set_runtime_output_name_override(",
                  "runtime_output_override_lease_t set_runtime_output_name_override_with_lease(",
                  "runtime_output_override_lease_t replace_retained_private_capture(",
                  "bool clear_runtime_output_name_override_if_lease(",
                  "std::optional<std::string> exclusive_private_capture_output()",
                  "std::optional<std::string> runtime_output_name_override()"):
    program += function(config, signature) + "\n"
program += """
}
struct session_t { std::optional<std::string> output_name_override, exclusive_private_capture; };
std::uint64_t resume(bool no_active_sessions, bool joining_existing_game_output, bool restore,
                     std::optional<std::string> physical = {}, bool is_input_only = false) {
  const bool exclusive_retained_catalog_owner = false;
  const bool allow_session_display_changes = restore && !joining_existing_game_output && !is_input_only;
  session_t session {physical, {}};
  auto *launch_session = &session;
  struct { std::string output_name; } PREPARED;
  config::runtime_output_override_lease_t pending_output_lease {};
""".replace("PREPARED", publication["prepared"])
program += early_clear + "\n" + publication[0].removesuffix("\n#endif")
program += """
  return pending_output_lease;
}
int main() {
  using namespace config;
  const auto pin = [] { return set_runtime_output_name_override_with_lease("Virtual-1", "card2:Virtual-1"); };
  for (bool restore : {false, true}) {
    pin();
    resume(true, false, restore);
    assert(!exclusive_private_capture_output() && !runtime_output_name_override());
    pin();
    resume(true, true, restore);
    assert(exclusive_private_capture_output() == "card2:Virtual-1");
    resume(false, false, restore);
    assert(exclusive_private_capture_output() == "card2:Virtual-1");
  }
  pin();
  resume(true, false, false, {}, true);
  assert(exclusive_private_capture_output() == "card2:Virtual-1");
  const auto stale = pin();
  const auto physical = resume(true, false, false, "HDMI-A-1");
  assert(physical && !exclusive_private_capture_output() && runtime_output_name_override() == "HDMI-A-1");
  assert(!clear_runtime_output_name_override_if_lease(stale));
  assert(clear_runtime_output_name_override_if_lease(physical));
  assert(!runtime_output_name_override());
  set_runtime_output_name_override("DP-3");
  assert(!resume(true, false, false, "HDMI-A-1") && runtime_output_name_override() == "DP-3");
}
"""
with tempfile.TemporaryDirectory() as work:
    source = Path(work) / "resume.cpp"
    binary = Path(work) / "resume"
    source.write_text(program)
    subprocess.run([sys.argv[2], "-std=c++20", "-pthread", source, "-o", binary], check=True)
    subprocess.run([binary], check=True)
