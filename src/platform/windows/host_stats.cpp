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
 * VRAM used   : Tries vendor-agnostic sources first. The picked source is
 *               logged once on the first successful sample so it's visible
 *               in the runtime log.
 *               Order:
 *                 1) PDH @c "\GPU Adapter Memory(*)\Dedicated Usage" filtered
 *                    to the DXGI adapter LUID — system-wide, vendor-agnostic
 *                    (same source Task Manager uses). Note: on some Windows
 *                    builds these instances are not published to session 0
 *                    (LocalSystem) and the counter stays at 0; in that case
 *                    we fall through.
 *                 2) PDH @c "\GPU Process Memory(*)\Dedicated Usage" summed
 *                    by adapter LUID and clamped to total VRAM.
 *                 3) D3DKMT @c D3DKMTQueryStatistics segment residency summed
 *                    over non-aperture segments — vendor-agnostic, kernel
 *                    mediated, and safe to call while the GPU is in TDR.
 *                 4) NVML @c nvmlDeviceGetMemoryInfo (NVIDIA only), but only
 *                    until D3DKMT delivers its first sample; see below.
 * VRAM total  : DXGI @c IDXGIAdapter::GetDesc.
 * GPU temp    : D3DKMT @c KMTQAITYPE_ADAPTERPERFDATA (same source Task
 *               Manager uses, deci-Celsius), NVML as a fallback.
 *
 * NVML is a user-mode shim into the NVIDIA driver and has been observed to
 * AV inside nvml.dll while the driver is mid-TDR-reset (the 2026-06-10
 * incident: GPU hang -> 25 s TDR cycle -> sampler called NVML each second ->
 * 0xc0000005 in nvml.dll killed the whole process while capture/encode were
 * recovering). D3DKMT goes through dxgkrnl, which fails such calls with an
 * NTSTATUS instead of faulting. Policy: once a D3DKMT query has succeeded
 * once, NVML is locked out for the lifetime of the process; it remains only
 * for systems where the D3DKMT path is unavailable.
 *
 * Counters that are not available on the current system are reported as
 * @c -1.f / @c 0 — the FE renders these as "N/A".
 */

#include "src/platform/common.h"

// standard includes
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
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
#include "src/utility.h"

#ifdef _MSC_VER
  #pragma comment(lib, "iphlpapi.lib")
  #pragma comment(lib, "pdh.lib")
  #pragma comment(lib, "dxgi.lib")
  #pragma comment(lib, "ws2_32.lib")
#endif

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

  double clamp_throughput_bps(double bits_per_second, std::uint64_t link_speed_mbps) {
    if (bits_per_second < 0.0 || link_speed_mbps == 0) {
      return bits_per_second;
    }
    const double max_bits_per_second = static_cast<double>(link_speed_mbps) * 1'000'000.0 * 1.10;
    return std::min(bits_per_second, max_bits_per_second);
  }

  /**
   * @brief Collect dedicated VRAM in use for one adapter.
   * @return Bytes; 0 on failure.
   */
  std::uint64_t
    collect_vram_used_bytes(pdh_wildcard_sum_t &mem_query, const std::wstring &adapter_instance, std::uint64_t vram_total_bytes) {
    float v = mem_query.collect(-1.f, adapter_instance);
    if (v < 0.f) {
      return 0;
    }
    auto used = static_cast<std::uint64_t>(v);
    return vram_total_bytes > 0 && used > vram_total_bytes ? vram_total_bytes : used;
  }

  /**
   * @brief Query the best-fit DXGI adapter and return total VRAM bytes,
   *        adapter LUID, and a printable description.
   *
   * On Windows we currently approximate the "host GPU" by selecting the
   * non-software adapter with the largest dedicated VRAM. That is stable and
   * good enough for host telemetry, but on hybrid/eGPU/multi-GPU systems it
   * may not be the exact adapter used by capture or encode for the stream.
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
   * @brief D3DKMT GPU telemetry (temperature + VRAM residency).
   *
   * MinGW ships no d3dkmthk.h, so the structures below are local replicas of
   * the Windows 11 SDK (10.0.26100) definitions. The x64 layout is pinned by
   * static_asserts against offsets measured from the canonical header; this
   * ABI is frozen — existing binaries would break if Microsoft changed it.
   *
   * All calls are kernel-mediated through gdi32/dxgkrnl: while the GPU is
   * hung or mid-TDR they fail with an NTSTATUS instead of faulting, which is
   * the property the sampler needs (see file header).
   */
  namespace kmt {
    using kmt_handle_t = UINT32;  // D3DKMT_HANDLE
    constexpr UINT QAITYPE_ADAPTERPERFDATA = 62;  // KMTQAITYPE_ADAPTERPERFDATA
    constexpr UINT QUERYSTATISTICS_ADAPTER = 0;  // D3DKMT_QUERYSTATISTICS_ADAPTER
    constexpr UINT QUERYSTATISTICS_SEGMENT = 3;  // D3DKMT_QUERYSTATISTICS_SEGMENT

    struct open_adapter_from_luid_t {  // D3DKMT_OPENADAPTERFROMLUID
      LUID adapter_luid;
      kmt_handle_t adapter;
    };

    static_assert(sizeof(open_adapter_from_luid_t) == 0xc);

    struct close_adapter_t {  // D3DKMT_CLOSEADAPTER
      kmt_handle_t adapter;
    };

    struct query_adapter_info_t {  // D3DKMT_QUERYADAPTERINFO
      kmt_handle_t adapter;
      UINT type;
      VOID *private_driver_data;
      UINT private_driver_data_size;
    };

    static_assert(sizeof(query_adapter_info_t) == 0x18);

    struct adapter_perfdata_t {  // D3DKMT_ADAPTER_PERFDATA
      UINT32 physical_adapter_index;  // in
      ULONGLONG memory_frequency;
      ULONGLONG max_memory_frequency;
      ULONGLONG max_memory_frequency_oc;
      ULONGLONG memory_bandwidth;
      ULONGLONG pcie_bandwidth;
      ULONG fan_rpm;
      ULONG power;
      ULONG temperature;  // deci-Celsius, 1 = 0.1 C
      UCHAR power_state_override;
    };

    static_assert(sizeof(adapter_perfdata_t) == 0x40);
    static_assert(offsetof(adapter_perfdata_t, fan_rpm) == 0x30);
    static_assert(offsetof(adapter_perfdata_t, temperature) == 0x38);

    struct query_statistics_segment_information_t {  // D3DKMT_QUERYSTATISTICS_SEGMENT_INFORMATION
      ULONGLONG commit_limit;
      ULONGLONG bytes_committed;
      ULONGLONG bytes_resident;

      struct {  // D3DKMT_QUERYSTATISTICS_MEMORY
        ULONGLONG total_bytes_evicted;
        ULONG allocs_committed;
        ULONG allocs_resident;
      } memory;

      ULONG aperture;  // boolean
      ULONGLONG total_bytes_evicted_by_priority[5];
      UINT64 system_memory_end_address;
      UINT64 power_flags;
      UINT64 segment_properties;
      UINT64 reserved[5];
    };

    static_assert(sizeof(query_statistics_segment_information_t) == 0x98);
    static_assert(offsetof(query_statistics_segment_information_t, aperture) == 0x28);

    struct query_statistics_t {  // D3DKMT_QUERYSTATISTICS
      UINT type;  // in
      LUID adapter_luid;  // in
      HANDLE process;  // in
      union {  // out
        struct {
          ULONG nb_segments;
          ULONG node_count;
        } adapter_information;  // prefix of D3DKMT_QUERYSTATISTICS_ADAPTER_INFORMATION
        query_statistics_segment_information_t segment_information;
        UCHAR result_storage[0x308];  // sizeof(D3DKMT_QUERYSTATISTICS_RESULT)
      } query_result;
      union {  // in
        ULONG segment_id;  // D3DKMT_QUERYSTATISTICS_QUERY_SEGMENT
        UCHAR query_storage[8];
      };
    };

    static_assert(sizeof(query_statistics_t) == 0x328);
    static_assert(offsetof(query_statistics_t, process) == 0x10);
    static_assert(offsetof(query_statistics_t, query_result) == 0x18);
    static_assert(offsetof(query_statistics_t, segment_id) == 0x320);

    using pfn_open_adapter_from_luid = LONG(APIENTRY *)(open_adapter_from_luid_t *);
    using pfn_close_adapter = LONG(APIENTRY *)(const close_adapter_t *);
    using pfn_query_adapter_info = LONG(APIENTRY *)(const query_adapter_info_t *);
    using pfn_query_statistics = LONG(APIENTRY *)(const query_statistics_t *);
  }  // namespace kmt

  class d3dkmt_gpu_stats_t {
  public:
    struct vram_t {
      std::uint64_t used = 0;
      std::uint64_t total = 0;
    };

    d3dkmt_gpu_stats_t() {
      HMODULE gdi32 = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!gdi32) {
        return;
      }
      _open = reinterpret_cast<kmt::pfn_open_adapter_from_luid>(GetProcAddress(gdi32, "D3DKMTOpenAdapterFromLuid"));
      _close = reinterpret_cast<kmt::pfn_close_adapter>(GetProcAddress(gdi32, "D3DKMTCloseAdapter"));
      _query_info = reinterpret_cast<kmt::pfn_query_adapter_info>(GetProcAddress(gdi32, "D3DKMTQueryAdapterInfo"));
      _query_stats = reinterpret_cast<kmt::pfn_query_statistics>(GetProcAddress(gdi32, "D3DKMTQueryStatistics"));
      // gdi32 stays loaded for the process lifetime; no FreeLibrary.
    }

    ~d3dkmt_gpu_stats_t() {
      close_adapter();
    }

    d3dkmt_gpu_stats_t(const d3dkmt_gpu_stats_t &) = delete;
    d3dkmt_gpu_stats_t &operator=(const d3dkmt_gpu_stats_t &) = delete;

    void
      set_adapter_luid(const LUID &luid) {
      if (_have_luid && luid.LowPart == _luid.LowPart && luid.HighPart == _luid.HighPart) {
        return;
      }
      close_adapter();
      _luid = luid;
      _have_luid = luid.LowPart != 0 || luid.HighPart != 0;
    }

    /**
     * @brief True once any D3DKMT temperature query has succeeded.
     *
     * Used to lock NVML out of the temperature path.
     */
    bool
      perfdata_works() const {
      return _perfdata_ok_once;
    }

    /**
     * @brief True once any D3DKMT VRAM query has succeeded.
     *
     * Used to lock NVML out of the VRAM path.
     */
    bool
      statistics_work() const {
      return _stats_ok_once;
    }

    /**
     * @brief True when the most recent query failed — e.g. the adapter LUID
     * went stale after a full driver restart. The owner may re-enumerate DXGI
     * and call @ref set_adapter_luid with the fresh LUID.
     */
    bool
      last_query_failed() const {
      return _last_query_failed;
    }

    float
      gpu_temp_c() {
      if (!_open || !_close || !_query_info || !_have_luid) {
        return -1.f;
      }
      if (!ensure_adapter()) {
        _last_query_failed = true;
        return -1.f;
      }
      kmt::adapter_perfdata_t perfdata {};
      kmt::query_adapter_info_t query {};
      query.adapter = _adapter;
      query.type = kmt::QAITYPE_ADAPTERPERFDATA;
      query.private_driver_data = &perfdata;
      query.private_driver_data_size = sizeof(perfdata);
      if (_query_info(&query) < 0) {
        // Fails while the GPU is in TDR or after a driver restart invalidated
        // the handle; drop the handle so the next sample reopens it.
        close_adapter();
        _last_query_failed = true;
        return -1.f;
      }
      _last_query_failed = false;
      if (perfdata.temperature == 0) {
        return -1.f;
      }
      _perfdata_ok_once = true;
      return static_cast<float>(perfdata.temperature) / 10.f;
    }

    vram_t
      gpu_vram() {
      vram_t out {};
      if (!_query_stats || !_have_luid) {
        return out;
      }
      kmt::query_statistics_t adapter_query {};
      adapter_query.type = kmt::QUERYSTATISTICS_ADAPTER;
      adapter_query.adapter_luid = _luid;
      if (_query_stats(&adapter_query) < 0) {
        _last_query_failed = true;
        return out;
      }
      const ULONG segment_count = std::min<ULONG>(adapter_query.query_result.adapter_information.nb_segments, 16);
      for (ULONG segment = 0; segment < segment_count; ++segment) {
        kmt::query_statistics_t segment_query {};
        segment_query.type = kmt::QUERYSTATISTICS_SEGMENT;
        segment_query.adapter_luid = _luid;
        segment_query.segment_id = segment;
        if (_query_stats(&segment_query) < 0) {
          continue;
        }
        const auto &info = segment_query.query_result.segment_information;
        if (info.aperture) {
          continue;
        }
        out.used += info.bytes_resident;
        out.total += info.commit_limit;
      }
      _last_query_failed = false;
      if (out.used > 0 || out.total > 0) {
        _stats_ok_once = true;
      }
      return out;
    }

  private:
    bool
      ensure_adapter() {
      if (_adapter) {
        return true;
      }
      kmt::open_adapter_from_luid_t open {};
      open.adapter_luid = _luid;
      if (_open(&open) < 0) {
        return false;
      }
      _adapter = open.adapter;
      return true;
    }

    void
      close_adapter() {
      if (_adapter && _close) {
        kmt::close_adapter_t close {_adapter};
        _close(&close);
      }
      _adapter = 0;
    }

    kmt::pfn_open_adapter_from_luid _open = nullptr;
    kmt::pfn_close_adapter _close = nullptr;
    kmt::pfn_query_adapter_info _query_info = nullptr;
    kmt::pfn_query_statistics _query_stats = nullptr;
    kmt::kmt_handle_t _adapter = 0;
    LUID _luid {};
    bool _have_luid = false;
    bool _perfdata_ok_once = false;
    bool _stats_ok_once = false;
    bool _last_query_failed = false;
  };

  /**
   * @brief NVIDIA NVML temperature query (runtime-loaded).
   *
   * Returns -1.f when nvml.dll is missing or the query fails. The DLL is
   * loaded only from trusted locations to avoid unsafe search-order behavior.
   */
  class nvml_t {
  public:
    struct memory_info_t {
      std::uint64_t total = 0;
      std::uint64_t used = 0;
    };
    struct utilization_info_t {
      float gpu = -1.f;
      float encoder = -1.f;
    };

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

    utilization_info_t
      gpu_utilization() {
      utilization_info_t rates {};
      if (!_ok) {
        return rates;
      }
      void *device = nullptr;
      // Just query device 0, same as gpu_temp_c()
      if (_device_get_handle(0, &device) != 0) {
        return rates;
      }

      if (_device_get_utilization_rates) {
        nvml_utilization_t util {};
        if (_device_get_utilization_rates(device, &util) == 0) {
          rates.gpu = static_cast<float>(util.gpu);
        }
      }

      if (_device_get_encoder_utilization) {
        unsigned int utilization = 0;
        unsigned int sampling_period_us = 0;
        if (_device_get_encoder_utilization(device, &utilization, &sampling_period_us) == 0) {
          rates.encoder = static_cast<float>(utilization);
        }
      }
      return rates;
    }

    memory_info_t
      gpu_memory(std::uint64_t preferred_total_bytes) {
      memory_info_t best {};
      if (!_ok || !_device_get_memory_info) {
        return best;
      }

      unsigned int count = 1;
      if (_device_get_count) {
        if (_device_get_count(&count) != 0 || count == 0) {
          count = 1;
        }
      }

      for (unsigned int i = 0; i < count; ++i) {
        void *device = nullptr;
        if (_device_get_handle(i, &device) != 0) {
          continue;
        }
        nvml_memory_t mem {};
        if (_device_get_memory_info(device, &mem) != 0) {
          continue;
        }

        memory_info_t current {
          static_cast<std::uint64_t>(mem.total),
          static_cast<std::uint64_t>(mem.used),
        };
        if (current.total == 0) {
          continue;
        }
        if (preferred_total_bytes > 0) {
          const auto delta = current.total > preferred_total_bytes ? current.total - preferred_total_bytes : preferred_total_bytes - current.total;
          if (delta < 256ULL * 1024ULL * 1024ULL) {
            return current;
          }
        }
        if (current.total > best.total) {
          best = current;
        }
      }

      return best;
    }

  private:
    struct nvml_memory_t {
      unsigned long long total;
      unsigned long long free;
      unsigned long long used;
    };

    struct nvml_utilization_t {
      unsigned int gpu;
      unsigned int memory;
    };

    static HMODULE
      load_module() {
      if (HMODULE mod = LoadLibraryExA("nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        return mod;
      }

      const auto registry_path = read_registry_nvml_path();
      if (!registry_path.empty() && is_trusted_nvml_path(registry_path)) {
        return LoadLibraryExA(
          registry_path.c_str(),
          nullptr,
          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
      }

      return nullptr;
    }

    static std::string read_registry_nvml_path() {
      HKEY key = nullptr;
      const LONG open_status = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\NVIDIA Corporation\\Global\\NVSMI",
        0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY,
        &key);
      if (open_status != ERROR_SUCCESS) {
        return {};
      }
      auto close_key = util::fail_guard([key]() {
        RegCloseKey(key);
      });

      char value[MAX_PATH] {};
      DWORD type = 0;
      DWORD size = sizeof(value);
      const LONG query_status = RegQueryValueExA(
        key,
        "NVSMIPATH",
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(value),
        &size);
      if (query_status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
      }

      std::string path(value);
      while (!path.empty() && path.back() == '\0') {
        path.pop_back();
      }
      if (path.empty()) {
        return {};
      }

      std::filesystem::path candidate {path};
      if (_stricmp(candidate.filename().string().c_str(), "nvml.dll") != 0) {
        candidate /= "nvml.dll";
      }
      return candidate.string();
    }

    static bool is_trusted_nvml_path(const std::string &path) {
      const std::filesystem::path candidate {path};
      if (!candidate.is_absolute()) {
        BOOST_LOG(warning) << "host_stats(win): ignoring non-absolute NVML registry path";
        return false;
      }
      if (_stricmp(candidate.filename().string().c_str(), "nvml.dll") != 0) {
        BOOST_LOG(warning) << "host_stats(win): ignoring NVML registry path with unexpected filename";
        return false;
      }

      const DWORD attributes = GetFileAttributesA(path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES ||
          (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        BOOST_LOG(warning) << "host_stats(win): ignoring NVML registry path with unsafe file attributes";
        return false;
      }

      return true;
    }

    nvml_t() {
      _module = load_module();
      if (!_module) {
        return;
      }
      using init_fn = int (*)();
      using count_fn = int (*)(unsigned int *);
      using devh_fn = int (*)(unsigned int, void **);
      using temp_fn = int (*)(void *, int, unsigned int *);
      using mem_fn = int (*)(void *, nvml_memory_t *);
      using util_fn = int (*)(void *, nvml_utilization_t *);
      using enc_util_fn = int (*)(void *, unsigned int *, unsigned int *);
      auto init_v2 = reinterpret_cast<init_fn>(GetProcAddress(_module, "nvmlInit_v2"));
      _device_get_count = reinterpret_cast<count_fn>(GetProcAddress(_module, "nvmlDeviceGetCount_v2"));
      _device_get_handle = reinterpret_cast<devh_fn>(GetProcAddress(_module, "nvmlDeviceGetHandleByIndex_v2"));
      _device_get_temperature = reinterpret_cast<temp_fn>(GetProcAddress(_module, "nvmlDeviceGetTemperature"));
      _device_get_memory_info = reinterpret_cast<mem_fn>(GetProcAddress(_module, "nvmlDeviceGetMemoryInfo"));
      _device_get_utilization_rates = reinterpret_cast<util_fn>(GetProcAddress(_module, "nvmlDeviceGetUtilizationRates"));
      _device_get_encoder_utilization = reinterpret_cast<enc_util_fn>(GetProcAddress(_module, "nvmlDeviceGetEncoderUtilization"));
      if (!init_v2 || !_device_get_handle || !_device_get_temperature) {
        FreeLibrary(_module);
        _module = nullptr;
        return;
      }
      if (init_v2() != 0) {
        FreeLibrary(_module);
        _module = nullptr;
        return;
      }
      _ok = true;
    }

    ~nvml_t() {
      if (_module) {
        FreeLibrary(_module);
      }
    }

    bool _ok = false;
    HMODULE _module = nullptr;
    int (*_device_get_count)(unsigned int *) = nullptr;
    int (*_device_get_handle)(unsigned int, void **) = nullptr;
    int (*_device_get_temperature)(void *, int, unsigned int *) = nullptr;
    int (*_device_get_utilization_rates)(void *, nvml_utilization_t *) = nullptr;
    int (*_device_get_encoder_utilization)(void *, unsigned int *, unsigned int *) = nullptr;
    int (*_device_get_memory_info)(void *, nvml_memory_t *) = nullptr;
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
      if (_vram_total_cached > 0) {
        _vram_adapter_instance = luid_instance_prefix(_vram_adapter_luid);
      }
      _kmt.set_adapter_luid(_vram_adapter_luid);
      if (!_vram_adapter_instance.empty() && _gpu_adapter_mem.open()) {
        _have_gpu_adapter_mem = true;
      } else if (!_vram_adapter_instance.empty()) {
        BOOST_LOG(::info) << "host_stats(win): \\GPU Adapter Memory(*) counter unavailable";
      }
      if (!_vram_adapter_instance.empty() && _gpu_mem.open()) {
        _have_gpu_mem = true;
      } else if (!_vram_adapter_instance.empty()) {
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
      const char *gpu_source = nullptr;
      const char *enc_source = nullptr;

      if (_have_gpu_3d) {
        s.gpu_percent = _gpu_3d.collect(100.f);
        if (s.gpu_percent > 0.f) {
          gpu_source = "pdh";
        }
      }
      if (_have_gpu_enc) {
        s.gpu_encoder_percent = _gpu_enc.collect(100.f);
        if (s.gpu_encoder_percent > 0.f) {
          enc_source = "pdh";
        }
      }
      // Fallback for GPU usage via NVML if PDH counters are zero/unavailable.
      if (gpu_source == nullptr || enc_source == nullptr) {
        auto nvml_util = nvml_t::instance().gpu_utilization();
        if (gpu_source == nullptr && nvml_util.gpu >= 0.f) {
          s.gpu_percent = nvml_util.gpu;
          gpu_source = "nvml";
        }
        if (enc_source == nullptr && nvml_util.encoder >= 0.f) {
          s.gpu_encoder_percent = nvml_util.encoder;
          enc_source = "nvml";
        }
      }

      if (gpu_source && (!_logged_gpu_source || _last_gpu_source != gpu_source)) {
        _logged_gpu_source = true;
        _last_gpu_source = gpu_source;
        BOOST_LOG(::info) << "host_stats(win): gpu usage source = " << gpu_source;
      }
      if (enc_source && (!_logged_enc_source || _last_enc_source != enc_source)) {
        _logged_enc_source = true;
        _last_enc_source = enc_source;
        BOOST_LOG(::info) << "host_stats(win): gpu encoder usage source = " << enc_source;
      }

      s.vram_total_bytes = _vram_total_cached;
      const char *vram_source = nullptr;

      // Primary: vendor-agnostic, system-wide adapter memory counter
      // (same one Task Manager uses for "Dedicated GPU memory").
      if (_have_gpu_adapter_mem) {
        auto v = collect_vram_used_bytes(_gpu_adapter_mem, _vram_adapter_instance, _vram_total_cached);
        if (v > 0) {
          s.vram_used_bytes = v;
          vram_source = "pdh-adapter";
        }
      }

      // Fallback A: vendor-agnostic per-process memory summed by adapter LUID.
      if (s.vram_used_bytes == 0 && _have_gpu_mem) {
        auto v = collect_vram_used_bytes(_gpu_mem, _vram_adapter_instance, _vram_total_cached);
        if (v > 0) {
          s.vram_used_bytes = v;
          vram_source = "pdh-process";
        }
      }

      // Fallback B: D3DKMT segment residency — vendor-agnostic and TDR-safe.
      if (s.vram_used_bytes == 0) {
        auto kmt_memory = _kmt.gpu_vram();
        if (kmt_memory.used > 0) {
          s.vram_used_bytes = _vram_total_cached > 0 && kmt_memory.used > _vram_total_cached
                                ? _vram_total_cached
                                : kmt_memory.used;
          if (s.vram_total_bytes == 0) {
            s.vram_total_bytes = kmt_memory.total;
          }
          vram_source = "d3dkmt";
        }
      }

      // Fallback C: NVIDIA-only NVML — locked out for good once D3DKMT has
      // worked, because NVML can AV in-process while the driver is in TDR.
      if (s.vram_used_bytes == 0 && !_kmt.statistics_work()) {
        auto nvml_memory = nvml_t::instance().gpu_memory(_vram_total_cached);
        if (nvml_memory.used > 0) {
          s.vram_used_bytes = _vram_total_cached > 0 && nvml_memory.used > _vram_total_cached
                                ? _vram_total_cached
                                : nvml_memory.used;
          if (s.vram_total_bytes == 0) {
            s.vram_total_bytes = nvml_memory.total;
          }
          vram_source = "nvml";
        }
      }

      if (vram_source && (!_logged_vram_source || _last_vram_source != vram_source)) {
        _logged_vram_source = true;
        _last_vram_source = vram_source;
        BOOST_LOG(::info)
          << "host_stats(win): vram source = " << vram_source
          << " (used=" << (s.vram_used_bytes / (1024ULL * 1024ULL))
          << " MiB, total=" << (s.vram_total_bytes / (1024ULL * 1024ULL))
          << " MiB, instance='" << std::string(_vram_adapter_instance.begin(), _vram_adapter_instance.end()) << "')";
      }

      // Temps (best effort): D3DKMT perfdata first, NVML only on systems
      // where perfdata has never worked (pre-WDDM 2.4).
      s.gpu_temp_c = _kmt.gpu_temp_c();
      if (s.gpu_temp_c < 0.f && !_kmt.perfdata_works()) {
        s.gpu_temp_c = nvml_t::instance().gpu_temp_c();
      }

      // A full driver restart (as opposed to a plain TDR reset) hands the
      // adapter a new LUID, leaving every D3DKMT/PDH query aimed at a stale
      // one. When D3DKMT keeps failing, re-enumerate DXGI occasionally and
      // re-target the fresh LUID.
      if (_kmt.last_query_failed()) {
        auto now = std::chrono::steady_clock::now();
        if (now - _last_adapter_reenum > std::chrono::seconds(30)) {
          _last_adapter_reenum = now;
          std::uint64_t vram_total = 0;
          LUID adapter_luid {};
          std::string gpu_model;
          query_dxgi(vram_total, adapter_luid, gpu_model);
          if (vram_total > 0) {
            _vram_total_cached = vram_total;
            _vram_adapter_luid = adapter_luid;
            _gpu_model_cached = gpu_model;
            _vram_adapter_instance = luid_instance_prefix(adapter_luid);
            _kmt.set_adapter_luid(adapter_luid);
          }
        }
      }

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
      const bool had_iface = _net_have_iface;
      const auto previous_luid = _net_iface_luid;
      _net_have_iface = false;
      _net_iface_name.clear();
      _net_link_speed_mbps = 0;

      NET_LUID luid {};
      if (!find_default_route_luid(luid)) {
        // fallback: first non-loopback interface that is up
        if (!find_first_up_interface_luid(luid)) {
          if (had_iface) {
            _have_net_baseline = false;
          }
          return;
        }
      }

      // Resolve LUID to friendly name + link speed via MIB_IF_ROW2.
      MIB_IF_ROW2 row {};
      row.InterfaceLuid = luid;
      if (GetIfEntry2(&row) != NO_ERROR) {
        if (had_iface) {
          _have_net_baseline = false;
        }
        return;
      }
      char narrow[ARRAYSIZE(row.Alias) + 1] {};
      WideCharToMultiByte(CP_UTF8, 0, row.Alias, -1, narrow, sizeof(narrow), nullptr, nullptr);
      _net_iface_name.assign(narrow);
      // ReceiveLinkSpeed is in bits/sec; report Mbps.
      _net_link_speed_mbps = row.ReceiveLinkSpeed / 1'000'000ULL;
      if (had_iface && previous_luid.Value != luid.Value) {
        _have_net_baseline = false;
      }
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
      if (rx < _last_rx || tx < _last_tx) {
        _last_rx = rx;
        _last_tx = tx;
        _last_net_sample_at = now;
        s.net_rx_bps = 0.0;
        s.net_tx_bps = 0.0;
        return;
      }
      auto drx = rx - _last_rx;
      auto dtx = tx - _last_tx;
      s.net_rx_bps = clamp_throughput_bps(static_cast<double>(drx) * 8.0 / dt, _net_link_speed_mbps);
      s.net_tx_bps = clamp_throughput_bps(static_cast<double>(dtx) * 8.0 / dt, _net_link_speed_mbps);

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
    pdh_wildcard_sum_t _gpu_adapter_mem {L"\\GPU Adapter Memory(*)\\Dedicated Usage"};
    pdh_wildcard_sum_t _gpu_mem {L"\\GPU Process Memory(*)\\Dedicated Usage"};
    bool _have_gpu_3d = false;
    bool _have_gpu_enc = false;
    bool _have_gpu_adapter_mem = false;
    bool _have_gpu_mem = false;
    bool _logged_vram_source = false;
    std::string _last_vram_source;
    bool _logged_gpu_source = false;
    std::string _last_gpu_source;
    bool _logged_enc_source = false;
    std::string _last_enc_source;

    std::string _cpu_model_cached;
    std::string _gpu_model_cached;
    int _cpu_logical_cores = 0;
    std::uint64_t _ram_total_cached = 0;
    std::uint64_t _vram_total_cached = 0;
    LUID _vram_adapter_luid {};
    std::wstring _vram_adapter_instance;
    d3dkmt_gpu_stats_t _kmt;
    std::chrono::steady_clock::time_point _last_adapter_reenum {};

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
