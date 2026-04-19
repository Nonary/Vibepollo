/**
 * @file src/platform/macos/host_stats.mm
 * @brief macOS implementation of @ref platf::host_stats_provider_t.
 *
 * Uses Mach + sysctl + getifaddrs for CPU%, RAM, network throughput, and
 * CPU/host model. GPU, GPU encoder utilization, GPU temperature, and VRAM
 * are not exposed by stable public APIs on modern macOS (especially Apple
 * Silicon, where IOReport is the only viable path and is private). Those
 * fields are left at the documented sentinels.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

// lib includes
#include <ifaddrs.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

// local includes
#include "src/logging.h"
#include "src/platform/common.h"

namespace {

  using std::chrono::steady_clock;

  std::string sysctl_string(const char *name) {
    size_t len = 0;
    if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return {};
    std::string buf(len, '\0');
    if (sysctlbyname(name, buf.data(), &len, nullptr, 0) != 0) return {};
    if (!buf.empty() && buf.back() == '\0') buf.pop_back();
    return buf;
  }

  std::uint64_t sysctl_u64(const char *name) {
    std::uint64_t v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return 0;
    return v;
  }

  int sysctl_int(const char *name) {
    int v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return 0;
    return v;
  }

  struct cpu_ticks_t {
    std::uint64_t user = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t nice = 0;

    std::uint64_t total() const {
      return user + system + idle + nice;
    }

    std::uint64_t busy() const {
      return user + system + nice;
    }
  };

  std::optional<cpu_ticks_t> read_cpu_ticks() {
    host_cpu_load_info_data_t load {};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        reinterpret_cast<host_info_t>(&load), &count) != KERN_SUCCESS) {
      return std::nullopt;
    }
    cpu_ticks_t t;
    t.user = load.cpu_ticks[CPU_STATE_USER];
    t.system = load.cpu_ticks[CPU_STATE_SYSTEM];
    t.idle = load.cpu_ticks[CPU_STATE_IDLE];
    t.nice = load.cpu_ticks[CPU_STATE_NICE];
    return t;
  }

  struct memory_t {
    std::uint64_t total_bytes = 0;
    std::uint64_t used_bytes = 0;
  };

  std::optional<memory_t> read_memory() {
    memory_t m;
    m.total_bytes = sysctl_u64("hw.memsize");
    if (!m.total_bytes) return std::nullopt;

    vm_size_t page_size = 0;
    host_page_size(mach_host_self(), &page_size);
    if (!page_size) page_size = 4096;

    vm_statistics64_data_t stats {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&stats), &count) != KERN_SUCCESS) {
      return std::nullopt;
    }
    // "Used" approximates Activity Monitor's App Memory + Wired + Compressed.
    std::uint64_t used_pages = static_cast<std::uint64_t>(stats.active_count)
                               + static_cast<std::uint64_t>(stats.wire_count)
                               + static_cast<std::uint64_t>(stats.compressor_page_count);
    m.used_bytes = used_pages * static_cast<std::uint64_t>(page_size);
    return m;
  }

  // Walk the kernel's routing table and find the interface that owns the
  // default route (destination 0.0.0.0). Returns the interface name or empty.
  std::string default_route_iface() {
    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_DUMP, 0};
    size_t needed = 0;
    if (sysctl(mib, 6, nullptr, &needed, nullptr, 0) != 0 || !needed) return {};
    std::string buf(needed, '\0');
    if (sysctl(mib, 6, buf.data(), &needed, nullptr, 0) != 0) return {};

    auto *next = buf.data();
    auto *end = next + needed;
    while (next < end) {
      auto *rtm = reinterpret_cast<struct rt_msghdr *>(next);
      if (rtm->rtm_version != RTM_VERSION) break;
      if ((rtm->rtm_flags & (RTF_UP | RTF_GATEWAY)) == (RTF_UP | RTF_GATEWAY)) {
        auto *sa = reinterpret_cast<struct sockaddr *>(rtm + 1);
        if (sa->sa_family == AF_INET) {
          auto *sin = reinterpret_cast<struct sockaddr_in *>(sa);
          if (sin->sin_addr.s_addr == 0) {
            char name[IF_NAMESIZE] = {};
            if (if_indextoname(rtm->rtm_index, name)) return name;
          }
        }
      }
      next += rtm->rtm_msglen;
    }
    return {};
  }

  struct net_counters_t {
    std::uint64_t rx_bytes = 0;
    std::uint64_t tx_bytes = 0;
  };

  std::optional<net_counters_t> read_net_counters(const std::string &iface) {
    if (iface.empty()) return std::nullopt;
    struct ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0 || !list) return std::nullopt;
    std::optional<net_counters_t> result;
    for (auto *p = list; p; p = p->ifa_next) {
      if (!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK) continue;
      if (iface != p->ifa_name) continue;
      auto *data = reinterpret_cast<struct if_data *>(p->ifa_data);
      if (!data) continue;
      net_counters_t c;
      c.rx_bytes = data->ifi_ibytes;
      c.tx_bytes = data->ifi_obytes;
      result = c;
      break;
    }
    freeifaddrs(list);
    return result;
  }

  std::uint64_t read_iface_link_speed_mbps(const std::string &iface) {
    if (iface.empty()) return 0;
    struct ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0 || !list) return 0;
    std::uint64_t mbps = 0;
    for (auto *p = list; p; p = p->ifa_next) {
      if (!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK) continue;
      if (iface != p->ifa_name) continue;
      auto *data = reinterpret_cast<struct if_data *>(p->ifa_data);
      if (!data) continue;
      // ifi_baudrate is in bits/sec on macOS.
      mbps = data->ifi_baudrate / 1'000'000ull;
      break;
    }
    freeifaddrs(list);
    return mbps;
  }

  class macos_host_stats_t: public platf::host_stats_provider_t {
  public:
    platf::host_stats_t sample() override {
      platf::host_stats_t out;
      sample_cpu(out);
      sample_memory(out);
      sample_network(out);
      // GPU / GPU encoder / GPU temp / VRAM: no stable public API on modern
      // macOS. Leave at sentinels.
      return out;
    }

    platf::host_info_t info() override {
      platf::host_info_t out;
      out.cpu_model = sysctl_string("machdep.cpu.brand_string");
      if (out.cpu_model.empty()) out.cpu_model = sysctl_string("hw.model");
      out.cpu_logical_cores = sysctl_int("hw.logicalcpu");
      out.ram_total_bytes = sysctl_u64("hw.memsize");
      // GPU model: best-effort via hw.model string (Apple Silicon collapses
      // CPU + GPU under a single SoC name).
      out.gpu_model = sysctl_string("hw.model");
      auto iface = pick_iface();
      out.net_interface = iface;
      out.net_link_speed_mbps = read_iface_link_speed_mbps(iface);
      return out;
    }

  private:
    void sample_cpu(platf::host_stats_t &out) {
      auto t = read_cpu_ticks();
      if (!t) return;
      if (_have_cpu_baseline) {
        auto dt = t->total() - _last_cpu.total();
        auto db = t->busy() - _last_cpu.busy();
        if (dt > 0) {
          out.cpu_percent = std::clamp(static_cast<float>(db) * 100.f / static_cast<float>(dt), 0.f, 100.f);
        }
      }
      _last_cpu = *t;
      _have_cpu_baseline = true;
    }

    void sample_memory(platf::host_stats_t &out) {
      auto m = read_memory();
      if (!m) return;
      out.ram_total_bytes = m->total_bytes;
      out.ram_used_bytes = m->used_bytes;
    }

    std::string pick_iface() {
      auto now = steady_clock::now();
      if (_iface.empty() || (now - _iface_picked_at) > std::chrono::seconds(30)) {
        auto candidate = default_route_iface();
        if (!candidate.empty()) _iface = candidate;
        _iface_picked_at = now;
      }
      return _iface;
    }

    void sample_network(platf::host_stats_t &out) {
      auto iface = pick_iface();
      if (iface.empty()) return;
      auto c = read_net_counters(iface);
      if (!c) return;
      auto now = steady_clock::now();
      if (_have_net_baseline && _last_net_iface == iface) {
        auto dt = std::chrono::duration<double>(now - _last_net_at).count();
        if (dt > 0.05) {
          double drx = static_cast<double>(c->rx_bytes - _last_net.rx_bytes);
          double dtx = static_cast<double>(c->tx_bytes - _last_net.tx_bytes);
          out.net_rx_bps = (drx * 8.0) / dt;
          out.net_tx_bps = (dtx * 8.0) / dt;
        } else {
          out.net_rx_bps = 0.0;
          out.net_tx_bps = 0.0;
        }
      } else {
        out.net_rx_bps = 0.0;
        out.net_tx_bps = 0.0;
      }
      _last_net = *c;
      _last_net_iface = iface;
      _last_net_at = now;
      _have_net_baseline = true;
    }

    cpu_ticks_t _last_cpu {};
    bool _have_cpu_baseline = false;

    std::string _iface;
    steady_clock::time_point _iface_picked_at {};

    net_counters_t _last_net {};
    std::string _last_net_iface;
    steady_clock::time_point _last_net_at {};
    bool _have_net_baseline = false;
  };

}  // namespace

namespace platf {

  std::unique_ptr<host_stats_provider_t>
    create_host_stats_provider() {
    return std::make_unique<macos_host_stats_t>();
  }

}  // namespace platf
