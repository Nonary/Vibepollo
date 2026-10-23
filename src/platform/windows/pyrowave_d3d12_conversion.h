// Experimental conversion-only backend. Capture and the Vulkan encoder keep
// their D3D11 texture contracts; no native AMF DX12 input is provided here.
#pragma once

#include "pyrowave_d3d11_core.h"

#include <array>

struct ID3D11DeviceContext4;
struct ID3D11Fence;

namespace pyrowave::d3d11 {
  class d3d12_conversion_t {
  public:
    enum class capture_e {
      ready,  // The exact source lease is pinned before AcquireSync.
      busy,  // Another source conversion owns this full-LUID frame gate.
      failed,  // The requested keyed source cannot be retained safely.
    };

    enum class result_e {
      ok,
      unsupported,  // No queue submission: the caller may use D3D11 instead.
      failed,  // Ordering is uncertain: end the session, never overwrite via fallback.
    };

    static std::unique_ptr<d3d12_conversion_t> create(
      IDXGIAdapter *adapter,
      ID3D11Device *device,
      ID3D11DeviceContext4 *context,
      const std::array<ID3D11Texture2D *, 3> &planes,
      ID3D11Fence *output_fence,
      const core_config_t &config,
      log_fn log
    );

    ~d3d12_conversion_t();

    // Create the retention sink before acquisition. This pins and validates the
    // complete caller lease, then moves the exact unarmed mutex interface into
    // the sink. arm_capture() is called only after AcquireSync returns S_OK.
    capture_e prepare_capture(const source_t &source, capture_mutex_guard_t &pin);
    [[nodiscard]] IDXGIKeyedMutex *capture_mutex() const noexcept;
    void arm_capture() noexcept;
    void cancel_capture() noexcept;
    // WAIT_ABANDONED does not establish ownership. Quarantine the staged
    // source and poison its full LUID without attempting ReleaseSync.
    void abandon_capture() noexcept;
    bool release_fallback() noexcept;
    void disable_conversion() noexcept;
    [[nodiscard]] bool using_conversion() const noexcept;

    // Unsupported means no D3D12 queue submission. The acquired owner remains
    // in the sink through D3D11 fallback; the caller must call release_fallback
    // after dispatch/unbind and before signaling or flushing.
    result_e convert(const source_t &source, const void *params, std::uint64_t value);

  private:
    d3d12_conversion_t();
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };
}  // namespace pyrowave::d3d11
