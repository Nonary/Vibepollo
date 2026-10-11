/**
 * @file src/wired_pin.cpp
 * @brief Eclipse: pin a client's addresses to its wired MAC for the session. See wired_pin.h.
 */
#include "wired_pin.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>

#include "logging.h"
#include "platform/common.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #include <netioapi.h>
#endif

using namespace std::literals;

namespace wired_pin {
  namespace {
    struct pinned_t {
      std::string ip;
      unsigned long if_index;
    };

    std::mutex lock;
    std::vector<pinned_t> pinned;

    std::string state_path() {
      return platf::appdata().string() + "/wired_pin_state.txt";
    }

    void save_state() {
      std::ofstream out(state_path(), std::ios::trunc);
      for (const auto &p : pinned) {
        out << p.ip << ' ' << p.if_index << '\n';
      }
    }

    /** "aa:bb:cc:dd:ee:ff" or "aa-bb-..." -> 6 bytes; false if it is not a MAC. */
    bool parse_mac(const std::string &text, unsigned char out[6]) {
      unsigned int b[6];
      std::string t = text;
      std::replace(t.begin(), t.end(), '-', ':');
      if (std::sscanf(t.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
      }
      for (int i = 0; i < 6; ++i) {
        out[i] = (unsigned char) b[i];
      }
      return true;
    }

#ifdef _WIN32
    bool unpin(const pinned_t &p) {
      MIB_IPNET_ROW2 row {};
      row.Address.si_family = AF_INET;
      row.Address.Ipv4.sin_family = AF_INET;
      if (inet_pton(AF_INET, p.ip.c_str(), &row.Address.Ipv4.sin_addr) != 1) {
        return false;
      }
      row.InterfaceIndex = p.if_index;
      const DWORD rc = DeleteIpNetEntry2(&row);
      return rc == NO_ERROR || rc == ERROR_NOT_FOUND;
    }
#endif
  }  // namespace

  bool supported() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
  }

  void apply(const std::string &client_ip, const std::string &wired_mac,
             const std::vector<std::string> &own_macs, const std::vector<std::string> &own_ips) {
    if (wired_mac.empty()) {
      return;
    }
#ifdef _WIN32
    unsigned char wired[6];
    if (!parse_mac(wired_mac, wired)) {
      BOOST_LOG(warning) << "Wired pin: ignored, not a MAC address: "sv << wired_mac;
      return;
    }
    if (std::find(own_ips.begin(), own_ips.end(), client_ip) == own_ips.end()) {
      BOOST_LOG(warning) << "Wired pin: ignored, the client's own addresses do not include "sv << client_ip;
      return;
    }
    std::vector<std::array<unsigned char, 6>> macs;
    for (const auto &m : own_macs) {
      std::array<unsigned char, 6> b {};
      if (parse_mac(m, b.data())) {
        macs.push_back(b);
      }
    }
    macs.push_back({wired[0], wired[1], wired[2], wired[3], wired[4], wired[5]});

    std::lock_guard lg {lock};
    for (const auto &ip : own_ips) {
      if (pinned.size() >= 8) {
        break;
      }
      sockaddr_in dest {};
      dest.sin_family = AF_INET;
      if (inet_pton(AF_INET, ip.c_str(), &dest.sin_addr) != 1) {
        continue;   // IPv4 only
      }
      // ON-LINK ONLY: a neighbour entry means nothing for an address reached through a router.
      SOCKADDR_INET to {};
      to.Ipv4 = dest;
      to.si_family = AF_INET;
      MIB_IPFORWARD_ROW2 route {};
      SOCKADDR_INET source {};
      if (GetBestRoute2(nullptr, 0, nullptr, &to, 0, &route, &source) != NO_ERROR ||
          route.NextHop.Ipv4.sin_addr.s_addr != 0) {
        BOOST_LOG(info) << "Wired pin: "sv << ip << " is not on this host's link, left alone"sv;
        continue;
      }
      MIB_IPNET_ROW2 row {};
      row.Address = to;
      row.InterfaceIndex = route.InterfaceIndex;
      // NEVER TAKE AN ADDRESS FROM SOMEONE ELSE: an existing entry must point at one of the client's own cards.
      if (GetIpNetEntry2(&row) == NO_ERROR && row.PhysicalAddressLength == 6) {
        const bool own = std::any_of(macs.begin(), macs.end(), [&](const auto &m) {
          return std::equal(m.begin(), m.end(), row.PhysicalAddress);
        });
        if (!own) {
          BOOST_LOG(warning) << "Wired pin: "sv << ip << " belongs to another device here, left alone"sv;
          continue;
        }
      }
      MIB_IPNET_ROW2 pin {};
      pin.Address = to;
      pin.InterfaceIndex = route.InterfaceIndex;
      pin.PhysicalAddressLength = 6;
      std::copy(wired, wired + 6, pin.PhysicalAddress);
      pin.State = NlnsPermanent;
      DeleteIpNetEntry2(&pin);   // replace whatever was learnt (that is the point)
      const DWORD rc = CreateIpNetEntry2(&pin);
      if (rc != NO_ERROR) {
        BOOST_LOG(warning) << "Wired pin: could not pin "sv << ip << " (error "sv << rc << ')';
        continue;
      }
      pinned.push_back({ip, route.InterfaceIndex});
      BOOST_LOG(info) << "Wired pin: "sv << ip << " -> "sv << wired_mac << " (interface "sv << route.InterfaceIndex << ") until the session ends"sv;
    }
    save_state();
#else
    (void) client_ip;
    (void) own_macs;
    (void) own_ips;
    BOOST_LOG(info) << "Wired pin: requested by the client, not supported on this platform"sv;
#endif
  }

  void revert() {
#ifdef _WIN32
    std::lock_guard lg {lock};
    if (pinned.empty()) {
      return;
    }
    std::vector<pinned_t> left;
    for (const auto &p : pinned) {
      if (unpin(p)) {
        BOOST_LOG(info) << "Wired pin: released "sv << p.ip;
      } else {
        left.push_back(p);
        BOOST_LOG(warning) << "Wired pin: could not release "sv << p.ip << " -- kept for the next start"sv;
      }
    }
    pinned = std::move(left);
    save_state();
#endif
  }

  void revert_leftovers() {
#ifdef _WIN32
    std::ifstream in(state_path());
    if (!in) {
      return;
    }
    std::string ip;
    unsigned long if_index;
    {
      std::lock_guard lg {lock};   // released before revert(), which takes it again
      while (in >> ip >> if_index) {
        pinned.push_back({ip, if_index});
      }
      if (!pinned.empty()) {
        BOOST_LOG(info) << "Wired pin: releasing "sv << pinned.size() << " entries left by the last run"sv;
      }
    }
    in.close();
#endif
    revert();
  }
}  // namespace wired_pin
