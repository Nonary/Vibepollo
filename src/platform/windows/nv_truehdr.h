/**
 * @file src/platform/windows/nv_truehdr.h
 * @brief NVIDIA TrueHDR (RTX Video SDK / NGX) SDR->HDR converter for the capture pipeline.
 *
 * Applies the same AI model as the NVIDIA App "RTX HDR" game filter to a captured SDR
 * frame, producing an FP16 scRGB HDR texture that drops straight into Sunshine's existing
 * HDR convert path (which already consumes R16G16B16A16_FLOAT scRGB). This lets us
 * synthesize real RTX-grade HDR for any source -- including SDR virtual displays -- without
 * the source display being in HDR mode.
 *
 * Built against the RTX Video SDK (NV_RTX_VIDEO_SDK). When the SDK is not available at
 * configure time the whole feature is compiled out (see SUNSHINE_ENABLE_NV_TRUEHDR), so
 * non-NVIDIA builds are unaffected. The RTX Video SDK nvngx_truehdr.dll feature runtime
 * must be present at runtime next to the executable and shipped per NVIDIA's terms.
 */
#pragma once

// standard includes
#include <memory>

// platform includes
#include <d3d11.h>

namespace platf::dxgi {

  /**
   * @brief TrueHDR tuning dials in RTX Video SDK units.
   *
   * User-facing config uses overlay units where applicable, then maps to these SDK
   * values before convert() is called.
   */
  struct truehdr_params_t {
    bool enabled = false;
    int contrast = 100;  ///< SDK 0..200, mapped from overlay -100..100.
    int saturation = 100;  ///< SDK 0..200, mapped from overlay -100..100.
    int middle_gray = 50;  ///< 10..100  (overlay "Middle Gray", default 50)
    int peak_brightness = 1000;  ///< 400..2000 nits (overlay "Peak Brightness", default 1000)
  };

  /**
   * @brief Wraps an NGX TrueHDR feature for a single D3D11 device. Not thread-safe; the
   *        caller must serialize convert() calls (NGX itself is not thread safe -- we hold
   *        the device's ID3D10Multithread critical section internally during evaluate).
   */
  class nv_truehdr_t {
  public:
    nv_truehdr_t() = default;
    ~nv_truehdr_t();

    nv_truehdr_t(const nv_truehdr_t &) = delete;
    nv_truehdr_t &operator=(const nv_truehdr_t &) = delete;

    /**
     * @brief Initialize NGX on the given device and create the TrueHDR feature.
     * @param device The same D3D11 device the capture textures live on.
     * @return true if TrueHDR is available and the feature was created.
     */
    bool init(ID3D11Device *device);

    /**
     * @brief Convert an SDR input texture to an FP16 scRGB HDR texture.
     * @param sdr_input R8G8B8A8_UNORM or B8G8R8A8_UNORM input.
     * @param params Tuning dials.
     * @return Borrowed pointer to the internal R16G16B16A16_FLOAT output texture (valid
     *         until the next convert()/release()), or nullptr on failure.
     */
    ID3D11Texture2D *convert(ID3D11Texture2D *sdr_input, const truehdr_params_t &params);

    /**
     * @brief Whether init() succeeded and TrueHDR is usable on this system.
     */
    bool available() const {
      return initialized;
    }

    /**
     * @brief Tear down the feature and NGX. Must be called before destruction on the
     *        capture thread (NGX shutdown deletes a critical section in a runtime dtor).
     */
    void release();

  private:
    bool initialized = false;
    void *shim_handle = nullptr;  ///< Opaque per-device handle owned by the shim DLL.
  };

}  // namespace platf::dxgi
