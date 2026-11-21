#!/usr/bin/env python3
"""Replay Windows GameStream RTP stamping after an admitted WGC update stalls."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    parser.add_argument("compiler", nargs="?", default="c++")
    parser.add_argument("--revision")
    args = parser.parse_args()
    if args.revision:
        source = subprocess.check_output(
            ["git", "-C", str(args.repository), "show", f"{args.revision}:src/stream.cpp"],
            text=True,
        )
    else:
        source = (args.repository / "src/stream.cpp").read_text()

    account_start = source.index("      std::size_t frame_packets_accepted = 0;")
    account_end = source.index("\n      try {", account_start)
    stamp_start = source.index("        bool frame_is_dupe = false;")
    stamp_end = source.index("\n        // PyroWave record framing:", stamp_start)
    fixture = r'''
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <utility>

#ifndef _WIN32
#define _WIN32
#endif

using Clock = std::chrono::steady_clock;
using WireTick = std::chrono::duration<std::int64_t, std::ratio<1, 90000>>;

Clock::time_point stamp_at(std::int64_t ticks) {
  return Clock::time_point(std::chrono::duration_cast<Clock::duration>(WireTick(ticks)));
}

namespace platf::dxgi::present_timing {
  // Production can return the input composition time, e.g. without an active refiner.
  Clock::time_point refine_send_timestamp(Clock::time_point composition) { return composition; }
}

namespace util {
  template<class F>
  struct fail_guard {
    F action;
    ~fail_guard() { action(); }
  };
}

struct Session {
  struct {
    std::optional<unsigned> cipher;
    std::optional<std::int64_t> last_sent_rtp_tick;
  } video;
  struct {
    std::atomic<std::size_t> packets_sent {0}, bytes_sent {0};
  } stats;
};

struct WireFrame {
  std::uint32_t timestamp;
  bool repeated;
  Clock::time_point source_stamp;
};

WireFrame send_frame(Session *session, std::optional<Clock::time_point> source_stamp,
                      Clock::time_point repeated_stamp, std::size_t accepted_packets = 1) {
  struct Packet {
    std::optional<Clock::time_point> frame_timestamp;
  } data {source_stamp};
  auto *packet = &data;
  struct video_packet_enc_prefix_t { char bytes[32]; };
  const std::size_t blocksize = 1400;
  const auto video_epoch = stamp_at(0);
  const auto ratecontrol_next_frame_start = repeated_stamp;
  // ACCOUNT
  // STAMP
  frame_packets_accepted = accepted_packets;
  return {timestamp, frame_is_dupe, *packet->frame_timestamp};
}

int failures = 0;
void check(bool ok, const char *name) {
  std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
  if (!ok) ++failures;
}

int main() {
  {
    Session session;
    auto first = send_frame(&session, stamp_at(90'000), {});
    auto repeat = send_frame(&session, std::nullopt, stamp_at(112'590));
    auto recovered = send_frame(&session, stamp_at(90'450), {});
    auto next = send_frame(&session, stamp_at(120'000), {});
    check(first.timestamp == 90'000 && repeat.timestamp == 112'590 && repeat.repeated &&
          recovered.timestamp == 112'591 && recovered.source_stamp == stamp_at(90'450) &&
          next.timestamp == 120'000 && !recovered.repeated,
          "retained source after a synthetic IDR advances only the wire clock");
  }
  {
    Session session;
    auto cached = send_frame(&session, stamp_at(180'000), {});
    auto recovered = send_frame(&session, stamp_at(135'000), {});
    check(cached.timestamp == 180'000 && recovered.timestamp == 180'001 &&
          recovered.source_stamp == stamp_at(135'000),
          "timestamped constant-frame-rate replay cannot overtake a recovered frame");
  }
  {
    Session first, second;
    auto discard = send_frame(&first, stamp_at(10'000), {}, 0);
    auto start = send_frame(&first, stamp_at(900), {});
    auto partial = send_frame(&first, stamp_at(2'000), {}, 1);
    auto recovered = send_frame(&first, stamp_at(1'500), {});
    auto independent = send_frame(&second, stamp_at(100), {});
    check(discard.timestamp == 10'000 && start.timestamp == 900 && partial.timestamp == 2'000 &&
          recovered.timestamp == 2'001 && independent.timestamp == 100 &&
          first.stats.packets_sent == 3 && second.stats.packets_sent == 1,
          "only accepted packets advance this session's wire clock");
  }
  {
    Session session;
    constexpr auto wrap = 1LL << 32;
    auto first = send_frame(&session, stamp_at(wrap - 2), {});
    auto wrapped = send_frame(&session, stamp_at(wrap), {});
    auto late = send_frame(&session, stamp_at(wrap - 1), {});
    auto next = send_frame(&session, stamp_at(wrap + 90), {});
    check(first.timestamp == 0xfffffffeu && wrapped.timestamp == 0 &&
          late.timestamp == 1 && next.timestamp == 90,
          "a natural 32-bit wrap advances through zero before repairing a late stamp");
  }
  return failures ? 1 : 0;
}
'''
    cpp_source = fixture.replace("// ACCOUNT", source[account_start:account_end]).replace(
        "// STAMP", source[stamp_start:stamp_end]
    )
    with tempfile.TemporaryDirectory(prefix="wgc-rtp-") as directory:
        scratch = Path(directory)
        cpp = scratch / "replay.cpp"
        executable = scratch / "replay"
        cpp.write_text(cpp_source)
        subprocess.run(
            [args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(executable)],
            check=True,
        )
        return subprocess.run([str(executable)], check=False, timeout=5).returncode


if __name__ == "__main__":
    raise SystemExit(main())
