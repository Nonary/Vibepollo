#include "stream_protocol.h"

#include <algorithm>

namespace stream {
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
