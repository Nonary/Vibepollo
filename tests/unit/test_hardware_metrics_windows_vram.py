#!/usr/bin/env python3
"""Execute production Windows KMT collection, VRAM fallback and GPU association.

Windows types and dynamically loaded OS functions are fixture boundaries. The
actual KMT layout/class and fallback/association blocks are compiled unchanged;
this is portable adapter behavior evidence, not a Windows SDK or device run.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"
source = (root / "src/platform/windows/host_stats.cpp").read_text()


def between(first, last):
    begin = source.index(first)
    return source[begin:source.index(last, begin)]


kmt_types = between("  namespace kmt {", "  class d3dkmt_gpu_stats_t {")
kmt_class = between("  class d3dkmt_gpu_stats_t {", "  /**\n   * @brief NVIDIA NVML temperature")
fallback = between("      // Fallback B: D3DKMT segment residency", "      if (vram_source && (!_logged_vram_source")
association = between('      // NVML fallbacks are not matched to the DXGI adapter;', "      s.gpu_temp_c = _kmt.gpu_temp_c();")
program = r'''
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include "src/host_stats_types.h"
using UINT = std::uint32_t;
using UINT32 = std::uint32_t;
using ULONG = std::uint32_t;
using UINT64 = std::uint64_t;
using ULONGLONG = std::uint64_t;
using UCHAR = unsigned char;
using LONG = std::int32_t;
using VOID = void;
using HANDLE = void *;
using HMODULE = void *;
struct LUID { std::uint32_t LowPart; std::int32_t HighPart; };
#define APIENTRY
constexpr int LOAD_LIBRARY_SEARCH_SYSTEM32 = 0;
HMODULE LoadLibraryExW(const wchar_t *, void *, int) { return reinterpret_cast<void *>(1); }
void *GetProcAddress(HMODULE, const char *);
''' + kmt_types + r'''
enum class query_mode_t { zero, positive, failed_adapter, failed_segment, aperture, empty };
query_mode_t mode;
int adapter_calls, segment_calls, nvml_calls;
LONG fake_query(const kmt::query_statistics_t *query) {
  assert(query->adapter_luid.LowPart == 17 && query->adapter_luid.HighPart == 3);
  // OS API fills an out union through the const pointer ABI used by production.
  auto &result = const_cast<kmt::query_statistics_t *>(query)->query_result;
  if (query->type == kmt::QUERYSTATISTICS_ADAPTER) {
    ++adapter_calls;
    if (mode == query_mode_t::failed_adapter) return -1;
    result.adapter_information.nb_segments = 1;
  } else {
    assert(query->type == kmt::QUERYSTATISTICS_SEGMENT && query->segment_id == 0);
    ++segment_calls;
    if (mode == query_mode_t::failed_segment) return -1;
    result.segment_information.aperture = mode == query_mode_t::aperture;
    result.segment_information.commit_limit = mode == query_mode_t::empty ? 0 : 4096;
    result.segment_information.bytes_resident = mode == query_mode_t::positive ? 512 : 0;
  }
  return 0;
}
void *GetProcAddress(HMODULE, const char *name) {
  return std::strcmp(name, "D3DKMTQueryStatistics") == 0 ? reinterpret_cast<void *>(&fake_query) : nullptr;
}
''' + kmt_class + r'''
struct nvml_t {
  struct memory_t { std::uint64_t used, total; };
  static nvml_t &instance() { static nvml_t value; return value; }
  memory_t gpu_memory(std::uint64_t) { ++nvml_calls; return {128, 4096}; }
};
platf::client_gpu_stats_t sample(d3dkmt_gpu_stats_t &_kmt, std::uint64_t _vram_total_cached = 4096) {
  platf::host_stats_t s;
  s.vram_total_bytes = _vram_total_cached;
  platf::client_gpu_stats_t gpu;
  const char *vram_source = nullptr;
''' + fallback + association + r'''
  return gpu;
}
int main() {
  for (auto current : {query_mode_t::zero, query_mode_t::positive, query_mode_t::failed_adapter,
                       query_mode_t::failed_segment, query_mode_t::aperture, query_mode_t::empty}) {
    mode = current;
    adapter_calls = segment_calls = nvml_calls = 0;
    d3dkmt_gpu_stats_t collector;
    collector.set_adapter_luid({17, 3});
    const auto result = sample(collector);
    assert(adapter_calls == 1);
    assert(segment_calls == (mode == query_mode_t::failed_adapter ? 0 : 1));
    if (mode == query_mode_t::zero || mode == query_mode_t::positive) {
      assert(result.vram_used_bytes.has_value());
      assert(*result.vram_used_bytes == (mode == query_mode_t::zero ? 0u : 512u));
      assert(result.vram_total_bytes == 4096);
      assert(collector.statistics_work());
      assert(nvml_calls == 0);
      // A later failed read cannot borrow last success or unrelated NVML.
      mode = query_mode_t::failed_adapter;
      const auto failed = sample(collector);
      assert(!failed.vram_used_bytes && !failed.vram_total_bytes && nvml_calls == 0);
    } else {
      assert(!result.vram_used_bytes && !result.vram_total_bytes);
      assert(!collector.statistics_work());
      // Positive unmatched NVML stays out of the selected-DXGI client GPU.
      assert(nvml_calls == 1);
    }
  }
  mode = query_mode_t::zero;
  d3dkmt_gpu_stats_t without_dxgi_capacity;
  without_dxgi_capacity.set_adapter_luid({17, 3});
  const auto zero = sample(without_dxgi_capacity, 0);
  assert(zero.vram_used_bytes && *zero.vram_used_bytes == 0);
  assert(zero.vram_total_bytes == 4096); // successful KMT total fills missing DXGI total
  std::puts("Windows production KMT/fallback/association: known zero, positive, failures, aperture, unknown and unmatched NVML passed");
}
'''
with tempfile.TemporaryDirectory(prefix="metrics-windows-vram-") as tmp:
    file = pathlib.Path(tmp) / "test.cpp"
    binary = pathlib.Path(tmp) / "test"
    file.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(root), str(file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
