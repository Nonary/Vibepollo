/**
 * @file src/platform/linux/host_stats.cpp
 * @brief Linux implementation of @ref platf::host_stats_provider_t.
 *
 * Pulls CPU, RAM, network, and CPU-temperature data from /proc and /sys.
 * Optionally dlopen()s libnvidia-ml.so.1 for NVIDIA GPU utilization /
 * temperature / VRAM. Anything that cannot be read is left at the documented
 * sentinels (-1 for percentages and temperatures, 0 for byte counts).
 *
 * The implementation is intentionally header-light (no linker-level NVML
 * dependency) so the binary stays portable across distros that ship no
 * proprietary NVIDIA bits.
 */
// standard includes
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// lib includes
#include <dlfcn.h>
#include <unistd.h>

// local includes
#include "src/logging.h"
#include "src/platform/common.h"

namespace {

  using std::chrono::steady_clock;

  // --- /proc/stat CPU sampler ---------------------------------------------------

  struct cpu_jiffies_t {
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
    std::uint64_t steal = 0;

    std::uint64_t total() const {
      return user + nice + system + idle + iowait + irq + softirq + steal;
    }

    std::uint64_t busy() const {
      return total() - idle - iowait;
    }
  };

  std::optional<cpu_jiffies_t> read_cpu_jiffies() {
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return std::nullopt;
    std::string label;
    cpu_jiffies_t j;
    f >> label;
    if (label != "cpu") return std::nullopt;
    f >> j.user >> j.nice >> j.system >> j.idle >> j.iowait >> j.irq >> j.softirq >> j.steal;
    if (!f) return std::nullopt;
    return j;
  }

  // --- /proc/meminfo ------------------------------------------------------------

  struct meminfo_t {
    std::uint64_t total_bytes = 0;
    std::uint64_t available_bytes = 0;
  };

  std::optional<meminfo_t> read_meminfo() {
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return std::nullopt;
    meminfo_t m;
    std::string line;
    while (std::getline(f, line)) {
      std::uint64_t kib = 0;
      if (std::sscanf(line.c_str(), "MemTotal: %lu kB", &kib) == 1) {
        m.total_bytes = kib * 1024ull;
      } else if (std::sscanf(line.c_str(), "MemAvailable: %lu kB", &kib) == 1) {
        m.available_bytes = kib * 1024ull;
      }
      if (m.total_bytes && m.available_bytes) break;
    }
    if (!m.total_bytes) return std::nullopt;
    return m;
  }

  // --- /proc/cpuinfo ------------------------------------------------------------

  std::string read_cpu_model() {
    std::ifstream f("/proc/cpuinfo");
    if (!f.is_open()) return {};
    std::string line;
    while (std::getline(f, line)) {
      auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      auto key = line.substr(0, colon);
      // strip trailing whitespace from key
      while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
      if (key == "model name" || key == "Hardware" || key == "Processor") {
        auto val = line.substr(colon + 1);
        size_t i = 0;
        while (i < val.size() && (val[i] == ' ' || val[i] == '\t')) ++i;
        return val.substr(i);
      }
    }
    return {};
  }

  int read_logical_cores() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : 0;
  }

  // --- thermal zones ------------------------------------------------------------

  float read_cpu_temp() {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists("/sys/class/thermal", ec)) return -1.f;
    float best = -1.f;
    for (auto &entry : fs::directory_iterator("/sys/class/thermal", ec)) {
      if (ec) break;
      auto name = entry.path().filename().string();
      if (name.rfind("thermal_zone", 0) != 0) continue;
      std::ifstream type_f(entry.path() / "type");
      std::string type;
      if (type_f.is_open()) std::getline(type_f, type);
      // x86_pkg_temp / coretemp / cpu-thermal / k10temp etc are CPU
      bool is_cpu = type.find("cpu") != std::string::npos
                    || type.find("x86_pkg_temp") != std::string::npos
                    || type.find("coretemp") != std::string::npos
                    || type.find("k10temp") != std::string::npos;
      if (!is_cpu) continue;
      std::ifstream temp_f(entry.path() / "temp");
      long milli = 0;
      if (temp_f >> milli) {
        float c = static_cast<float>(milli) / 1000.f;
        if (c > best) best = c;
      }
    }
    return best;
  }

  // --- /proc/net/dev + /proc/net/route -----------------------------------------

  std::string default_route_iface() {
    std::ifstream f("/proc/net/route");
    if (!f.is_open()) return {};
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
      std::istringstream ss(line);
      std::string iface, dest;
      ss >> iface >> dest;
      if (dest == "00000000") return iface;  // default route
    }
    return {};
  }

  struct net_counters_t {
    std::uint64_t rx_bytes = 0;
    std::uint64_t tx_bytes = 0;
  };

  std::optional<net_counters_t> read_net_counters(const std::string &iface) {
    if (iface.empty()) return std::nullopt;
    std::ifstream f("/proc/net/dev");
    if (!f.is_open()) return std::nullopt;
    std::string line;
    std::getline(f, line);  // header
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
      auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      auto name = line.substr(0, colon);
      // strip whitespace
      size_t s = 0;
      while (s < name.size() && (name[s] == ' ' || name[s] == '\t')) ++s;
      name = name.substr(s);
      if (name != iface) continue;
      std::istringstream ss(line.substr(colon + 1));
      net_counters_t c;
      std::uint64_t skip;
      ss >> c.rx_bytes;
      for (int i = 0; i < 7; ++i) ss >> skip;
      ss >> c.tx_bytes;
      if (!ss) return std::nullopt;
      return c;
    }
    return std::nullopt;
  }

  std::uint64_t read_iface_link_speed_mbps(const std::string &iface) {
    if (iface.empty()) return 0;
    std::ifstream f("/sys/class/net/" + iface + "/speed");
    if (!f.is_open()) return 0;
    long mbps = 0;
    f >> mbps;
    return mbps > 0 ? static_cast<std::uint64_t>(mbps) : 0;
  }

  // --- NVML (dlopen) ------------------------------------------------------------

  struct nvml_memory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
  };

  struct nvml_utilization_t {
    unsigned int gpu;
    unsigned int memory;
  };

  using nvmlReturn_t = int;
  static constexpr nvmlReturn_t NVML_SUCCESS = 0;
  static constexpr int NVML_TEMPERATURE_GPU = 0;

  using nvmlInit_v2_t = nvmlReturn_t (*)();
  using nvmlShutdown_t = nvmlReturn_t (*)();
  using nvmlDeviceGetCount_v2_t = nvmlReturn_t (*)(unsigned int *);
  using nvmlDeviceGetHandleByIndex_v2_t = nvmlReturn_t (*)(unsigned int, void **);
  using nvmlDeviceGetUtilizationRates_t = nvmlReturn_t (*)(void *, nvml_utilization_t *);
  using nvmlDeviceGetEncoderUtilization_t = nvmlReturn_t (*)(void *, unsigned int *, unsigned int *);
  using nvmlDeviceGetTemperature_t = nvmlReturn_t (*)(void *, int, unsigned int *);
  using nvmlDeviceGetMemoryInfo_t = nvmlReturn_t (*)(void *, nvml_memory_t *);
  using nvmlDeviceGetName_t = nvmlReturn_t (*)(void *, char *, unsigned int);

  class nvml_t {
  public:
    nvml_t() = default;

    ~nvml_t() {
      if (_inited && _shutdown) _shutdown();
      if (_handle) dlclose(_handle);
    }

    bool try_open() {
      _handle = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
      if (!_handle) return false;
#define LOAD(name) name = reinterpret_cast<name##_t>(dlsym(_handle, #name))
      LOAD(nvmlInit_v2);
      LOAD(nvmlShutdown);
      LOAD(nvmlDeviceGetCount_v2);
      LOAD(nvmlDeviceGetHandleByIndex_v2);
      LOAD(nvmlDeviceGetUtilizationRates);
      LOAD(nvmlDeviceGetEncoderUtilization);
      LOAD(nvmlDeviceGetTemperature);
      LOAD(nvmlDeviceGetMemoryInfo);
      LOAD(nvmlDeviceGetName);
#undef LOAD
      if (!nvmlInit_v2 || !nvmlShutdown || !nvmlDeviceGetHandleByIndex_v2) return false;
      if (nvmlInit_v2() != NVML_SUCCESS) return false;
      _inited = true;
      _shutdown = nvmlShutdown;
      if (nvmlDeviceGetHandleByIndex_v2(0, &_dev) != NVML_SUCCESS) {
        _dev = nullptr;
      }
      return _dev != nullptr;
    }

    void *device() const { return _dev; }

    nvmlDeviceGetUtilizationRates_t nvmlDeviceGetUtilizationRates = nullptr;
    nvmlDeviceGetEncoderUtilization_t nvmlDeviceGetEncoderUtilization = nullptr;
    nvmlDeviceGetTemperature_t nvmlDeviceGetTemperature = nullptr;
    nvmlDeviceGetMemoryInfo_t nvmlDeviceGetMemoryInfo = nullptr;
    nvmlDeviceGetName_t nvmlDeviceGetName = nullptr;

  private:
    void *_handle = nullptr;
    void *_dev = nullptr;
    bool _inited = false;
    nvmlInit_v2_t nvmlInit_v2 = nullptr;
    nvmlShutdown_t nvmlShutdown = nullptr;
    nvmlDeviceGetCount_v2_t nvmlDeviceGetCount_v2 = nullptr;
    nvmlDeviceGetHandleByIndex_v2_t nvmlDeviceGetHandleByIndex_v2 = nullptr;
  };

  // --- provider -----------------------------------------------------------------

  class linux_host_stats_t: public platf::host_stats_provider_t {
  public:
    linux_host_stats_t() {
      _have_nvml = _nvml.try_open();
      if (!_have_nvml) {
        BOOST_LOG(::info) << "host_stats(linux): NVML not available; GPU stats will be N/A.";
      }
    }

    platf::host_stats_t sample() override {
      platf::host_stats_t out;
      sample_cpu(out);
      sample_memory(out);
      sample_temps(out);
      sample_network(out);
      sample_gpu(out);
      return out;
    }

    platf::host_info_t info() override {
      platf::host_info_t out;
      out.cpu_model = read_cpu_model();
      out.cpu_logical_cores = read_logical_cores();
      if (auto m = read_meminfo()) out.ram_total_bytes = m->total_bytes;
      auto iface = pick_iface();
      out.net_interface = iface;
      out.net_link_speed_mbps = read_iface_link_speed_mbps(iface);
      if (_have_nvml && _nvml.device() && _nvml.nvmlDeviceGetName) {
        char name[96] = {};
        if (_nvml.nvmlDeviceGetName(_nvml.device(), name, sizeof(name)) == NVML_SUCCESS) {
          out.gpu_model = name;
        }
        if (_nvml.nvmlDeviceGetMemoryInfo) {
          nvml_memory_t mem {};
          if (_nvml.nvmlDeviceGetMemoryInfo(_nvml.device(), &mem) == NVML_SUCCESS) {
            out.vram_total_bytes = mem.total;
          }
        }
      }
      return out;
    }

  private:
    void sample_cpu(platf::host_stats_t &out) {
      auto j = read_cpu_jiffies();
      if (!j) return;
      if (_have_cpu_baseline) {
        auto dt = j->total() - _last_cpu.total();
        auto db = j->busy() - _last_cpu.busy();
        if (dt > 0) {
          out.cpu_percent = std::clamp(static_cast<float>(db) * 100.f / static_cast<float>(dt), 0.f, 100.f);
        }
      }
      _last_cpu = *j;
      _have_cpu_baseline = true;
    }

    void sample_memory(platf::host_stats_t &out) {
      auto m = read_meminfo();
      if (!m) return;
      out.ram_total_bytes = m->total_bytes;
      if (m->available_bytes <= m->total_bytes) {
        out.ram_used_bytes = m->total_bytes - m->available_bytes;
      }
    }

    void sample_temps(platf::host_stats_t &out) {
      out.cpu_temp_c = read_cpu_temp();
    }

    std::string pick_iface() {
      auto now = steady_clock::now();
      if (_iface.empty() || (now - _iface_picked_at) > std::chrono::seconds(30)) {
        auto candidate = default_route_iface();
        if (!candidate.empty()) {
          _iface = candidate;
        }
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
        if (c->rx_bytes < _last_net.rx_bytes || c->tx_bytes < _last_net.tx_bytes) {
          // Interface counters reset or wrapped: reset the baseline without
          // emitting a synthetic spike for this sample.
          out.net_rx_bps = 0.0;
          out.net_tx_bps = 0.0;
        } else if (dt > 0.05) {
          double drx = static_cast<double>(c->rx_bytes - _last_net.rx_bytes);
          double dtx = static_cast<double>(c->tx_bytes - _last_net.tx_bytes);
          out.net_rx_bps = (drx * 8.0) / dt;
          out.net_tx_bps = (dtx * 8.0) / dt;
        } else {
          out.net_rx_bps = 0.0;
          out.net_tx_bps = 0.0;
        }
      } else {
        // First sample after start (or interface changed): no spike.
        out.net_rx_bps = 0.0;
        out.net_tx_bps = 0.0;
      }
      _last_net = *c;
      _last_net_iface = iface;
      _last_net_at = now;
      _have_net_baseline = true;
    }

    void sample_gpu(platf::host_stats_t &out) {
      if (!_have_nvml || !_nvml.device()) return;
      void *dev = _nvml.device();
      if (_nvml.nvmlDeviceGetUtilizationRates) {
        nvml_utilization_t u {};
        if (_nvml.nvmlDeviceGetUtilizationRates(dev, &u) == NVML_SUCCESS) {
          out.gpu_percent = static_cast<float>(u.gpu);
        }
      }
      if (_nvml.nvmlDeviceGetEncoderUtilization) {
        unsigned int util = 0;
        unsigned int sampling = 0;
        if (_nvml.nvmlDeviceGetEncoderUtilization(dev, &util, &sampling) == NVML_SUCCESS) {
          out.gpu_encoder_percent = static_cast<float>(util);
        }
      }
      if (_nvml.nvmlDeviceGetTemperature) {
        unsigned int temp = 0;
        if (_nvml.nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) {
          out.gpu_temp_c = static_cast<float>(temp);
        }
      }
      if (_nvml.nvmlDeviceGetMemoryInfo) {
        nvml_memory_t mem {};
        if (_nvml.nvmlDeviceGetMemoryInfo(dev, &mem) == NVML_SUCCESS) {
          out.vram_total_bytes = mem.total;
          out.vram_used_bytes = mem.used;
        }
      }
    }

    cpu_jiffies_t _last_cpu {};
    bool _have_cpu_baseline = false;

    std::string _iface;
    steady_clock::time_point _iface_picked_at {};

    net_counters_t _last_net {};
    std::string _last_net_iface;
    steady_clock::time_point _last_net_at {};
    bool _have_net_baseline = false;

    nvml_t _nvml;
    bool _have_nvml = false;
  };

}  // namespace

namespace platf {

  std::unique_ptr<host_stats_provider_t>
    create_host_stats_provider() {
    return std::make_unique<linux_host_stats_t>();
  }

}  // namespace platf
