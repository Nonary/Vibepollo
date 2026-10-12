#pragma once

#include <cstddef>

namespace stream {
  struct video_batch_send_result_t {
    bool sent = false;
    bool cancelled = false;
    bool used_fallback = false;
    std::size_t packets_accepted = 0;
  };

  // A rejected batch may use individual sends only when the platform says
  // batching is unsupported. Stop at the first failed packet: continuing would
  // silently punch holes in a frame and falsely count it as delivered.
  template<class SendBatch, class SendPacket, class Cancelled>
  video_batch_send_result_t send_video_batch(std::size_t packet_count, bool &allow_fallback,
                                           SendBatch &&send_batch, SendPacket &&send_packet, Cancelled &&cancelled) {
    video_batch_send_result_t result;
    if (cancelled()) {
      result.cancelled = true;
      return result;
    }
    if (send_batch()) {
      result.sent = true;
      result.packets_accepted = packet_count;
      return result;
    }
    if (cancelled()) {
      result.cancelled = true;
      return result;
    }
    if (!allow_fallback) {
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
