/**
 * @file src/video_packet.h
 * @brief Video packet sizes above Moonlight's 1400 bytes, for wired paths with jumbo frames.
 *
 * A host lists the largest video packet it accepts in the serverinfo element `MaxVideoPacket` and,
 * per stream, in the DESCRIBE attribute `a=x-rl-max-packet-size`. A client may then ask for up to
 * that size in `x-nv-video[0].packetSize`; without the offer, 1400 stays the most a client may ask
 * for. This is the extension Rubylight defines and Nightjar implements (Jumbo video packets).
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace video_packet {
  /// What stock Moonlight sends and older hosts accept; a size above it needs an offer.
  constexpr int STOCK_MAX = 1400;
  /// Stock Moonlight's packet size, which an unoffered jumbo request falls back to.
  constexpr int STOCK_PACKET = 1392;
  /// The largest video packet the extension defines (a multiple of 16).
  constexpr int MAX = 8192;
  /// Bytes a path's MTU must carry beyond the packet size: RTP 16 + UDP 8 + IPv6 40.
  constexpr int DATAGRAM_OVERHEAD = 64;

  /// The configured host maximum rounded down to 16 and capped at MAX; 0 when not above stock.
  inline int configured(int max_video_packet) {
    const int packet = std::min(max_video_packet, MAX) & ~0xF;
    return packet > STOCK_MAX ? packet : 0;
  }

  /// The largest packet a path whose IP MTU is `mtu` carries; 0 for a 1500-byte (or unknown) path.
  inline int for_mtu(std::uint32_t mtu) {
    if (mtu <= 1500) {
      return 0;
    }
    const auto room = std::min<std::uint32_t>(mtu - DATAGRAM_OVERHEAD, MAX);
    const int packet = int(room) & ~0xF;
    return packet > STOCK_MAX ? packet : 0;
  }

  /// A stream's offer: the configured maximum, limited by the route's MTU; 0 sends none.
  inline int offer(int max_video_packet, std::uint32_t route_mtu) {
    const int configured_packet = configured(max_video_packet);
    const int path_packet = for_mtu(route_mtu);
    return configured_packet && path_packet ? std::min(configured_packet, path_packet) : 0;
  }
}  // namespace video_packet
