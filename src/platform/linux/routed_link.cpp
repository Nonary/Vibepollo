/**
 * @file src/platform/linux/routed_link.cpp
 * @brief Resolve the sender's route without transmitting a packet.
 */
#include "routed_link.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

namespace platf {
  namespace {
    namespace fs = std::filesystem;

    boost::asio::ip::address normalize(const boost::asio::ip::address &address) {
      if (address.is_v6() && address.to_v6().is_v4_mapped()) {
        return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6());
      }
      return address;
    }

    socklen_t socket_address(const boost::asio::ip::address &address, sockaddr_storage &storage,
                             std::uint16_t port) {
      if (address.is_v4()) {
        auto &ipv4 = reinterpret_cast<sockaddr_in &>(storage);
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(port);
        const auto bytes = address.to_v4().to_bytes();
        std::memcpy(&ipv4.sin_addr, bytes.data(), bytes.size());
        return sizeof(ipv4);
      }
      auto &ipv6 = reinterpret_cast<sockaddr_in6 &>(storage);
      ipv6.sin6_family = AF_INET6;
      ipv6.sin6_port = htons(port);
      ipv6.sin6_scope_id = address.to_v6().scope_id();
      const auto bytes = address.to_v6().to_bytes();
      std::memcpy(&ipv6.sin6_addr, bytes.data(), bytes.size());
      return sizeof(ipv6);
    }

    bool same_address(const sockaddr *left, const sockaddr *right) {
      if (!left || !right || left->sa_family != right->sa_family) return false;
      if (left->sa_family == AF_INET) {
        return reinterpret_cast<const sockaddr_in *>(left)->sin_addr.s_addr ==
               reinterpret_cast<const sockaddr_in *>(right)->sin_addr.s_addr;
      }
      if (left->sa_family == AF_INET6) {
        const auto &a = *reinterpret_cast<const sockaddr_in6 *>(left);
        const auto &b = *reinterpret_cast<const sockaddr_in6 *>(right);
        return std::memcmp(&a.sin6_addr, &b.sin6_addr, sizeof(a.sin6_addr)) == 0 &&
               (!a.sin6_scope_id || !b.sin6_scope_id || a.sin6_scope_id == b.sin6_scope_id);
      }
      return false;
    }

    std::uint64_t ethernet_speed_bps(const std::string &name) {
      const auto path = fs::path("/sys/class/net") / name;
      std::error_code error;
      // Virtual Ethernet devices commonly advertise synthetic 10+ Gbps rates.
      if (!fs::exists(path / "device", error) ||
          fs::exists(path / "wireless", error) || fs::exists(path / "phy80211", error)) return 0;
      unsigned int type = 0;
      std::ifstream(path / "type") >> type;
      if (type != ARPHRD_ETHER) return 0;
      std::string duplex;
      std::ifstream(path / "duplex") >> duplex;
      if (duplex != "full") return 0;
      std::uint64_t mbps = 0;
      std::ifstream(path / "speed") >> mbps;
      return mbps > 0 && mbps <= 400'000 ? mbps * 1'000'000 : 0;
    }
  }

  std::uint64_t routed_link_bps(const boost::asio::ip::address &raw_source,
                                const boost::asio::ip::address &raw_target,
                                routed_link_info_t *info) {
    if (info) *info = {};
    const auto source = normalize(raw_source);
    const auto target = normalize(raw_target);
    if (source.is_v4() != target.is_v4()) return 0;

    const int family = target.is_v4() ? AF_INET : AF_INET6;
    struct socket_guard_t {
      int descriptor;
      ~socket_guard_t() { if (descriptor >= 0) close(descriptor); }
    } fd {socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
    if (fd.descriptor < 0) return 0;

    sockaddr_storage local {};
    if (!source.is_unspecified()) {
      const auto length = socket_address(source, local, 0);
      if (bind(fd.descriptor, reinterpret_cast<sockaddr *>(&local), length) != 0) return 0;
    }
    sockaddr_storage destination {};
    const auto destination_length = socket_address(target, destination, 9);
    if (connect(fd.descriptor, reinterpret_cast<sockaddr *>(&destination), destination_length) != 0) return 0;
    socklen_t local_length = sizeof(local);
    if (getsockname(fd.descriptor, reinterpret_cast<sockaddr *>(&local), &local_length) != 0) return 0;

    ifaddrs *raw = nullptr;
    if (getifaddrs(&raw) != 0) return 0;
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> interfaces(raw, freeifaddrs);
    for (auto *entry = interfaces.get(); entry; entry = entry->ifa_next) {
      if (!same_address(entry->ifa_addr, reinterpret_cast<sockaddr *>(&local)) ||
          !(entry->ifa_flags & IFF_UP) || !(entry->ifa_flags & IFF_RUNNING) ||
          (entry->ifa_flags & IFF_LOOPBACK)) continue;
      if (!entry->ifa_name) continue;
      const std::string name(entry->ifa_name);
      const auto speed = ethernet_speed_bps(name);
      if (info) {
        info->if_index = if_nametoindex(entry->ifa_name);
        info->alias = name;
        info->transmit_bps = speed;
      }
      return speed;
    }
    return 0;
  }
}
