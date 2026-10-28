#!/usr/bin/env python3
"""Execute production metrics config reset/parser/publication and HTTPS adapter.

The full media host is not linked. Unrelated fields, TLS identity storage, file
I/O and platform side effects are fixture boundaries. Real config writer-lock
entry, reset, bool/int parsers, final publication, getter, metrics access adapter
and discovery predicate are compiled unmodified. Current certificate/permission
helpers and the real query policy execute before and after a stub sampling hook;
TLS storage, RTSP collection and JSON/provider execution remain boundaries. The
independent timeout local is omitted from this metrics-only apply fixture. Pauses
surround omitted unrelated apply work; they add no production synchronization.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"
config = (root / "src/config.cpp").read_text()
http = (root / "src/confighttp.cpp").read_text()
nvhttp = (root / "src/nvhttp.cpp").read_text()
metrics = (root / "src/hardware_metrics.cpp").read_text()
metrics_header = (root / "src/hardware_metrics.h").read_text()
config_header = (root / "src/config.h").read_text()
crypto = (root / "src/crypto.h").read_text()
utility = (root / "src/utility.h").read_text()


def between(source, first, last):
    begin = source.index(first)
    return source[begin:source.index(last, begin)]


def function(source, first):
    begin = source.index(first)
    depth = 0
    for end in range(source.index("{", begin), len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[begin:end + 1] + "\n"
    raise ValueError("unterminated function")


utility_helpers = (function(utility, "  template<class T>\n  T from_hex(") +
                   function(utility, "  inline std::int64_t from_chars(") +
                   function(utility, "  inline std::int64_t from_view("))
tuple_macro = between(utility, "#define TUPLE_2D_REF(", "#define TUPLE_3D(")
state = between(config, "    hardware_metrics_config_t g_hardware_metrics_config;", "\n\n")
getter = function(config, "  hardware_metrics_settings_t get_hardware_metrics_settings() {")
reset = function(config, "    void reset_runtime_config_to_defaults() {")
entry = between(config, "  void apply_config(std::unordered_map<std::string, std::string> &&vars) {", "#ifndef __ANDROID__")
parser = between(config, '    bool_f(vars, "hardware_metrics",', '    bool_f(vars, "realtime_stats_enabled",')
publication = between(config, "    g_hardware_metrics_config.publish(", "\n") + "\n"
hot_entry = between(config, "  void apply_config_now(const display_transition::before_mutation_f &before_display_mutation) {", "    try {")
hot_install = between(config, "      auto install_effective_configuration = [&] {", "        if (sunshine.session_history_enabled")
hot_policy = between(http, "    bool can_hot_apply_during_session(", "\n  }  // namespace")
access = between(nvhttp, "    hardware_metrics::access_t hardware_metrics_access(", "    void write_hardware_metrics(")
verified_access = (function(nvhttp, "    inline verified_client_t get_verified_cert(") +
                   function(nvhttp, "    inline PERM client_perm(") +
                   function(nvhttp, "    inline bool has_client_perm("))
permissions = between(crypto, "  enum class PERM :", "  struct command_entry_t")
# The timeout local belongs to an independently tested feature, not this fixture.
entry = entry.replace("    std::chrono::milliseconds control_peer_timeout {0};\n", "")
defaults = between(config_header, "    bool hardware_metrics {", "    bool realtime_stats_enabled {")
access_type = between(metrics_header, "  struct access_t {", "  struct response_t")
advertised = between(metrics, "  bool advertised(access_t access)", "  nlohmann::json serialize")
query = function(metrics, "  response_t query(")
route_query = function(nvhttp, "    void hardware_metrics_query(")
helpers = (between(config, "  void string_f(", "  template<typename T, typename F>") +
           between(config, "  void int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input) {", "  void int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, ::std::optional<int>") +
           between(config, "  void int_between_f(", "  void double_f("))

program = r'''
#include <cassert>
#include <algorithm>
#include <cctype>
#include <tuple>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <vector>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include "src/hardware_metrics_config.h"
''' + tuple_macro + r'''
namespace util {
''' + utility_helpers + r'''
}
using namespace std::literals;
std::function<void(int)> apply_test_hook = [](int) {};
namespace display_transition { using before_mutation_f = std::function<void()>; }
namespace proc::catalog { struct config_apply_scope { ~config_apply_scope() {} }; }
namespace config {
std::shared_mutex g_apply_gate;
std::mutex g_apply_mutex;
std::unordered_map<std::string, std::string> fixture_vars;
''' + state + getter + r'''
struct sunshine_t {
  std::string username, password, salt, config_file, cmd;
''' + defaults + r'''
} sunshine;
const sunshine_t default_sunshine;
int video, audio, stream, input, frame_limiter, rtss, lossless_scaling;
constexpr int default_video = 0, default_audio = 0, default_stream = 0,
  default_input = 0, default_frame_limiter = 0, default_rtss = 0, default_lossless_scaling = 0;
''' + reset + helpers + entry + r'''
  apply_test_hook(1); // real reset is complete; old publication must survive
''' + parser + r'''
  apply_test_hook(2); // real fields parsed; remainder still pending
  apply_test_hook(3); // omitted unrelated parse/platform side effects may fail
''' + publication + r'''
  apply_test_hook(4); // real publication complete; writer gate still held
}
''' + hot_entry + r'''
    (void) before_display_mutation;
    auto vars = std::move(fixture_vars);
''' + hot_install + r'''
      };
    install_effective_configuration();
  }
}
namespace hardware_metrics {
''' + access_type + advertised + r'''
// Sampling and JSON are independent fixture boundaries; the actual query policy
// and nvhttp adapter below execute unchanged before and after this sample hook.
struct sample_t {};
struct service_t {
  std::function<void()> on_sample;
  int calls = 0;
  std::optional<sample_t> request(std::chrono::milliseconds) {
    ++calls;
    if (on_sample) on_sample();
    return sample_t {};
  }
};
using body_t = std::unordered_map<std::string, std::string>;
struct response_t { int status; body_t body; };
body_t serialize(const sample_t &, int interval) { return {{"interval_ms", std::to_string(interval)}}; }
''' + query + r'''
}
using req_https_t = bool;
''' + permissions + r'''
struct identity_t { std::string uuid; PERM perm; bool enabled; };
using verified_client_t = std::optional<identity_t>;
std::mutex client_mutex;
struct { std::vector<identity_t> named_devices {{"paired-a", PERM::view, true}}; } client_root;
struct {
  verified_client_t resolve_snapshot(req_https_t known, const std::vector<identity_t> &devices) {
    if (known && !devices.empty() && devices.front().enabled) return devices.front();
    return {};
  }
} tls_client_identities;
namespace rtsp_stream {
std::string owner = "paired-a", last_checked;
bool running = true;
bool has_running_video_for_client(std::string_view uuid) {
  last_checked = uuid;
  return running && uuid == owner;
}
}
''' + verified_access + r'''
''' + access + r'''
using resp_https_t = int;
hardware_metrics::response_t last_response {};
void write_hardware_metrics(resp_https_t, const hardware_metrics::response_t &result) {
  last_response = result;
}
''' + route_query + r'''
bool is_rtx_hdr_live_key(const std::string &) { return false; }
''' + hot_policy + r'''

void expect(config::hardware_metrics_settings_t expected) {
  const auto settings = config::get_hardware_metrics_settings();
  assert(settings.enabled == expected.enabled && settings.interval_ms == expected.interval_ms);
  const auto client = hardware_metrics_access(true);
  assert(client.enabled == expected.enabled && client.interval_ms == expected.interval_ms);
  assert(hardware_metrics::advertised(client) == expected.enabled);
  assert(!hardware_metrics::advertised(hardware_metrics_access(false)));
}

void overlap(int pause, std::unordered_map<std::string, std::string> next,
             config::hardware_metrics_settings_t after, bool fail = false) {
  config::apply_config({{"hardware_metrics", "true"}, {"hardware_metrics_interval", "10000"}});
  std::mutex mutex;
  std::condition_variable cv;
  bool reached = false, proceed = false, caught = false;
  apply_test_hook = [&](int stage) {
    if (stage != pause) return;
    std::unique_lock lock(mutex);
    reached = true;
    cv.notify_one();
    cv.wait(lock, [&] { return proceed; });
    if (fail) throw std::runtime_error("fixture late apply failure");
  };
  config::fixture_vars = std::move(next);
  std::thread writer([&] {
    try { config::apply_config_now({}); }
    catch (const std::runtime_error &) { caught = true; }
  });
  {
    std::unique_lock lock(mutex);
    assert(cv.wait_for(lock, 2s, [&] { return reached; }));
  }
  // Run actual getter/access/discovery while the writer owns g_apply_gate.
  // A blocking reader or a reset publication fails this schedule.
  auto reader = std::async(std::launch::async, [&] {
    expect(pause == 4 ? after : config::hardware_metrics_settings_t {true, 10000});
  });
  const bool nonblocking = reader.wait_for(500ms) == std::future_status::ready;
  {
    std::lock_guard lock(mutex);
    proceed = true;
  }
  cv.notify_one();
  writer.join();
  reader.get();
  assert(nonblocking);
  apply_test_hook = [](int) {};
  assert(caught == fail);
  expect(fail ? config::hardware_metrics_settings_t {true, 10000} : after);
}

void permissions_and_owner() {
  config::apply_config({{"hardware_metrics", "true"}});
  for (const auto permission : {PERM::view, PERM::launch, PERM::_all}) {
    client_root.named_devices.front().perm = permission;
    assert(hardware_metrics::advertised(hardware_metrics_access(true)));
    assert(hardware_metrics_has_running_video(true));
    assert(rtsp_stream::last_checked == "paired-a");
  }
  for (const auto permission : {PERM::_no, PERM::list, PERM::server_cmd, PERM::input_controller}) {
    client_root.named_devices.front().perm = permission;
    assert(!hardware_metrics::advertised(hardware_metrics_access(true)));
    assert(!hardware_metrics_has_running_video(true));
  }
  client_root.named_devices.front().perm = PERM::view;
  rtsp_stream::owner = "history-alias";
  assert(!hardware_metrics_has_running_video(true));
  assert(rtsp_stream::last_checked == "paired-a");
  client_root.named_devices.front().uuid = "history-alias";
  assert(hardware_metrics_has_running_video(true));
  assert(rtsp_stream::last_checked == "history-alias");
  client_root.named_devices.front().enabled = false;
  assert(!hardware_metrics::advertised(hardware_metrics_access(true)));
  assert(!hardware_metrics_has_running_video(true));
  client_root.named_devices.front().enabled = true;
  client_root.named_devices.front().uuid.clear();
  assert(!hardware_metrics::advertised(hardware_metrics_access(true)));
  client_root.named_devices.front().uuid = "paired-a";
  rtsp_stream::owner = "paired-a";
}

void query_authority_rechecks() {
  config::apply_config({{"hardware_metrics", "true"}});
  hardware_metrics::service_t service;
  auto ask = [&] { hardware_metrics_query(service, 0, true); return last_response.status; };
  client_root.named_devices.front().perm = PERM::list;
  assert(ask() == 403 && service.calls == 0);
  client_root.named_devices.front().perm = PERM::view;
  service.on_sample = [&] { client_root.named_devices.front().perm = PERM::list; };
  assert(ask() == 403 && service.calls == 1);
  client_root.named_devices.front().perm = PERM::launch;
  service.on_sample = [&] { client_root.named_devices.front().enabled = false; };
  assert(ask() == 403 && service.calls == 2);
  client_root.named_devices.front().enabled = true;
  service.on_sample = [&] { client_root.named_devices.front().uuid = "new-certificate-owner"; };
  assert(ask() == 409 && service.calls == 3);
  assert(rtsp_stream::last_checked == "new-certificate-owner");
  client_root.named_devices.front().uuid = "paired-a";
  service.on_sample = [&] { rtsp_stream::running = false; };
  assert(ask() == 409 && service.calls == 4);
  rtsp_stream::running = true;
  service.on_sample = [&] { config::apply_config({{"hardware_metrics", "false"}}); };
  assert(ask() == 403 && service.calls == 5);
  config::apply_config({{"hardware_metrics", "true"}});
  service.on_sample = [&] { client_root.named_devices.front().perm = PERM::view; };
  assert(ask() == 200 && service.calls == 6);
}

int main() {
  expect({false, 1000});
  permissions_and_owner();
  query_authority_rechecks();
  assert(can_hot_apply_during_session({"hardware_metrics"}));
  assert(can_hot_apply_during_session({"hardware_metrics_interval"}));
  assert(can_hot_apply_during_session({"realtime_stats_enabled"}));
  for (int stage : {1, 2, 3, 4}) {
    overlap(stage, {{"hardware_metrics", "true"}, {"hardware_metrics_interval", "10000"}, {"realtime_stats_enabled", "false"}}, {true, 10000});
    overlap(stage, {{"hardware_metrics", "false"}, {"hardware_metrics_interval", "500"}}, {false, 500});
    overlap(stage, {}, {false, 1000});
    overlap(stage, {{"hardware_metrics", "true"}, {"hardware_metrics_interval", "499"}}, {true, 1000});
    overlap(stage, {{"hardware_metrics", "true"}, {"hardware_metrics_interval", "10001"}}, {true, 1000});
  }
  for (int stage : {1, 2, 3}) {
    overlap(stage, {{"hardware_metrics", "false"}, {"hardware_metrics_interval", "500"}}, {false, 500}, true);
  }
  config::apply_config({{"hardware_metrics", "true"}, {"hardware_metrics_interval", "10000"}});
  std::atomic_bool run {true}, ready {false};
  std::atomic<unsigned> reads {0};
  std::thread reader([&] {
    ready.store(true, std::memory_order_release);
    while (run.load(std::memory_order_acquire)) {
      const auto client = hardware_metrics_access(true);
      assert((client.enabled && client.interval_ms == 10000) || (!client.enabled && client.interval_ms == 500));
      reads.fetch_add(1, std::memory_order_relaxed);
    }
  });
  while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
  for (int i = 0; i != 20000; ++i) {
    config::fixture_vars = {{"hardware_metrics", i % 2 ? "true" : "false"},
                            {"hardware_metrics_interval", i % 2 ? "10000" : "500"}};
    config::apply_config_now({});
  }
  run.store(false, std::memory_order_release);
  reader.join();
  assert(reads.load() > 0);
  std::puts("Metrics completed-config publication: reset/parse/failure/disable/discovery, live View/Launch/cert/exact-owner schedules and 20000 concurrent applies passed");
}
'''

with tempfile.TemporaryDirectory(prefix="metrics-config-") as tmp:
    source = pathlib.Path(tmp) / "test.cpp"
    binary = pathlib.Path(tmp) / "test"
    source.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(root), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
