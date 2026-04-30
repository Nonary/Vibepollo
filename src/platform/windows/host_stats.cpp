/**
 * @file src/platform/windows/host_stats.cpp
 * @brief Windows implementation of @ref platf::host_stats_provider_t.
 *
 * CPU usage   : @c GetSystemTimes deltas.
 * RAM         : @c GlobalMemoryStatusEx.
 * GPU usage   : PDH counters @c "\GPU Engine(*engtype_3D)\Utilization Percentage"
 *               and @c "\GPU Engine(*engtype_VideoEncode)\Utilization Percentage".
 *               Vendor-agnostic (NVIDIA / AMD / Intel) — uses the WDDM
 *               scheduler, the same source Task Manager reads.
 * VRAM used   : PDH @c "\GPU Process Memory(*)\Dedicated Usage" filtered to
 *               the selected DXGI adapter LUID.
 * VRAM total  : DXGI @c IDXGIAdapter::GetDesc.
 * GPU temp    : optional NVML loaded at runtime via @c LoadLibrary.
 *
 * Counters that are not available on the current system are reported as
 * @c -1.f / @c 0 — the FE renders these as "N/A".
 */

#include "src/platform/common.h"

// standard includes
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// platform includes
#include <windows.h>

#include <dxgi.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <ws2tcpip.h>

// local includes
#include "src/logging.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")

using namespace std::chrono_literals;

namespace {

  using filetime_u64_t = std::uint64_t;

  filetime_u64_t
    to_u64(const FILETIME &ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
  }

  std::string
    read_processor_name() {
    HKEY key {};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0,
                      KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS) {
      return {};
    }
    char buf[256] {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LONG rc = RegQueryValueExA(key, "ProcessorNameString", nullptr, &type,
                               reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) {
      return {};
    }
    std::string s(buf);
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) {
      s.pop_back();
    }
    return s;
  }

  /**
   * @brief Open a PDH query that sums all matching counter instances.
   *
   * Uses a wildcard path; on each collect we read the cooked instance values
   * and sum them. Suitable for "\GPU Engine(*engtype_*)\Utilization Percentage".
   */
  class pdh_wildcard_sum_t {
  public:
    explicit pdh_wildcard_sum_t(std::wstring path):
        _path(std::move(path)) {
    }

    ~pdh_wildcard_sum_t() {
      if (_query) {
        PdhCloseQuery(_query);
      }
    }

    pdh_wildcard_sum_t(const pdh_wildcard_sum_t &) = delete;
    pdh_wildcard_sum_t &operator=(const pdh_wildcard_sum_t &) = delete;

    bool
      open() {
      if (PdhOpenQueryW(nullptr, 0, &_query) != ERROR_SUCCESS) {
        _query = nullptr;
        return false;
      }
      if (PdhAddEnglishCounterW(_query, _path.c_str(), 0, &_counter) != ERROR_SUCCESS) {
        // fall back to localized counter name
        if (PdhAddCounterW(_query, _path.c_str(), 0, &_counter) != ERROR_SUCCESS) {
          PdhCloseQuery(_query);
          _query = nullptr;
          return false;
        }
      }
      // first collect seeds the rate counters; the value is undefined.
      PdhCollectQueryData(_query);
      return true;
    }

    /**
     * @brief Collect and return the sum of all instance values.
     * @param max_value Optional clamp (e.g. 100.0 for percentages).
     * @return -1.f on failure.
     */
    float
      collect(float max_value = -1.f, const std::wstring &instance_contains = {}) {
      if (!_query) {
        return -1.f;
      }
      if (PdhCollectQueryData(_query) != ERROR_SUCCESS) {
        return -1.f;
      }
      DWORD buf_size = 0;
      DWORD item_count = 0;
      PDH_STATUS rc = PdhGetFormattedCounterArrayW(
        _counter, PDH_FMT_DOUBLE, &buf_size, &item_count, nullptr);
      if (rc != PDH_MORE_DATA) {
        return -1.f;
      }
      std::vector<std::uint8_t> buf(buf_size);
      auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buf.data());
      if (PdhGetFormattedCounterArrayW(_counter, PDH_FMT_DOUBLE, &buf_size,
                                       &item_count, items) != ERROR_SUCCESS) {
        return -1.f;
      }
      double total = 0.0;
      for (DWORD i = 0; i < item_count; ++i) {
        if (!instance_contains.empty()) {
          const wchar_t *name = items[i].szName;
          if (!name || std::wstring_view {name}.find(instance_contains) == std::wstring_view::npos) {
            continue;
          }
        }
        if (items[i].FmtValue.CStatus == ERROR_SUCCESS ||
            items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA ||
            items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) {
          total += items[i].FmtValue.doubleValue;
        }
      }
      if (max_value > 0.f && total > max_value) {
        total = max_value;
      }
      return static_cast<float>(total);
    }

  private:
    std::wstring _path;
    PDH_HQUERY _query {};
    PDH_HCOUNTER _counter {};
  };

  std::wstring
    luid_instance_prefix(const LUID &luid) {
    wchar_t buf[64] {};
    swprintf(buf,
             ARRAYSIZE(buf),
             L"luid_0x%08x_0x%08x",
             static_cast<unsigned int>(luid.HighPart),
             static_cast<unsigned int>(luid.LowPart));
    return buf;
  }

  /**
   * @brief Collect dedicated VRAM in use for one adapter.
   * @return Bytes; 0 on failure.
   */
  std::uint64_t
    collect_vram_used_bytes(pdh_wildcard_sum_t &mem_query, const std::wstring &adapter_instance, std::uint64_t vram_total_bytes) {
    float v = mem_query.collect(-1.f, adapter_instance);
    if (v == 0.f && !adapter_instance.empty()) {
      // Some multi-adapter systems expose memory on a different active LUID
      // than the static "largest VRAM" adapter chosen for host info. Falling
      // back to the all-adapter sum is safer than reporting a permanently
      // stuck 0; the clamp below still prevents impossible percentages.
      v = mem_query.collect();
    }
    if (v < 0.f) {
      return 0;
    }
    auto used = static_cast<std::uint64_t>(v);
    return vram_total_bytes > 0 && used > vram_total_bytes ? vram_total_bytes : used;
  }

  /**
   * @brief Query the primary DXGI adapter and return total VRAM bytes,
   *        adapter LUID, and a printable description.
   */
  void
    query_dxgi(std::uint64_t &vram_total_bytes, LUID &adapter_luid, std::string &gpu_model) {
    vram_total_bytes = 0;
    adapter_luid = {};
    gpu_model.clear();

    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
      return;
    }

    IDXGIAdapter1 *best_adapter = nullptr;
    SIZE_T best_vram = 0;
    DXGI_ADAPTER_DESC1 best_desc {};

    for (UINT i = 0;; ++i) {
      IDXGIAdapter1 *adapter = nullptr;
      if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (!adapter) {
        break;
      }
      DXGI_ADAPTER_DESC1 desc {};
      if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        // skip the Basic Render Driver / WARP
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            desc.DedicatedVideoMemory > best_vram) {
          if (best_adapter) {
            best_adapter->Release();
          }
          best_adapter = adapter;
          best_vram = desc.DedicatedVideoMemory;
          best_desc = desc;
          continue;
        }
      }
      adapter->Release();
    }

    if (best_adapter) {
      vram_total_bytes = static_cast<std::uint64_t>(best_desc.DedicatedVideoMemory);
      adapter_luid = best_desc.AdapterLuid;
      // wide → narrow conversion for the model name (ASCII subset is enough)
      char narrow[ARRAYSIZE(best_desc.Description) + 1] {};
      WideCharToMultiByte(CP_UTF8, 0, best_desc.Description, -1, narrow,
                          sizeof(narrow), nullptr, nullptr);
      gpu_model.assign(narrow);
      while (!gpu_model.empty() && (gpu_model.back() == '\0' || gpu_model.back() == ' ')) {
        gpu_model.pop_back();
      }
      best_adapter->Release();
    }

    factory->Release();
  }

  /**
   * @brief NVIDIA NVML temperature query (runtime-loaded).
   *
   * Returns -1.f when nvml.dll is missing or the query fails. Loaded once
   * per process and the loaded module is intentionally leaked at process
   * exit (matches existing nvprefs pattern).
   */
  class nvml_t {
  public:
    static nvml_t &
      instance() {
      static nvml_t inst;
      return inst;
    }

    float
      gpu_temp_c() {
      if (!_ok) {
        return -1.f;
      }
      void *device = nullptr;
      if (_device_get_handle(0, &device) != 0) {
        return -1.f;
      }
      unsigned int temp = 0;
      // NVML_TEMPERATURE_GPU == 0
      if (_device_get_temperature(device, 0, &temp) != 0) {
        return -1.f;
      }
      return static_cast<float>(temp);
    }

  private:
    nvml_t() {
      HMODULE mod = LoadLibraryA("nvml.dll");
      if (!mod) {
        // try the 64-bit driver path
        char buf[MAX_PATH];
        UINT n = GetSystemDirectoryA(buf, sizeof(buf));
        if (n && n < sizeof(buf)) {
          std::string p(buf);
          p += "\\..\\NVSMI\\nvml.dll";
          mod = LoadLibraryA(p.c_str());
        }
      }
      if (!mod) {
        return;
      }
      using init_fn = int (*)();
      using devh_fn = int (*)(unsigned int, void **);
      using temp_fn = int (*)(void *, int, unsigned int *);
      auto init_v2 = reinterpret_cast<init_fn>(GetProcAddress(mod, "nvmlInit_v2"));
      _device_get_handle = reinterpret_cast<devh_fn>(GetProcAddress(mod, "nvmlDeviceGetHandleByIndex_v2"));
      _device_get_temperature = reinterpret_cast<temp_fn>(GetProcAddress(mod, "nvmlDeviceGetTemperature"));
      if (!init_v2 || !_device_get_handle || !_device_get_temperature) {
        return;
      }
      if (init_v2() != 0) {
        return;
      }
      _ok = true;
    }

    bool _ok = false;
    int (*_device_get_handle)(unsigned int, void **) = nullptr;
    int (*_device_get_temperature)(void *, int, unsigned int *) = nullptr;
  };

  class windows_host_stats_t: public platf::host_stats_provider_t {
  public:
    windows_host_stats_t() {
      // CPU baseline
      FILETIME idle, kernel, user;
      if (GetSystemTimes(&idle, &kernel, &user)) {
        _last_idle = to_u64(idle);
        _last_kernel = to_u64(kernel);
        _last_user = to_u64(user);
        _have_cpu_baseline = true;
      }

      // PDH queries
      if (_gpu_3d.open()) {
        _have_gpu_3d = true;
      } else {
        BOOST_LOG(::info) << "host_stats(win): \\GPU Engine(*engtype_3D) counter unavailable";
      }
      if (_gpu_enc.open()) {
        _have_gpu_enc = true;
      } else {
        BOOST_LOG(::info) << "host_stats(win): \\GPU Engine(*engtype_VideoEncode) counter unavailable";
      }
      // static info — DXGI + registry
      query_dxgi(_vram_total_cached, _vram_adapter_luid, _gpu_model_cached);
      _vram_adapter_instance = luid_instance_prefix(_vram_adapter_luid);
      if (_gpu_mem.open()) {
        _have_gpu_mem = true;
      } else {
        BOOST_LOG(::info) << "host_stats(win): \\GPU Process Memory(*) counter unavailable";
      }
      _cpu_model_cached = read_processor_name();

      SYSTEM_INFO si;
      GetSystemInfo(&si);
      _cpu_logical_cores = static_cast<int>(si.dwNumberOfProcessors);

      MEMORYSTATUSEX ms {};
      ms.dwLength = sizeof(ms);
      if (GlobalMemoryStatusEx(&ms)) {
        _ram_total_cached = ms.ullTotalPhys;
      }

      // Network: pick the primary interface up-front so info() can return
      // a non-empty name even before sample() runs.
      refresh_primary_interface();
    }

    platf::host_stats_t
      sample() override {
      platf::host_stats_t s {};

      // CPU
      FILETIME idle, kernel, user;
      if (GetSystemTimes(&idle, &kernel, &user)) {
        auto idle_now = to_u64(idle);
        auto kernel_now = to_u64(kernel);
        auto user_now = to_u64(user);
        if (_have_cpu_baseline) {
          auto idle_d = idle_now - _last_idle;
          auto kernel_d = kernel_now - _last_kernel;
          auto user_d = user_now - _last_user;
          auto total = kernel_d + user_d;  // kernel includes idle
          if (total > 0) {
            double busy = static_cast<double>(total - idle_d) * 100.0 / static_cast<double>(total);
            if (busy < 0.0) {
              busy = 0.0;
            }
            if (busy > 100.0) {
              busy = 100.0;
            }
            s.cpu_percent = static_cast<float>(busy);
          }
        }
        _last_idle = idle_now;
        _last_kernel = kernel_now;
        _last_user = user_now;
        _have_cpu_baseline = true;
      }

      // RAM
      MEMORYSTATUSEX ms {};
      ms.dwLength = sizeof(ms);
      if (GlobalMemoryStatusEx(&ms)) {
        s.ram_total_bytes = ms.ullTotalPhys;
        s.ram_used_bytes = ms.ullTotalPhys - ms.ullAvailPhys;
      }

      // GPU
      if (_have_gpu_3d) {
        s.gpu_percent = _gpu_3d.collect(100.f);
      }
      if (_have_gpu_enc) {
        s.gpu_encoder_percent = _gpu_enc.collect(100.f);
      }
      s.vram_total_bytes = _vram_total_cached;
      if (_have_gpu_mem) {
        s.vram_used_bytes = collect_vram_used_bytes(_gpu_mem, _vram_adapter_instance, _vram_total_cached);
      }

      // Temps (best effort, NVIDIA only via NVML)
      s.gpu_temp_c = nvml_t::instance().gpu_temp_c();

      // Network throughput (delta over wall-clock time on the primary interface).
      sample_network(s);

      return s;
    }

    platf::host_info_t
      info() override {
      platf::host_info_t i;
      i.cpu_model = _cpu_model_cached;
      i.gpu_model = _gpu_model_cached;
      i.cpu_logical_cores = _cpu_logical_cores;
      i.ram_total_bytes = _ram_total_cached;
      i.vram_total_bytes = _vram_total_cached;
      i.net_interface = _net_iface_name;
      i.net_link_speed_mbps = _net_link_speed_mbps;
      return i;
    }

  private:
    // -- Network sampling ------------------------------------------------------

    /**
     * @brief Pick the primary outbound interface (the one carrying the
     * default route) and remember its LUID + friendly name for sampling.
     *
     * Falls back to the first non-loopback up interface if route lookup
     * fails.
     */
    void
      refresh_primary_interface() {
      _net_have_iface = false;
      _net_iface_name.clear();
      _net_link_speed_mbps = 0;

      NET_LUID luid {};
      if (!find_default_route_luid(luid)) {
        // fallback: first non-loopback interface that is up
        if (!find_first_up_interface_luid(luid)) {
          return;
        }
      }

      // Resolve LUID to friendly name + link speed via MIB_IF_ROW2.
      MIB_IF_ROW2 row {};
      row.InterfaceLuid = luid;
      if (GetIfEntry2(&row) != NO_ERROR) {
        return;
      }
      char narrow[ARRAYSIZE(row.Alias) + 1] {};
      WideCharToMultiByte(CP_UTF8, 0, row.Alias, -1, narrow, sizeof(narrow), nullptr, nullptr);
      _net_iface_name.assign(narrow);
      // ReceiveLinkSpeed is in bits/sec; report Mbps.
      _net_link_speed_mbps = row.ReceiveLinkSpeed / 1'000'000ULL;
      _net_iface_luid = luid;
      _net_have_iface = true;
      _net_iface_chosen_at = std::chrono::steady_clock::now();
    }

    static bool
      find_default_route_luid(NET_LUID &out_luid) {
      // Use GetBestInterfaceEx with a dummy public destination; gives us
      // the IF index of the route that would be taken to reach the
      // internet, which is what users typically care about.
      sockaddr_in dst {};
      dst.sin_family = AF_INET;
      dst.sin_addr.s_addr = htonl(0x08080808);  // 8.8.8.8
      DWORD if_index = 0;
      if (GetBestInterfaceEx(reinterpret_cast<sockaddr *>(&dst), &if_index) != NO_ERROR) {
        return false;
      }
      return ConvertInterfaceIndexToLuid(if_index, &out_luid) == NO_ERROR;
    }

    static bool
      find_first_up_interface_luid(NET_LUID &out_luid) {
      MIB_IF_TABLE2 *table = nullptr;
      if (GetIfTable2(&table) != NO_ERROR || !table) {
        return false;
      }
      bool found = false;
      for (ULONG i = 0; i < table->NumEntries; ++i) {
        const auto &row = table->Table[i];
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) {
          continue;
        }
        if (row.OperStatus != IfOperStatusUp) {
          continue;
        }
        out_luid = row.InterfaceLuid;
        found = true;
        break;
      }
      FreeMibTable(table);
      return found;
    }

    void
      sample_network(platf::host_stats_t &s) {
      using clock = std::chrono::steady_clock;

      // Re-pick the interface periodically so we adapt to e.g. a VPN
      // coming up or a cable being unplugged.
      auto now = clock::now();
      if (!_net_have_iface ||
          now - _net_iface_chosen_at > std::chrono::seconds(30)) {
        refresh_primary_interface();
      }
      if (!_net_have_iface) {
        s.net_rx_bps = -1.0;
        s.net_tx_bps = -1.0;
        return;
      }

      MIB_IF_ROW2 row {};
      row.InterfaceLuid = _net_iface_luid;
      if (GetIfEntry2(&row) != NO_ERROR) {
        s.net_rx_bps = -1.0;
        s.net_tx_bps = -1.0;
        return;
      }

      auto rx = row.InOctets;
      auto tx = row.OutOctets;

      if (!_have_net_baseline) {
        _last_rx = rx;
        _last_tx = tx;
        _last_net_sample_at = now;
        _have_net_baseline = true;
        // First sample after start: report 0 (not -1) so the chart shows
        // a defined baseline rather than a gap.
        s.net_rx_bps = 0.0;
        s.net_tx_bps = 0.0;
        return;
      }

      double dt = std::chrono::duration<double>(now - _last_net_sample_at).count();
      if (dt <= 0.0) {
        s.net_rx_bps = 0.0;
        s.net_tx_bps = 0.0;
        return;
      }
      // Counters wrap around modulo 2^64; the unsigned subtraction does
      // the right thing if the wrap is small enough that one tick fits.
      auto drx = rx - _last_rx;
      auto dtx = tx - _last_tx;
      s.net_rx_bps = static_cast<double>(drx) * 8.0 / dt;
      s.net_tx_bps = static_cast<double>(dtx) * 8.0 / dt;

      _last_rx = rx;
      _last_tx = tx;
      _last_net_sample_at = now;
    }
    bool _have_cpu_baseline = false;
    filetime_u64_t _last_idle = 0;
    filetime_u64_t _last_kernel = 0;
    filetime_u64_t _last_user = 0;

    pdh_wildcard_sum_t _gpu_3d {L"\\GPU Engine(*engtype_3D)\\Utilization Percentage"};
    pdh_wildcard_sum_t _gpu_enc {L"\\GPU Engine(*engtype_VideoEncode)\\Utilization Percentage"};
    pdh_wildcard_sum_t _gpu_mem {L"\\GPU Process Memory(*)\\Dedicated Usage"};
    bool _have_gpu_3d = false;
    bool _have_gpu_enc = false;
    bool _have_gpu_mem = false;

    std::string _cpu_model_cached;
    std::string _gpu_model_cached;
    int _cpu_logical_cores = 0;
    std::uint64_t _ram_total_cached = 0;
    std::uint64_t _vram_total_cached = 0;
    LUID _vram_adapter_luid {};
    std::wstring _vram_adapter_instance;

    // Network state
    bool _net_have_iface = false;
    NET_LUID _net_iface_luid {};
    std::string _net_iface_name;
    std::uint64_t _net_link_speed_mbps = 0;
    std::chrono::steady_clock::time_point _net_iface_chosen_at {};
    bool _have_net_baseline = false;
    std::uint64_t _last_rx = 0;
    std::uint64_t _last_tx = 0;
    std::chrono::steady_clock::time_point _last_net_sample_at {};
  };

}  // namespace

namespace platf {

  std::unique_ptr<host_stats_provider_t>
    create_host_stats_provider() {
    return std::make_unique<windows_host_stats_t>();
  }

}  // namespace platf
