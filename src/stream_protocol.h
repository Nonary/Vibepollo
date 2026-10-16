#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stream {
  // Count the complete UDP payload, including the optional encryption prefix,
  // against the 64 KiB Windows buffering limit and 64-segment Linux GSO limit.
  std::size_t video_send_batch_size(std::size_t block_size, std::size_t prefix_size, std::size_t max_batch_bytes);

  // An explicit ceiling counts complete video UDP payloads (including RTP,
  // packet/FEC headers and encryption prefixes), not encoded image bytes.
  std::size_t video_pacing_packets_per_ms(int ceiling_kbps, std::size_t datagram_bytes);

  struct video_pacing_demand_t {
    int encoder_kbps;
    int frames_per_second;
    std::size_t payload_bytes;
    std::size_t datagram_bytes;
    std::size_t frame_header_bytes;
    int fec_percentage;
    int min_parity_shards;
    // PyroWave may split critical and detail independently of RS capacity.
    bool independent_fec_blocks = false;
  };

  struct video_pacing_reservation_t {
    bool admissible = false;
    std::uint64_t packets_per_second = 0;
    std::uint64_t send_ns_per_second = 0;
  };

  // Conservative one-second demand at the negotiated encoder rate/cadence.
  // Includes per-frame padding and per-block parity rounding/minima. This is
  // nominal admission, not a maximum IDR size or a physical network estimate.
  video_pacing_reservation_t video_pacing_reservation(int ceiling_kbps, const video_pacing_demand_t &demand);
  bool reserve_video_pacing_capacity(std::uint64_t &used_ns_per_second, const video_pacing_reservation_t &reservation);

  inline std::string canonical_codec_name(std::string_view codec) {
    if (codec.empty()) {
      return {};
    }
    std::string lowered(codec);
    for (char &ch : lowered) {
      if (ch >= 'A' && ch <= 'Z') {
        ch = static_cast<char>(ch - 'A' + 'a');
      }
    }
    if (lowered == "h264" || lowered == "h.264") return "H.264";
    if (lowered == "h265" || lowered == "hevc") return "HEVC";
    if (lowered == "av1") return "AV1";
    return std::string(codec);
  }

  struct control_packet_view_t {
    std::uint16_t type = 0;
    std::string_view payload;
  };

  std::optional<control_packet_view_t> decode_control_packet(std::string_view packet_bytes);
  std::vector<std::uint8_t> concat_and_insert(
    std::uint64_t insert_size,
    std::uint64_t slice_size,
    std::string_view data1,
    std::string_view data2
  );
}  // namespace stream
