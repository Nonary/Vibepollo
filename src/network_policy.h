/**
 * @file src/network_policy.h
 * @brief Pure networking address and naming policies.
 */
#pragma once

// standard includes
#include <string>
#include <string_view>

// lib includes
#include <boost/asio.hpp>

namespace net {
  enum af_e : int {
    IPV4,  ///< IPv4 only
    BOTH  ///< IPv4 and IPv6
  };

  /**
   * @brief Get the address family enum value from a configuration value.
   * @param view The config option value.
   * @return The address family enum value.
   */
  af_e af_from_enum_string(std::string_view view);

  /**
   * @brief Get the wildcard binding address for a given address family.
   * @param af Address family.
   * @return Normalized address.
   */
  std::string_view af_to_any_address_string(af_e af);

  /**
   * @brief Select a binding address from a configured value and address family.
   * @param configured_bind_address The optionally configured address.
   * @param af Address family to use when the configured address is empty.
   * @return Trimmed configured address or the matching wildcard address.
   */
  std::string select_bind_address(std::string_view configured_bind_address, af_e af);

  boost::asio::ip::tcp tcp_protocol_for_address(const boost::asio::ip::address &address);

  /**
   * @brief Returns a string for use as the instance name for mDNS.
   * @param hostname The hostname to use for instance name generation.
   * @return Hostname-based instance name or "Sunshine" if hostname is invalid.
   */
  std::string mdns_instance_name(const std::string_view &hostname);
}  // namespace net
