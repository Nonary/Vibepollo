#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stream {
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
