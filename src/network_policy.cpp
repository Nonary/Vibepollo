/**
 * @file src/network_policy.cpp
 * @brief Definitions for pure networking address and naming policies.
 */
// standard includes
#include <cctype>

// lib includes
#include <boost/algorithm/string/trim.hpp>

// local includes
#include "network_policy.h"

using namespace std::literals;

namespace net {
  af_e af_from_enum_string(const std::string_view view) {
    if (view == "ipv4") {
      return IPV4;
    }
    if (view == "both") {
      return BOTH;
    }

    return BOTH;
  }

  std::string_view af_to_any_address_string(const af_e af) {
    switch (af) {
      case IPV4:
        return "0.0.0.0"sv;
      case BOTH:
        return "::"sv;
    }

    return "::"sv;
  }

  std::string select_bind_address(const std::string_view configured_bind_address, const af_e af) {
    std::string configured {configured_bind_address};
    boost::algorithm::trim(configured);
    return configured.empty() ? std::string(af_to_any_address_string(af)) : configured;
  }

  boost::asio::ip::tcp tcp_protocol_for_address(const boost::asio::ip::address &address) {
    return address.is_v6() ? boost::asio::ip::tcp::v6() : boost::asio::ip::tcp::v4();
  }

  std::string mdns_instance_name(const std::string_view &hostname) {
    std::string instancename {hostname.data(), hostname.size()};

    // Truncate to 63 characters per RFC 6763 section 7.2.
    if (instancename.size() > 63) {
      instancename.resize(63);
    }

    for (auto i = 0; i < instancename.size(); i++) {
      if (instancename[i] == ' ') {
        instancename[i] = '-';
      } else if (!std::isalnum(static_cast<unsigned char>(instancename[i])) && instancename[i] != '-') {
        instancename.resize(i);
        break;
      }
    }

    return !instancename.empty() ? instancename : "Apollo";
  }
}  // namespace net
