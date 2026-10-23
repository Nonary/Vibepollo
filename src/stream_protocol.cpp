#include "stream_protocol.h"

#include <algorithm>
#include <limits>

namespace stream {
  std::size_t video_pacing_batch_packets(int ceiling_kbps, std::size_t datagram_bytes) {
    if (ceiling_kbps <= 0 || datagram_bytes == 0) {
      return 0;
    }
    return std::max<std::uint64_t>(1, std::uint64_t(ceiling_kbps) / 8 / datagram_bytes);
  }

  std::uint64_t video_pacing_delay_ns(std::uint64_t packets, std::size_t datagram_bytes, int ceiling_kbps) {
    if (ceiling_kbps == 0 || packets == 0) {
      return 0;
    }
    constexpr auto limit = std::uint64_t(std::numeric_limits<std::int64_t>::max());
    if (ceiling_kbps < 0 || datagram_bytes == 0 || datagram_bytes > 65535 + 16 + 32) {
      return limit;
    }
    const auto packet_bit_ns = std::uint64_t(datagram_bytes) * 8 * 1'000'000;
    if (packets > limit / packet_bit_ns) {
      return limit;
    }
    const auto numerator = packets * packet_bit_ns;
    return (numerator + std::uint64_t(ceiling_kbps) - 1) / std::uint64_t(ceiling_kbps);
  }

  video_pacing_reservation_t video_pacing_reservation(int ceiling_kbps, const video_pacing_demand_t &demand) {
    if (ceiling_kbps == 0) {
      return {true, 0, 0};
    }
    // Match the transport's packet-size, RTSP cadence and FEC domains. Besides
    // rejecting invalid input, these bounds keep every intermediate in u64.
    if (ceiling_kbps < 0 || demand.encoder_kbps <= 0 ||
        demand.frames_per_second <= 0 || demand.frames_per_second > 4000 ||
        demand.payload_bytes == 0 || demand.datagram_bytes <= demand.payload_bytes ||
        demand.datagram_bytes > 65535 + 16 + 32 ||
        demand.frame_header_bytes > demand.datagram_bytes ||
        demand.fec_percentage < 0 || demand.fec_percentage > 255 ||
        demand.min_parity_shards < 0 || demand.min_parity_shards > 255) {
      return {};
    }
    const auto ceil_div = [](std::uint64_t value, std::uint64_t divisor) {
      return (value + divisor - 1) / divisor;
    };
    const auto fps = std::uint64_t(demand.frames_per_second);
    const auto image_bytes = std::uint64_t(demand.encoder_kbps) * 125;
    // sum(ceil(frame_bytes / payload)) <= ceil(sum / payload) + frames - 1.
    const auto data = ceil_div(image_bytes + fps * demand.frame_header_bytes, demand.payload_bytes) + fps - 1;
    std::uint64_t parity = 0;
    if (demand.fec_percentage != 0) {
      const auto max_data_per_block = 255 * 100 / (100 + demand.fec_percentage);
      const auto blocks = demand.independent_fec_blocks ? 4 * fps :
                          std::min(4 * fps, ceil_div(data, max_data_per_block) + fps - 1);
      // Each protected block has at least one parity shard. Bound its ceil
      // and any client minimum separately, including uneven frame sizes.
      parity = ceil_div(data * demand.fec_percentage, 100) +
               blocks * std::max(demand.min_parity_shards, 1) - 1;
    }
    const auto packets = data + parity;
    const auto send_ns = video_pacing_delay_ns(packets, demand.datagram_bytes, ceiling_kbps);
    return {send_ns <= 1'000'000'000, packets, send_ns};
  }

  bool reserve_video_pacing_capacity(std::uint64_t &used_ns_per_second, const video_pacing_reservation_t &reservation) {
    if (!reservation.admissible || used_ns_per_second > 1'000'000'000 ||
        reservation.send_ns_per_second > 1'000'000'000 - used_ns_per_second) {
      return false;
    }
    used_ns_per_second += reservation.send_ns_per_second;
    return true;
  }

  std::size_t video_send_batch_size(std::size_t block_size, std::size_t prefix_size, std::size_t max_batch_bytes) {
    const auto byte_limit = std::min<std::size_t>(64 * 1024, max_batch_bytes);
    // Even an individual packet larger than the configured batch budget must
    // still be sent. Check before adding so invalid sizes cannot overflow.
    if (block_size > byte_limit || prefix_size > byte_limit - block_size || block_size + prefix_size == 0) {
      return 1;
    }
    return std::clamp<std::size_t>(byte_limit / (block_size + prefix_size), 1, 64);
  }

  std::optional<control_packet_view_t> decode_control_packet(std::string_view packet_bytes) {
    if (packet_bytes.size() < sizeof(std::uint16_t)) {
      return std::nullopt;
    }
    const auto lo = static_cast<std::uint8_t>(packet_bytes[0]);
    const auto hi = static_cast<std::uint8_t>(packet_bytes[1]);
    const auto type = static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(hi) << 8));
    return control_packet_view_t {type, packet_bytes.substr(sizeof(type))};
  }

  std::vector<std::uint8_t> concat_and_insert(
    std::uint64_t insert_size,
    std::uint64_t slice_size,
    std::string_view data1,
    std::string_view data2
  ) {
    if (slice_size == 0) {
      return {};
    }
    std::string joined;
    joined.reserve(data1.size() + data2.size());
    joined.append(data1);
    joined.append(data2);

    const auto slices = (joined.size() + slice_size - 1) / slice_size;
    std::vector<std::uint8_t> result;
    result.reserve(joined.size() + slices * insert_size);
    for (std::size_t offset = 0; offset < joined.size(); offset += slice_size) {
      result.insert(result.end(), insert_size, 0);
      const auto count = std::min<std::size_t>(slice_size, joined.size() - offset);
      result.insert(result.end(), joined.begin() + offset, joined.begin() + offset + count);
    }
    return result;
  }
}  // namespace stream
