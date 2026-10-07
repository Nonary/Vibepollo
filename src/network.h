/**
 * @file src/network.h
 * @brief Declarations for networking related functions.
 */
#pragma once

// standard includes
#include <string>
#include <tuple>
#include <utility>

// lib includes
#include <enet/enet.h>

// local includes
#include "network_policy.h"
#include "utility.h"

namespace net {
  void free_host(ENetHost *host);

  /**
   * @brief Map a specified port based on the base port.
   * @param port The port to map as a difference from the base port.
   * @return The mapped port number.
   * @examples
   * std::uint16_t mapped_port = net::map_port(1);
   * @examples_end
   * @todo Ensure port is not already in use by another application.
   */
  std::uint16_t map_port(int port);

  using host_t = util::safe_ptr<ENetHost, free_host>;
  using peer_t = ENetPeer *;
  using packet_t = util::safe_ptr<ENetPacket, enet_packet_destroy>;

  enum net_e : int {
    PC,  ///< PC
    LAN,  ///< LAN
    WAN  ///< WAN
  };

  net_e from_enum_string(const std::string_view &view);
  std::string_view to_enum_string(net_e net);

  net_e from_address(const std::string_view &view);

  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port);

  /**
   * @brief Get the binding address to use based on config.
   * @param af Address family.
   * @return Configured bind address or the wildcard address.
   */
  std::string get_bind_address(af_e af);

  /**
   * @brief Get the UDP protocol matching the provided address family.
   * @param address The address that will be used for binding.
   * @return IPv4 or IPv6 UDP protocol matching the address.
   */
  boost::asio::ip::udp udp_protocol_for_address(const boost::asio::ip::address &address);

  /**
   * @brief Convert an address to a normalized form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address.
   */
  boost::asio::ip::address normalize_address(boost::asio::ip::address address);

  /**
   * @brief Get the given address in normalized string form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address in string form.
   */
  std::string addr_to_normalized_string(boost::asio::ip::address address);

  /**
   * @brief Get the given address in a normalized form for the host portion of a URL.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize and escape.
   * @return Normalized address in URL-escaped string.
   */
  std::string addr_to_url_escaped_string(boost::asio::ip::address address);

  /**
   * @brief Get the encryption mode for the given remote endpoint address.
   * @param address The address used to look up the desired encryption mode.
   * @return The WAN or LAN encryption mode, based on the provided address.
   */
  int encryption_mode_for_address(boost::asio::ip::address address);

}  // namespace net
