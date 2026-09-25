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

#include <d3d11.h>
#include <dxgi.h>

namespace pyrowave::d3d11 {
  /// How captured values become the display-encoded RGB the colour matrix expects.
  enum class transfer_e {
    unorm,  ///< UNORM capture already display encoded (SDR).
    linear_sdr,  ///< FP16 scRGB capture, SDR stream: apply the sRGB curve.
    pq,  ///< FP16 scRGB capture, HDR stream: Rec. 2100 PQ.
    sdr_to_pq,  ///< UNORM sRGB capture, HDR stream: SDR white at `sdr_white_nits`.
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

  struct core_config_t {
    int width = 0;  ///< Encoded width; even for 4:2:0.
    int height = 0;  ///< Encoded height; even for 4:2:0.
    bool yuv444 = false;
    bool ten_bit = false;  ///< R16 planes instead of R8.
    color_matrix_t color_matrix {};  ///< video::color_vectors_from_colorspace(colorspace, true).
    std::wstring shader_path;  ///< Full path of convert_pyrowave_cs.hlsl.
    float sdr_white_nits = 100.0f;  ///< For transfer_e::sdr_to_pq.
  };

  /// A captured frame, already opened on core_t::device().
  struct source_t {
    ID3D11ShaderResourceView *srv = nullptr;  ///< Null encodes a black frame.
    IDXGIKeyedMutex *mutex = nullptr;  ///< Acquired with key 0 around the conversion when set.
    unsigned width = 0;  ///< Texture extent before rotation.
    unsigned height = 0;
    transfer_e transfer = transfer_e::unorm;
    int rotate_texture_steps = 0;  ///< Quarter turns, as passed to Sunshine's vertex shaders.
  };

  struct framing_params_t {
    pyrowave::policy::framing_e framing = pyrowave::policy::framing_e::records;
    std::size_t shard_payload = 0;  ///< policy::shard_payload_bytes(packetsize); 0 disables alignment.
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
