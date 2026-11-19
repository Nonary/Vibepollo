"""Exercise reservation and rollback with KScreen temporarily losing a two-GPU connector."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
private = (root / "src/platform/linux/private_display.cpp").read_text()
backend = (root / "src/platform/linux/display_backend.cpp").read_text()


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


lookup = section(private, "    std::string connector_sysfs_path(", "    bool connector_is_connected(")
reserve = section(private, "    std::optional<std::string> reserve_output(", "  }  // namespace")
prepare = section(private, "      const auto previous = manager.reservations.find(identity);",
                  "        result.active = true;")
rollback = section(backend, "  void prepared_display_t::rollback(", "  const backend_t &backend()")
program = r"""
#include "src/platform/linux/private_display_capture_policy.h"
#include <cassert>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <set>
using namespace platf;
#define BOOST_LOG(level) std::cerr
#define SUNSHINE_BUILD_DRM 1
std::filesystem::path drm_class_fixture;
namespace restore_policy {
enum class connection_result_e { failed, published, pending };
template<class... Args> connection_result_e connect_with_snapshot_and_publication(Args&&...) { std::abort(); }
}
struct state_t {
  std::map<std::string, std::string> reservations;
  linux_private_display_capture::reserved_connector_t reserved_capture;
  std::set<std::string> newly_connected_reservations;
  int snapshot {};
};
bool published = true;
int reads, fail_read;
std::optional<int> query_configuration() {
  if (++reads == fail_read) return std::nullopt;
  return 1;
}
const int *find_output(const int &config, const std::string &name) { return published && name == "DP-1" ? &config : nullptr; }
bool connected(int) { return true; }
bool enabled(int) { return true; }
bool snapshot_configuration_if_needed(state_t &, int) { std::abort(); }
bool connect_managed_output(const std::string &) { std::abort(); }
bool wait_for_output_publication(const std::string &) { std::abort(); }
bool is_managed_output(const std::string &) { return false; }
std::vector<std::string> configured_outputs() { return {"DP-1"}; }
namespace config {
struct video_t {
  enum class virtual_display_layout_e { exclusive };
  virtual_display_layout_e virtual_display_layout {virtual_display_layout_e::exclusive};
  std::string capture;
} video;
}
constexpr auto output_verification_timeout = std::chrono::seconds(3);
bool stale_crtc {};
bool capture_output_available(const std::string &, const std::optional<std::string> &, std::chrono::steady_clock::time_point) {
  return stale_crtc;
}
struct result_t { std::string error; bool reused_private_capture {}; };
"""
program += lookup.replace('"/sys/class/drm"', "drm_class_fixture") + reserve
program += r"""
bool prepare(state_t &manager, const std::string &identity, std::string &target, bool &reused) {
  struct {
    std::optional<config::video_t::virtual_display_layout_e> virtual_display_layout_override;
    std::optional<std::string> exclusive_private_capture;
    bool virtual_display_failed {};
  } session;
  result_t result;
  bool no_active_sessions = true, shared = false;
  reads = 0;
"""
program += prepare + r"""
    target = session.exclusive_private_capture.value_or("");
    reused = result.reused_private_capture;
    return true;
  }
  return false;
}
int immediate, scheduled;
int token_rollbacks, recompositions, native_releases;
bool remote_owner {};
namespace platf::linux_private_display {
  void restore_after_failed_preparation() { ++scheduled; }
  bool remote_remove_owned_display(const std::string &) { ++native_releases; return true; }
}
namespace remote_display_topology {
  struct coordinator_t {
    bool rollback_normal_game_identity(const std::string &, std::uint64_t) { ++token_rollbacks; return true; }
    std::vector<std::string> protected_remote_monitor_client_ids() {
      return remote_owner ? std::vector<std::string>{"owner"} : std::vector<std::string>{};
    }
    bool reapply_composed_topology() { ++recompositions; return true; }
  };
  coordinator_t &instance() { static coordinator_t coordinator; return coordinator; }
}
struct backend_t { void revert() const { ++immediate; } };
const backend_t &backend();
struct prepared_display_t { bool owns_output, reused_private_capture; void rollback(bool) const; };
"""
transport = (root / "src/nvhttp.cpp").read_text()
program += section(transport, "    void rollback_new_linux_display_identity(", "    linux_normal_identity_result_e reserve_linux_normal_display_identity(")
program += rollback + r"""
const backend_t &backend() { static backend_t instance; return instance; }
int main(int, char **argv) {
  drm_class_fixture = argv[1];
  auto status = [](int card, const char *value) {
    auto path = drm_class_fixture / ("card" + std::to_string(card) + "-DP-1");
    std::filesystem::create_directories(path);
    std::ofstream(path / "status") << value;
  };
  state_t manager;
  std::string target;
  bool reused {};
  status(0, "connected"); status(1, "disconnected");
  assert(prepare(manager, "owner", target, reused) && target == "card0:DP-1" && !reused);
  status(0, "disconnected");
  published = false;
  assert(!prepare(manager, "owner", target, reused));
  assert(manager.reservations.at("owner") == "DP-1");
  published = true;
  fail_read = 2; // KScreen confirms the reservation, then its validation query fails.
  assert(!prepare(manager, "owner", target, reused));
  assert(manager.reservations.contains("owner"));
  fail_read = 0;
  status(1, "connected");
  stale_crtc = true;
  assert(!prepare(manager, "owner", target, reused));
  assert(manager.reservations.contains("owner"));
  status(0, "unknown");
  assert(prepare(manager, "owner", target, reused) && target == "card0:DP-1" && reused);
  stale_crtc = false;
  assert(!prepare(manager, "owner", target, reused));
  status(0, "connected");
  assert(prepare(manager, "owner", target, reused) && target == "card0:DP-1" && reused);
  prepared_display_t{true, reused}.rollback(true);
  assert(immediate == 0 && scheduled == 1);

  // Reusing this reservation in another capture mode also preserves its prior owner on abort.
  config::video.capture = "x11";
  assert(prepare(manager, "owner", target, reused) && target.empty() && reused);
  prepared_display_t{true, reused}.rollback(true);
  assert(immediate == 0 && scheduled == 2);
  config::video.capture.clear();

  manager.reservations.clear(); // Real release ends the old pin.
  status(0, "disconnected");
  fail_read = 2;
  assert(!prepare(manager, "fresh", target, reused) && manager.reservations.empty());
  fail_read = 0;
  assert(prepare(manager, "fresh", target, reused) && target == "card1:DP-1" && !reused);
  prepared_display_t{true, reused}.rollback(true);
  prepared_display_t{true, true}.rollback(false);
  prepared_display_t{false, false}.rollback(true);
  assert(immediate == 2 && scheduled == 2);
  // An RTSP token created for an existing WebRTC reservation can be retired independently.
  rollback_new_linux_display_identity("owner", 47, true);
  assert(token_rollbacks == 1 && recompositions == 0 && native_releases == 0);
  rollback_new_linux_display_identity("owner", 48, false);
  assert(token_rollbacks == 2 && recompositions == 1 && native_releases == 1);
  remote_owner = true;
  rollback_new_linux_display_identity("owner", 49, false);
  assert(token_rollbacks == 3 && recompositions == 2 && native_releases == 1);
}
"""
with tempfile.TemporaryDirectory() as scratch:
    work = Path(scratch)
    cpp, exe = work / "reservation.cpp", work / "reservation"
    cpp.write_text(program)
    subprocess.run([sys.argv[2], "-std=c++20", "-I", str(root), cpp, "-o", exe], check=True)
    subprocess.run([exe, work], check=True)
