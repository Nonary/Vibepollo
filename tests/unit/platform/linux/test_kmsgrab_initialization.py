"""Compile production KMS entry points with inert DRM/compositor boundaries.

Exercise descriptor initialization and its locking through the real factory and
Linux routing functions, without opening GPUs, changing outputs or permissions.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else "g++-15"
kms = (root / "src/platform/linux/kmsgrab.cpp").read_text()
misc = (root / "src/platform/linux/misc.cpp").read_text()


def function(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


factory = function(kms, "  std::shared_ptr<display_t> kms_display(")
query = function(kms, "  std::vector<std::string> kms_display_names(")
route = function(misc, "  std::shared_ptr<display_t> display(\n")
initialize = function(misc, "  std::unique_ptr<deinit_t> init()")
verify = function(misc, "  bool verify_kms()")
policy = (root / "src/platform/linux/private_display_capture_policy.h").read_text()

program = policy.replace("#pragma once", "") + r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
using namespace std::literals;
#define SUNSHINE_BUILD_DRM
#define SUNSHINE_BUILD_WAYLAND
#define SUNSHINE_BUILD_KWIN
struct NullLog { template<class T> NullLog& operator<<(const T&) { return *this; } };
#define BOOST_LOG(level) NullLog{}
namespace video { struct config_t { int dynamicRange=0; bool force_sdr=false; bool prefer_sdr_10bit=false; }; }
namespace config {
struct video_t {
  enum class virtual_display_mode_e { disabled, enabled };
  std::string capture;
  virtual_display_mode_e virtual_display_mode=virtual_display_mode_e::enabled;
};
inline video_t video;
}
namespace platf {
enum class mem_type_e { unknown, cuda, vaapi, system, vulkan };
enum class window_system_e { NONE, WAYLAND, X11 };
inline window_system_e window_system;
struct display_t { std::string selected_card; int desktop_width=0; };
struct adapter_id_t {};
struct deinit_t {};
namespace source { enum { KMS, WAYLAND, KWIN, MAX_FLAGS }; }
inline std::bitset<source::MAX_FLAGS> sources;
namespace fake {
  bool pool=true, kwin=false, kms_available=true, direct_fails=false, all_fail=false, enumeration_throws=false;
  std::string virtual_card="card2";
  mem_type_e last_type=mem_type_e::unknown;
  std::vector<std::string> cards;
  int desktop_width=0, enumerations=0, ram_attempts=0;
  std::atomic_bool pause_init=false, init_started=false, release_init=false;
}
namespace linux_private_display {
bool kernel_pool_available() { return fake::pool; }
bool is_private_output(const std::string& name) { return name=="Virtual-1"; }
}
namespace linux_display {
struct backend_t { std::string capture_target(const std::string& name) { return name; } };
backend_t& backend() { static backend_t b; return b; }
}
namespace kms {
  // The same mutex used by the compiled production entry points.
  std::mutex display_mutex;
  struct display_vram_t : display_t {
    mem_type_e type;
    explicit display_vram_t(mem_type_e value) : type(value) {}
    int init(const std::string& name, const video::config_t&) {
      if (fake::pause_init) {
        fake::init_started=true;
        while (!fake::release_init) std::this_thread::yield();
      }
      assert(fake::last_type==type);
      selected_card = name=="Virtual-1" ? fake::virtual_card : "card1";
      if (std::find(fake::cards.begin(), fake::cards.end(), selected_card)==fake::cards.end()) return -1;
      desktop_width=fake::desktop_width;
      return fake::all_fail || fake::direct_fails ? -1 : 0;
    }
  };
  struct display_ram_t : display_vram_t {
    using display_vram_t::display_vram_t;
    int init(const std::string& name, const video::config_t& config) {
      ++fake::ram_attempts;
      const auto fail=fake::direct_fails;
      fake::direct_fails=false;
      auto result=display_vram_t::init(name, config);
      fake::direct_fails=fail;
      return result;
    }
  };
}
// DRM enumeration and capture classes are inert boundaries. Both public
// entry points and the backend routing/initialization below are production.
static std::vector<std::string> kms_display_names_unlocked(mem_type_e type) {
  if (fake::enumeration_throws) throw 1;
  ++fake::enumerations;
  fake::last_type=type;
  fake::cards.clear();
  if (!fake::kms_available) return {};
  if (fake::pool) fake::cards.push_back(fake::virtual_card);
  // CUDA queries filter physical AMD cards, as real KMS enumeration does.
  if (type!=mem_type_e::cuda) fake::cards.push_back("card1");
  fake::desktop_width=type==mem_type_e::cuda ? 800 : 2560;
  return {"DP-1", "Virtual-1"};
}
'''
program += query + "\n" + factory + "\n" + verify + r'''
std::vector<std::string> wl_display_names() { return {}; }
bool verify_wl() { return false; }
std::shared_ptr<display_t> wl_display(mem_type_e, const std::string&, const video::config_t&) { return {}; }
bool verify_kwin() { return fake::kwin; }
std::shared_ptr<display_t> kwin_display(mem_type_e, const std::string&, const video::config_t&) { auto result=std::make_shared<display_t>(); result->selected_card="kwin"; return result; }
bool has_elevated_privileges(bool) { return false; }
bool drop_elevated_privileges(bool) { return true; }
bool drop_effective_elevated_privileges(bool) { return true; }
void set_env(const char*, const char*) {}
namespace gbm { void init() {} }
namespace egl { bool ensure_loader() { return true; } }
'''
program += route + "\n" + initialize + r'''
}
int main() {
  using namespace platf;
  setenv("WAYLAND_DISPLAY", "wayland-test", 1);
  const video::config_t config;
  const auto capture = [&](const std::string& name="") {
    return display(mem_type_e::vaapi, name, config, std::nullopt);
  };
  // Refresh policy for both reported card identities. Actual DRM plane
  // ordering and name resolution remain in the KMS selection tests.
  for (const auto virtual_card : {"card0", "card2"}) {
    fake::virtual_card=virtual_card;
    for (const auto method : {"", "kms"}) {
      config::video.capture=method;
      fake::cards.clear();
      auto lifetime=init();
      for (const auto name : {"", "0", "DP-1", "Virtual-1"}) {
        const auto before=fake::enumerations;
        auto result=capture(name);
        assert(result);
        assert(fake::enumerations>before);
        assert(result->selected_card==(std::string(name)=="Virtual-1" ? virtual_card : "card1"));
        assert(result->desktop_width==2560);
      }
    }
  }
  // Explicit KWin and automatic KWin fallback still avoid KMS capture.
  fake::kwin=true;
  config::video.capture="kwin";
  auto lifetime=init();
  auto before=fake::enumerations;
  assert(capture()->selected_card=="kwin");
  assert(fake::enumerations==before);
  fake::pool=false;
  fake::kms_available=false;
  config::video.capture="";
  lifetime=init();
  fake::cards.clear();
  assert(capture()->selected_card=="kwin");
  // An old CUDA-only query must not poison the subsequent AMD/software probe.
  fake::pool=true;
  fake::kms_available=true;
  config::video.capture="kms";
  lifetime=init();
  (void)kms_display_names(mem_type_e::cuda);
  assert(capture()->selected_card=="card1");
  (void)kms_display_names(mem_type_e::cuda);
  assert(kms_display(mem_type_e::system, "", config)->desktop_width==2560);
  // Both direct-import failure and complete failure release the transaction.
  fake::direct_fails=true;
  before=fake::enumerations;
  assert(capture());
  assert(fake::enumerations==before+1 && fake::ram_attempts>0);
  fake::direct_fails=false;
  fake::all_fail=true;
  assert(!capture());
  fake::all_fail=false;
  assert(capture());
  fake::enumeration_throws=true;
  try { (void)capture(); assert(false); } catch (int) {}
  fake::enumeration_throws=false;
  assert(capture());
  // Force a query to try replacing the maps during capture initialization.
  fake::pause_init=true;
  auto initializing=std::async(std::launch::async, [&] { return capture(); });
  while (!fake::init_started) std::this_thread::yield();
  std::promise<void> query_attempted;
  auto querying=std::async(std::launch::async, [&] {
    query_attempted.set_value();
    return kms_display_names(mem_type_e::cuda);
  });
  query_attempted.get_future().wait();
  assert(querying.wait_for(30ms)==std::future_status::timeout);
  fake::release_init=true;
  assert(initializing.get()->selected_card=="card1");
  (void)querying.get();
  fake::pause_init=false;
  // Exercise competing queries and mixed encoder capture creation repeatedly.
  std::vector<std::thread> workers;
  for (int worker=0; worker<8; ++worker) {
    workers.emplace_back([&,worker] {
      for (int n=0; n<100; ++n) {
        if (worker%2) (void)kms_display_names(mem_type_e::cuda);
        else assert(kms_display(worker%4 ? mem_type_e::system : mem_type_e::vaapi, "", config));
      }
    });
  }
  for (auto& worker : workers) worker.join();
  std::cout << "KMS initialization, routing, fallback and concurrency checks passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix="kmsgrab-initialization-") as work:
    work = pathlib.Path(work)
    cpp, binary = work / "fixture.cpp", work / "fixture"
    cpp.write_text(program)
    subprocess.run([compiler, "-std=c++23", "-O0", "-pthread", str(cpp), "-o", str(binary)], check=True, timeout=20)
    subprocess.run([str(binary)], check=True, timeout=10)
