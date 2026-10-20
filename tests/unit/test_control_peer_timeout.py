#!/usr/bin/env python3
"""Execute production timeout parsing, CONNECT dispatch, expiry and stop paths.

The host media stack is not required: the ENet/event, logging, unrelated config
fields and session container edges are stubbed. Vibepollo retains its real
request_stop/first-stop owner and graceful reaper branch; termination packet
encoding and control flush calls are stubbed, so no on-wire guarantee is made. Compile the production reset
helper, apply_config entry and option parser block in their real order. Other
option parsing and config-file I/O are omitted; this is a focused production
boundary test, not a complete configuration reload or transport timing test.
Deterministic reload pauses exercise CONNECT before publication and after it;
a concurrent reader exercises only the independently published timeout, since
other existing mutable config fields (including ping_timeout) are outside scope.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"
config = (root / "src/config.cpp").read_text()
stream = (root / "src/stream.cpp").read_text()
http = (root / "src/confighttp.cpp").read_text()


def between(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


def function(source, first):
    begin = source.index(first)
    depth = 0
    for end in range(source.index("{", begin), len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[begin:end + 1] + "\n"
    raise ValueError("unterminated function")


initializers = between(config, "  stream_t stream {", "    APPS_JSON_PATH,")
publication_state = between(config, "    std::atomic<std::uint32_t> g_control_peer_timeout_ms", "\n") + "\n"
getter = function(config, "  std::chrono::milliseconds get_control_peer_timeout() {")
publication = between(config, "    g_control_peer_timeout_ms.store(", "\n") + "\n"
hot_policy = between(http, "    bool can_hot_apply_during_session(", "\n  }  // namespace")
hot_entry = between(config, "  void apply_config_now(const display_transition::before_mutation_f &before_display_mutation) {", "    try {")
hot_install = between(config, "      auto install_effective_configuration = [&] {", "        if (sunshine.session_history_enabled")
default_stream = between(config, "    const stream_t default_stream =", "    const input_t default_input =")
reset = function(config, "    void reset_runtime_config_to_defaults() {")
apply_entry = between(config, "  void apply_config(std::unordered_map<std::string, std::string> &&vars) {", "#ifndef __ANDROID__")
helpers = between(config, "  void string_f(", "  void string_restricted_f(")
parse = between(config, '    generic_f(vars, "control_peer_timeout",', '    int_between_f(vars, "lan_encryption_mode",')
iterate = between(stream, "  void control_server_t::iterate(", "  namespace fec {")
stop_reason = between(stream, "  enum class stop_reason_e {", "  struct session_t {")
stop = between(stream, "    static void request_stop(session_t &session,", "    void join(session_t &session,")
reap = between(stream, "          if (now > session->pingTimeout) {", "          const bool game_session_requires_shutdown =")

program = r'''
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <cstdio>
#include "src/control_peer_timeout.h"
using namespace std::literals;
struct logger_t { template<class T> logger_t &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) logger_t {}
// Scheduling hooks are fixture-only. Production has no hooks or extra locks.
std::function<void(int)> apply_test_hook = [](int) {};
namespace display_transition { using before_mutation_f = std::function<void()>; }
namespace proc::catalog { struct config_apply_scope { ~config_apply_scope() {} }; }
namespace config {
std::shared_mutex g_apply_gate;
std::mutex g_apply_mutex;
std::unordered_map<std::string, std::string> fixture_hot_vars;
struct stream_t { std::chrono::milliseconds ping_timeout; };
''' + publication_state + getter + r'''
''' + initializers + r'''
};
''' + default_stream + r'''
// Unrelated config fields only satisfy the production reset helper's interface.
int video, audio, input, frame_limiter, rtss, lossless_scaling;
constexpr int default_video = 0, default_audio = 0, default_input = 0,
  default_frame_limiter = 0, default_rtss = 0, default_lossless_scaling = 0;
struct sunshine_t { std::string username, password, salt, config_file, cmd; } sunshine;
const sunshine_t default_sunshine;
''' + reset + r'''
''' + helpers + r'''
// The production entry calls the full production reset before this option block.
''' + apply_entry + r'''
  apply_test_hook(1); // actual reset completed; timeout still local/default
''' + parse + r'''
  apply_test_hook(2); // actual timeout parse completed; not yet published
  assert(!vars.contains("control_peer_timeout"));
  apply_test_hook(3); // stand-in for the omitted unrelated parse/side effects
''' + publication + r'''
  apply_test_hook(4); // store completed; hold writer/gate while CONNECT runs
}
// Compile the real hot-apply lock entry and effective-install call. File I/O,
// override merges, display side effects and the outer error handler are omitted.
''' + hot_entry + r'''
    (void) before_display_mutation;
    auto vars = std::move(fixture_hot_vars);
''' + hot_install + r'''
      };
    install_effective_configuration();
  }
}
bool is_rtx_hdr_live_key(const std::string &) { return false; }
''' + hot_policy + r'''

constexpr int ENET_PEER_TIMEOUT_LIMIT = 32;
constexpr int ENET_TIME_OVERFLOW = 86400000;
using enet_uint32 = std::uint32_t;
struct sockaddr {};
struct address_t { sockaddr address; };
struct ENetPeer { address_t address; int calls = 0; bool connected = true; std::uint32_t limit = 32, minimum = 5000, maximum = 30000; };
struct ENetPacket { unsigned char *data; std::size_t dataLength; };
enum { ENET_EVENT_TYPE_NONE, ENET_EVENT_TYPE_CONNECT, ENET_EVENT_TYPE_DISCONNECT, ENET_EVENT_TYPE_RECEIVE };
struct ENetEvent { int type; ENetPeer *peer; std::uint32_t data; ENetPacket *packet; };
struct host_t { std::uint32_t totalReceivedPackets = 0; };
ENetEvent next_event {};
int disconnects = 0, callbacks = 0, input_resets = 0;
int termination_sends = 0, control_flushes = 0;
int enet_host_service(host_t *, ENetEvent *out, enet_uint32) { *out = next_event; return 1; }
void enet_peer_timeout(ENetPeer *peer, enet_uint32 limit, enet_uint32 minimum, enet_uint32 maximum) {
  ++peer->calls; peer->limit = limit; peer->minimum = minimum; peer->maximum = maximum;
}
void enet_peer_disconnect_now(ENetPeer *peer, enet_uint32) { ++disconnects; peer->connected = false; }
namespace platf { std::string from_sockaddr(sockaddr *) { return "peer"; } }
namespace net { using packet_t = std::unique_ptr<ENetPacket>; int map_port(int port) { return port; } }
constexpr int CONTROL_PORT = 47999;
struct event_t { bool value = false; void raise(bool v) { value = v; } };
namespace input { void reset(int &) { ++input_resets; } }
template<class T> struct lockable_t {
  T value;
  struct lock_t { ~lock_t() {} };
  lock_t lock() { return {}; }
  T *operator->() { return &value; }
};
namespace stream {
''' + stop_reason + r'''
namespace session { enum class state_e { STARTING, RUNNING, STOPPING }; }
struct session_t {
  std::atomic<session::state_e> state {session::state_e::RUNNING};
  std::atomic<stop_reason_e> stop_reason {stop_reason_e::none};
  std::chrono::steady_clock::time_point pingTimeout {};
  std::atomic_bool client_disconnected {false};
  struct { ENetPeer *peer = nullptr; std::uint32_t enet_recv_baseline = 0; std::string expected_peer_address; } control;
  struct { int controlProtocolType = 0; } config;
  int input = 0;
  std::shared_ptr<event_t> shutdown_event = std::make_shared<event_t>();
  event_t controlEnd;
};
void while_starting_do_nothing(const std::atomic<session::state_e> &state) { assert(state != session::state_e::STARTING); }
namespace session {
''' + stop + r'''
}
struct decoded_t { std::uint16_t type; std::string_view payload; };
std::optional<decoded_t> decode_control_packet(std::string_view bytes) { return decoded_t {1, bytes}; }
void send_termination(session_t *) { ++termination_sends; }
struct control_server_t {
  void flush() { ++control_flushes; }
  std::unique_ptr<host_t> _host = std::make_unique<host_t>();
  std::unique_ptr<std::vector<session_t *>> _sessions = std::make_unique<std::vector<session_t *>>();
  lockable_t<std::unordered_map<ENetPeer *, session_t *>> _peer_to_session;
  session_t *get_session(ENetPeer *peer, std::uint32_t) {
    auto it = _peer_to_session->find(peer); return it == _peer_to_session->end() ? nullptr : it->second;
  }
  void call(std::uint16_t, session_t *, std::string_view, bool) { ++callbacks; }
  void iterate(std::chrono::milliseconds timeout);
};
''' + iterate + r'''
void reap_sessions(control_server_t *server, std::chrono::steady_clock::time_point now) {
  for (auto pos = server->_sessions->begin(); pos != server->_sessions->end();) {
    auto session = *pos;
''' + reap + r'''
    ++pos;
  }
}
}
void connect(stream::control_server_t &server, stream::session_t &session, ENetPeer &peer) {
  session.control.peer = &peer;
  server._sessions->push_back(&session);
  server._peer_to_session->emplace(&peer, &session);
  next_event = {ENET_EVENT_TYPE_CONNECT, &peer, 0, nullptr};
  auto before = std::chrono::steady_clock::now();
  server.iterate(0ms);
  auto after = std::chrono::steady_clock::now();
  assert(session.pingTimeout >= before + config::stream.ping_timeout);
  assert(session.pingTimeout <= after + config::stream.ping_timeout);
}
// Run a pending, matched CONNECT while a real apply owns the writer gate.
// The CV pause orders every remaining mutable config access in this fixture;
// the timeout getter itself must work without taking that gate.
void overlap(int stage, const std::optional<std::string> &next, std::chrono::milliseconds expected_before,
             std::chrono::milliseconds expected_after, bool fail = false) {
  config::apply_config({{"control_peer_timeout", "60000"}});
  std::mutex mutex;
  std::condition_variable cv;
  bool reached = false, proceed = false, caught = false;
  apply_test_hook = [&](int current) {
    if (current == stage) {
      std::unique_lock lock(mutex);
      reached = true;
      cv.notify_one();
      cv.wait(lock, [&] { return proceed; });
      if (fail) throw std::runtime_error("fixture late apply failure");
    }
  };
  // This unrelated key is allowed during an active stream. The file's timeout
  // remains unchanged unless this fixture explicitly supplies a replacement.
  config::fixture_hot_vars = {{"realtime_stats_enabled", "true"}};
  if (next) config::fixture_hot_vars.emplace("control_peer_timeout", *next);
  std::thread writer([&] {
    try { config::apply_config_now({}); }
    catch (const std::runtime_error &) { caught = true; }
  });
  {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return reached; });
  }
  if (config::get_control_peer_timeout() != expected_before) std::fprintf(stderr, "stage=%d next=%s actual=%lld expected=%lld\n", stage, next ? next->c_str() : "missing", (long long) config::get_control_peer_timeout().count(), (long long) expected_before.count());
  assert(config::get_control_peer_timeout() == expected_before);
  stream::control_server_t server;
  stream::session_t pending, later;
  ENetPeer early, late;
  connect(server, pending, early);
  assert(early.calls == (expected_before.count() > 0 ? 1 : 0));
  assert(early.minimum == (expected_before.count() > 0 ? expected_before.count() : 5000));
  assert(early.maximum == (expected_before.count() > 0 ? expected_before.count() : 30000));
  {
    std::lock_guard lock(mutex);
    proceed = true;
  }
  cv.notify_one();
  writer.join();
  apply_test_hook = [](int) {};
  assert(caught == fail);
  assert(config::get_control_peer_timeout() == expected_after);
  connect(server, later, late);
  assert(late.calls == (expected_after.count() > 0 ? 1 : 0));
  assert(late.minimum == (expected_after.count() > 0 ? expected_after.count() : 5000));
  assert(late.maximum == (expected_after.count() > 0 ? expected_after.count() : 30000));
  // Reload never revisits a peer that has already taken its snapshot.
  assert(early.minimum == (expected_before.count() > 0 ? expected_before.count() : 5000));
}

int main() {
  assert(config::get_control_peer_timeout() == 0ms); // before the first apply
  assert(can_hot_apply_during_session({"realtime_stats_enabled"}));
  assert(can_hot_apply_during_session({"realtime_stats_poll_interval_ms"}));
  assert(!can_hot_apply_during_session({"control_peer_timeout"}));
  // Unrelated hot applies must never temporarily reset a positive policy.
  for (int stage : {1, 2, 3, 4}) overlap(stage, "60000", 60000ms, 60000ms);
  // Before publication, even a newly parsed policy is still pending.
  for (int stage : {1, 2, 3}) {
    overlap(stage, "120000", 60000ms, 120000ms);
    overlap(stage, "malformed", 60000ms, 0ms);
    overlap(stage, std::nullopt, 60000ms, 0ms);
    overlap(stage, "0", 60000ms, 0ms);
    overlap(stage, "120000", 60000ms, 60000ms, true);
  }
  // The release store is the cutover even before the writer gate is released.
  overlap(4, "120000", 120000ms, 120000ms);
  overlap(4, "malformed", 0ms, 0ms);
  overlap(4, std::nullopt, 0ms, 0ms);
  config::apply_config({});

  assert(config::stream.ping_timeout == 10s);
  assert(config::get_control_peer_timeout() == 0ms);
  config::apply_config({});
  assert(config::get_control_peer_timeout() == 0ms);
  for (auto text : {"0", "1", "60000", "86399999", "\"60000\"", "00060000"}) {
    auto parsed = config::parse_control_peer_timeout(text);
    assert(parsed);
    config::apply_config({{"control_peer_timeout", text}});
    assert(config::get_control_peer_timeout() == *parsed);
  }
  for (auto text : {"", "\"\"", "-1", "-60000", "+1", "86399999x", "86400000", "2147483647", "2147483648", "4294967296", "4295027296", "18446744073709551616", "60000.0", "0xEA60", " 60000", "60000 ", "\"60000", "60000\"", "\"60000x\""}) {
    assert(!config::parse_control_peer_timeout(text));
    // Reach the previous nonzero setting through the same application boundary.
    config::apply_config({{"control_peer_timeout", "60000"}});
    assert(config::get_control_peer_timeout() == 60000ms);
    config::apply_config({{"control_peer_timeout", text}});
    assert(config::get_control_peer_timeout() == 0ms);
  }
  assert(!config::parse_control_peer_timeout(std::string_view {}));
  config::apply_config({{"control_peer_timeout", "60000"}});
  config::apply_config({});
  assert(config::get_control_peer_timeout() == 0ms); // missing option resets too
  config::apply_config({{"control_peer_timeout", "60000"}});
  config::apply_config({{"control_peer_timeout", "0"}});
  assert(config::get_control_peer_timeout() == 0ms);
  // Reapplying invalid text resets the setting for new peers, not existing ones.
  {
    stream::control_server_t reload_server;
    stream::session_t prior, after_reload;
    ENetPeer prior_peer, new_peer;
    config::apply_config({{"control_peer_timeout", "60000"}});
    connect(reload_server, prior, prior_peer);
    config::apply_config({{"control_peer_timeout", "malformed"}});
    assert(config::get_control_peer_timeout() == 0ms);
    connect(reload_server, after_reload, new_peer);
    assert(prior_peer.calls == 1 && prior_peer.minimum == 60000 && prior_peer.maximum == 60000);
    assert(new_peer.calls == 0 && new_peer.minimum == 5000 && new_peer.maximum == 30000);
  }
  stream::control_server_t server;
  stream::session_t first, second, third;
  ENetPeer p1, p2, p3, rejected;
  connect(server, first, p1);
  assert(p1.calls == 0 && p1.minimum == 5000 && p1.maximum == 30000);
  config::apply_config({{"control_peer_timeout", "60000"}});
  connect(server, second, p2);
  connect(server, third, p3);
  assert(p2.calls == 1 && p2.limit == ENET_PEER_TIMEOUT_LIMIT && p2.minimum == 60000 && p2.maximum == 60000);
  assert(p3.calls == 1 && p3.minimum == 60000 && p3.maximum == 60000);
  assert(p1.calls == 0); // existing peers keep their connection-time policy
  next_event = {ENET_EVENT_TYPE_CONNECT, &rejected, 0, nullptr};
  server.iterate(0ms);
  assert(rejected.calls == 0 && !rejected.connected);
  first.pingTimeout = std::chrono::steady_clock::now() - 1s;
  next_event = {ENET_EVENT_TYPE_RECEIVE, &p1, 0, new ENetPacket {nullptr, 0}};
  server.iterate(0ms);
  assert(callbacks == 1 && first.pingTimeout >= std::chrono::steady_clock::now() + 9s);
  assert(p1.calls == 0);
  // A large ENet retry window never replaces the independent ping deadline.
  const auto deadline = second.pingTimeout;
  first.pingTimeout = third.pingTimeout = deadline + 100s;
  stream::reap_sessions(&server, deadline);
  assert(second.state == stream::session::state_e::RUNNING);
  assert(p2.connected && !second.controlEnd.value);
  stream::reap_sessions(&server, deadline + 1ns);
  assert(second.state == stream::session::state_e::STOPPING);
  assert(second.shutdown_event->value && second.client_disconnected && second.controlEnd.value);
  assert(!p2.connected && p3.connected);
  assert(!server._peer_to_session->contains(&p2));
  // An explicit host stop is immediate even with the configured retry window.
  const int resets = input_resets;
  stream::session::stop(third);
  assert(third.state == stream::session::state_e::STOPPING && third.shutdown_event->value);
  assert(input_resets == resets + 1);
  stream::session::stop(third);
  assert(input_resets == resets + 1); // repeated stop is idempotent
  stream::reap_sessions(&server, deadline);
  assert(!p3.connected && third.controlEnd.value && server._sessions->size() == 1);
  // Preserve Vibepollo's first-stop ownership and graceful termination branch.
  assert(termination_sends == 0 && control_flushes == 0);
  {
    stream::control_server_t graceful_server;
    stream::session_t graceful;
    ENetPeer graceful_peer;
    connect(graceful_server, graceful, graceful_peer);
    stream::session::graceful_stop(graceful);
    assert(graceful.stop_reason == stream::stop_reason_e::graceful);
    stream::session::stop(graceful);
    assert(graceful.stop_reason == stream::stop_reason_e::graceful);
    stream::reap_sessions(&graceful_server, std::chrono::steady_clock::now());
    assert(termination_sends == 1 && control_flushes == 1);
    assert(!graceful_peer.connected && graceful.controlEnd.value);
  }
  // A peer's explicit disconnect also stops immediately rather than waiting.
  next_event = {ENET_EVENT_TYPE_DISCONNECT, &p1, 0, nullptr};
  server.iterate(0ms);
  assert(first.state == stream::session::state_e::STOPPING && first.shutdown_event->value && first.client_disconnected);
  stream::reap_sessions(&server, first.pingTimeout - 1s);
  assert(!p1.connected && first.controlEnd.value && server._sessions->empty());
  // The maximum accepted value is passed through without narrowing/wrapping.
  config::apply_config({{"control_peer_timeout", "86399999"}});
  stream::session_t maximum;
  ENetPeer p4;
  connect(server, maximum, p4);
  assert(p4.minimum == 86399999 && p4.maximum == 86399999);
  // Concurrent production getter/apply execution. Do not touch ping_timeout or
  // other mutable configuration objects from this reader (pre-existing scope).
  std::atomic_bool run {true}, ready {false};
  std::atomic<std::uint32_t> reads {0};
  std::thread reader([&] {
    while (run.load(std::memory_order_acquire)) {
      const auto timeout = config::get_control_peer_timeout();
      assert(timeout == 86399999ms || timeout == 60000ms);
      reads.fetch_add(1, std::memory_order_relaxed);
      ready.store(true, std::memory_order_release);
    }
  });
  while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
  for (int i = 0; i != 20000; ++i) {
    config::apply_config({{"control_peer_timeout", i % 2 == 0 ? "60000" : "86399999"}});
  }
  run.store(false, std::memory_order_release);
  reader.join();
  assert(reads.load() > 0);
  std::puts("Completed-apply timeout publication: deterministic CONNECT schedules, failures and 20000 concurrent reloads passed");
}
'''
with tempfile.TemporaryDirectory(prefix="control-peer-timeout-") as directory:
    src = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    src.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(root), str(src), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=45)
print("Production control timeout: reset/reapply parsing, per-peer CONNECT, ping expiry and immediate stop checks passed")
