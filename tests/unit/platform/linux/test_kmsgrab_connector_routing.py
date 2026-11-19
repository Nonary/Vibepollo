"""Exercise the KMS open and pre-delivery checks when another output takes over the same CRTC."""
from pathlib import Path
import subprocess
import sys
import tempfile
from test_kmsgrab_hdr_worker import function

source = (Path(sys.argv[1]) / "src/platform/linux/kmsgrab.cpp").read_text()
start = source.index("            qualified_connector = card_qualified;")
bind = source[start:source.index("            direct_import_required", start)]
refresh = function(source, "      inline capture_e refresh(")
frame_checks = refresh[refresh.index("        // Sample the framebuffer"):refresh.index("        if (pending_exported_frame) {")]
program = r"""
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
using namespace std::literals;
#define BOOST_LOG(level) std::ostringstream()
namespace linux_display { bool hyprland_kms_session() { return false; } }
enum class capture_e { ok, reinit, timeout };
constexpr int DRM_MODE_CONNECTED = 1;
struct property_t { std::string name; };
using prop_t = std::shared_ptr<property_t>;
using props_t = std::vector<std::pair<prop_t, std::uint64_t>>;
prop_t drmModeGetProperty(int, unsigned id) { return std::make_shared<property_t>(id == 1 ? "CRTC_ID" : "HDR_OUTPUT_METADATA"); }
using fb_t = std::shared_ptr<int>;
struct plane { unsigned crtc_id, fb_id = 123; };
using plane_t = std::shared_ptr<plane>;
unsigned plane_crtc = 7;
plane_t drmModeGetPlane(int, int) { return std::make_shared<plane>(plane_crtc); }
struct card_t {
  struct port { unsigned crtc_id, connector_id; bool connected; std::string name; };
  const port original {7, 17, true, "DP-1"}, replacement {7, 18, true, "HDMI-A-1"};
  std::vector<port> ports {original};
  struct { int el = 9; } fd;
  bool connected = true, atomic = true, reroute_at_sample = false;
  unsigned route = 7, live_reads = 0, encoder_reads = 0, frame_reads = 0;
  std::uint32_t ids[2] {1, 2};
  std::uint64_t values[2] {7, 0};
  struct state { int connection; unsigned encoder_id, count_props; std::uint32_t *props; std::uint64_t *prop_values; };
  struct encoder_t { unsigned crtc_id; };
  std::optional<state> connector(unsigned id) {
    assert(id == original.connector_id); ++live_reads; values[0] = route;
    return state {connected ? DRM_MODE_CONNECTED : 0, 4, atomic ? 2u : 1u, ids + !atomic, values + !atomic};
  }
  std::optional<encoder_t> encoder(unsigned) { ++encoder_reads; return encoder_t {route}; }
  auto monitors() { return ports; }
  props_t connector_props(unsigned) { return {}; }
  fb_t fb(plane *) { ++frame_reads; if (reroute_at_sample) route = 8; return std::make_shared<int>(123); }
"""
for signature in ("      std::vector<std::pair<prop_t, std::uint64_t>> read_properties(",
                  "      std::optional<std::uint64_t> prop_value_by_name(",
                  "      std::optional<std::vector<std::pair<prop_t, std::uint64_t>>> routed_connector_props("):
    program += function(source, signature) + "\n"
program += """
};
struct capture {
  card_t card;
  bool qualified_connector = false, pending_exported_frame = false;
  unsigned crtc_id = 7, plane_id = 2;
  std::optional<unsigned> connector_id;
  std::optional<std::uint64_t> hdr_metadata_blob_id;
  struct { capture_e refresh(int, auto, auto, bool) { return capture_e::ok; } } hdr_metadata_generation;
  bool sdr_capture_allowed() { return true; }
  int open(bool card_qualified) {
    struct selected { std::string connector_name; };
    std::optional<selected> selected_monitor {selected {"DP-1"}};
""" + bind + """
    return 0;
  }
  capture_e frame() {
    std::chrono::steady_clock::time_point frame_timestamp;
""" + frame_checks + """
    return capture_e::ok;
  }
};
int main() {
  capture lost; lost.card.ports = {lost.card.replacement};
  assert(lost.open(true) == -1);
  capture mirrored; mirrored.card.ports.push_back(mirrored.card.replacement);
  assert(mirrored.open(true) == 0 && mirrored.connector_id == mirrored.card.original.connector_id);
  for (bool atomic : {true, false}) {
    for (bool event : {true, false}) {
      capture live; live.card.atomic = atomic; live.pending_exported_frame = event;
      assert(live.open(true) == 0 && live.frame() == capture_e::ok);
      assert(live.card.live_reads == 1 && live.card.encoder_reads == !atomic);
      assert(live.card.frame_reads == !event);
      if (event) live.card.route = 8;
      else live.card.reroute_at_sample = true;
      assert(live.frame() == capture_e::reinit);
      live.card.reroute_at_sample = false; live.card.route = 7; live.card.connected = false;
      assert(live.frame() == capture_e::reinit);
    }
  }
  plane_crtc = 8;
  assert(mirrored.frame() == capture_e::reinit && mirrored.card.frame_reads == 0);
  capture normal; normal.card.ports = {normal.card.replacement};
  assert(normal.open(false) == 0 && normal.frame() == capture_e::ok && normal.card.live_reads == 0);
}
"""
with tempfile.TemporaryDirectory() as work:
    cpp, binary = Path(work) / "route.cpp", Path(work) / "route"
    cpp.write_text(program)
    subprocess.run([sys.argv[2], "-std=c++20", cpp, "-o", binary], check=True)
    subprocess.run([binary], check=True)
