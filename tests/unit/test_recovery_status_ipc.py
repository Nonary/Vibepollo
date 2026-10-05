"""Compile the production recovery-status IPC query with deterministic pipes.

This extracts query_recovery_status unchanged and replaces its cached transport
collaborators. The fixture verifies frame admission, park acknowledgement, and
that the observer never creates or reconnects a helper session.
"""
import pathlib
import re
import subprocess
import sys
import tempfile


root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "g++-15"
source_path = root / "src/platform/windows/ipc/display_settings_client.cpp"
source = source_path.read_text()


def function(text, signature):
    tokens = re.findall(r"[A-Za-z_]\w*|::|[^\w\s]", signature)
    match = re.search(r"\s*".join(re.escape(token) for token in tokens), text)
    if match is None:
        raise ValueError(f"Production function not found: {signature}")
    start = match.start()
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


query = function(
    source,
    "std::optional<RecoveryStatusResult> query_recovery_status(const std::uint64_t ticket",
)

program = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace platf::dxgi {
  enum class PipeResult { Success, Timeout, Disconnected, Error };
}

namespace platf::display_helper_client {
  enum class RecoveryStatus : std::uint8_t { Unknown = 0, Active = 1, Failed = 2, Restored = 3 };
  struct RecoveryStatusResult {
    std::uint64_t ticket = 0;
    std::uint64_t connection_generation = 0;
    RecoveryStatus status = RecoveryStatus::Unknown;
    std::uint64_t event_revision = 0;
    bool parked = false;
  };
  enum class MsgType : std::uint8_t { RecoveryStatus = 15, RecoveryStatusResult = 16, Ping = 0xfe };
  namespace fake { extern bool current_session; }

  struct FakePipe {
    std::deque<std::vector<std::uint8_t>> frames;
    std::deque<platf::dxgi::PipeResult> failures;
    bool disconnect_after_read = false;
    platf::dxgi::PipeResult receive(std::array<std::uint8_t, 65536> &buffer, std::size_t &bytes_read, int) {
      if (!frames.empty()) {
        const auto frame = std::move(frames.front());
        frames.pop_front();
        bytes_read = std::min(frame.size(), buffer.size());
        std::copy_n(frame.begin(), bytes_read, buffer.begin());
        if (disconnect_after_read) fake::current_session = false;
        return platf::dxgi::PipeResult::Success;
      }
      bytes_read = 0;
      if (!failures.empty()) {
        const auto result = failures.front();
        failures.pop_front();
        return result;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return platf::dxgi::PipeResult::Timeout;
    }
  };
  struct Session {
    std::uint64_t generation = 0;
    std::shared_ptr<FakePipe> pipe;
    std::timed_mutex response_mutex;
    std::mutex response_inbox_mutex;
    std::deque<std::vector<std::uint8_t>> response_inbox;
  };
  using SessionPtr = std::shared_ptr<Session>;

  namespace fake {
    SessionPtr session;
    bool current_session = true;
    int cached_session_lookups = 0;
    int reconnect_attempts = 0;
    int helper_starts = 0;
    int sends = 0;
    std::vector<std::uint8_t> sent_payload;
    void reset(std::uint64_t generation = 7) {
      current_session = true;
      cached_session_lookups = reconnect_attempts = helper_starts = sends = 0;
      sent_payload.clear();
      session = std::make_shared<Session>();
      session->generation = generation;
      session->pipe = std::make_shared<FakePipe>();
    }
  }

  bool session_is_current(const SessionPtr &) { return fake::current_session; }
  SessionPtr cached_connected_session_within(std::chrono::steady_clock::time_point) {
    ++fake::cached_session_lookups;
    return fake::session;
  }
  bool lock_response_reader_until(
    const SessionPtr &session,
    std::unique_lock<std::timed_mutex> &lock,
    std::chrono::steady_clock::time_point deadline,
    const std::function<bool()> &cancelled) {
    while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
      if (session->response_mutex.try_lock()) {
        lock = std::unique_lock<std::timed_mutex>(session->response_mutex, std::adopt_lock);
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }
  void append_u64_le(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
    for (unsigned int i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  }
  int remaining_timeout_ms(std::chrono::steady_clock::time_point deadline) {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
  }
  bool send_serialized_within(
    const SessionPtr &, MsgType, const std::vector<std::uint8_t> &payload,
    std::chrono::steady_clock::time_point, std::optional<std::uint64_t>, int,
    void *, bool, const std::function<bool()> &) {
    ++fake::sends;
    fake::sent_payload = payload;
    return true;
  }
  bool consume_mutation_frame(const SessionPtr &, std::span<const std::uint8_t>) { return false; }
  bool is_bufferable_response(std::uint8_t type) { return type == static_cast<std::uint8_t>(MsgType::RecoveryStatusResult); }
  void buffer_response(const SessionPtr &session, std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || !is_bufferable_response(bytes.front())) return;
    std::lock_guard lock(session->response_inbox_mutex);
    session->response_inbox.emplace_back(bytes.begin(), bytes.end());
  }
  std::optional<std::vector<std::uint8_t>> take_buffered_response(
    const SessionPtr &session,
    MsgType type,
    const std::function<bool(std::span<const std::uint8_t>)> &matches) {
    std::lock_guard lock(session->response_inbox_mutex);
    const auto it = std::find_if(session->response_inbox.begin(), session->response_inbox.end(), [&](const auto &frame) {
      return !frame.empty() && frame.front() == static_cast<std::uint8_t>(type) && matches(frame);
    });
    if (it == session->response_inbox.end()) return std::nullopt;
    auto result = std::move(*it);
    session->response_inbox.erase(it);
    return result;
  }

  std::optional<RecoveryStatusResult> query_recovery_status(
    std::uint64_t, std::uint64_t, bool, std::chrono::milliseconds, bool receive_only = false);

''' + query + r'''

  std::vector<std::uint8_t> frame(std::uint64_t ticket, std::uint8_t status, std::uint64_t revision, std::uint8_t parked) {
    std::vector<std::uint8_t> result {static_cast<std::uint8_t>(MsgType::RecoveryStatusResult)};
    for (unsigned int i = 0; i < 8; ++i) result.push_back(static_cast<std::uint8_t>(ticket >> (8 * i)));
    result.push_back(status);
    for (unsigned int i = 0; i < 8; ++i) result.push_back(static_cast<std::uint8_t>(revision >> (8 * i)));
    result.push_back(parked);
    return result;
  }

  void assert_no_reconnect_or_start() {
    assert(fake::cached_session_lookups == 1);
    assert(fake::reconnect_attempts == 0);
    assert(fake::helper_starts == 0);
  }

  void test_buffered_nonpark_failure_does_not_satisfy_park_query() {
    fake::reset();
    {
      std::lock_guard lock(fake::session->response_inbox_mutex);
      fake::session->response_inbox.push_back(frame(44, static_cast<std::uint8_t>(RecoveryStatus::Failed), 9, 0));
    }
    fake::session->pipe->frames.push_back(frame(45, static_cast<std::uint8_t>(RecoveryStatus::Failed), 10, 1));
    fake::session->pipe->frames.push_back(frame(44, static_cast<std::uint8_t>(RecoveryStatus::Failed), 11, 0));
    fake::session->pipe->frames.push_back(frame(44, static_cast<std::uint8_t>(RecoveryStatus::Failed), 12, 1));
    const auto result = query_recovery_status(44, 7, true, std::chrono::milliseconds(200));
    assert(result && result->ticket == 44 && result->connection_generation == 7);
    assert(result->status == RecoveryStatus::Failed && result->event_revision == 12 && result->parked);
    assert(fake::sends == 1 && fake::sent_payload.size() == 9 && fake::sent_payload.back() == 1);
    assert_no_reconnect_or_start();
  }

  void test_only_matching_ticket_and_current_connection_park_ack_is_returned() {
    fake::reset();
    fake::session->pipe->frames.push_back(frame(101, static_cast<std::uint8_t>(RecoveryStatus::Failed), 1, 1));
    fake::session->pipe->frames.push_back(frame(100, static_cast<std::uint8_t>(RecoveryStatus::Failed), 2, 0));
    fake::session->pipe->frames.push_back(frame(100, static_cast<std::uint8_t>(RecoveryStatus::Failed), 3, 1));
    const auto result = query_recovery_status(100, 7, true, std::chrono::milliseconds(200));
    assert(result && result->ticket == 100 && result->connection_generation == 7 && result->parked);
    assert(result->event_revision == 3);
    assert_no_reconnect_or_start();

    fake::reset();
    const auto wrong_connection = query_recovery_status(100, 8, true, std::chrono::milliseconds(50));
    assert(!wrong_connection && fake::sends == 0);
    assert_no_reconnect_or_start();
  }

  void test_short_truncated_and_invalid_ack_frames_remain_unknown() {
    fake::reset();
    fake::session->pipe->frames.push_back({static_cast<std::uint8_t>(MsgType::RecoveryStatusResult)});
    auto truncated = frame(200, static_cast<std::uint8_t>(RecoveryStatus::Failed), 4, 1);
    truncated.resize(18);
    fake::session->pipe->frames.push_back(std::move(truncated));
    fake::session->pipe->frames.push_back(frame(200, static_cast<std::uint8_t>(RecoveryStatus::Failed), 5, 2));
    const auto result = query_recovery_status(200, 7, true, std::chrono::milliseconds(30));
    assert(!result);
    assert_no_reconnect_or_start();
  }

  void test_timeout_and_disconnect_use_only_the_cached_session() {
    fake::reset();
    const auto timed_out = query_recovery_status(300, 7, true, std::chrono::milliseconds(10));
    assert(!timed_out && fake::sends == 1);
    assert_no_reconnect_or_start();

    fake::reset();
    fake::session->pipe->frames.push_back(frame(300, static_cast<std::uint8_t>(RecoveryStatus::Restored), 6, 0));
    fake::session->pipe->disconnect_after_read = true;
    const auto disconnected = query_recovery_status(300, 7, true, std::chrono::milliseconds(50));
    assert(!disconnected);
    assert_no_reconnect_or_start();
  }

  void test_event_receive_is_passive_and_never_acknowledges_parking() {
    fake::reset();
    fake::session->pipe->frames.push_back(frame(400, static_cast<std::uint8_t>(RecoveryStatus::Failed), 9, 0));
    const auto event = query_recovery_status(400, 7, false, std::chrono::milliseconds(50), true);
    assert(event && event->status == RecoveryStatus::Failed && event->event_revision == 9 && !event->parked);
    assert(fake::sends == 0);
    assert_no_reconnect_or_start();

    fake::reset();
    assert(!query_recovery_status(400, 7, false, std::chrono::milliseconds(10), true));
    assert(fake::sends == 0);
    assert_no_reconnect_or_start();

    fake::reset();
    assert(!query_recovery_status(400, 7, true, std::chrono::milliseconds(50), true));
    assert(fake::sends == 0 && fake::cached_session_lookups == 0);
  }
}

int main() {
  using namespace platf::display_helper_client;
  test_buffered_nonpark_failure_does_not_satisfy_park_query();
  test_only_matching_ticket_and_current_connection_park_ack_is_returned();
  test_short_truncated_and_invalid_ack_frames_remain_unknown();
  test_timeout_and_disconnect_use_only_the_cached_session();
  test_event_receive_is_passive_and_never_acknowledges_parking();
}
'''

with tempfile.TemporaryDirectory(prefix="vibeshine-recovery-ipc-") as temp_dir:
    temp = pathlib.Path(temp_dir)
    cpp_path = temp / "recovery_status_ipc.cpp"
    binary_path = temp / "recovery_status_ipc"
    cpp_path.write_text(program)
    subprocess.run(
        [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", str(cpp_path), "-o", str(binary_path)],
        check=True,
    )
    subprocess.run([str(binary_path)], check=True)
