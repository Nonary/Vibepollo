/**
 * @file src/video_pacing_policy.h
 * @brief Shared packet pacing math for video transports.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace stream::pacing {
  /// Data plus parity packets for one FEC block, matching the video send path.
  inline std::size_t fec_expanded_packets(std::size_t data_packets, int fec_percentage, std::size_t min_parity_packets) {
    auto parity_packets = (data_packets * std::size_t(std::max(fec_percentage, 0)) + 99) / 100;
    if (fec_percentage > 0 && parity_packets < min_parity_packets) {
      parity_packets = min_parity_packets;
    }
    return data_packets + parity_packets;
  }

  /// Packets in one millisecond of pacing time, based on stream and frame demand.
  /// `frame_packets` includes any transport parity packets for the frame.
  inline std::size_t packets_per_ms(
    std::uint64_t link_bps,
    int bitrate_kbps,
    std::size_t payload_bytes,
    std::size_t wire_bytes,
    std::size_t frame_packets,
    int framerate,
    std::uint64_t calibrated_pace_bps
  ) {
    if (payload_bytes == 0 || wire_bytes == 0) {
      return 1;
    }
    const auto frame_allowance = (frame_packets * std::uint64_t(std::max(framerate, 0)) + 999) / 1000;
    const auto bitrate_allowance = (std::uint64_t(std::max(bitrate_kbps, 0)) + 8 * payload_bytes - 1) / (8 * payload_bytes);
    const auto demand = std::max(frame_allowance, bitrate_allowance);
    const auto calibrated_packets = calibrated_pace_bps / 8 / 1000 / wire_bytes;
    if (link_bps == 0) {
      return std::max<std::size_t>(1, std::max(calibrated_packets, demand));
    }
    const auto link_packets = link_bps / 8 / 1000 / wire_bytes;
    const auto target_packets = calibrated_pace_bps ? calibrated_packets : link_bps * 80 / 100 / 8 / 1000 / wire_bytes;
    return std::max<std::size_t>(1, std::min(link_packets, std::max(target_packets, demand)));
  }

  /// Bound a send batch by both its configured maximum and one pacing quantum.
  inline std::size_t max_batch_bytes(std::size_t configured_max, std::size_t packets_per_quantum, std::size_t datagram_bytes) {
    const auto quantum_bytes = datagram_bytes && packets_per_quantum > std::numeric_limits<std::size_t>::max() / datagram_bytes ?
                                 std::numeric_limits<std::size_t>::max() :
                                 packets_per_quantum * datagram_bytes;
    return std::min(configured_max, quantum_bytes);
  }
}  // namespace stream::pacing
