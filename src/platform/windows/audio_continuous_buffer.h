/**
 * @file src/platform/windows/audio_continuous_buffer.h
 * @brief Bounded loopback audio on a device-frame timeline with paced silence.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace platf::audio_detail {
  // QPC timestamps pace output, but are not sample-accurate packet positions.
  // Use device-frame continuity between clock anchors so timestamp jitter
  // cannot punch holes in audio or overwrite the preceding packet's tail.
  class continuous_buffer_t {
  public:
    static constexpr std::int64_t ticks_per_second = 10000000;

    continuous_buffer_t(std::uint32_t rate, std::uint32_t channels, std::uint32_t packet_frames, std::uint32_t device_frames, std::int64_t origin):
        rate_ {rate},
        channels_ {channels},
        packet_frames_ {packet_frames},
        resync_frames_ {std::max(device_frames, packet_frames)},
        margin_frames_ {2 * device_frames + packet_frames},
        max_backlog_ {std::max(2 * device_frames, 4 * packet_frames)},
        capacity_ {std::max(rate, 4 * (device_frames + packet_frames))},
        origin_ {origin},
        samples_(std::size_t(capacity_) * channels, 0.0f) {}

    // Drop old time after a scheduler stall rather than emit seconds of stale
    // packets in a burst. Retiring frames always clears their circular slots.
    void advance(std::int64_t now) {
      auto frontier = frame_at(std::max(now, origin_) - origin_) - margin_frames_;
      if (frontier - cursor_ > max_backlog_) {
        retire(std::max(cursor_, frontier - packet_frames_));
      }
    }

    bool put(std::int64_t timestamp, std::uint64_t device_position, std::uint32_t frames, const float *data, bool silent, bool discontinuity = false) {
      // Reject untrustworthy timestamps before arithmetic or allocation. The
      // caller discards unlocated audio while retaining the output clock.
      if (timestamp < 0 || timestamp < origin_ - ticks_per_second ||
          device_position > std::numeric_limits<std::uint64_t>::max() - frames) {
        return false;
      }
      auto clock_first = frame_at(timestamp - origin_);
      auto first = clock_first;
      if (anchored_ && !discontinuity && device_position == next_device_position_) {
        // Ignore sub-period QPC jitter. A larger offset starts a new segment:
        // loopback can stop producing packets during silence, and the device
        // clock can gradually drift relative to QPC even within one session.
        auto offset = clock_first - next_frame_;
        if (offset >= -resync_frames_ && offset <= resync_frames_) {
          first = next_frame_;
        }
      }
      auto end = first + frames;
      if (first > cursor_ + capacity_ || end > cursor_ + capacity_) {
        return false;
      }
      anchored_ = true;
      next_device_position_ = device_position + frames;
      next_frame_ = end;
      if (end <= cursor_) {
        return true;
      }
      auto skip = std::max<std::int64_t>(0, cursor_ - first);
      for (auto frame = first + skip; frame < end; ++frame) {
        auto destination = std::size_t(frame % capacity_) * channels_;
        auto source = std::size_t(frame - first) * channels_;
        for (std::uint32_t channel = 0; channel < channels_; ++channel) {
          samples_[destination + channel] = silent ? 0.0f : data[source + channel];
        }
      }
      return true;
    }

    // Erase queued PCM without disturbing the device timeline or output clock.
    void silence() {
      std::fill(samples_.begin(), samples_.end(), 0.0f);
    }

    bool ready(std::int64_t now) const {
      return frame_at(std::max(now, origin_) - origin_) >= cursor_ + packet_frames_ + margin_frames_;
    }

    std::uint32_t wait_ms(std::int64_t now) const {
      auto remaining = cursor_ + packet_frames_ + margin_frames_ - frame_at(std::max(now, origin_) - origin_);
      return remaining <= 0 ? 0 : std::uint32_t((remaining * 1000 + rate_ - 1) / rate_);
    }

    bool take(std::int64_t now, std::span<float> output) {
      if (output.size() != std::size_t(packet_frames_) * channels_ || !ready(now)) {
        return false;
      }
      for (std::uint32_t frame = 0; frame < packet_frames_; ++frame) {
        auto source = std::size_t((cursor_ + frame) % capacity_) * channels_;
        std::copy_n(samples_.begin() + source, channels_, output.begin() + std::size_t(frame) * channels_);
      }
      retire(cursor_ + packet_frames_);
      return true;
    }

  private:
    std::int64_t frame_at(std::int64_t ticks) const {
      // Split the integer conversion so uptime cannot overflow ticks * rate.
      // Round to the nearest frame, including negative pre-start timestamps.
      auto seconds = ticks / ticks_per_second;
      auto remainder = ticks % ticks_per_second;
      auto scaled = remainder * rate_;
      return seconds * rate_ + (scaled + (scaled < 0 ? -ticks_per_second / 2 : ticks_per_second / 2)) / ticks_per_second;
    }

    void retire(std::int64_t next) {
      if (next - cursor_ >= capacity_) {
        std::fill(samples_.begin(), samples_.end(), 0.0f);
      } else {
        for (auto frame = cursor_; frame < next; ++frame) {
          auto start = std::size_t(frame % capacity_) * channels_;
          std::fill_n(samples_.begin() + start, channels_, 0.0f);
        }
      }
      cursor_ = next;
    }

    std::int64_t rate_, channels_, packet_frames_, resync_frames_, margin_frames_, max_backlog_, capacity_;
    std::int64_t origin_, cursor_ = 0;
    std::int64_t next_frame_ = 0;
    std::uint64_t next_device_position_ = 0;
    bool anchored_ = false;
    std::vector<float> samples_;
  };
}  // namespace platf::audio_detail
