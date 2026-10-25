/**
 * @file src/video_adaptive_pacing.h
 * @brief Experimental loss-responsive packet spacing for ordinary H.264/HEVC/AV1 video.
 *
 * Start with room for the stream's wire demand, then cautiously probe the
 * route's ordinary ceiling. Validated video loss can reduce bursts, but never
 * below the current wire demand plus headroom. This is not a measurement of path
 * capacity: control pings establish activity, not successful video delivery.
 * Silence alone never authorizes faster probing. Encoder bitrate and FPS are
 * never changed; only the spacing of packets on the wire is.
 *
 * A port of Rubylight's `adaptive_pacing.rs`. The controller is not
 * thread-safe: callers serialize access.
 */
#pragma once

#include "video_fec_status.h"
#include "video_pacing_policy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace stream::pacing {
  /// Counters the control loop samples; the send loop only increments them.
  struct traffic_t {
    std::uint64_t frames {};
    std::uint64_t bytes {};
    std::uint64_t packets {};
    std::uint64_t send_losses {};
  };

  /// What changed since the previous take_events(), for logging only.
  struct adaptive_pacing_events_t {
    bool started {};  ///< The first rate was chosen.
    bool probe_started {};
    bool probe_accepted {};  ///< A tentative increase survived its feedback interval.
    bool probe_rejected {};  ///< Loss restored the rate that preceded the probe.
    bool probe_retracted {};  ///< A changed wire budget withdrew an unconfirmed probe.
    bool cut {};  ///< Loss outside a probe halved the rate.
    bool cut_by_failed_frames {};  ///< Else: recovered frames missed at least 2% of packets.
    bool ceiling_confirmed {};  ///< Discovery finished at the ceiling.

    bool any() const {
      return started || probe_started || probe_accepted || probe_rejected || probe_retracted || cut || ceiling_confirmed;
    }
  };

  struct adaptive_pacing_snapshot_t {
    bool automatic {};
    bool startup {};
    bool probing {};
    bool limited {};
    std::uint64_t current_kbps {};
    std::uint64_t ceiling_kbps {};
    std::uint64_t wire_floor_kbps {};
    std::uint64_t recovery_hold_seconds {};

    bool operator==(const adaptive_pacing_snapshot_t &) const = default;
  };

  struct adaptive_pacing_access_t;

  class adaptive_pacing_t {
  public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;
    using duration = std::chrono::nanoseconds;

    explicit adaptive_pacing_t(bool enabled = false):
        enabled_ {enabled},
        startup_ {enabled} {}

    /// Refresh on each frame: FEC, FPS, bitrate and the route can change.
    /// The physical/configured ceiling wins when the requested stream cannot
    /// fit. The caller reports that shortfall; this controller cannot fix it.
    std::uint64_t rate(std::uint64_t ceiling, std::uint64_t needed, std::uint32_t encoder_kbps) {
      const auto previous_rate = current();
      const auto previous_ceiling = ceiling_;
      const auto previous_floor = floor_;
      ceiling_ = std::max<std::uint64_t>(ceiling, 1);
      floor_ = wire_floor(needed, ceiling_);
      if (!enabled_) {
        return ceiling;
      }
      if (!initialized_) {
        initialized_ = true;
        limit_ = std::clamp(saturating_mul(std::uint64_t(encoder_kbps), 2000), floor_, ceiling_);
        events_.started = true;
      } else if (ceiling_ != previous_ceiling || floor_ != previous_floor) {
        if (ceiling_ != previous_ceiling) {
          // Route activity and RTT from before a changed ceiling do not
          // validate probing the replacement route.
          last_ping_.reset();
          minimum_rtt_.reset();
        }
        if (ceiling_ > previous_ceiling && !limit_) {
          // A newly faster host route says nothing about downstream capacity.
          // Keep the previously confirmed rate and probe the extra headroom
          // using fresh evidence, without reseeding.
          limit_ = previous_rate;
          startup_ = !any_loss_;
        }
        // A tentative faster rate has not earned promotion. Retain its
        // preceding rate, adjusted for the new demand, and gather fresh
        // evidence instead of confirming across a changed wire budget.
        if (probe_) {
          limit_ = probe_->stable;
          probe_.reset();
          events_.probe_retracted = true;
          if (any_loss_) {
            // The same report may raise FEC before the control loop samples
            // it. A changed wire floor must not erase this failed probe or
            // restart fast startup probing.
            startup_ = false;
            recovery_hold_ = std::min(recovery_hold_ * 2, duration {std::chrono::seconds {60}});
          }
        }
        clean_since_.reset();
        last_sample_.reset();
        awaiting_fence_ = true;
      }
      if (limit_) {
        const auto next = std::clamp(*limit_, floor_, ceiling_);
        if (next != *limit_) {
          probe_.reset();
          clean_since_.reset();
        }
        limit_ = next;
      }
      return current();
    }

    /// Standard periodic pings (type 0x0200, payload starting 04 00 00 00)
    /// establish that the client is alive and sending control traffic.
    void on_ping(time_point now, std::string_view payload) {
      if (enabled_ && payload.size() == 8 && payload[0] == 4 && payload[1] == 0 && payload[2] == 0 && payload[3] == 0) {
        last_ping_ = now;
      }
    }

    /// Call only after protocol validation and block deduplication. Count
    /// distinct lost frames, not their (up to four) block reports. Delayed
    /// reports must not penalize the replacement rate for an earlier burst.
    void on_block(std::uint32_t frame, std::uint32_t last_sent, duration period, fec_block_t block) {
      const auto period_ns = std::max<std::int64_t>(period.count(), 1);
      const auto recent_frames = std::uint32_t(std::clamp<std::int64_t>(2'000'000'000 / period_ns, 1, 1024));
      if (!enabled_ || awaiting_fence_ || std::uint32_t(last_sent - frame) >= recent_frames || (after_frame_ && !newer(frame, *after_frame_)) || block.kind == fec_block_t::kind_e::clean || (block.kind == fec_block_t::kind_e::recovered && block.lost == 0)) {
        return;
      }
      any_loss_ = true;
      clean_since_.reset();
      if (block.kind == fec_block_t::kind_e::recovered) {
        lost_packets_ = saturating_add(lost_packets_, block.lost);
      }
      std::erase_if(reported_frames_, [&](const reported_frame_t &seen) {
        return std::uint32_t(last_sent - seen.frame) >= recent_frames;
      });
      const bool failed = block.kind == fec_block_t::kind_e::unrecoverable;
      const auto seen = std::find_if(reported_frames_.begin(), reported_frames_.end(), [&](const reported_frame_t &entry) {
        return entry.frame == frame;
      });
      if (seen != reported_frames_.end()) {
        if (failed && !seen->failed) {
          seen->failed = true;
          failed_frames_ = saturating_add(failed_frames_, 1u);
        }
      } else {
        reported_frames_.push_back({frame, failed});
        lost_frames_ = saturating_add(lost_frames_, 1u);
        failed_frames_ = saturating_add(failed_frames_, failed ? 1u : 0u);
      }
    }

    /// The control loop supplies counters and ENet RTT, never the send loop.
    /// Outside tentative probes, cuts require two distinct failed frames, or
    /// multiple recovered frames missing at least 2% of all sent packets.
    /// Sparse, fully repaired losses therefore don't ratchet every connection
    /// to the floor; any loss can reject an unconfirmed increase.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> tick(
      time_point now,
      const traffic_t &traffic,
      std::optional<std::uint32_t> last_sent,
      duration period,
      std::uint32_t encoder_kbps,
      duration rtt
    ) {
      using namespace std::chrono_literals;
      if (!enabled_ || ceiling_ == 0) {
        return std::nullopt;
      }
      if (awaiting_fence_) {
        awaiting_fence_ = false;
        after_frame_ = last_sent;
        lost_frames_ = 0;
        failed_frames_ = 0;
        lost_packets_ = 0;
        any_loss_ = false;
      }
      if (!last_sample_) {
        last_sample_ = sample_t {now, traffic};
        return std::nullopt;
      }
      const auto previous = last_sample_->at;
      const auto before = last_sample_->traffic;
      const auto elapsed = since(now, previous);
      // A validated loss during a tentative increase need not wait for a full
      // sampling window before restoring the preceding rate.
      const bool probe_loss = probe_.has_value() && any_loss_;
      if (elapsed < 500ms && !probe_loss) {
        return std::nullopt;
      }
      last_sample_ = sample_t {now, traffic};
      const auto frames = saturating_sub(traffic.frames, before.frames);
      const auto packets = saturating_sub(traffic.packets, before.packets);
      const bool reported_loss = std::exchange(any_loss_, false);
      const bool loss = reported_loss || traffic.send_losses > before.send_losses;
      const bool failed_cut = failed_frames_ >= 2;
      const bool substantial = failed_cut ||
                               (lost_frames_ >= 2 && packets > 0 && lost_packets_ >= (packets + 49) / 50);
      lost_frames_ = 0;
      failed_frames_ = 0;
      lost_packets_ = 0;
      const auto old = current();
      const bool recent = elapsed <= 2s;
      const bool ping = last_ping_ && since(now, *last_ping_) < 2s;
      if (ping && rtt > duration::zero()) {
        minimum_rtt_ = minimum_rtt_ ? std::min(*minimum_rtt_, rtt) : rtt;
      }
      const bool queued = minimum_rtt_ && rtt > *minimum_rtt_ + std::max<duration>(*minimum_rtt_ / 2, 8ms);
      if ((substantial || (probe_ && loss)) && recent && (frames > 0 || probe_loss)) {
        std::uint64_t next;
        if (probe_) {
          // A failed probe restores the last working rate, and increases the
          // quiet period before another attempt (up to a minute).
          recovery_hold_ = std::min(recovery_hold_ * 2, duration {60s});
          next = probe_->stable;
          probe_.reset();
          events_.probe_rejected = true;
        } else {
          next = old / 2;
          events_.cut = true;
          events_.cut_by_failed_frames = failed_cut;
        }
        limit_ = std::clamp(next, floor_, ceiling_);
        startup_ = false;
        after_frame_ = last_sent;
        clean_since_.reset();
      } else {
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const double period_seconds = std::chrono::duration<double>(period).count();
        const bool busy = recent &&
                          double(frames) >= seconds / std::max(period_seconds, 0.001) * 0.6 &&
                          double(saturating_sub(traffic.bytes, before.bytes)) * 8. >=
                            double(encoder_kbps) * 1000. * seconds * 0.6;
        if (loss || !ping || !busy || rtt == duration::zero() || queued) {
          clean_since_.reset();
        } else if (limit_) {
          // The counters describe the interval just completed. Startup can
          // use that first active interval; recovery retains its longer quiet
          // hold after an established loss.
          if (!clean_since_) {
            clean_since_ = startup_ ? previous : now;
          }
          const auto clean = *clean_since_;
          if (probe_) {
            const auto settle = startup_ ? std::max(probe_->settle, startup_feedback_wait(rtt)) : probe_->settle;
            if (since(now, std::max(probe_->started, clean)) >= settle) {
              probe_.reset();
              events_.probe_accepted = true;
              if (old == ceiling_) {
                limit_.reset();
                startup_ = false;
                events_.ceiling_confirmed = true;
              }
            }
          }
          const auto hold = startup_ ? startup_feedback_wait(rtt) : recovery_hold_;
          if (!probe_ && since(now, clean) >= hold) {
            if (old < ceiling_) {
              // Smaller startup probes leave ordinary FEC more room to repair
              // losses while feedback returns. This is headroom, not a
              // guarantee of loss-free probing.
              const auto step = startup_ ? std::max<std::uint64_t>(old / 8, 1) : std::max<std::uint64_t>(old / 20, 1'000'000);
              limit_ = saturating_add(old, step);
              limit_ = std::min(*limit_, ceiling_);
              probe_ = probe_t {old, now, startup_ ? hold : duration {2s}};
              after_frame_ = last_sent;
              events_.probe_started = true;
            } else if (startup_) {
              // The initial wire-demand floor can already equal a low
              // physical ceiling. Confirm it without probing.
              limit_.reset();
              startup_ = false;
              events_.ceiling_confirmed = true;
            }
          }
        }
      }
      const auto next = current();
      if (next != old) {
        return std::pair {old, next};
      }
      return std::nullopt;
    }

    /// Events since the last call, then cleared. Use under the caller's lock.
    adaptive_pacing_events_t take_events() {
      return std::exchange(events_, adaptive_pacing_events_t {});
    }

    adaptive_pacing_snapshot_t snapshot() const {
      return {
        enabled_,
        startup_,
        probe_.has_value(),
        enabled_ && current() < ceiling_,
        current() / 1000,
        ceiling_ / 1000,
        floor_ / 1000,
        std::uint64_t(std::chrono::duration_cast<std::chrono::seconds>(recovery_hold_).count()),
      };
    }

    std::uint64_t current() const {
      return limit_ ? *limit_ : ceiling_;
    }

  private:
    friend struct adaptive_pacing_access_t;

    struct probe_t {
      std::uint64_t stable;
      time_point started;
      duration settle;
    };

    struct sample_t {
      time_point at;
      traffic_t traffic;
    };

    struct reported_frame_t {
      std::uint32_t frame;
      bool failed;
    };

    static std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) {
      return a > std::numeric_limits<std::uint64_t>::max() - b ? std::numeric_limits<std::uint64_t>::max() : a + b;
    }

    static std::uint32_t saturating_add(std::uint32_t a, std::uint32_t b) {
      return a > std::numeric_limits<std::uint32_t>::max() - b ? std::numeric_limits<std::uint32_t>::max() : a + b;
    }

    static std::uint64_t saturating_sub(std::uint64_t a, std::uint64_t b) {
      return a > b ? a - b : 0;
    }

    static std::uint64_t saturating_mul(std::uint64_t a, std::uint64_t b) {
      return b != 0 && a > std::numeric_limits<std::uint64_t>::max() / b ? std::numeric_limits<std::uint64_t>::max() : a * b;
    }

    /// 1.25x the stream's wire demand, at least 1 Mbps, never above the ceiling.
    static std::uint64_t wire_floor(std::uint64_t needed, std::uint64_t ceiling) {
      return std::min(std::max<std::uint64_t>(saturating_add(needed, needed / 4), 1'000'000), ceiling);
    }

    static duration since(time_point now, time_point then) {
      return now > then ? std::chrono::duration_cast<duration>(now - then) : duration::zero();
    }

    /// A probe's effect returns after about one round trip plus margin.
    static duration startup_feedback_wait(duration rtt) {
      using namespace std::chrono_literals;
      return std::max<duration>(500ms, rtt * 2 + 100ms);
    }

    /// Wrapping comparison of frame counters.
    static bool newer(std::uint32_t frame, std::uint32_t previous) {
      const std::uint32_t distance = frame - previous;
      return distance > 0 && distance < (1u << 31);
    }

    bool enabled_;
    bool initialized_ {false};
    bool startup_;
    std::uint64_t ceiling_ {};
    std::uint64_t floor_ {};
    std::optional<std::uint64_t> limit_;
    std::optional<sample_t> last_sample_;
    std::optional<time_point> last_ping_;
    std::optional<duration> minimum_rtt_;
    /// Feedback at/before this frame describes the previous rate.
    std::optional<std::uint32_t> after_frame_;
    /// Recent frames may report their blocks out of order. Retain whether a
    /// failed block was already counted, independently of a recovered block.
    std::vector<reported_frame_t> reported_frames_;
    std::uint32_t lost_frames_ {};
    std::uint32_t failed_frames_ {};
    std::uint64_t lost_packets_ {};
    bool any_loss_ {false};
    std::optional<time_point> clean_since_;
    /// A rate increase is tentative until its feedback interval has elapsed.
    std::optional<probe_t> probe_;
    /// A changed wire budget invalidates feedback for the old rate. The control
    /// loop supplies the new sent-frame fence before sampling again.
    bool awaiting_fence_ {false};
    duration recovery_hold_ {std::chrono::seconds {8}};
    adaptive_pacing_events_t events_;
  };

  /// Wire rate of an average frame at the stream's bitrate, as packetized and
  /// FEC-expanded by the ordinary video send path.
  ///
  /// `blocksize` is a whole datagram slot including headers, `payload_blocksize`
  /// the video bytes in it, and `wire_bytes` one packet's link-layer cost.
  /// Frames are split into FEC blocks like the sender does; the share per block
  /// is an even split rounded up, which can differ from the sender's alignment
  /// by a packet. That is headroom accuracy, not a promise of exact demand.
  inline std::uint64_t nominal_wire_demand_bps(
    int bitrate_kbps,
    int framerate,
    std::size_t frame_header_bytes,
    std::size_t payload_blocksize,
    std::size_t wire_bytes,
    int fec_percentage,
    std::size_t min_parity_packets
  ) {
    if (bitrate_kbps <= 0 || framerate <= 0 || payload_blocksize == 0) {
      return 0;
    }
    const std::uint64_t frame_bytes = (std::uint64_t(bitrate_kbps) * 125 + std::uint64_t(framerate) - 1) / std::uint64_t(framerate);
    const std::size_t total_shards = std::size_t((frame_bytes + frame_header_bytes + payload_blocksize - 1) / payload_blocksize);
    const auto percentage = std::size_t(std::max(fec_percentage, 0));
    // A Reed-Solomon block holds at most 255 shards (data plus parity) and a
    // frame at most 4 blocks; beyond that the sender turns FEC off.
    constexpr std::size_t max_blocks = 4;
    const std::size_t max_data_per_block = std::max<std::size_t>(255 * 100 / (100 + percentage), 1);
    std::size_t blocks = std::max<std::size_t>((total_shards + max_data_per_block - 1) / max_data_per_block, 1);
    const bool fec_off = blocks > max_blocks;
    blocks = std::min(blocks, max_blocks);
    const std::size_t per_block = (total_shards + blocks - 1) / blocks;
    std::size_t packets = 0;
    for (std::size_t remaining = total_shards, block = 0; block < blocks && remaining > 0; ++block) {
      const auto data = block + 1 == blocks ? remaining : std::min(per_block, remaining);
      packets += fec_off ? data : fec_expanded_packets(data, int(percentage), min_parity_packets);
      remaining -= data;
    }
    const auto bits_per_second = double(packets) * double(wire_bytes) * 8. * double(framerate);
    return std::uint64_t(std::ceil(bits_per_second));
  }

  /// Packets per millisecond of pacing time at `rate_bps`, always at least one.
  inline std::size_t packets_per_ms_at_rate(std::uint64_t rate_bps, std::size_t wire_bytes) {
    if (wire_bytes == 0) {
      return 1;
    }
    return std::size_t(std::max<std::uint64_t>(1, rate_bps / 8 / 1000 / wire_bytes));
  }

  /// How the sender spaces one frame's packets: up to `packets_per_ms` packets
  /// between pacing waits, and a deadline for each packet sent so far.
  ///
  /// A fixed pace charges whole packets per millisecond. A rate pace charges
  /// each packet its exact wire time at `rate_bps`, so rounding the burst
  /// quantum down to whole packets cannot slow the sustained rate below what
  /// the controller chose (Rubylight's pacer charges exact wire time too).
  struct packet_pace_t {
    std::size_t packets_per_ms {1};
    std::uint64_t rate_bps {};  ///< Zero for a fixed pace.
    std::size_t wire_bytes {};

    static packet_pace_t fixed(std::size_t packets_per_ms, std::size_t wire_bytes = 0) {
      return {std::max<std::size_t>(packets_per_ms, 1), 0, wire_bytes};
    }

    static packet_pace_t at_rate(std::uint64_t rate_bps, std::size_t wire_bytes) {
      if (rate_bps == 0 || wire_bytes == 0) {
        return fixed(packets_per_ms_at_rate(rate_bps, wire_bytes), wire_bytes);
      }
      return {packets_per_ms_at_rate(rate_bps, wire_bytes), rate_bps, wire_bytes};
    }

    /// Pacing time owed once `packets` packets of the frame were sent.
    std::chrono::nanoseconds offset(std::size_t packets) const {
      if (rate_bps == 0) {
        return std::chrono::nanoseconds {std::chrono::milliseconds {1}} * packets / packets_per_ms;
      }
      // bits * 1e9 / rate, split so large frames cannot overflow.
      const auto bits = std::uint64_t(packets) * wire_bytes * 8;
      const auto whole = bits / rate_bps;
      const auto fraction = double(bits % rate_bps) * 1e9 / double(rate_bps);
      return std::chrono::nanoseconds {std::int64_t(whole * 1'000'000'000 + std::uint64_t(fraction))};
    }

    /// The sustained rate these deadlines enforce.
    std::uint64_t applied_bps() const {
      return rate_bps ? rate_bps : std::uint64_t(packets_per_ms) * wire_bytes * 8000;
    }
  };
}  // namespace stream::pacing
