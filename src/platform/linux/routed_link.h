/**
 * @file src/platform/linux/routed_link.h
 * @brief Outbound Ethernet speed for a Linux client route.
 */
#pragma once

#include <boost/asio/ip/address.hpp>

#include <cstdint>
#include <string>

namespace platf {
  struct routed_link_info_t {
    std::uint32_t if_index = 0;
    std::string alias;
    std::uint64_t transmit_bps = 0;
    std::uint32_t mtu = 0;  ///< Path MTU of the routed connection, 0 when unknown.
  };

  /// Returns zero when the selected route is not an active full-duplex wired link.
  std::uint64_t routed_link_bps(const boost::asio::ip::address &source,
                                const boost::asio::ip::address &target,
                                routed_link_info_t *info = nullptr);
}
