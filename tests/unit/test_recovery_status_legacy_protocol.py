"""Execute recovery-status protocol fragments directly from both helpers.

This harness extracts production enums, JSON parsers, IPC branches, and the
v2 result callback. It uses deterministic pipe/queue/thread collaborators at
the OS boundary, while running the production statements unchanged.
"""
import argparse
import os
import pathlib
import re
import subprocess
import tempfile


parser = argparse.ArgumentParser()
parser.add_argument("repository_root", type=pathlib.Path)
parser.add_argument("compiler")
parser.add_argument("--json-include", type=pathlib.Path, default=os.environ.get("NLOHMANN_JSON_INCLUDE_DIR"))
args = parser.parse_args()
root = args.repository_root.resolve()
legacy = (root / "tools/display_settings_helper.cpp").read_text()
v2 = (root / "tools/display_settings_helper_v2.cpp").read_text()


def block_after(source, marker, occurrence=0):
    start = -1
    for _ in range(occurrence + 1):
        start = source.index(marker, start + 1)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def function(source, signature):
    tokens = re.findall(r"[A-Za-z_]\w*|::|[^\w\s]", signature)
    match = re.search(r"\s*".join(map(re.escape, tokens)), source)
    if match is None:
        raise AssertionError(f"Production function not found: {signature}")
    open_paren = source.index("(", match.start())
    paren_depth = 1
    end_signature = open_paren + 1
    while paren_depth:
        paren_depth += (source[end_signature] == "(") - (source[end_signature] == ")")
        end_signature += 1
    brace = source.index("{", end_signature)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def declaration(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


legacy_enum = declaration(legacy, "enum class MsgType")
v2_enum = declaration(v2, "enum class MsgType")
legacy_read = function(legacy, "std::optional<std::uint64_t> read_u64_le(")
legacy_append = function(legacy, "void append_u64_le(")
legacy_send = function(legacy, "inline void send_framed_content(")
legacy_json_type = declaration(legacy, "struct RevertOptions")
legacy_json_parser = function(legacy, "RevertOptions parse_revert_payload(")
v2_read = function(v2, "std::optional<std::uint64_t> read_u64_le(")
v2_append = function(v2, "void append_u64_le(")
v2_json_parser = function(v2, "void parse_revert_payload(")
v2_status_case = block_after(v2, "case MsgType::RecoveryStatus:")
legacy_status_branch = block_after(legacy, "if (type == MsgType::RecoveryStatus)")
v2_result_callback = block_after(v2, "[&response_pipe](std::uint64_t ticket, display_helper::v2::RecoveryStatus status,")


program = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "src/platform/windows/recovery_status.h"
#include "src/platform/windows/display_helper_borrow_protocol.h"

namespace platf::dxgi {
  struct AsyncNamedPipe {
    std::vector<std::uint8_t> frame;
    bool *require_joined_before_send = nullptr;
    void send(const std::vector<std::uint8_t> &bytes) {
      if (require_joined_before_send) assert(*require_joined_before_send);
      frame = bytes;
    }
  };
}

namespace legacy {
  using std::uint8_t;
''' + legacy_enum + ";\n" + legacy_read + "\n" + legacy_append + "\n" + legacy_send + "\n" + legacy_json_type + ";\n" + legacy_json_parser + r'''
  using display_helper::recovery_status::status;
  struct Joiner {
    bool can_join = true;
    unsigned joins = 0;
    std::function<void()> on_join;
    bool joinable() const { return can_join; }
    void join() { ++joins; can_join = false; if (on_join) on_join(); }
  };
  struct ServiceState {
    display_helper::recovery_status::policy recovery_status;
    std::mutex recovery_status_mutex;
    std::atomic<std::uint64_t> restore_cancel_generation {9};
    std::atomic<bool> restore_poll_active {false}, restore_stage_running {false};
    std::mutex command_queue_mutex;
    std::deque<int> command_queue;
    Joiner restore_poll_thread;
    std::uint64_t epoch = 3;
    unsigned liveness_pings = 0;
    std::uint64_t current_connection_epoch() const { return epoch; }
    void record_recovery_status_liveness_ping() { ++liveness_pings; }
  };
  void run_status_request(ServiceState &state, platf::dxgi::AsyncNamedPipe &async_pipe,
                          MsgType type, std::span<const std::uint8_t> payload, std::uint64_t worker_epoch) {
''' + legacy_status_branch + r'''
  }
}

namespace display_helper::v2 {
''' + v2_enum + ";\n" + v2_read + "\n" + v2_append + r'''
  enum class RecoveryStatus : std::uint8_t { Unknown = 0, Active = 1, Failed = 2, Restored = 3 };
  struct RevertCommand {
    bool prefer_golden_if_current_missing = true;
    std::optional<bool> always_restore_from_golden;
    std::uint64_t restore_ticket = 0;
    std::uint64_t generation = 0;
    std::uint64_t connection_epoch = 0;
  };
  struct RecoveryStatusCommand {
    std::uint64_t ticket = 0, generation = 0, connection_epoch = 0;
    bool park = false;
  };
''' + v2_json_parser + r'''
  struct Cancellation { std::uint64_t current_generation() const { return 44; } };
  struct Queue { std::vector<RecoveryStatusCommand> commands; void push(RecoveryStatusCommand command) { commands.push_back(command); } };
  void parse_status_request(std::span<const std::uint8_t> payload, std::uint64_t epoch, Queue &queue) {
    const MsgType type = MsgType::RecoveryStatus;
    const Cancellation cancellation;
    switch (type) {
''' + v2_status_case + r'''
    default: break;
    }
  }
  struct ResponsePipe {
    std::uint64_t epoch = 0;
    MsgType type {};
    std::vector<std::uint8_t> payload;
    void send_for_epoch(std::uint64_t value, MsgType kind, const std::vector<std::uint8_t> &bytes) {
      epoch = value; type = kind; payload = bytes;
    }
  };
  void send_status_result(ResponsePipe &response_pipe, std::uint64_t ticket, RecoveryStatus status,
                          std::uint64_t event_revision, bool parked, std::uint64_t origin_epoch) {
    auto callback = ''' + v2_result_callback + r''';
    callback(ticket, status, event_revision, parked, origin_epoch);
  }
}
namespace v2 = display_helper::v2;

static std::vector<std::uint8_t> ticket_bytes(std::uint64_t ticket, std::uint8_t park) {
  std::vector<std::uint8_t> result;
  for (unsigned i = 0; i < 8; ++i) result.push_back(static_cast<std::uint8_t>(ticket >> (8 * i)));
  result.push_back(park);
  return result;
}

static void test_wire_enums_are_the_production_values() {
  static_assert(static_cast<std::uint8_t>(legacy::MsgType::RecoveryStatus) == 15);
  static_assert(static_cast<std::uint8_t>(legacy::MsgType::RecoveryStatusResult) == 16);
  static_assert(static_cast<std::uint8_t>(v2::MsgType::RecoveryStatus) == 15);
  static_assert(static_cast<std::uint8_t>(v2::MsgType::RecoveryStatusResult) == 16);
}

static void test_ticket_parsers_accept_unsigned_ticket_and_reject_signed_ticket() {
  const std::string valid = R"({"sunshine_restore_ticket":77})";
  const auto bytes = std::span(reinterpret_cast<const std::uint8_t *>(valid.data()), valid.size());
  const auto legacy_options = legacy::parse_revert_payload(bytes);
  assert(legacy_options.restore_ticket == 77);
  v2::RevertCommand command;
  v2::parse_revert_payload(bytes, command);
  assert(command.restore_ticket == 77);

  const std::string signed_value = R"({"sunshine_restore_ticket":-1})";
  const auto signed_bytes = std::span(reinterpret_cast<const std::uint8_t *>(signed_value.data()), signed_value.size());
  assert(legacy::parse_revert_payload(signed_bytes).restore_ticket == 0);
  v2::RevertCommand signed_command;
  v2::parse_revert_payload(signed_bytes, signed_command);
  assert(signed_command.restore_ticket == 0);
}

static void test_v2_status_request_decoder_emits_real_command_values() {
  v2::Queue queue;
  const auto valid = ticket_bytes(0x0807060504030201ull, 1);
  v2::parse_status_request(valid, 55, queue);
  assert(queue.commands.size() == 1);
  assert(queue.commands.back().ticket == 0x0807060504030201ull);
  assert(queue.commands.back().generation == 44 && queue.commands.back().connection_epoch == 55);
  assert(queue.commands.back().park);

  auto invalid_park = ticket_bytes(9, 2);
  v2::parse_status_request(invalid_park, 56, queue);
  assert(queue.commands.back().ticket == 0 && !queue.commands.back().park);
  v2::parse_status_request({}, 57, queue);
  assert(queue.commands.back().ticket == 0 && !queue.commands.back().park);
}

static void test_legacy_status_branch_queries_joins_and_serializes() {
  using namespace display_helper::recovery_status;
  legacy::ServiceState state;
  state.recovery_status.begin(77, 9, 3);
  assert(state.recovery_status.observe_event() == 1);
  assert(state.recovery_status.publish(status::failed, 77, 9, 3));
  bool joined_before_send = false;
  state.restore_poll_thread.on_join = [&] { joined_before_send = true; };
  platf::dxgi::AsyncNamedPipe pipe;
  pipe.require_joined_before_send = &joined_before_send;
  const auto request = ticket_bytes(77, 1);
  legacy::run_status_request(state, pipe, legacy::MsgType::RecoveryStatus, request, 3);
  assert(state.restore_poll_thread.joins == 1 && joined_before_send);
  assert(state.liveness_pings == 1 && state.recovery_status.parked());
  assert(pipe.frame.size() == 19);
  assert(pipe.frame.front() == 16);
  assert(pipe.frame[1] == 77 && pipe.frame[9] == static_cast<std::uint8_t>(status::failed));
  assert(pipe.frame[10] == 1 && pipe.frame[18] == 1);

  legacy::ServiceState busy;
  busy.recovery_status.begin(77, 9, 3);
  busy.recovery_status.publish(status::failed, 77, 9, 3);
  busy.restore_poll_active.store(true);
  platf::dxgi::AsyncNamedPipe busy_pipe;
  legacy::run_status_request(busy, busy_pipe, legacy::MsgType::RecoveryStatus, request, 3);
  assert(busy.restore_poll_thread.joins == 0 && !busy.recovery_status.parked());
  assert(busy_pipe.frame[9] == static_cast<std::uint8_t>(status::active) && busy_pipe.frame[18] == 0);

  legacy::ServiceState malformed;
  platf::dxgi::AsyncNamedPipe malformed_pipe;
  legacy::run_status_request(malformed, malformed_pipe, legacy::MsgType::RecoveryStatus,
                             ticket_bytes(77, 2), 3);
  assert(malformed.liveness_pings == 0);
  assert(malformed_pipe.frame[9] == static_cast<std::uint8_t>(status::unknown));
  assert(malformed_pipe.frame[18] == 0);
}

static void test_v2_result_callback_serializes_status_and_parked_bit() {
  v2::ResponsePipe pipe;
  v2::send_status_result(pipe, 91, v2::RecoveryStatus::Failed, 0x1234, true, 12);
  assert(pipe.epoch == 12 && pipe.type == v2::MsgType::RecoveryStatusResult);
  assert(pipe.payload.size() == 18);
  assert(pipe.payload[0] == 91 && pipe.payload[8] == static_cast<std::uint8_t>(v2::RecoveryStatus::Failed));
  assert(pipe.payload[9] == 0x34 && pipe.payload[10] == 0x12 && pipe.payload[17] == 1);
  v2::send_status_result(pipe, 91, v2::RecoveryStatus::Active, 0, false, 13);
  assert(pipe.payload.back() == 0);
}

int main() {
  test_wire_enums_are_the_production_values();
  test_ticket_parsers_accept_unsigned_ticket_and_reject_signed_ticket();
  test_v2_status_request_decoder_emits_real_command_values();
  test_legacy_status_branch_queries_joins_and_serializes();
  test_v2_result_callback_serializes_status_and_parked_bit();
}
'''

with tempfile.TemporaryDirectory(prefix="recovery-status-protocol-") as temporary:
    source = pathlib.Path(temporary) / "recovery_status_protocol.cpp"
    binary = pathlib.Path(temporary) / "recovery_status_protocol"
    source.write_text(program)
    command = [args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(root)]
    if args.json_include:
        command += ["-I", str(pathlib.Path(args.json_include).resolve())]
    command += [str(source), "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
