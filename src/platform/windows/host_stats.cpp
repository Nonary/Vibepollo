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
 * VRAM used   : PDH @c "\GPU Process Memory(*)\Dedicated Usage" (sum across
 *               processes).
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
#include <vector>

// platform includes
#include <windows.h>

#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

// local includes
#include "src/logging.h"

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")

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
      collect(float max_value = -1.f) {
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

  /**
   * @brief Collect total dedicated VRAM in use across all GPU processes.
   * @return Bytes; 0 on failure.
   */
  std::uint64_t
    collect_vram_used_bytes(pdh_wildcard_sum_t &mem_query) {
    float v = mem_query.collect();
    if (v < 0.f) {
      return 0;
    }
    return static_cast<std::uint64_t>(v);
  }

  /**
   * @brief Query the primary DXGI adapter and return total VRAM bytes
   *        plus a printable description.
   */
  void
    query_dxgi(std::uint64_t &vram_total_bytes, std::string &gpu_model) {
    vram_total_bytes = 0;
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
      if (_gpu_mem.open()) {
        _have_gpu_mem = true;
      } else {
        BOOST_LOG(::info) << "host_stats(win): \\GPU Process Memory(*) counter unavailable";
      }

      // static info — DXGI + registry
      query_dxgi(_vram_total_cached, _gpu_model_cached);
      _cpu_model_cached = read_processor_name();

      SYSTEM_INFO si;
      GetSystemInfo(&si);
      _cpu_logical_cores = static_cast<int>(si.dwNumberOfProcessors);

      MEMORYSTATUSEX ms {};
      ms.dwLength = sizeof(ms);
      if (GlobalMemoryStatusEx(&ms)) {
        _ram_total_cached = ms.ullTotalPhys;
      }
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
      if (_have_gpu_mem) {
        s.vram_used_bytes = collect_vram_used_bytes(_gpu_mem);
      }
      s.vram_total_bytes = _vram_total_cached;

      // Temps (best effort, NVIDIA only via NVML)
      s.gpu_temp_c = nvml_t::instance().gpu_temp_c();

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
      return i;
    }

  private:
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
  };

}  // namespace

namespace platf {

  std::unique_ptr<host_stats_provider_t>
    create_host_stats_provider() {
    return std::make_unique<windows_host_stats_t>();
  }

}  // namespace platf
