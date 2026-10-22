/**
 * @file src/platform/windows/pyrowave_d3d11_core.h
 * @brief PyroWave encoding of Direct3D 11 frames through PyroWave's Vulkan C API.
 *
 * Frame path, after dimizago's Vibepollo PyroWave host (which ran a D3D12 port of the
 * encoder, AMD only) and andygrundman's Windows PyroWave host:
 *
 *   captured frame (D3D11 texture, keyed mutex)
 *     -> private D3D11 device on the capture adapter: RGB -> Y'CbCr compute shader
 *        writes three NT-shared plane textures
 *     -> D3D11 signals a shared fence and flushes
 *     -> PyroWave's own Vulkan device on the same adapter imports the planes and the
 *        fence (VK_KHR_external_memory_win32 / external_semaphore_win32), waits for
 *        the fence on the GPU, encodes, and the CPU waits for the result
 *     -> the bitstream is packetized and framed (src/pyrowave_policy.h)
 *
 * This file has no Sunshine dependencies so it can be driven by a standalone test
 * harness. src/platform/windows/pyrowave_encode.cpp adapts it to Sunshine.
 */
#pragma once

#include "src/pyrowave_policy.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <utility>

#include <d3d11.h>
#include <dxgi.h>

namespace pyrowave::d3d11 {
  /// How captured values become the display-encoded RGB the colour matrix expects.
  enum class transfer_e {
    unorm,  ///< UNORM capture already display encoded (SDR).
    linear_sdr,  ///< FP16 scRGB capture, SDR stream: apply the sRGB curve.
    pq,  ///< FP16 scRGB capture, HDR stream: Rec. 2100 PQ.
    sdr_to_pq,  ///< UNORM sRGB capture, HDR stream: SDR white at `sdr_white_nits`.
    truehdr_pq,  ///< NGX FP16 scRGB with compensation for its 1000-nit ceiling.
  };

  /// Same layout as video::color_t (one float4 per plane row, then two ranges).
  struct alignas(16) color_matrix_t {
    float color_vec_y[4];
    float color_vec_u[4];
    float color_vec_v[4];
    float range_y[2];
    float range_uv[2];
  };

  static_assert(sizeof(color_matrix_t) == 64, "color_matrix_t must match the HLSL cbuffer");

  struct source_t;

  struct core_config_t {
    int width = 0;  ///< Encoded width; even for 4:2:0.
    int height = 0;  ///< Encoded height; even for 4:2:0.
    bool yuv444 = false;
    bool ten_bit = false;  ///< R16 planes instead of R8.
    color_matrix_t color_matrix {};  ///< video::color_vectors_from_colorspace(colorspace, true).
    std::wstring shader_path;  ///< Full path of convert_pyrowave_cs.hlsl.
    float sdr_white_nits = 100.0f;  ///< For transfer_e::sdr_to_pq.
    std::chrono::milliseconds capture_wait {};  ///< Negotiated frame interval, rounded up.
    // Optional RGB processing on a persistent private snapshot. Capture ownership
    // is released before this callback; its borrowed SRV must survive encode().
    std::function<bool(ID3D11Texture2D *, source_t &)> transform_rgb;
    bool experimental_d3d12_conversion = false;  ///< Build-gated conversion-only prototype; D3D11 fallback.
  };

  /// Caller attestation, not a generic shared_ptr guarantee. Every writer of
  /// the exact capture resource must acquire its keyed mutex first. The host's
  /// async pool additionally excludes a pinned existing image control block;
  /// standalone callers must provide the same no-write-through-release contract.
  enum class source_lease_e { unproven, async_capture_pool, keyed_standalone };

  // Exact acquired interface ownership. Pin before AcquireSync, arm only on S_OK.
  // Transfer is pointer/scalar-only and never reacquires or releases ownership.
  class capture_mutex_guard_t {
  public:
    explicit capture_mutex_guard_t(IDXGIKeyedMutex *mutex = nullptr) noexcept:
        mutex_(mutex) {
      if (mutex_) mutex_->AddRef();
    }
    capture_mutex_guard_t(const capture_mutex_guard_t &) = delete;
    capture_mutex_guard_t &operator=(const capture_mutex_guard_t &) = delete;
    ~capture_mutex_guard_t() { reset(); }
    void arm() noexcept { acquired_ = true; }
    [[nodiscard]] IDXGIKeyedMutex *held() const noexcept { return acquired_ ? mutex_ : nullptr; }
    // The exact interface pinned before AcquireSync. Unlike held(), this is
    // non-null before acquisition so the caller can acquire through the pin.
    [[nodiscard]] IDXGIKeyedMutex *pinned() const noexcept { return mutex_; }
    [[nodiscard]] bool empty() const noexcept { return mutex_ == nullptr; }
    // Requires an empty destination, checked before command preparation.
    void take(capture_mutex_guard_t &from) noexcept {
      mutex_ = std::exchange(from.mutex_, nullptr);
      acquired_ = std::exchange(from.acquired_, false);
    }
    HRESULT release() noexcept {
      if (!std::exchange(acquired_, false)) return S_OK;
      return mutex_->ReleaseSync(0);  // One attempt, including an ambiguous failure.
    }
    void reset() noexcept {
      release();
      if (auto *mutex = std::exchange(mutex_, nullptr)) mutex->Release();
    }
  private:
    IDXGIKeyedMutex *mutex_ = nullptr;
    bool acquired_ = false;
  };

  /// A captured frame, already opened on core_t::device().
  struct source_t {
    ID3D11ShaderResourceView *srv = nullptr;  ///< Null encodes a black frame.
    IDXGIKeyedMutex *mutex = nullptr;  ///< Acquired with key 0 around the conversion when set.
    unsigned width = 0;  ///< Texture extent before rotation.
    unsigned height = 0;
    transfer_e transfer = transfer_e::unorm;
    float sdr_white_nits = 0.0f;  ///< Zero uses the session default.
    float luminance_scale = 1.0f;  ///< Linear scale for transfer_e::truehdr_pq.
    int rotate_texture_steps = 0;  ///< Quarter turns, as passed to Sunshine's vertex shaders.
    // Optional for D3D11. D3D12 nonblank copies require this explicit lease,
    // an existing image/source control block, and exact original COM identities.
    source_lease_e lease = source_lease_e::unproven;
    std::shared_ptr<const void> capture_owner;
    ID3D11Texture2D *capture_texture = nullptr;
    IDXGIKeyedMutex *capture_mutex = nullptr;
  };

  struct framing_params_t {
    pyrowave::policy::framing_e framing = pyrowave::policy::framing_e::records;
    std::size_t shard_payload = 0;  ///< policy::shard_payload_bytes(packetsize); 0 disables alignment.
    std::size_t max_frame_bytes = 0;  ///< Transport capacity; zero for standalone callers.
  };

  struct frame_stats_t {
    double convert_ms = 0;  ///< CPU time to record and submit the conversion.
    double encode_ms = 0;  ///< Encode submission until the bitstream is readable (CPU wait).
    double packetize_ms = 0;
    std::size_t encoded_bytes = 0;  ///< Bitstream bytes, without framing overhead.
    std::size_t frame_bytes = 0;  ///< Framed bytes appended to the output.
    std::size_t packets = 0;
    pyrowave::policy::record_frame_stats_t records;
  };

  /// 0 debug, 1 info, 2 warning, 3 error.
  using log_fn = std::function<void(int level, const std::string &message)>;

  enum class result_e {
    ok,
    skipped,  ///< The frame was dropped (e.g. capture mutex timeout); keep going.
    failed,  ///< The session cannot continue.
  };

  class core_t {
  public:
    ~core_t();

    /**
     * @brief Create the conversion device and the PyroWave encoder on `adapter`.
     * @return nullptr on failure, after logging why.
     */
    static std::unique_ptr<core_t> create(IDXGIAdapter *adapter, const core_config_t &config, log_fn log);

    /// The private D3D11 device captured textures must be opened on.
    [[nodiscard]] ID3D11Device *device() const;

    /// Reports the selected producer, so native tests cannot silently test fallback.
    [[nodiscard]] bool using_d3d12_conversion() const;

    /**
     * @brief Convert and encode one frame, appending the framed frame to `out`.
     * @param max_bitstream_bytes PyroWave's rate cap for this frame (policy::budget_t).
     */
    result_e encode(
      const source_t &source,
      std::size_t max_bitstream_bytes,
      const framing_params_t &framing,
      std::vector<std::uint8_t> &out,
      frame_stats_t *stats = nullptr
    );

  private:
    core_t();
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };

  /**
   * @brief Check that PyroWave can encode on an adapter with D3D11 interop.
   * @param luid Adapter LUID, or nullptr for the first Vulkan device.
   * @param detail Receives a human-readable reason on failure.
   */
  bool probe(const LUID *luid, std::string &detail);
}  // namespace pyrowave::d3d11
