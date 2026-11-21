#!/usr/bin/env python3
"""Replay the Windows RTP clock, shard stamps and partial-send accounting."""

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

    def read_source(name):
        if args.revision:
            return subprocess.check_output(
                ["git", "-C", str(args.repository), "show", f"{args.revision}:{name}"],
                text=True,
            )
        return (args.repository / name).read_text()

    source = read_source("src/stream.cpp")
    owner_start = source.index("      std::uint32_t lowseq;")
    owner_end = source.index("      udp::endpoint peer;", owner_start)
    account_start = source.index("      std::size_t frame_packets_accepted = 0;")
    account_end = source.index("\n      try {", account_start)
    stamp_start = source.index("        bool frame_is_dupe = false;")
    stamp_end = source.index("\n        // PyroWave record framing:", stamp_start)
    shard_start = source.index("            inspect->rtp.timestamp =", stamp_end)
    shard_end = source.index("\n", shard_start)
    result_start = source.index("              frame_packets_accepted +=", shard_end)
    result_end = source.index("              frame_send_batch_latency_logger", result_start)
    fixture = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>
#include "video_send.h"

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

  namespace endian {
    template<class T>
    T big(T value) {
      if constexpr (std::endian::native == std::endian::little) {
        auto *bytes = reinterpret_cast<unsigned char *>(&value);
        std::reverse(bytes, bytes + sizeof(value));
      }
      return value;
    }
  }
}

struct Session {
  struct {
    std::optional<unsigned> cipher;
    // SESSION_CLOCK
  } video;
  struct {
    std::atomic<std::size_t> packets_sent {0}, bytes_sent {0};
  } stats;
};

struct WireFrame {
  std::uint32_t timestamp = 0;
  bool repeated = false;
  Clock::time_point source_stamp {};
  bool complete = true;
  std::vector<std::uint32_t> emitted_timestamps;
};

struct video_send_failed_t {
  bool cancelled;
  unsigned block;
  std::size_t packets_accepted;
};

constexpr std::size_t shards_per_block = 3;
constexpr unsigned fec_blocks = 2;
constexpr std::size_t shards_per_frame = shards_per_block * fec_blocks;

WireFrame send_frame(Session *session, std::optional<Clock::time_point> source_stamp,
                      Clock::time_point repeated_stamp, std::size_t accept_limit = shards_per_frame) {
  struct Packet {
    std::optional<Clock::time_point> frame_timestamp;
  } data {source_stamp};
  auto *packet = &data;
  struct video_packet_enc_prefix_t { char bytes[32]; };
  const std::size_t blocksize = 1400;
  const auto video_epoch = stamp_at(0);
  auto ratecontrol_next_frame_start = repeated_stamp;
  const auto ratecontrol_frame_start = repeated_stamp;
  const auto frame_pacing_delay = [](std::size_t count) { return Clock::duration(count); };
  std::size_t ratecontrol_group_packets_sent = 0, ratecontrol_frame_packets_sent = 0;
  // ACCOUNT
  WireFrame wire;
  try {
    // STAMP
    wire.timestamp = timestamp;
    wire.repeated = frame_is_dupe;
    wire.source_stamp = *packet->frame_timestamp;

    // Model two FEC blocks and a platform that falls back to individual
    // datagrams. Execute the real shard assignment, send fallback and
    // acceptance/error accounting.
    struct Shard { struct { std::uint32_t timestamp = 0; } rtp; };
    for (unsigned blockIndex = 0; blockIndex < fec_blocks; ++blockIndex) {
      std::array<Shard, shards_per_block> shards;
      for (auto &shard: shards) {
        auto *inspect = &shard;
        // SHARD_STAMP
      }
      bool allow_fallback = true;
      const auto send_result = stream::send_video_batch(shards.size(), allow_fallback,
        [] { return false; },
        [&](std::size_t index) {
          if (wire.emitted_timestamps.size() == accept_limit) {
            return false;
          }
          wire.emitted_timestamps.push_back(util::endian::big(shards[index].rtp.timestamp));
          return true;
        },
        [] { return false; });
      // BATCH_RESULT
    }
  } catch (const video_send_failed_t &) {
    wire.complete = false;
  }
  return wire;
}

bool emitted_only(const WireFrame &frame, std::size_t count, std::uint32_t expected_stamp) {
  return frame.emitted_timestamps.size() == count &&
         std::all_of(frame.emitted_timestamps.begin(), frame.emitted_timestamps.end(),
           [expected_stamp](auto stamp) { return stamp == expected_stamp; });
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
    check(emitted_only(first, shards_per_frame, first.timestamp) &&
          emitted_only(repeat, shards_per_frame, repeat.timestamp) &&
          emitted_only(recovered, shards_per_frame, recovered.timestamp) &&
          emitted_only(next, shards_per_frame, next.timestamp),
          "all data and parity shards of each frame carry its one stamp across both blocks");
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
    Session session;
    auto discard = send_frame(&session, stamp_at(10'000), {}, 0);
    auto start = send_frame(&session, stamp_at(900), {});
    auto partial = send_frame(&session, stamp_at(2'000), {}, shards_per_block + 1);
    auto recovered = send_frame(&session, stamp_at(1'500), {});
    check(!discard.complete && discard.emitted_timestamps.empty() &&
          start.timestamp == 900 && !partial.complete &&
          emitted_only(partial, shards_per_block + 1, 2'000) &&
          recovered.timestamp == 2'001 &&
          session.stats.packets_sent == 2 * shards_per_frame + shards_per_block + 1,
          "a partially emitted frame advances the clock, a wholly unsent frame does not");
  }
  {
    std::optional<Session> first(std::in_place);
    Session second;
    const auto initial_address = reinterpret_cast<std::uintptr_t>(&*first);
    auto a1 = send_frame(&*first, stamp_at(900), {});
    auto b1 = send_frame(&second, stamp_at(100), {});
    auto a2 = send_frame(&*first, stamp_at(3'000), {});
    auto b2 = send_frame(&second, stamp_at(200), {});
    first.reset();
    first.emplace();
    auto reconnected = send_frame(&*first, stamp_at(50), {});
    auto b3 = send_frame(&second, stamp_at(150), {});
    check(initial_address == reinterpret_cast<std::uintptr_t>(&*first) &&
          a1.timestamp == 900 && b1.timestamp == 100 && a2.timestamp == 3'000 && b2.timestamp == 200 &&
          reconnected.timestamp == 50 && b3.timestamp == 201,
          "interleaved clocks are independent and reconnect resets the clock even at the same address");
  }
  {
    Session session;
    constexpr auto wrap = 1LL << 32;
    auto first = send_frame(&session, stamp_at(wrap - 2), {});
    auto wrapped = send_frame(&session, stamp_at(wrap), {});
    auto late = send_frame(&session, stamp_at(wrap - 1), {});
    auto next = send_frame(&session, stamp_at(wrap + 90), {});
    check(first.timestamp == 0xfffffffeu && wrapped.timestamp == 0 &&
          wrapped.timestamp - first.timestamp == 2 && late.timestamp - wrapped.timestamp == 1 &&
          emitted_only(wrapped, shards_per_frame, 0) && late.timestamp == 1 && next.timestamp == 90,
          "a natural 32-bit wrap advances through zero before repairing a late stamp");
  }
  return failures ? 1 : 0;
}
'''
    cpp_source = fixture
    for marker, start, end in (
        ("SESSION_CLOCK", owner_start, owner_end),
        ("ACCOUNT", account_start, account_end),
        ("STAMP", stamp_start, stamp_end),
        ("SHARD_STAMP", shard_start, shard_end),
        ("BATCH_RESULT", result_start, result_end),
    ):
        cpp_source = cpp_source.replace(f"// {marker}", source[start:end])
    with tempfile.TemporaryDirectory(prefix="wgc-rtp-") as directory:
        scratch = Path(directory)
        cpp = scratch / "replay.cpp"
        executable = scratch / "replay"
        cpp.write_text(cpp_source)
        (scratch / "video_send.h").write_text(read_source("src/video_send.h"))
        subprocess.run(
            [args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(executable)],
            check=True,
        )
        return subprocess.run([str(executable)], check=False, timeout=5).returncode


if __name__ == "__main__":
    raise SystemExit(main())
