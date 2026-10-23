#!/usr/bin/env python3
"""Replay the production WGC callback and admission method with controlled clocks.

WinRT, GPU delivery/publication and snapshot boundaries are deterministic models;
the callback body, admission state/method and IPC rate-update guard are extracted
unchanged from the helper. This establishes source admission, not Windows overlay
acceptance. --source-file replays an older helper as a negative control.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile


def block(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    parser.add_argument("compiler", nargs="?", default="c++")
    parser.add_argument("--source-file", type=Path)
    parser.add_argument("--case", choices=("all", "cadence"), default="all")
    parser.add_argument("--emit-cpp", type=Path, help="write the replay for inspection without compiling or running it")
    args = parser.parse_args()
    source = (args.source_file or args.repository / "tools/sunshine_wgc_capture.cpp").read_text()
    state_start = source.index("  uint32_t _activity_admission_generation =")
    state_end = source.index("  std::mutex _delivery_mutex;", state_start)
    ipc = source[source.index("void handle_ipc_message("):]
    code = r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "src/platform/windows/wgc_capture_policy.h"
using namespace std::chrono_literals;
std::ostringstream log_output;
#define BOOST_LOG(level) log_output
struct LARGE_INTEGER { int64_t QuadPart = 0; };
void QueryPerformanceFrequency(LARGE_INTEGER *frequency) { frequency->QuadPart = 24'000'000; }
namespace winrt {
  struct hresult_error { int code() const { return -1; } std::string message() const { return "stub error"; } };
  std::string to_string(const std::string &text) { return text; }
}
namespace platf::dxgi {
  constexpr uint32_t WGC_IPC_FLAG_DRAIN_TO_LATEST = 1;
  constexpr uint32_t WGC_ACTIVITY_ADMISSION_MESSAGE_MAGIC = 0x57474341;
  struct activity_admission_data_t { uint32_t magic; int32_t admission_fps; };
}
struct { uint32_t flags = platf::dxgi::WGC_IPC_FLAG_DRAIN_TO_LATEST; } g_config;
std::atomic<int32_t> g_activity_admission_fps {60};
std::atomic<uint32_t> g_activity_admission_generation {0};
struct ReplayClock {
  using rep = int64_t;
  using period = std::nano;
  using duration = std::chrono::nanoseconds;
  using time_point = std::chrono::time_point<ReplayClock>;
  static constexpr bool is_steady = true;
  static inline duration time {};
  static time_point now() { return time_point(time); }
};
struct Direct3D11CaptureFrame {
  bool valid = false;
  int pixels = 0;
  int64_t source = 0;
  Direct3D11CaptureFrame(std::nullptr_t = nullptr) {}
  Direct3D11CaptureFrame(int64_t timestamp, int content): valid(true), pixels(content), source(timestamp) {}
  explicit operator bool() const { return valid; }
  int Surface() const { return pixels; }
  struct timestamp_t { int64_t ticks; int64_t count() const { return ticks; } };
  timestamp_t SystemRelativeTime() const { return {source}; }
};
struct Direct3D11CaptureFramePool {
  mutable std::deque<Direct3D11CaptureFrame> frames;
  Direct3D11CaptureFrame TryGetNextFrame() const {
    if (frames.empty()) return nullptr;
    auto result = frames.front(); frames.pop_front(); return result;
  }
};
struct Delivery { int pixels; uint64_t qpc; int64_t callback_ns; };
// Shadow only the clock name. Extracted production bodies remain unchanged.
namespace replay {
namespace std {
  using namespace ::std;
  namespace chrono { using namespace ::std::chrono; using steady_clock = ::ReplayClock; }
}
struct Manager {
// ADMISSION_STATE
  std::deque<std::chrono::steady_clock::time_point> _drop_timestamps;
  std::atomic<uint64_t> _frame_pool_empty_drops {0}, _activity_rate_limited_frames {0};
  std::vector<Delivery> admitted, snapshots;
  std::optional<Delivery> pending;
  uint32_t captured = 0, drained = 0, replaced = 0, checks = 0;
  uint32_t _current_buffer_size = 1, _max_buffer_size = 2;
// DRAIN_POLICY
  void record_frame_pool_pressure(std::chrono::steady_clock::time_point) {}
  void record_frame_arrival(uint32_t count) { ++captured; drained += count; }
  void check_and_adjust_frame_buffer() { ++checks; }
  void queue_frame_for_delivery(Direct3D11CaptureFrame, int pixels, uint64_t qpc) {
    Delivery next {pixels, qpc, ReplayClock::time.count()};
    admitted.push_back(next);
    replaced += pending.has_value();
    pending = next;
  }
  void publish_and_snapshot() {
    if (pending) { snapshots.push_back(*pending); pending.reset(); }
  }
// PROCESS_FRAME
// ADMISSION_METHOD
};
}
void handle_admission_message(std::span<const uint8_t> message) {
// IPC_UPDATE
}
void update(int fps, bool valid_magic = true) {
  platf::dxgi::activity_admission_data_t packet {
    valid_magic ? platf::dxgi::WGC_ACTIVITY_ADMISSION_MESSAGE_MAGIC : 0, fps
  };
  handle_admission_message({reinterpret_cast<const uint8_t *>(&packet), sizeof(packet)});
}
int failures = 0, checks = 0;
void check(bool condition, const char *message) {
  ++checks;
  if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
bool submit(replay::Manager &manager, int64_t source, int64_t callback_ns, int pixels = 7, bool snapshot = true) {
  ReplayClock::time = std::chrono::nanoseconds(callback_ns);
  Direct3D11CaptureFramePool pool;
  pool.frames.emplace_back(source, pixels);
  const auto before = manager.admitted.size();
  manager.process_frame(pool);
  if (snapshot) manager.publish_and_snapshot();
  return manager.admitted.size() != before;
}
void reset(int fps = 60) {
  g_activity_admission_fps = fps; g_activity_admission_generation = 0;
  g_config.flags = platf::dxgi::WGC_IPC_FLAG_DRAIN_TO_LATEST;
}
void cadence(int source_fps, int ceiling, int expected, bool jitter) {
  reset(ceiling); replay::Manager manager;
  for (int index = 0; index < source_fps; ++index) {
    submit(manager, 10'000'000LL + index * 10'000'000LL / source_fps,
           5'000'000'000LL + index * 1'000'000'000LL / source_fps + (jitter && index % 2 ? 4'000'000 : 0), index);
  }
  std::cout << "source=" << source_fps << " ceiling=" << ceiling << " jitter=" << jitter
            << " admitted=" << manager.admitted.size() << '/' << source_fps << '\n';
  check(manager.admitted.size() == static_cast<size_t>(expected), "production callback preserves expected source cadence");
  check(manager.snapshots.size() == manager.admitted.size(), "modeled publication/snapshot carries each admitted frame");
  check(manager._activity_rate_limited_frames == static_cast<uint64_t>(source_fps - expected), "admission diagnostics count rejections");
  check(manager.captured == static_cast<uint32_t>(source_fps), "callback records each source frame before admission");
  for (const auto &frame : manager.snapshots) {
    const auto source = 10'000'000LL + frame.pixels * 10'000'000LL / source_fps;
    check(frame.qpc == static_cast<uint64_t>(source * 12 / 5), "publication retains compositor timestamp in non-10MHz QPC ticks");
  }
  if (expected == source_fps) {
    for (int index = 0; index < source_fps && index < static_cast<int>(manager.snapshots.size()); ++index)
      check(manager.snapshots[index].pixels == index, "changing content survives callback admission in order");
  }
}
int main() {
  cadence(60, 60, 60, true);
  if (CADENCE_ONLY) return failures ? 1 : 0;
  cadence(60, 60, 60, false);
  cadence(120, 60, 60, true);
  cadence(240, 60, 60, false);
  cadence(144, 60, 48, false); // Minimum-gap ceiling, deliberately no credit accumulation.
  cadence(60, 30, 30, true);
  cadence(60, 120, 60, true);

  reset(); replay::Manager unchanged;
  for (int index = 0; index < 60; ++index)
    submit(unchanged, 10'000'000LL + index * 10'000'000LL / 60,
           1'000'000'000LL + index * 1'000'000'000LL / 60, 99);
  check(unchanged.admitted.size() == 60, "unchanged pixels with advancing source timestamps are not deduplicated");

  reset(); replay::Manager duplicates;
  submit(duplicates, 10'000'000, 1'000'000'000);
  for (int index = 1; index <= 10; ++index) submit(duplicates, 10'000'000, 1'000'000'000LL + index * 20'000'000, index);
  check(duplicates.admitted.size() == 11 && duplicates.snapshots.back().pixels == 10,
        "repeated timestamp uses bounded fallback and keeps changing content live");
  check(submit(duplicates, 10'166'667, 1'300'000'000), "fresh source recovers after repeated timestamp");

  for (int64_t invalid : {0LL, -1LL, 10'000'000LL}) {
    reset(); replay::Manager fallback;
    for (int index = 0; index < 1000; ++index) submit(fallback, invalid, 1'000'000'000LL + index * 1'000'000);
    check(fallback.admitted.size() <= 60 && fallback.admitted.size() >= 58, "invalid timestamps retain bounded arrival fallback");
    for (size_t index = 1; index < fallback.admitted.size(); ++index)
      check(fallback.admitted[index].callback_ns - fallback.admitted[index - 1].callback_ns >= 16'666'666, "invalid fallback never accumulates callback credits");
  }

  reset(); replay::Manager recovery;
  check(submit(recovery, 10'000'000, 1'000'000'000), "first source frame admitted");
  check(!submit(recovery, 0, 1'001'000'000), "missing source cannot bypass cap");
  check(!submit(recovery, 10'166'666, 1'002'000'000), "returning source cannot bypass fallback cap");
  check(submit(recovery, 10'333'333, 1'017'000'000), "fresh source reanchors after missing timestamps");
  check(submit(recovery, 10'500'000, 1'025'000'000), "recovered source uses compositor gap despite callback jitter");
  check(submit(recovery, 10'500'000, 1'050'000'000), "repeated source uses bounded arrival fallback");
  check(!submit(recovery, 1, 1'051'000'000), "backwards source cannot bypass fallback cap");
  check(!submit(recovery, 166'667, 1'052'000'000), "reset source timeline cannot burst during reanchor");
  check(submit(recovery, 333'334, 1'068'000'000), "reset source timeline reanchors without waiting for old timestamp");
  check(submit(recovery, 500'001, 1'076'000'000), "reset timeline recovers source-based admission");

  reset(); replay::Manager backwards;
  for (int index = 0; index < 1000; ++index)
    submit(backwards, 10'000'000 - index, 1'000'000'000LL + index * 1'000'000);
  check(backwards.admitted.size() <= 60 && backwards.admitted.size() >= 58, "persistently nonmonotonic source remains bounded and live");

  reset(); replay::Manager idle;
  submit(idle, 10'000'000, 1'000'000'000);
  check(submit(idle, 60'000'000, 6'000'000'000), "first fresh frame after idle admitted immediately");
  check(!submit(idle, 60'000'001, 6'000'000'001), "idle creates no catch-up credit");
  check(submit(idle, 60'166'666, 6'001'000'000), "valid source gap after idle independent of callback gap");

  reset(); replay::Manager caps;
  submit(caps, 10'000'000, 1'000'000'000);
  update(30);
  check(g_activity_admission_fps == 30 && g_activity_admission_generation == 2, "production IPC changes explicit cap and generation");
  check(submit(caps, 20'000'000, 2'000'000'000), "cap update resets source admission state");
  check(!submit(caps, 20'166'667, 2'100'000'000), "explicit 30 FPS cap applies to source despite long callback gap");
  check(submit(caps, 20'333'333, 2'101'000'000), "explicit 30 FPS source interval admitted");
  update(120);
  check(submit(caps, 20'333'334, 2'102'000'000), "higher cap generation also resets admission");
  check(!submit(caps, 20'333'335, 2'102'000'001), "higher cap creates no credit burst");
  check(submit(caps, 20'416'667, 2'103'000'000), "explicit 120 FPS cap honors quantized source interval");
  update(0); update(-1); update(30, false);
  check(g_activity_admission_fps == 120 && g_activity_admission_generation == 4, "invalid IPC updates preserve current cap and generation");

  reset(240); replay::Manager transition;
  check(submit(transition, 30'000'000, 3'000'000'000), "transition fixture establishes old cadence state");
  g_activity_admission_generation.store(1, std::memory_order_release);
  g_activity_admission_fps.store(120, std::memory_order_relaxed);
  std::atomic<bool> reader_started = false, reader_finished = false;
  bool transition_admitted = false;
  std::jthread reader([&] {
    reader_started = true;
    transition_admitted = submit(transition, 30'041'667, 3'004'166'700);
    reader_finished = true;
  });
  while (!reader_started) std::this_thread::yield();
  std::this_thread::sleep_for(1ms);
  check(!reader_finished, "reader waits while cadence snapshot generation is odd");
  g_activity_admission_generation.store(2, std::memory_order_release);
  reader.join();
  check(transition_admitted, "completed cadence snapshot resets admission once");
  check(!submit(transition, 30'083'334, 3'008'333'400), "new cap rejects the adjacent post-transition source frame");

  reset(0); replay::Manager disabled;
  for (int index = 0; index < 10; ++index) submit(disabled, 0, 1'000'000'000 + index);
  check(disabled.admitted.size() == 10, "disabled admission cap preserves existing pass-through behavior");

  reset(); replay::Manager backlog;
  Direct3D11CaptureFramePool pool;
  for (int index = 0; index < 60; ++index) pool.frames.emplace_back(10'000'000LL + index * 10'000'000LL / 60, index);
  ReplayClock::time = 2s;
  backlog.process_frame(pool);
  check(backlog.admitted.size() == 1 && backlog.admitted.back().pixels == 59 && backlog.drained == 59,
        "production drain-to-latest retains only newest backlog frame");
  submit(backlog, 20'000'000, 2'020'000'000, 60, false);
  check(backlog.pending.has_value() && backlog.replaced == 1, "modeled blocked delivery remains one latest pending frame");
  backlog.publish_and_snapshot();
  check(backlog.snapshots.size() == 1 && backlog.snapshots[0].pixels == 60, "modeled slow snapshot sees latest frame without backlog replay");
  backlog.process_frame(pool);
  check(backlog._frame_pool_empty_drops == 1 && backlog.admitted.size() == 2, "empty pool never manufactures admitted frames");

  reset(); g_config.flags = 0; replay::Manager fifo;
  pool.frames.emplace_back(10'000'000, 1); pool.frames.emplace_back(10'166'667, 2);
  fifo.process_frame(pool);
  check(fifo.admitted.size() == 1 && fifo.admitted[0].pixels == 1 && pool.frames.size() == 1,
        "existing non-draining mode still retrieves one frame");
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
'''
    replacements = {
        "// ADMISSION_STATE": source[state_start:state_end],
        "// DRAIN_POLICY": block(source, "  bool drain_to_latest() const"),
        "// PROCESS_FRAME": block(source, "  void process_frame(Direct3D11CaptureFramePool"),
        "// ADMISSION_METHOD": block(source, "  bool admit_activity_frame("),
        "// IPC_UPDATE": block(ipc, "  if (message.size() == sizeof(platf::dxgi::activity_admission_data_t))"),
        "CADENCE_ONLY": "true" if args.case == "cadence" else "false",
    }
    for marker, replacement in replacements.items():
        code = code.replace(marker, replacement)
    if args.emit_cpp:
        args.emit_cpp.write_text(code)
        return
    with tempfile.TemporaryDirectory(prefix="wgc-source-admission-") as directory:
        cpp = Path(directory) / "replay.cpp"
        executable = Path(directory) / "replay"
        cpp.write_text(code)
        subprocess.run([args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(args.repository), str(cpp), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
