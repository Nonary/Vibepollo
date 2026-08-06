/**
 * @file src/network.cpp
 * @brief Definitions for networking related functions.
 */
// standard includes
#include <algorithm>
#include <sstream>

// local includes
#include "config.h"
#include "logging.h"
#include "network.h"
#include "utility.h"

using namespace std::literals;

namespace ip = boost::asio::ip;

namespace net {
  std::vector<ip::network_v4> pc_ips_v4 {
    ip::make_network_v4("127.0.0.0/8"sv),
  };
  std::vector<ip::network_v4> lan_ips_v4 {
    ip::make_network_v4("192.168.0.0/16"sv),
    ip::make_network_v4("172.16.0.0/12"sv),
    ip::make_network_v4("10.0.0.0/8"sv),
    ip::make_network_v4("100.64.0.0/10"sv),
    ip::make_network_v4("169.254.0.0/16"sv),
  };

  std::vector<ip::network_v6> pc_ips_v6 {
    ip::make_network_v6("::1/128"sv),
  };
  std::vector<ip::network_v6> lan_ips_v6 {
    ip::make_network_v6("fc00::/7"sv),
    ip::make_network_v6("fe80::/64"sv),
  };

  net_e from_enum_string(const std::string_view &view) {
    if (view == "wan") {
      return WAN;
    }
    if (view == "lan") {
      return LAN;
    }

    return PC;
  }

  net_e from_address(const std::string_view &view) {
    auto addr = normalize_address(ip::make_address(view));

    if (addr.is_v6()) {
      for (auto &range : pc_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return PC;
        }
      }

      for (auto &range : lan_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return LAN;
        }
      }
    } else {
      for (auto &range : pc_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return PC;
        }
      }

      for (auto &range : lan_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return LAN;
        }
      }
    }

    return WAN;
  }

  std::string_view to_enum_string(net_e net) {
    switch (net) {
      case PC:
        return "pc"sv;
      case LAN:
        return "lan"sv;
      case WAN:
        return "wan"sv;
    }

    // avoid warning
    return "wan"sv;
  }

  std::string get_bind_address(const af_e af) {
    return select_bind_address(config::sunshine.bind_address, af);
  }

  boost::asio::ip::udp udp_protocol_for_address(const boost::asio::ip::address &address) {
    return address.is_v6() ? boost::asio::ip::udp::v6() : boost::asio::ip::udp::v4();
  }

  boost::asio::ip::address normalize_address(boost::asio::ip::address address) {
    // Convert IPv6-mapped IPv4 addresses into regular IPv4 addresses
    if (address.is_v6()) {
      const auto v6 = address.to_v6();
      if (v6.is_v4_mapped()) {
        return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);
      }
    }

    return address;
  }

  std::string addr_to_normalized_string(boost::asio::ip::address address) {
    return normalize_address(address).to_string();
  }

  std::string addr_to_url_escaped_string(boost::asio::ip::address address) {
    address = normalize_address(address);
    if (address.is_v6()) {
      auto host = address.to_string();
      if (const auto scope_separator = host.find('%'); scope_separator != std::string::npos) {
        host.replace(scope_separator, 1, "%25");
      }
      std::stringstream ss;
      ss << '[' << host << ']';
      return ss.str();
    } else {
      return address.to_string();
    }
  }

  int encryption_mode_for_address(boost::asio::ip::address address) {
    auto nettype = net::from_address(address.to_string());
    if (nettype == net::net_e::PC || nettype == net::net_e::LAN) {
      return config::stream.lan_encryption_mode;
    } else {
      return config::stream.wan_encryption_mode;
    }
  }

  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port) {
    static std::once_flag enet_init_flag;
    std::call_once(enet_init_flag, []() {
      enet_initialize();
    });

    const auto bind_addr = net::get_bind_address(af);
    enet_address_set_host(&addr, bind_addr.c_str());
    enet_address_set_port(&addr, port);

    // The socket family must match the bind address: an AF_INET6 socket cannot
    // bind an IPv4 bind_address even when address_family is set to "both".
    boost::system::error_code ec;
    const auto parsed_bind_addr = boost::asio::ip::make_address(bind_addr, ec);
    const bool use_ipv4 = ec ? af == IPV4 : parsed_bind_addr.is_v4();

    // Maximum of 128 clients, which should be enough for anyone
    auto host = host_t {enet_host_create(use_ipv4 ? AF_INET : AF_INET6, &addr, 128, 0, 0, 0)};

    if (host) {
      // Enable opportunistic QoS tagging (automatically disables if the network appears to drop tagged packets)
      enet_socket_set_option(host->socket, ENET_SOCKOPT_QOS, 1);
    }

    return host;
  }

  void free_host(ENetHost *host) {
    std::for_each(host->peers, host->peers + host->peerCount, [](ENetPeer &peer_ref) {
      ENetPeer *peer = &peer_ref;

      if (peer) {
        enet_peer_disconnect_now(peer, 0);
      }
    });

    enet_host_destroy(host);
  }

  std::uint16_t map_port(int port) {
    // calculate the port from the config port
    auto mapped_port = (std::uint16_t) ((int) config::sunshine.port + port);

    // Ensure port is in the range of 1024-65535
    if (mapped_port < 1024 || mapped_port > 65535) {
      BOOST_LOG(warning) << "Port out of range: "sv << mapped_port;
    }

    return mapped_port;
  }

}  // namespace net
