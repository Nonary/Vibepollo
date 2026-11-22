#pragma once

#include <chrono>
#include <cstddef>

namespace stream {
  // Windows may refuse a segmented message even when individual datagrams fit.
  // Probe with the next complete frame, never by replaying part of a failed one.
  class video_send_fallback_t {
  public:
    using clock = std::chrono::steady_clock;

    bool send_individually(clock::time_point now) const {
      return blocked_frames == blocked_frames_before_probe || now < individual_until;
    }

    // Report entry only after a whole frame has been accepted as individual
    // datagrams. A working fallback has a fixed lifetime, so batching is tried again.
    bool frame_sent(bool individual, clock::time_point now) {
      const bool recovered = individual && blocked_frames == blocked_frames_before_probe;
      blocked_frames = 0;
      if (recovered) {
        individual_until = now + individual_period;
      }
      return recovered;
    }

    void frame_failed(bool individual, bool segmented_would_block, clock::time_point now) {
      if (individual) {
        individual_until = {};
        probe_after = now + probe_cooldown;
      }
      if (segmented_would_block && now >= probe_after) {
        ++blocked_frames;
      } else {
        blocked_frames = 0;
      }
    }

  private:
    static constexpr unsigned blocked_frames_before_probe = 2;
    static constexpr auto individual_period = std::chrono::seconds {5};
    static constexpr auto probe_cooldown = std::chrono::seconds {1};

    unsigned blocked_frames = 0;
    clock::time_point individual_until {};
    clock::time_point probe_after {};
  };

  struct video_batch_send_result_t {
    bool sent = false;
    bool cancelled = false;
    bool used_fallback = false;
    std::size_t packets_accepted = 0;
  };

  // The sender can select individual sends before the frame starts. Otherwise
  // fall back only when the platform says batching is unsupported. Stop at the
  // first failed packet and report only the prefix accepted by the socket.
  template<class SendBatch, class SendPacket, class Cancelled>
  video_batch_send_result_t send_video_batch(std::size_t packet_count, bool &allow_fallback,
                                           SendBatch &&send_batch, SendPacket &&send_packet, Cancelled &&cancelled,
                                           bool send_individually = false) {
    video_batch_send_result_t result;
    if (cancelled()) {
      result.cancelled = true;
      return result;
    }
    if (!send_individually && send_batch()) {
      result.sent = true;
      result.packets_accepted = packet_count;
      return result;
    }
    if (cancelled()) {
      result.cancelled = true;
      return result;
    }
    if (!send_individually && !allow_fallback) {
      return result;
    }
    result.used_fallback = true;
    for (std::size_t i = 0; i < packet_count; ++i) {
      if (cancelled()) {
        result.cancelled = true;
        return result;
      }
      if (!send_packet(i)) {
        result.cancelled = cancelled();
        return result;
      }
      ++result.packets_accepted;
    }
    result.sent = true;
    return result;
  }
}  // namespace stream
