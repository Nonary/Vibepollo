/**
 * @file src/platform/windows/display_vram.cpp
 * @brief Definitions for handling video ram.
 */
// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

// platform includes
#include <winsock2.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_d3d11va.h>
}

// lib includes
#include "display.h"
#include "display_vram.h"
#include "misc.h"
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
  #include "nv_truehdr.h"
  #include "rtx_hdr_runtime.h"
#endif
#include "src/config.h"
#include "src/logging.h"
#include "src/nvenc/nvenc_config.h"
#include "src/nvenc/nvenc_d3d11_native.h"
#include "src/nvenc/nvenc_d3d11_on_cuda.h"
#include "src/nvenc/nvenc_utils.h"
#include "src/video.h"
#include "utf_utils.h"

#include <AMF/core/Factory.h>
#include <boost/algorithm/string/predicate.hpp>

#if !defined(SUNSHINE_SHADERS_DIR)  // for testing this needs to be defined in cmake as we don't do an install
  #define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/directx"
#endif
namespace platf {
  using namespace std::literals;
}

static void free_frame(AVFrame *frame) {
  av_frame_free(&frame);
}

using frame_t = util::safe_ptr<AVFrame, free_frame>;

namespace platf::dxgi {

  template<class T>
  buf_t make_buffer(device_t::pointer device, const T &t) {
    static_assert(sizeof(T) % 16 == 0, "Buffer needs to be aligned on a 16-byte alignment");

    D3D11_BUFFER_DESC buffer_desc {
      sizeof(T),
      D3D11_USAGE_IMMUTABLE,
      D3D11_BIND_CONSTANT_BUFFER
    };

    D3D11_SUBRESOURCE_DATA init_data {
      &t
    };

    buf_t::pointer buf_p;
    auto status = device->CreateBuffer(&buffer_desc, &init_data, &buf_p);
    if (status) {
      BOOST_LOG(error) << "Failed to create buffer: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return buf_t {buf_p};
  }

  struct alignas(16) sdr_to_pq_params_t {
    float sdr_white_nits;
    float padding[3];
  };

#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
  struct alignas(16) truehdr_peak_params_t {
    float luminance_scale;
    float padding[3];
  };
#endif

  blend_t make_blend(device_t::pointer device, bool enable, bool invert) {
    D3D11_BLEND_DESC bdesc {};
    auto &rt = bdesc.RenderTarget[0];
    rt.BlendEnable = enable;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (enable) {
      rt.BlendOp = D3D11_BLEND_OP_ADD;
      rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;

      if (invert) {
        // Invert colors
        rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
        rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
      } else {
        // Regular alpha blending
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
      }

      rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
      rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    }

    blend_t blend;
    auto status = device->CreateBlendState(&bdesc, &blend);
    if (status) {
      BOOST_LOG(error) << "Failed to create blend state: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return blend;
  }

  blob_t convert_yuv420_packed_uv_type0_ps_hlsl;
  blob_t convert_yuv420_packed_uv_type0_ps_linear_hlsl;
  blob_t convert_yuv420_packed_uv_type0_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_packed_uv_type0_ps_sdr_to_pq_hlsl;
  blob_t convert_yuv420_packed_uv_type0_vs_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_linear_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_sdr_to_pq_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_vs_hlsl;
  blob_t convert_yuv420_planar_y_ps_hlsl;
  blob_t convert_yuv420_planar_y_ps_linear_hlsl;
  blob_t convert_yuv420_planar_y_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv420_planar_y_ps_sdr_to_pq_hlsl;
  blob_t convert_yuv420_planar_y_vs_hlsl;
  blob_t convert_yuv444_packed_ayuv_ps_hlsl;
  blob_t convert_yuv444_packed_ayuv_ps_linear_hlsl;
  blob_t convert_yuv444_packed_vs_hlsl;
  blob_t convert_yuv444_planar_ps_hlsl;
  blob_t convert_yuv444_planar_ps_linear_hlsl;
  blob_t convert_yuv444_planar_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv444_planar_ps_sdr_to_pq_hlsl;
  blob_t convert_yuv444_packed_y410_ps_hlsl;
  blob_t convert_yuv444_packed_y410_ps_linear_hlsl;
  blob_t convert_yuv444_packed_y410_ps_perceptual_quantizer_hlsl;
  blob_t convert_yuv444_packed_y410_ps_sdr_to_pq_hlsl;
  blob_t convert_yuv444_planar_vs_hlsl;
  blob_t cursor_ps_hlsl;
  blob_t cursor_ps_normalize_white_hlsl;
  blob_t cursor_vs_hlsl;

#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
  blob_t convert_yuv420_packed_uv_type0_ps_truehdr_peak_hlsl;
  blob_t convert_yuv420_packed_uv_type0s_ps_truehdr_peak_hlsl;
  blob_t convert_yuv420_planar_y_ps_truehdr_peak_hlsl;
  blob_t convert_yuv444_planar_ps_truehdr_peak_hlsl;
  blob_t convert_yuv444_packed_y410_ps_truehdr_peak_hlsl;

  namespace {
    constexpr int TRUEHDR_NGX_OUTPUT_CEILING_NITS = 1000;
    constexpr UINT TRUEHDR_NATIVE_HDR_GRID_SIZE = 4;
    constexpr UINT TRUEHDR_NATIVE_HDR_PATCH_SIZE = 2;
    constexpr UINT TRUEHDR_NATIVE_HDR_SAMPLE_WIDTH = TRUEHDR_NATIVE_HDR_GRID_SIZE * TRUEHDR_NATIVE_HDR_PATCH_SIZE;
    constexpr UINT TRUEHDR_NATIVE_HDR_SAMPLE_HEIGHT = TRUEHDR_NATIVE_HDR_SAMPLE_WIDTH;
    constexpr float TRUEHDR_NATIVE_HDR_SDR_WHITE_THRESHOLD = 1.25f;
    constexpr unsigned TRUEHDR_NATIVE_HDR_MIN_BRIGHT_PIXELS = 3;
    constexpr unsigned TRUEHDR_NATIVE_HDR_CONFIRMATION_FRAMES = 5;

    bool truehdr_native_hdr_detectable_format(DXGI_FORMAT format) {
      return format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    double pq_to_nits(double pq) {
      // SMPTE ST 2084 inverse EOTF. PQ is absolute, so this lets the live P010
      // readback report the luminance represented by the codes handed to NVENC.
      constexpr double m1 = 2610.0 / 4096.0 / 4.0;
      constexpr double m2 = 2523.0 / 4096.0 * 128.0;
      constexpr double c1 = 3424.0 / 4096.0;
      constexpr double c2 = 2413.0 / 4096.0 * 32.0;
      constexpr double c3 = 2392.0 / 4096.0 * 32.0;

      const double p = std::pow(std::clamp(pq, 0.0, 1.0), 1.0 / m2);
      const double numerator = std::max(p - c1, 0.0);
      const double denominator = c2 - c3 * p;
      if (denominator <= 0.0) {
        return 10000.0;
      }
      return 10000.0 * std::pow(numerator / denominator, 1.0 / m1);
    }

    double p010_luma_code_to_nits(const int code, const bool full_range) {
      // H.273 10-bit luma ranges: 0..1023 full range, 64..940 limited range.
      const double pq = full_range ?
                          static_cast<double>(code) / 1023.0 :
                          (static_cast<double>(code) - 64.0) / 876.0;
      return pq_to_nits(std::clamp(pq, 0.0, 1.0));
    }

    float half_to_float(std::uint16_t half) {
      const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
      int exponent = (half & 0x7C00u) >> 10;
      std::uint32_t mantissa = half & 0x03FFu;

      std::uint32_t bits = 0;
      if (exponent == 0) {
        if (mantissa == 0) {
          bits = sign;
        } else {
          while ((mantissa & 0x0400u) == 0) {
            mantissa <<= 1;
            --exponent;
          }
          ++exponent;
          mantissa &= ~0x0400u;
          bits = sign | (static_cast<std::uint32_t>(exponent + 127 - 15) << 23) | (mantissa << 13);
        }
      } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
      } else {
        bits = sign | (static_cast<std::uint32_t>(exponent + 127 - 15) << 23) | (mantissa << 13);
      }

      float value = 0.0f;
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    }
  }  // namespace
#endif

  struct texture_lock_helper {
    keyed_mutex_t _mutex;
    bool _locked = false;

    texture_lock_helper(const texture_lock_helper &) = delete;
    texture_lock_helper &operator=(const texture_lock_helper &) = delete;

    texture_lock_helper(texture_lock_helper &&other) {
      _mutex.reset(other._mutex.release());
      _locked = other._locked;
      other._locked = false;
    }

    texture_lock_helper &operator=(texture_lock_helper &&other) {
      if (_locked) {
        _mutex->ReleaseSync(0);
      }
      _mutex.reset(other._mutex.release());
      _locked = other._locked;
      other._locked = false;
      return *this;
    }

    texture_lock_helper(IDXGIKeyedMutex *mutex):
        _mutex(mutex) {
      if (_mutex) {
        _mutex->AddRef();
      }
    }

    ~texture_lock_helper() {
      if (_locked) {
        _mutex->ReleaseSync(0);
      }
    }

    bool lock() {
      if (_locked) {
        return true;
      }
      HRESULT status = _mutex->AcquireSync(0, 3000);
      if (status == S_OK || status == WAIT_ABANDONED) {
        if (status == WAIT_ABANDONED) {
          BOOST_LOG(error) << "Keyed mutex was abandoned; continuing with lock held";
        }
        _locked = true;
      } else {
        BOOST_LOG(error) << "Failed to acquire texture mutex [0x"sv << util::hex(status).to_string_view() << ']';
      }
      return _locked;
    }
  };

  util::buffer_t<std::uint8_t> make_cursor_xor_image(const util::buffer_t<std::uint8_t> &img_data, DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info) {
    constexpr std::uint32_t inverted = 0xFFFFFFFF;
    constexpr std::uint32_t transparent = 0;

    switch (shape_info.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        // This type doesn't require any XOR-blending
        return {};
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        {
          util::buffer_t<std::uint8_t> cursor_img = img_data;
          std::for_each((std::uint32_t *) std::begin(cursor_img), (std::uint32_t *) std::end(cursor_img), [](auto &pixel) {
            auto alpha = (std::uint8_t) ((pixel >> 24) & 0xFF);
            if (alpha == 0xFF) {
              // Pixels with 0xFF alpha will be XOR-blended as is.
            } else if (alpha == 0x00) {
              // Pixels with 0x00 alpha will be blended by make_cursor_alpha_image().
              // We make them transparent for the XOR-blended cursor image.
              pixel = transparent;
            } else {
              // Other alpha values are illegal in masked color cursors
              BOOST_LOG(warning) << "Illegal alpha value in masked color cursor: " << alpha;
            }
          });
          return cursor_img;
        }
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        // Monochrome is handled below
        break;
      default:
        BOOST_LOG(error) << "Invalid cursor shape type: " << shape_info.Type;
        return {};
    }

    shape_info.Height /= 2;

    util::buffer_t<std::uint8_t> cursor_img {shape_info.Width * shape_info.Height * 4};

    auto bytes = shape_info.Pitch * shape_info.Height;
    auto pixel_begin = (std::uint32_t *) std::begin(cursor_img);
    auto pixel_data = pixel_begin;
    auto and_mask = std::begin(img_data);
    auto xor_mask = std::begin(img_data) + bytes;

    for (auto x = 0; x < bytes; ++x) {
      for (auto c = 7; c >= 0 && ((std::uint8_t *) pixel_data) != std::end(cursor_img); --c) {
        auto bit = 1 << c;
        auto color_type = ((*and_mask & bit) ? 1 : 0) + ((*xor_mask & bit) ? 2 : 0);

        switch (color_type) {
          case 0:  // Opaque black (handled by alpha-blending)
          case 2:  // Opaque white (handled by alpha-blending)
          case 1:  // Color of screen (transparent)
            *pixel_data = transparent;
            break;
          case 3:  // Inverse of screen
            *pixel_data = inverted;
            break;
        }

        ++pixel_data;
      }
      ++and_mask;
      ++xor_mask;
    }

    return cursor_img;
  }

  util::buffer_t<std::uint8_t> make_cursor_alpha_image(const util::buffer_t<std::uint8_t> &img_data, DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info) {
    constexpr std::uint32_t black = 0xFF000000;
    constexpr std::uint32_t white = 0xFFFFFFFF;
    constexpr std::uint32_t transparent = 0;

    switch (shape_info.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        {
          util::buffer_t<std::uint8_t> cursor_img = img_data;
          std::for_each((std::uint32_t *) std::begin(cursor_img), (std::uint32_t *) std::end(cursor_img), [](auto &pixel) {
            auto alpha = (std::uint8_t) ((pixel >> 24) & 0xFF);
            if (alpha == 0xFF) {
              // Pixels with 0xFF alpha will be XOR-blended by make_cursor_xor_image().
              // We make them transparent for the alpha-blended cursor image.
              pixel = transparent;
            } else if (alpha == 0x00) {
              // Pixels with 0x00 alpha will be blended as opaque with the alpha-blended image.
              pixel |= 0xFF000000;
            } else {
              // Other alpha values are illegal in masked color cursors
              BOOST_LOG(warning) << "Illegal alpha value in masked color cursor: " << alpha;
            }
          });
          return cursor_img;
        }
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        // Color cursors are just an ARGB bitmap which requires no processing.
        return img_data;
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        // Monochrome cursors are handled below.
        break;
      default:
        BOOST_LOG(error) << "Invalid cursor shape type: " << shape_info.Type;
        return {};
    }

    shape_info.Height /= 2;

    util::buffer_t<std::uint8_t> cursor_img {shape_info.Width * shape_info.Height * 4};

    auto bytes = shape_info.Pitch * shape_info.Height;
    auto pixel_begin = (std::uint32_t *) std::begin(cursor_img);
    auto pixel_data = pixel_begin;
    auto and_mask = std::begin(img_data);
    auto xor_mask = std::begin(img_data) + bytes;

    for (auto x = 0; x < bytes; ++x) {
      for (auto c = 7; c >= 0 && ((std::uint8_t *) pixel_data) != std::end(cursor_img); --c) {
        auto bit = 1 << c;
        auto color_type = ((*and_mask & bit) ? 1 : 0) + ((*xor_mask & bit) ? 2 : 0);

        switch (color_type) {
          case 0:  // Opaque black
            *pixel_data = black;
            break;
          case 2:  // Opaque white
            *pixel_data = white;
            break;
          case 3:  // Inverse of screen (handled by XOR blending)
          case 1:  // Color of screen (transparent)
            *pixel_data = transparent;
            break;
        }

        ++pixel_data;
      }
      ++and_mask;
      ++xor_mask;
    }

    return cursor_img;
  }

  blob_t compile_shader(LPCSTR file, LPCSTR entrypoint, LPCSTR shader_model) {
    blob_t::pointer msg_p = nullptr;
    blob_t::pointer compiled_p;

    DWORD flags = D3DCOMPILE_ENABLE_STRICTNESS;

#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto wFile = utf_utils::from_utf8(file);
    auto status = D3DCompileFromFile(wFile.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entrypoint, shader_model, flags, 0, &compiled_p, &msg_p);

    if (msg_p) {
      BOOST_LOG(warning) << std::string_view {(const char *) msg_p->GetBufferPointer(), msg_p->GetBufferSize() - 1};
      msg_p->Release();
    }

    if (status) {
      BOOST_LOG(error) << "Couldn't compile ["sv << file << "] [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    return blob_t {compiled_p};
  }

  blob_t compile_pixel_shader(LPCSTR file) {
    return compile_shader(file, "main_ps", "ps_5_0");
  }

  blob_t compile_vertex_shader(LPCSTR file) {
    return compile_shader(file, "main_vs", "vs_5_0");
  }

  class d3d_base_encode_device final {
  public:
    int convert(platf::img_t &img_base) {
      // Garbage collect mapped capture images whose weak references have expired
      for (auto it = img_ctx_map.begin(); it != img_ctx_map.end();) {
        if (it->second.img_weak.expired()) {
          it = img_ctx_map.erase(it);
        } else {
          it++;
        }
      }

      auto &img = (img_d3d_t &) img_base;
      auto draw = [&](auto &input, auto &y_or_yuv_viewports, auto &uv_viewport, DXGI_FORMAT input_format, bool sdr_to_pq, bool truehdr_peak_expand) {
        device_ctx->PSSetShaderResources(0, 1, &input);
        ID3D11Buffer *conversion_params_buffer = sdr_to_pq ? sdr_to_pq_params.get() : nullptr;
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
        if (truehdr_peak_expand) {
          conversion_params_buffer = truehdr_peak_params.get();
        }
#else
        (void) truehdr_peak_expand;
#endif
        device_ctx->PSSetConstantBuffers(1, 1, &conversion_params_buffer);

        // Draw Y/YUV
        device_ctx->OMSetRenderTargets(1, &out_Y_or_YUV_rtv, nullptr);
        device_ctx->VSSetShader(convert_Y_or_YUV_vs.get(), nullptr, 0);
        auto *y_or_yuv_ps = convert_Y_or_YUV_ps.get();
        if (input_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
          y_or_yuv_ps = convert_Y_or_YUV_fp16_ps.get();
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
          if (truehdr_peak_expand) {
            y_or_yuv_ps = convert_Y_or_YUV_truehdr_peak_ps.get();
          }
#endif
        } else if (sdr_to_pq && convert_Y_or_YUV_sdr_to_pq_ps) {
          y_or_yuv_ps = convert_Y_or_YUV_sdr_to_pq_ps.get();
        }
        device_ctx->PSSetShader(y_or_yuv_ps, nullptr, 0);
        auto viewport_count = (format == DXGI_FORMAT_R16_UINT) ? 3 : 1;
        assert(viewport_count <= y_or_yuv_viewports.size());
        device_ctx->RSSetViewports(viewport_count, y_or_yuv_viewports.data());
        device_ctx->Draw(3 * viewport_count, 0);  // vertex shader will spread vertices across viewports

        // Draw UV if needed
        if (out_UV_rtv) {
          assert(format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010);
          device_ctx->OMSetRenderTargets(1, &out_UV_rtv, nullptr);
          device_ctx->VSSetShader(convert_UV_vs.get(), nullptr, 0);
          auto *uv_ps = convert_UV_ps.get();
          if (input_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            uv_ps = convert_UV_fp16_ps.get();
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
            if (truehdr_peak_expand) {
              uv_ps = convert_UV_truehdr_peak_ps.get();
            }
#endif
          } else if (sdr_to_pq && convert_UV_sdr_to_pq_ps) {
            uv_ps = convert_UV_sdr_to_pq_ps.get();
          }
          device_ctx->PSSetShader(uv_ps, nullptr, 0);
          device_ctx->RSSetViewports(1, &uv_viewport);
          device_ctx->Draw(3, 0);
        }
      };

      auto unbind_shader_resource = [&]() {
        ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
        device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);
      };

      auto clear_output_to_black = [&]() -> bool {
        if (!ensure_black_texture_for_rtv_clear()) {
          return false;
        }

        draw(black_texture_for_clear_srv, out_Y_or_YUV_viewports_for_clear, out_UV_viewport_for_clear, DXGI_FORMAT_B8G8R8A8_UNORM, false, false);
        rtvs_cleared = true;
        unbind_shader_resource();
        return true;
      };

      if (img.blank) {
        return clear_output_to_black() ? 0 : -1;
      }

      auto &img_ctx = img_ctx_map[img.id];

      // Open the shared capture texture with our ID3D11Device
      if (initialize_image_context(img, img_ctx)) {
        return -1;
      }

      // Acquire encoder mutex to synchronize with capture code.
      // Use a finite timeout to avoid hard-deadlocks during display re-init / device loss.
      auto status = img_ctx.encoder_mutex->AcquireSync(0, 3000);
      if (status == WAIT_TIMEOUT) {
        BOOST_LOG(error) << "Timed out acquiring encoder mutex; capture/encoder sync likely wedged";
        return -1;
      }
      if (status != S_OK && status != WAIT_ABANDONED) {
        BOOST_LOG(error) << "Failed to acquire encoder mutex [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      if (status == WAIT_ABANDONED) {
        BOOST_LOG(error) << "Encoder mutex was abandoned; continuing with lock held";
      }

      bool encoder_mutex_held = true;
      auto release_encoder_mutex_now = [&]() {
        if (!encoder_mutex_held) {
          return true;
        }
        const HRESULT hr = img_ctx.encoder_mutex->ReleaseSync(0);
        if (FAILED(hr)) {
          BOOST_LOG(warning) << "Failed to release encoder mutex [0x"sv << util::hex(hr).to_string_view() << ']';
          return false;
        }
        encoder_mutex_held = false;
        return true;
      };
      auto release_encoder_mutex = util::fail_guard([&]() {
        (void) release_encoder_mutex_now();
      });

      // Clear render target view(s) once so that the aspect ratio mismatch "bars" appear black
      if (!rtvs_cleared && !clear_output_to_black()) {
        return -1;
      }

      // Draw captured frame. When RTX HDR (NVIDIA TrueHDR) is active and the target
      // colorspace is HDR but the captured frame is still in a supported SDR-compatible
      // format, run it through the TrueHDR model first, yielding an FP16 scRGB texture
      // the existing PQ convert path consumes.
      auto *encode_input_res = &img_ctx.encoder_input_res;
      DXGI_FORMAT encode_input_format = img.format;
      bool encode_input_sdr_to_pq = false;
      bool encode_input_truehdr_peak_expand = false;
      bool encode_input_truehdr_converted = false;
      float encode_input_sdr_white_nits = 100.0f;
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
      ID3D11Texture2D *truehdr_input_texture = img_ctx.encoder_texture.get();
      const bool truehdr_supported_sdr_input =
        img.format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        img.format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        img.format == DXGI_FORMAT_R10G10B10A2_UNORM;
      platf::rtx_hdr::frame_state_t truehdr_frame_state;
      bool truehdr_should_convert = false;
      bool truehdr_native_hdr_bypass = false;
      if (truehdr_active && truehdr_output_hdr) {
        const RECT capture_rect {
          display->offset_x,
          display->offset_y,
          display->offset_x + display->width,
          display->offset_y + display->height
        };
        truehdr_frame_state = rtx_hdr_runtime.update_for_frame(capture_rect);
        encode_input_sdr_white_nits = platf::rtx_hdr::sdr_brightness_to_white_nits(truehdr_frame_state.sdr_brightness);
        truehdr_should_convert = truehdr_frame_state.enabled;
        if (truehdr_should_convert && update_truehdr_native_hdr_detector(img_ctx.encoder_texture.get(), img.format)) {
          truehdr_should_convert = false;
          truehdr_native_hdr_bypass = true;
        } else if (!truehdr_should_convert) {
          reset_truehdr_native_hdr_detector();
        }
        const auto transition_key = std::string(platf::rtx_hdr::source_name(truehdr_frame_state.source)) +
                                    (truehdr_native_hdr_bypass ? ":native-hdr" : (truehdr_should_convert ? ":convert" : ":bypass")) +
                                    ":" + truehdr_frame_state.foreground_source;
        if (transition_key != truehdr_last_transition_key) {
          truehdr_last_transition_key = transition_key;
          if (truehdr_native_hdr_bypass) {
            if (!truehdr_native_hdr_logged) {
              BOOST_LOG(info) << "RTX HDR: native HDR content detected; not enabling RTX HDR because the source is not SDR.";
              truehdr_native_hdr_logged = true;
            }
          } else if (truehdr_should_convert) {
            BOOST_LOG(info) << "RTX HDR: applying " << platf::rtx_hdr::source_name(truehdr_frame_state.source)
                            << " TrueHDR conversion"
                            << " (contrast=" << truehdr_frame_state.contrast
                            << ", saturation=" << truehdr_frame_state.saturation
                            << ", middleGray=" << truehdr_frame_state.middle_gray
                            << ", peak=" << truehdr_frame_state.peak_brightness
                            << " nits, foreground=" << truehdr_frame_state.foreground_source << ").";
          } else {
            BOOST_LOG(info) << "RTX HDR: bypassing TrueHDR conversion"
                            << (truehdr_frame_state.foreground_matches ? " because RTX HDR is disabled for the focused app/runtime override." : " because the foreground window does not match the streamed app.")
                            << " Encoding through neutral SDR-to-PQ path"
                            << " (SDR white=" << encode_input_sdr_white_nits << " nits).";
          }
        }
      }
      if (truehdr_should_convert) {
        if (truehdr_supported_sdr_input) {
          bool truehdr_private_input_ready = false;
          if (ensure_truehdr_input_context(img_ctx)) {
            device_ctx->CopyResource(img_ctx.truehdr_input_texture.get(), img_ctx.encoder_texture.get());
            encode_input_res = &img_ctx.truehdr_input_res;
            truehdr_input_texture = img_ctx.truehdr_input_texture.get();
            truehdr_private_input_ready = release_encoder_mutex_now();
          } else if (!truehdr_failure_logged) {
            BOOST_LOG(warning) << "RTX HDR: streaming unconverted frame because private TrueHDR input setup failed.";
            truehdr_failure_logged = true;
          }

          bool truehdr_converted = false;
          bool truehdr_peak_compensated = false;
          if (truehdr_private_input_ready && !truehdr_engine) {
            truehdr_engine = std::make_unique<nv_truehdr_t>();
            truehdr_engine->init(device.get());
          }
          if (truehdr_private_input_ready && truehdr_engine->available()) {
            truehdr_params_t p;
            p.contrast = truehdr_frame_state.contrast;
            p.saturation = truehdr_frame_state.saturation;
            p.middle_gray = truehdr_frame_state.middle_gray;
            p.peak_brightness = std::clamp(truehdr_frame_state.peak_brightness, 400, 2000);

            // The public RTX Video SDK accepts MaxLuminance up to 2000, but its TrueHDR
            // output texture hard-clips near 1000 nits. Preserve the requested tone curve
            // by evaluating NGX at 1000 nits with a proportionally lower middle-gray, then
            // restore the requested scRGB headroom immediately before PQ encoding.
            if (p.peak_brightness > TRUEHDR_NGX_OUTPUT_CEILING_NITS) {
              const int requested_peak_nits = p.peak_brightness;
              const float luminance_scale = static_cast<float>(requested_peak_nits) /
                                            static_cast<float>(TRUEHDR_NGX_OUTPUT_CEILING_NITS);
              if (ensure_truehdr_peak_params(luminance_scale)) {
                const int requested_middle_gray = p.middle_gray;
                p.middle_gray = std::clamp(
                  static_cast<int>(std::lround(static_cast<float>(requested_middle_gray) / luminance_scale)),
                  10,
                  100
                );
                p.peak_brightness = TRUEHDR_NGX_OUTPUT_CEILING_NITS;
                truehdr_peak_compensated = true;

                if (truehdr_last_compensated_peak_nits != requested_peak_nits ||
                    truehdr_last_compensated_middle_gray != requested_middle_gray) {
                  BOOST_LOG(info) << "RTX HDR: compensating for the NGX 1000-nit output ceiling"
                                  << " (requested peak=" << requested_peak_nits
                                  << " nits, NGX middleGray=" << requested_middle_gray << "->" << p.middle_gray
                                  << ", post-scale=" << luminance_scale << "x).";
                  truehdr_last_compensated_peak_nits = requested_peak_nits;
                  truehdr_last_compensated_middle_gray = requested_middle_gray;
                }
              } else if (!truehdr_peak_compensation_failure_logged) {
                BOOST_LOG(warning) << "RTX HDR: failed to create the peak compensation buffer; output above 1000 nits may clip.";
                truehdr_peak_compensation_failure_logged = true;
              }
            } else {
              truehdr_last_compensated_peak_nits = 0;
              truehdr_last_compensated_middle_gray = 0;
            }
            if (auto *hdr_tex = truehdr_engine->convert(truehdr_input_texture, p)) {
              if (hdr_tex != truehdr_srv_texture) {
                truehdr_srv.reset();
                truehdr_srv_texture = nullptr;
                shader_res_t::pointer srv_p = nullptr;
                if (SUCCEEDED(device->CreateShaderResourceView(hdr_tex, nullptr, &srv_p))) {
                  truehdr_srv.reset(srv_p);
                  truehdr_srv_texture = hdr_tex;
                }
              }
              if (truehdr_srv) {
                if (!truehdr_conversion_logged) {
                  BOOST_LOG(info) << "RTX HDR: converting capture format " << display->dxgi_format_to_string(img.format)
                                  << " to FP16 scRGB HDR.";
                  truehdr_conversion_logged = true;
                }
                encode_input_res = &truehdr_srv;
                encode_input_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                encode_input_truehdr_peak_expand = truehdr_peak_compensated;
                encode_input_truehdr_converted = true;
                truehdr_converted = true;
              } else if (!truehdr_failure_logged) {
                BOOST_LOG(warning) << "RTX HDR: failed to create shader resource view for TrueHDR output; streaming unconverted frame.";
                truehdr_failure_logged = true;
              }
            } else if (!truehdr_failure_logged) {
              BOOST_LOG(warning) << "RTX HDR: TrueHDR conversion failed; streaming unconverted frame.";
              truehdr_failure_logged = true;
            }
          }
          if (!truehdr_converted) {
            encode_input_sdr_to_pq = true;
          }
        } else if (!truehdr_unsupported_format_logged) {
          BOOST_LOG(warning) << "RTX HDR: capture format " << display->dxgi_format_to_string(img.format)
                             << " is not supported for TrueHDR conversion; streaming unconverted frame.";
          truehdr_unsupported_format_logged = true;
        }
      } else if (truehdr_active && truehdr_output_hdr && truehdr_supported_sdr_input) {
        encode_input_sdr_to_pq = true;
      } else if (truehdr_active && !truehdr_output_hdr && !truehdr_not_hdr_stream_logged) {
        BOOST_LOG(info) << "RTX HDR: stream is not HDR; TrueHDR conversion inactive.";
        truehdr_not_hdr_stream_logged = true;
      }
#endif
      if (encode_input_sdr_to_pq) {
        ensure_sdr_to_pq_params(encode_input_sdr_white_nits);
      }
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
      if (encode_input_truehdr_converted) {
        update_truehdr_live_readback_request(truehdr_frame_state);
      } else {
        truehdr_live_readback_request.reset();
      }
#endif
      draw(
        *encode_input_res,
        out_Y_or_YUV_viewports,
        out_UV_viewport,
        encode_input_format,
        encode_input_sdr_to_pq,
        encode_input_truehdr_peak_expand
      );

#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
      if (encode_input_truehdr_converted) {
        maybe_log_truehdr_live_readback();
      }
#endif

      // Release encoder mutex to allow capture code to reuse this image
      if (release_encoder_mutex_now()) {
        release_encoder_mutex.disable();
      }

      unbind_shader_resource();

      return 0;
    }

    void ensure_sdr_to_pq_params(float sdr_white_nits) {
      if (sdr_to_pq_params && std::abs(sdr_to_pq_white_nits - sdr_white_nits) < 0.5f) {
        return;
      }

      sdr_to_pq_params_t params {sdr_white_nits, {}};
      if (auto buffer = make_buffer(device.get(), params)) {
        sdr_to_pq_params = std::move(buffer);
        sdr_to_pq_white_nits = sdr_white_nits;
      }
    }

#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
    bool ensure_truehdr_peak_params(float luminance_scale) {
      if (truehdr_peak_params && std::abs(truehdr_peak_luminance_scale - luminance_scale) < 0.001f) {
        return true;
      }

      truehdr_peak_params_t params {luminance_scale, {}};
      if (auto buffer = make_buffer(device.get(), params)) {
        truehdr_peak_params = std::move(buffer);
        truehdr_peak_luminance_scale = luminance_scale;
        return true;
      }

      return false;
    }

    struct truehdr_live_readback_request_t {
      int contrast;
      int saturation;
      int middle_gray;
      int peak_brightness;
      std::chrono::steady_clock::time_point stable_since;
      bool logged {false};

      bool same_settings(const platf::rtx_hdr::frame_state_t &frame) const {
        return contrast == frame.contrast &&
               saturation == frame.saturation &&
               middle_gray == frame.middle_gray &&
               peak_brightness == frame.peak_brightness;
      }
    };

    void update_truehdr_live_readback_request(const platf::rtx_hdr::frame_state_t &frame) {
      // A full GPU readback is deliberately restricted to debug/verbose logging and
      // debounced so dragging a slider cannot stall every frame.
      if (config::sunshine.min_log_level > 1 || format != DXGI_FORMAT_P010) {
        truehdr_live_readback_request.reset();
        return;
      }

      if (truehdr_live_readback_request && truehdr_live_readback_request->same_settings(frame)) {
        return;
      }

      truehdr_live_readback_request = truehdr_live_readback_request_t {
        frame.contrast,
        frame.saturation,
        frame.middle_gray,
        frame.peak_brightness,
        std::chrono::steady_clock::now(),
      };
    }

    void maybe_log_truehdr_live_readback() {
      using namespace std::chrono_literals;

      auto &request = truehdr_live_readback_request;
      if (!request || request->logged || std::chrono::steady_clock::now() - request->stable_since < 500ms) {
        return;
      }
      request->logged = true;

      D3D11_TEXTURE2D_DESC output_desc {};
      output_texture->GetDesc(&output_desc);
      if (output_desc.Format != DXGI_FORMAT_P010) {
        return;
      }

      bool create_readback = !truehdr_live_readback_texture;
      if (truehdr_live_readback_texture) {
        D3D11_TEXTURE2D_DESC readback_desc {};
        truehdr_live_readback_texture->GetDesc(&readback_desc);
        create_readback = readback_desc.Width != output_desc.Width ||
                          readback_desc.Height != output_desc.Height ||
                          readback_desc.ArraySize != output_desc.ArraySize ||
                          readback_desc.Format != output_desc.Format;
      }

      if (create_readback) {
        auto readback_desc = output_desc;
        readback_desc.Usage = D3D11_USAGE_STAGING;
        readback_desc.BindFlags = 0;
        readback_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        readback_desc.MiscFlags = 0;
        texture2d_t::pointer readback_p = nullptr;
        const HRESULT create_status = device->CreateTexture2D(&readback_desc, nullptr, &readback_p);
        if (FAILED(create_status)) {
          if (!truehdr_live_readback_failure_logged) {
            BOOST_LOG(warning) << "RTX HDR live readback: failed to create P010 staging texture: " << util::log_hex(create_status);
            truehdr_live_readback_failure_logged = true;
          }
          return;
        }
        truehdr_live_readback_texture.reset(readback_p);
      }

      // The luma/UV render targets must be unbound before copying the encoder input.
      device_ctx->OMSetRenderTargets(0, nullptr, nullptr);
      device_ctx->CopyResource(truehdr_live_readback_texture.get(), output_texture.get());

      D3D11_MAPPED_SUBRESOURCE mapped {};
      const HRESULT map_status = device_ctx->Map(truehdr_live_readback_texture.get(), 0, D3D11_MAP_READ, 0, &mapped);
      if (FAILED(map_status)) {
        if (!truehdr_live_readback_failure_logged) {
          BOOST_LOG(warning) << "RTX HDR live readback: failed to map P010 staging texture: " << util::log_hex(map_status);
          truehdr_live_readback_failure_logged = true;
        }
        return;
      }

      std::array<std::uint64_t, 1024> histogram {};
      std::uint64_t pixel_count = 0;
      for (UINT y = 0; y < output_desc.Height; ++y) {
        const auto *row = reinterpret_cast<const std::uint16_t *>(
          static_cast<const std::uint8_t *>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch
        );
        for (UINT x = 0; x < output_desc.Width; ++x) {
          ++histogram[std::min<int>(row[x] >> 6, 1023)];
          ++pixel_count;
        }
      }
      device_ctx->Unmap(truehdr_live_readback_texture.get(), 0);

      auto highest_populated_code = [&]() {
        for (int code = 1023; code >= 0; --code) {
          if (histogram[code] != 0) {
            return code;
          }
        }
        return 0;
      };
      auto percentile_code = [&](const double fraction) {
        const auto target = static_cast<std::uint64_t>(std::ceil(static_cast<double>(pixel_count) * fraction));
        std::uint64_t accumulated = 0;
        for (int code = 0; code <= 1023; ++code) {
          accumulated += histogram[code];
          if (accumulated >= target) {
            return code;
          }
        }
        return 1023;
      };
      auto count_above_nits = [&](const double threshold_nits) {
        std::uint64_t count = 0;
        for (int code = 0; code <= 1023; ++code) {
          if (p010_luma_code_to_nits(code, truehdr_output_full_range) > threshold_nits) {
            count += histogram[code];
          }
        }
        return count;
      };

      const int maximum_code = highest_populated_code();
      const int p9999_code = percentile_code(0.9999);
      const auto over_1000 = count_above_nits(1000.0);
      const auto over_90_percent_target = count_above_nits(std::max(request->peak_brightness * 0.9, 1000.0));
      const double over_1000_percent = pixel_count == 0 ? 0.0 : 100.0 * static_cast<double>(over_1000) / static_cast<double>(pixel_count);
      const double over_target_percent = pixel_count == 0 ? 0.0 : 100.0 * static_cast<double>(over_90_percent_target) / static_cast<double>(pixel_count);

      BOOST_LOG(info) << "RTX HDR live P010 readback"
                      << " (peak=" << request->peak_brightness
                      << " nits, middleGray=" << request->middle_gray
                      << ", contrast=" << request->contrast
                      << ", saturation=" << request->saturation
                      << ", range=" << (truehdr_output_full_range ? "full" : "limited")
                      << ", maxCode=" << maximum_code
                      << ", maxLuma=" << pq_to_nits(truehdr_output_full_range ?
                                           static_cast<double>(maximum_code) / 1023.0 :
                                           std::clamp((static_cast<double>(maximum_code) - 64.0) / 876.0, 0.0, 1.0))
                      << " nits, p99.99Luma=" << p010_luma_code_to_nits(p9999_code, truehdr_output_full_range)
                      << " nits, pixelsAbove1000=" << over_1000 << '/' << pixel_count
                      << " [" << over_1000_percent << "%]"
                      << ", pixelsAbove90PercentTarget=" << over_90_percent_target << '/' << pixel_count
                      << " [" << over_target_percent << "%]).";
    }
#endif

    void apply_colorspace(const ::video::sunshine_colorspace_t &colorspace, bool rtx_hdr_active) {
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
      // Remember whether we are emitting HDR, so the convert step knows it may need to
      // synthesize HDR from an SDR capture via TrueHDR.
      truehdr_active = rtx_hdr_active;
      truehdr_output_hdr = ::video::colorspace_is_hdr(colorspace);
      truehdr_output_full_range = colorspace.full_range;
#endif
      auto color_vectors = ::video::color_vectors_from_colorspace(colorspace, true);

      if (format == DXGI_FORMAT_AYUV ||
          format == DXGI_FORMAT_R16_UINT ||
          format == DXGI_FORMAT_Y410) {
        color_vectors = ::video::color_vectors_from_colorspace(colorspace, false);
      }

      if (!color_vectors) {
        BOOST_LOG(error) << "No vector data for colorspace"sv;
        return;
      }

      auto color_matrix = make_buffer(device.get(), *color_vectors);
      if (!color_matrix) {
        BOOST_LOG(warning) << "Failed to create color matrix"sv;
        return;
      }

      device_ctx->VSSetConstantBuffers(3, 1, &color_matrix);
      device_ctx->PSSetConstantBuffers(0, 1, &color_matrix);
      this->color_matrix = std::move(color_matrix);
    }

    int init_output(ID3D11Texture2D *frame_texture, int width, int height, const ::video::sunshine_colorspace_t &colorspace) {
      // The underlying frame pool owns the texture, so we must reference it for ourselves
      frame_texture->AddRef();
      output_texture.reset(frame_texture);

      HRESULT status = S_OK;

#define create_vertex_shader_helper(x, y) \
  if (!x) { \
    BOOST_LOG(error) << "Cannot create vertex shader " << #x << ": shader bytecode is missing"; \
    return -1; \
  } \
  if (FAILED(status = device->CreateVertexShader(x->GetBufferPointer(), x->GetBufferSize(), nullptr, &y))) { \
    BOOST_LOG(error) << "Failed to create vertex shader " << #x << ": " << util::log_hex(status); \
    return -1; \
  }
#define create_pixel_shader_helper(x, y) \
  if (!x) { \
    BOOST_LOG(error) << "Cannot create pixel shader " << #x << ": shader bytecode is missing"; \
    return -1; \
  } \
  if (FAILED(status = device->CreatePixelShader(x->GetBufferPointer(), x->GetBufferSize(), nullptr, &y))) { \
    BOOST_LOG(error) << "Failed to create pixel shader " << #x << ": " << util::log_hex(status); \
    return -1; \
  }

      const bool downscaling = display->width > width || display->height > height;
      const bool target_hdr = ::video::colorspace_is_hdr(colorspace);

      switch (format) {
        case DXGI_FORMAT_NV12:
          // Semi-planar 8-bit YUV 4:2:0
          create_vertex_shader_helper(convert_yuv420_planar_y_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv420_planar_y_ps_hlsl, convert_Y_or_YUV_ps);
          create_pixel_shader_helper(convert_yuv420_planar_y_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          convert_Y_or_YUV_sdr_to_pq_ps.reset();
          if (downscaling) {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_hlsl, convert_UV_ps);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear_hlsl, convert_UV_fp16_ps);
            convert_UV_sdr_to_pq_ps.reset();
          } else {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_hlsl, convert_UV_ps);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear_hlsl, convert_UV_fp16_ps);
            convert_UV_sdr_to_pq_ps.reset();
          }
          break;

        case DXGI_FORMAT_P010:
          // Semi-planar 16-bit YUV 4:2:0, 10 most significant bits store the value
          create_vertex_shader_helper(convert_yuv420_planar_y_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv420_planar_y_ps_hlsl, convert_Y_or_YUV_ps);
          if (target_hdr) {
            create_pixel_shader_helper(convert_yuv420_planar_y_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
            create_pixel_shader_helper(convert_yuv420_planar_y_ps_sdr_to_pq_hlsl, convert_Y_or_YUV_sdr_to_pq_ps);
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
            create_pixel_shader_helper(convert_yuv420_planar_y_ps_truehdr_peak_hlsl, convert_Y_or_YUV_truehdr_peak_ps);
#endif
          } else {
            create_pixel_shader_helper(convert_yuv420_planar_y_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
            convert_Y_or_YUV_sdr_to_pq_ps.reset();
          }
          if (downscaling) {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_hlsl, convert_UV_ps);
            if (target_hdr) {
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer_hlsl, convert_UV_fp16_ps);
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_sdr_to_pq_hlsl, convert_UV_sdr_to_pq_ps);
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_truehdr_peak_hlsl, convert_UV_truehdr_peak_ps);
#endif
            } else {
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear_hlsl, convert_UV_fp16_ps);
              convert_UV_sdr_to_pq_ps.reset();
            }
          } else {
            create_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs_hlsl, convert_UV_vs);
            create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_hlsl, convert_UV_ps);
            if (target_hdr) {
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_perceptual_quantizer_hlsl, convert_UV_fp16_ps);
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_sdr_to_pq_hlsl, convert_UV_sdr_to_pq_ps);
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_truehdr_peak_hlsl, convert_UV_truehdr_peak_ps);
#endif
            } else {
              create_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear_hlsl, convert_UV_fp16_ps);
              convert_UV_sdr_to_pq_ps.reset();
            }
          }
          break;

        case DXGI_FORMAT_R16_UINT:
          // Planar 16-bit YUV 4:4:4, 10 most significant bits store the value
          create_vertex_shader_helper(convert_yuv444_planar_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_planar_ps_hlsl, convert_Y_or_YUV_ps);
          if (target_hdr) {
            create_pixel_shader_helper(convert_yuv444_planar_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
            create_pixel_shader_helper(convert_yuv444_planar_ps_sdr_to_pq_hlsl, convert_Y_or_YUV_sdr_to_pq_ps);
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
            create_pixel_shader_helper(convert_yuv444_planar_ps_truehdr_peak_hlsl, convert_Y_or_YUV_truehdr_peak_ps);
#endif
          } else {
            create_pixel_shader_helper(convert_yuv444_planar_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
            convert_Y_or_YUV_sdr_to_pq_ps.reset();
          }
          break;

        case DXGI_FORMAT_AYUV:
          // Packed 8-bit YUV 4:4:4
          create_vertex_shader_helper(convert_yuv444_packed_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_hlsl, convert_Y_or_YUV_ps);
          create_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
          convert_Y_or_YUV_sdr_to_pq_ps.reset();
          break;

        case DXGI_FORMAT_Y410:
          // Packed 10-bit YUV 4:4:4
          create_vertex_shader_helper(convert_yuv444_packed_vs_hlsl, convert_Y_or_YUV_vs);
          create_pixel_shader_helper(convert_yuv444_packed_y410_ps_hlsl, convert_Y_or_YUV_ps);
          if (target_hdr) {
            create_pixel_shader_helper(convert_yuv444_packed_y410_ps_perceptual_quantizer_hlsl, convert_Y_or_YUV_fp16_ps);
            create_pixel_shader_helper(convert_yuv444_packed_y410_ps_sdr_to_pq_hlsl, convert_Y_or_YUV_sdr_to_pq_ps);
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
            create_pixel_shader_helper(convert_yuv444_packed_y410_ps_truehdr_peak_hlsl, convert_Y_or_YUV_truehdr_peak_ps);
#endif
          } else {
            create_pixel_shader_helper(convert_yuv444_packed_y410_ps_linear_hlsl, convert_Y_or_YUV_fp16_ps);
            convert_Y_or_YUV_sdr_to_pq_ps.reset();
          }
          break;

        default:
          BOOST_LOG(error) << "Unable to create shaders because of the unrecognized surface format";
          return -1;
      }

#undef create_vertex_shader_helper
#undef create_pixel_shader_helper

      auto out_width = width;
      auto out_height = height;

      float in_width = display->width;
      float in_height = display->height;

      // Ensure aspect ratio is maintained
      auto scalar = std::fminf(out_width / in_width, out_height / in_height);
      auto out_width_f = in_width * scalar;
      auto out_height_f = in_height * scalar;

      // result is always positive
      auto offsetX = (out_width - out_width_f) / 2;
      auto offsetY = (out_height - out_height_f) / 2;

      out_Y_or_YUV_viewports[0] = {offsetX, offsetY, out_width_f, out_height_f, 0.0f, 1.0f};  // Y plane
      out_Y_or_YUV_viewports[1] = out_Y_or_YUV_viewports[0];  // U plane
      out_Y_or_YUV_viewports[1].TopLeftY += out_height;
      out_Y_or_YUV_viewports[2] = out_Y_or_YUV_viewports[1];  // V plane
      out_Y_or_YUV_viewports[2].TopLeftY += out_height;

      out_Y_or_YUV_viewports_for_clear[0] = {0, 0, (float) out_width, (float) out_height, 0.0f, 1.0f};  // Y plane
      out_Y_or_YUV_viewports_for_clear[1] = out_Y_or_YUV_viewports_for_clear[0];  // U plane
      out_Y_or_YUV_viewports_for_clear[1].TopLeftY += out_height;
      out_Y_or_YUV_viewports_for_clear[2] = out_Y_or_YUV_viewports_for_clear[1];  // V plane
      out_Y_or_YUV_viewports_for_clear[2].TopLeftY += out_height;

      out_UV_viewport = {offsetX / 2, offsetY / 2, out_width_f / 2, out_height_f / 2, 0.0f, 1.0f};
      out_UV_viewport_for_clear = {0, 0, (float) out_width / 2, (float) out_height / 2, 0.0f, 1.0f};

      float subsample_offset_in[16 / sizeof(float)] {1.0f / (float) out_width_f, 1.0f / (float) out_height_f};  // aligned to 16-byte
      subsample_offset = make_buffer(device.get(), subsample_offset_in);

      if (!subsample_offset) {
        BOOST_LOG(error) << "Failed to create subsample offset vertex constant buffer";
        return -1;
      }
      device_ctx->VSSetConstantBuffers(0, 1, &subsample_offset);

      {
        int32_t rotation_modifier = display->display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? 0 : display->display_rotation - 1;
        int32_t rotation_data[16 / sizeof(int32_t)] {-rotation_modifier};  // aligned to 16-byte
        auto rotation = make_buffer(device.get(), rotation_data);
        if (!rotation) {
          BOOST_LOG(error) << "Failed to create display rotation vertex constant buffer";
          return -1;
        }
        device_ctx->VSSetConstantBuffers(1, 1, &rotation);
      }

      DXGI_FORMAT rtv_Y_or_YUV_format = DXGI_FORMAT_UNKNOWN;
      DXGI_FORMAT rtv_UV_format = DXGI_FORMAT_UNKNOWN;
      bool rtv_simple_clear = false;

      switch (format) {
        case DXGI_FORMAT_NV12:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R8_UNORM;
          rtv_UV_format = DXGI_FORMAT_R8G8_UNORM;
          rtv_simple_clear = true;
          break;

        case DXGI_FORMAT_P010:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R16_UNORM;
          rtv_UV_format = DXGI_FORMAT_R16G16_UNORM;
          rtv_simple_clear = true;
          break;

        case DXGI_FORMAT_AYUV:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R8G8B8A8_UINT;
          break;

        case DXGI_FORMAT_R16_UINT:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R16_UINT;
          break;

        case DXGI_FORMAT_Y410:
          rtv_Y_or_YUV_format = DXGI_FORMAT_R10G10B10A2_UINT;
          break;

        default:
          BOOST_LOG(error) << "Unable to create render target views because of the unrecognized surface format";
          return -1;
      }

      auto create_rtv = [&](auto &rt, DXGI_FORMAT rt_format) -> bool {
        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = rt_format;
        rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        auto status = device->CreateRenderTargetView(output_texture.get(), &rtv_desc, &rt);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Failed to create render target view: " << util::log_hex(status);
          return false;
        }

        return true;
      };

      // Create Y/YUV render target view
      if (!create_rtv(out_Y_or_YUV_rtv, rtv_Y_or_YUV_format)) {
        return -1;
      }

      // Create UV render target view if needed
      if (rtv_UV_format != DXGI_FORMAT_UNKNOWN && !create_rtv(out_UV_rtv, rtv_UV_format)) {
        return -1;
      }

      if (rtv_simple_clear) {
        // Clear the RTVs to ensure the aspect ratio padding is black
        const float y_black[] = {0.0f, 0.0f, 0.0f, 0.0f};
        device_ctx->ClearRenderTargetView(out_Y_or_YUV_rtv.get(), y_black);
        if (out_UV_rtv) {
          const float uv_black[] = {0.5f, 0.5f, 0.5f, 0.5f};
          device_ctx->ClearRenderTargetView(out_UV_rtv.get(), uv_black);
        }
        rtvs_cleared = true;
      } else {
        // Can't use ClearRenderTargetView(), will clear on first convert()
        rtvs_cleared = false;
      }

      return 0;
    }

    int init(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      switch (pix_fmt) {
        case pix_fmt_e::nv12:
          format = DXGI_FORMAT_NV12;
          break;

        case pix_fmt_e::p010:
          format = DXGI_FORMAT_P010;
          break;

        case pix_fmt_e::ayuv:
          format = DXGI_FORMAT_AYUV;
          break;

        case pix_fmt_e::yuv444p16:
          format = DXGI_FORMAT_R16_UINT;
          break;

        case pix_fmt_e::y410:
          format = DXGI_FORMAT_Y410;
          break;

        default:
          BOOST_LOG(error) << "D3D11 backend doesn't support pixel format: " << from_pix_fmt(pix_fmt);
          return -1;
      }

      D3D_FEATURE_LEVEL featureLevels[] {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
      };

      HRESULT status = D3D11CreateDevice(
        adapter_p,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_FLAGS | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        featureLevels,
        sizeof(featureLevels) / sizeof(D3D_FEATURE_LEVEL),
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        &device_ctx
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create encoder D3D11 device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      dxgi::dxgi_t dxgi;
      status = device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      status = dxgi->SetGPUThreadPriority(0x4000001E);
      if (FAILED(status)) {
        BOOST_LOG(info) << "Failed to request absoloute encoding GPU thread priority. Trying relative priority.";
        status = dxgi->SetGPUThreadPriority(7);
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Failed to request relative encoding GPU thread priority. Please run application as administrator for optimal performance.";
        } else {
          BOOST_LOG(info) << "Relative encoding GPU thread priority request success.";
        }
      }

      auto default_color_vectors = ::video::color_vectors_from_colorspace({::video::colorspace_e::rec601, false, 8}, true);
      if (!default_color_vectors) {
        BOOST_LOG(error) << "Missing color vectors for Rec. 601"sv;
        return -1;
      }

      color_matrix = make_buffer(device.get(), *default_color_vectors);
      if (!color_matrix) {
        BOOST_LOG(error) << "Failed to create color matrix buffer"sv;
        return -1;
      }
      ensure_sdr_to_pq_params(100.0f);
      if (!sdr_to_pq_params) {
        BOOST_LOG(error) << "Failed to create SDR-to-PQ parameter buffer"sv;
        return -1;
      }
      device_ctx->VSSetConstantBuffers(3, 1, &color_matrix);
      device_ctx->PSSetConstantBuffers(0, 1, &color_matrix);

      this->display = std::dynamic_pointer_cast<display_base_t>(display);
      if (!this->display) {
        return -1;
      }
      display = nullptr;

      blend_disable = make_blend(device.get(), false, false);
      if (!blend_disable) {
        return -1;
      }

      D3D11_SAMPLER_DESC sampler_desc {};
      sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
      sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
      sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
      sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
      sampler_desc.MinLOD = 0;
      sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

      status = device->CreateSamplerState(&sampler_desc, &sampler_linear);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create point sampler state [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
      device_ctx->PSSetSamplers(0, 1, &sampler_linear);
      device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      return 0;
    }

    struct encoder_img_ctx_t {
      // Used to determine if the underlying texture changes.
      // Not safe for actual use by the encoder!
      texture2d_t::const_pointer capture_texture_p;

      texture2d_t encoder_texture;
      shader_res_t encoder_input_res;
      keyed_mutex_t encoder_mutex;
      texture2d_t truehdr_input_texture;
      shader_res_t truehdr_input_res;

      std::weak_ptr<const platf::img_t> img_weak;

      void reset() {
        capture_texture_p = nullptr;
        encoder_texture.reset();
        encoder_input_res.reset();
        encoder_mutex.reset();
        truehdr_input_texture.reset();
        truehdr_input_res.reset();
        img_weak.reset();
      }
    };

    int initialize_image_context(const img_d3d_t &img, encoder_img_ctx_t &img_ctx) {
      // If we've already opened the shared texture, we're done
      if (img_ctx.encoder_texture && img.capture_texture.get() == img_ctx.capture_texture_p) {
        return 0;
      }

      // Reset this image context in case it was used before with a different texture.
      // Textures can change when transitioning from a dummy image to a real image.
      img_ctx.reset();

      device1_t device1;
      auto status = device->QueryInterface(__uuidof(ID3D11Device1), (void **) &device1);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query ID3D11Device1 [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Open a handle to the shared texture
      status = device1->OpenSharedResource1(img.encoder_texture_handle, __uuidof(ID3D11Texture2D), (void **) &img_ctx.encoder_texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to open shared image texture [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Get the keyed mutex to synchronize with the capture code
      status = img_ctx.encoder_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &img_ctx.encoder_mutex);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to query IDXGIKeyedMutex [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Create the SRV for the encoder texture
      status = device->CreateShaderResourceView(img_ctx.encoder_texture.get(), nullptr, &img_ctx.encoder_input_res);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create shader resource view for encoding [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      img_ctx.capture_texture_p = img.capture_texture.get();

      img_ctx.img_weak = img.weak_from_this();

      return 0;
    }

#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
    bool ensure_truehdr_input_context(encoder_img_ctx_t &img_ctx) {
      D3D11_TEXTURE2D_DESC src_desc {};
      img_ctx.encoder_texture->GetDesc(&src_desc);

      if (img_ctx.truehdr_input_texture) {
        D3D11_TEXTURE2D_DESC existing_desc {};
        img_ctx.truehdr_input_texture->GetDesc(&existing_desc);
        if (existing_desc.Width == src_desc.Width &&
            existing_desc.Height == src_desc.Height &&
            existing_desc.Format == src_desc.Format &&
            existing_desc.SampleDesc.Count == src_desc.SampleDesc.Count &&
            existing_desc.SampleDesc.Quality == src_desc.SampleDesc.Quality) {
          return true;
        }
      }

      img_ctx.truehdr_input_res.reset();
      img_ctx.truehdr_input_texture.reset();

      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = src_desc.Width;
      desc.Height = src_desc.Height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = src_desc.Format;
      desc.SampleDesc = src_desc.SampleDesc;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

      auto status = device->CreateTexture2D(&desc, nullptr, &img_ctx.truehdr_input_texture);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "RTX HDR: failed to create private TrueHDR input texture [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }

      status = device->CreateShaderResourceView(img_ctx.truehdr_input_texture.get(), nullptr, &img_ctx.truehdr_input_res);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "RTX HDR: failed to create private TrueHDR input view [0x"sv << util::hex(status).to_string_view() << ']';
        img_ctx.truehdr_input_texture.reset();
        return false;
      }

      return true;
    }

    bool ensure_truehdr_native_hdr_readback(DXGI_FORMAT format) {
      if (truehdr_native_hdr_readback_texture) {
        D3D11_TEXTURE2D_DESC existing_desc {};
        truehdr_native_hdr_readback_texture->GetDesc(&existing_desc);
        if (existing_desc.Width == TRUEHDR_NATIVE_HDR_SAMPLE_WIDTH &&
            existing_desc.Height == TRUEHDR_NATIVE_HDR_SAMPLE_HEIGHT &&
            existing_desc.Format == format) {
          return true;
        }
      }

      truehdr_native_hdr_readback_texture.reset();

      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = TRUEHDR_NATIVE_HDR_SAMPLE_WIDTH;
      desc.Height = TRUEHDR_NATIVE_HDR_SAMPLE_HEIGHT;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = format;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto status = device->CreateTexture2D(&desc, nullptr, &truehdr_native_hdr_readback_texture);
      if (FAILED(status)) {
        if (!truehdr_native_hdr_readback_failure_logged) {
          BOOST_LOG(warning) << "RTX HDR: failed to create native HDR detector readback texture [0x"sv << util::hex(status).to_string_view() << ']';
          truehdr_native_hdr_readback_failure_logged = true;
        }
        return false;
      }

      return true;
    }

    void reset_truehdr_native_hdr_detector() {
      truehdr_native_hdr_confirmed = false;
      truehdr_native_hdr_confirmation_count = 0;
    }

    bool update_truehdr_native_hdr_detector(ID3D11Texture2D *texture, DXGI_FORMAT format) {
      if (!texture || !truehdr_native_hdr_detectable_format(format)) {
        reset_truehdr_native_hdr_detector();
        return false;
      }

      D3D11_TEXTURE2D_DESC src_desc {};
      texture->GetDesc(&src_desc);
      if (src_desc.Width < TRUEHDR_NATIVE_HDR_PATCH_SIZE ||
          src_desc.Height < TRUEHDR_NATIVE_HDR_PATCH_SIZE ||
          src_desc.SampleDesc.Count != 1 ||
          !ensure_truehdr_native_hdr_readback(format)) {
        reset_truehdr_native_hdr_detector();
        return false;
      }

      for (UINT y = 0; y < TRUEHDR_NATIVE_HDR_GRID_SIZE; ++y) {
        const UINT center_y = ((y + 1) * src_desc.Height) / (TRUEHDR_NATIVE_HDR_GRID_SIZE + 1);
        const UINT src_y = std::min(
          src_desc.Height - TRUEHDR_NATIVE_HDR_PATCH_SIZE,
          center_y > TRUEHDR_NATIVE_HDR_PATCH_SIZE / 2 ? center_y - TRUEHDR_NATIVE_HDR_PATCH_SIZE / 2 : 0
        );
        for (UINT x = 0; x < TRUEHDR_NATIVE_HDR_GRID_SIZE; ++x) {
          const UINT center_x = ((x + 1) * src_desc.Width) / (TRUEHDR_NATIVE_HDR_GRID_SIZE + 1);
          const UINT src_x = std::min(
            src_desc.Width - TRUEHDR_NATIVE_HDR_PATCH_SIZE,
            center_x > TRUEHDR_NATIVE_HDR_PATCH_SIZE / 2 ? center_x - TRUEHDR_NATIVE_HDR_PATCH_SIZE / 2 : 0
          );
          const D3D11_BOX box {
            src_x,
            src_y,
            0,
            src_x + TRUEHDR_NATIVE_HDR_PATCH_SIZE,
            src_y + TRUEHDR_NATIVE_HDR_PATCH_SIZE,
            1
          };
          device_ctx->CopySubresourceRegion(
            truehdr_native_hdr_readback_texture.get(),
            0,
            x * TRUEHDR_NATIVE_HDR_PATCH_SIZE,
            y * TRUEHDR_NATIVE_HDR_PATCH_SIZE,
            0,
            texture,
            0,
            &box
          );
        }
      }

      D3D11_MAPPED_SUBRESOURCE mapped {};
      const auto status = device_ctx->Map(truehdr_native_hdr_readback_texture.get(), 0, D3D11_MAP_READ, 0, &mapped);
      if (FAILED(status)) {
        if (!truehdr_native_hdr_readback_failure_logged) {
          BOOST_LOG(warning) << "RTX HDR: failed to read native HDR detector samples [0x"sv << util::hex(status).to_string_view() << ']';
          truehdr_native_hdr_readback_failure_logged = true;
        }
        reset_truehdr_native_hdr_detector();
        return false;
      }

      unsigned bright_pixels = 0;
      for (UINT y = 0; y < TRUEHDR_NATIVE_HDR_SAMPLE_HEIGHT; ++y) {
        const auto *row = static_cast<const std::uint8_t *>(mapped.pData) + mapped.RowPitch * y;
        for (UINT x = 0; x < TRUEHDR_NATIVE_HDR_SAMPLE_WIDTH; ++x) {
          const auto *pixel = reinterpret_cast<const std::uint16_t *>(row + x * sizeof(std::uint16_t) * 4);
          const float r = half_to_float(pixel[0]);
          const float g = half_to_float(pixel[1]);
          const float b = half_to_float(pixel[2]);
          if (r > TRUEHDR_NATIVE_HDR_SDR_WHITE_THRESHOLD ||
              g > TRUEHDR_NATIVE_HDR_SDR_WHITE_THRESHOLD ||
              b > TRUEHDR_NATIVE_HDR_SDR_WHITE_THRESHOLD) {
            ++bright_pixels;
          }
        }
      }
      device_ctx->Unmap(truehdr_native_hdr_readback_texture.get(), 0);

      if (bright_pixels >= TRUEHDR_NATIVE_HDR_MIN_BRIGHT_PIXELS) {
        truehdr_native_hdr_confirmation_count = std::min(
          TRUEHDR_NATIVE_HDR_CONFIRMATION_FRAMES,
          truehdr_native_hdr_confirmation_count + 1
        );
      } else {
        reset_truehdr_native_hdr_detector();
        return false;
      }

      truehdr_native_hdr_confirmed = truehdr_native_hdr_confirmation_count >= TRUEHDR_NATIVE_HDR_CONFIRMATION_FRAMES;
      return truehdr_native_hdr_confirmed;
    }
#endif

    bool ensure_black_texture_for_rtv_clear() {
      if (black_texture_for_clear_srv) {
        return true;
      }

      constexpr auto width = 32;
      constexpr auto height = 32;

      D3D11_TEXTURE2D_DESC texture_desc = {};
      texture_desc.Width = width;
      texture_desc.Height = height;
      texture_desc.MipLevels = 1;
      texture_desc.ArraySize = 1;
      texture_desc.SampleDesc.Count = 1;
      texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
      texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

      std::vector<uint8_t> mem(4 * width * height, 0);
      D3D11_SUBRESOURCE_DATA texture_data = {mem.data(), 4 * width, 0};

      texture2d_t texture;
      auto status = device->CreateTexture2D(&texture_desc, &texture_data, &texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create black texture: " << util::log_hex(status);
        return false;
      }

      shader_res_t resource_view;
      status = device->CreateShaderResourceView(texture.get(), nullptr, &resource_view);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create black texture resource view: " << util::log_hex(status);
        return false;
      }

      black_texture_for_clear = std::move(texture);
      black_texture_for_clear_srv = std::move(resource_view);
      return true;
    }

    ::video::color_t *color_p;

    // Destroyed last (declared first): the encoder holds a shared_ptr to the capture display, and the
    // capture thread waits on `while (display_wp->use_count() != 1)` before it tears down / recreates the
    // capture device. Releasing this reference is what unblocks that wait, so it must outlive everything
    // below that still references the capture device -- the encoder's own D3D11 device/context and the
    // shared encoder textures + keyed mutexes in img_ctx_map (opened from the capture device via
    // OpenSharedResource1). If `display` were released first, the capture thread could destroy the capture
    // device concurrently with this device's DestroyDriverInstance, faulting the NVIDIA UMD on a freed
    // cross-device shared-resource dependency.
    std::shared_ptr<display_base_t> display;

    // Keep the underlying D3D device/context alive until after all dependent resources have been released.
    device_t device;
    device_ctx_t device_ctx;

#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
    // NVIDIA TrueHDR (RTX HDR) SDR->HDR. Lazily created on the first HDR frame.
    std::unique_ptr<nv_truehdr_t> truehdr_engine;
    shader_res_t truehdr_srv;
    ID3D11Texture2D *truehdr_srv_texture = nullptr;
    bool truehdr_active = false;
    bool truehdr_output_hdr = false;
    bool truehdr_conversion_logged = false;
    bool truehdr_failure_logged = false;
    bool truehdr_unsupported_format_logged = false;
    bool truehdr_not_hdr_stream_logged = false;
    std::string truehdr_last_transition_key;
    platf::rtx_hdr::runtime_t rtx_hdr_runtime;
    texture2d_t truehdr_native_hdr_readback_texture;
    unsigned truehdr_native_hdr_confirmation_count = 0;
    bool truehdr_native_hdr_confirmed = false;
    bool truehdr_native_hdr_logged = false;
    bool truehdr_native_hdr_readback_failure_logged = false;
    buf_t truehdr_peak_params;
    float truehdr_peak_luminance_scale = 1.0f;
    int truehdr_last_compensated_peak_nits = 0;
    int truehdr_last_compensated_middle_gray = 0;
    bool truehdr_peak_compensation_failure_logged = false;
    bool truehdr_output_full_range = false;
    texture2d_t truehdr_live_readback_texture;
    std::optional<truehdr_live_readback_request_t> truehdr_live_readback_request;
    bool truehdr_live_readback_failure_logged = false;
#endif

    texture2d_t output_texture;
    texture2d_t black_texture_for_clear;
    shader_res_t black_texture_for_clear_srv;

    buf_t subsample_offset;
    buf_t color_matrix;
    buf_t sdr_to_pq_params;
    float sdr_to_pq_white_nits = 100.0f;

    blend_t blend_disable;
    sampler_state_t sampler_linear;

    render_target_t out_Y_or_YUV_rtv;
    render_target_t out_UV_rtv;
    bool rtvs_cleared = false;

    // d3d_img_t::id -> encoder_img_ctx_t
    // These store the encoder textures for each img_t that passes through
    // convert(). We can't store them in the img_t itself because it is shared
    // amongst multiple hwdevice_t objects (and therefore multiple ID3D11Devices).
    std::map<uint32_t, encoder_img_ctx_t> img_ctx_map;

    // NOTE: `display` is intentionally declared near the top of this member list (not here) so it is
    // destroyed last. See the comment on its declaration above.

    vs_t convert_Y_or_YUV_vs;
    ps_t convert_Y_or_YUV_ps;
    ps_t convert_Y_or_YUV_fp16_ps;
    ps_t convert_Y_or_YUV_sdr_to_pq_ps;
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
    ps_t convert_Y_or_YUV_truehdr_peak_ps;
#endif

    vs_t convert_UV_vs;
    ps_t convert_UV_ps;
    ps_t convert_UV_fp16_ps;
    ps_t convert_UV_sdr_to_pq_ps;
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
    ps_t convert_UV_truehdr_peak_ps;
#endif

    std::array<D3D11_VIEWPORT, 3> out_Y_or_YUV_viewports;
    std::array<D3D11_VIEWPORT, 3> out_Y_or_YUV_viewports_for_clear;
    D3D11_VIEWPORT out_UV_viewport;
    D3D11_VIEWPORT out_UV_viewport_for_clear;

    DXGI_FORMAT format;
  };

  class d3d_avcodec_encode_device_t: public avcodec_encode_device_t {
  public:
    int init(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      int result = base.init(display, adapter_p, pix_fmt);
      data = base.device.get();
      return result;
    }

    int convert(platf::img_t &img_base) override {
      return base.convert(img_base);
    }

    void apply_colorspace() override {
      base.apply_colorspace(colorspace, rtx_hdr_active);
    }

    void init_hwframes(AVHWFramesContext *frames) override {
      // We may be called with a QSV or D3D11VA context
      if (frames->device_ctx->type == AV_HWDEVICE_TYPE_D3D11VA) {
        auto d3d11_frames = (AVD3D11VAFramesContext *) frames->hwctx;

        // The encoder requires textures with D3D11_BIND_RENDER_TARGET set
        d3d11_frames->BindFlags = D3D11_BIND_RENDER_TARGET;
        d3d11_frames->MiscFlags = 0;
      }

      // We require a single texture
      frames->initial_pool_size = 1;
    }

    int prepare_to_derive_context(int hw_device_type) override {
      // QuickSync requires our device to be multithread-protected
      if (hw_device_type == AV_HWDEVICE_TYPE_QSV) {
        multithread_t mt;

        auto status = base.device->QueryInterface(IID_ID3D11Multithread, (void **) &mt);
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Failed to query ID3D11Multithread interface from device [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }

        mt->SetMultithreadProtected(TRUE);
      }

      return 0;
    }

    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override {
      this->hwframe.reset(frame);
      this->frame = frame;

      // Populate this frame with a hardware buffer if one isn't there already
      if (!frame->buf[0]) {
        auto err = av_hwframe_get_buffer(hw_frames_ctx, frame, 0);
        if (err) {
          char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
          BOOST_LOG(error) << "Failed to get hwframe buffer: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);
          return -1;
        }
      }

      // If this is a frame from a derived context, we'll need to map it to D3D11
      ID3D11Texture2D *frame_texture;
      if (frame->format != AV_PIX_FMT_D3D11) {
        frame_t d3d11_frame {av_frame_alloc()};

        d3d11_frame->format = AV_PIX_FMT_D3D11;

        auto err = av_hwframe_map(d3d11_frame.get(), frame, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
        if (err) {
          char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
          BOOST_LOG(error) << "Failed to map D3D11 frame: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);
          return -1;
        }

        // Get the texture from the mapped frame
        frame_texture = (ID3D11Texture2D *) d3d11_frame->data[0];
      } else {
        // Otherwise, we can just use the texture inside the original frame
        frame_texture = (ID3D11Texture2D *) frame->data[0];
      }

      return base.init_output(frame_texture, frame->width, frame->height, colorspace);
    }

  private:
    d3d_base_encode_device base;
    frame_t hwframe;
  };

  class d3d_nvenc_encode_device_t: public nvenc_encode_device_t {
  public:
    bool init_device(std::shared_ptr<platf::display_t> display, adapter_t::pointer adapter_p, pix_fmt_e pix_fmt) {
      buffer_format = nvenc::nvenc_format_from_sunshine_format(pix_fmt);
      if (buffer_format == NV_ENC_BUFFER_FORMAT_UNDEFINED) {
        BOOST_LOG(error) << "Unexpected pixel format for NvENC ["sv << from_pix_fmt(pix_fmt) << ']';
        return false;
      }

      if (base.init(display, adapter_p, pix_fmt)) {
        return false;
      }

      // Async encoder teardown may destroy D3D resources on a different thread.
      // Enable D3D multithread protection for safety.
      multithread_t mt;
      auto status = base.device->QueryInterface(IID_ID3D11Multithread, (void **) &mt);
      if (SUCCEEDED(status)) {
        mt->SetMultithreadProtected(TRUE);
      } else {
        BOOST_LOG(warning) << "Failed to query ID3D11Multithread interface from device [0x"sv << util::hex(status).to_string_view() << ']';
      }

      if (pix_fmt == pix_fmt_e::yuv444p16) {
        nvenc_d3d = std::make_unique<nvenc::nvenc_d3d11_on_cuda>(base.device.get());
      } else {
        nvenc_d3d = std::make_unique<nvenc::nvenc_d3d11_native>(base.device.get());
      }
      nvenc = nvenc_d3d.get();

      return true;
    }

    bool init_encoder(const ::video::config_t &client_config, const ::video::sunshine_colorspace_t &colorspace) override {
      if (!nvenc_d3d) {
        return false;
      }

      auto nvenc_colorspace = nvenc::nvenc_colorspace_from_sunshine_colorspace(colorspace);
      if (!nvenc_d3d->create_encoder(
            config::video.nv,
            client_config,
            nvenc_colorspace,
            buffer_format,
            hdr_metadata_valid ? &hdr_metadata : nullptr
          )) {
        return false;
      }

      base.apply_colorspace(colorspace, client_config.rtx_hdr_active);
      return base.init_output(nvenc_d3d->get_input_texture(), client_config.width, client_config.height, colorspace) == 0;
    }

    int convert(platf::img_t &img_base) override {
      return base.convert(img_base);
    }

  private:
    d3d_base_encode_device base;
    std::unique_ptr<nvenc::nvenc_d3d11> nvenc_d3d;
    NV_ENC_BUFFER_FORMAT buffer_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;
  };

  bool set_cursor_texture(device_t::pointer device, gpu_cursor_t &cursor, util::buffer_t<std::uint8_t> &&cursor_img, DXGI_OUTDUPL_POINTER_SHAPE_INFO &shape_info) {
    // This cursor image may not be used
    if (cursor_img.size() == 0) {
      cursor.input_res.reset();
      cursor.set_texture(0, 0, nullptr);
      return true;
    }

    D3D11_SUBRESOURCE_DATA data {
      std::begin(cursor_img),
      4 * shape_info.Width,
      0
    };

    // Create texture for cursor
    D3D11_TEXTURE2D_DESC t {};
    t.Width = shape_info.Width;
    t.Height = cursor_img.size() / data.SysMemPitch;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_IMMUTABLE;
    t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    texture2d_t texture;
    auto status = device->CreateTexture2D(&t, &data, &texture);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create mouse texture [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    // Free resources before allocating on the next line.
    cursor.input_res.reset();
    status = device->CreateShaderResourceView(texture.get(), nullptr, &cursor.input_res);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create cursor shader resource view [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    cursor.set_texture(t.Width, t.Height, std::move(texture));
    return true;
  }

  capture_e display_ddup_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    DXGI_OUTDUPL_FRAME_INFO frame_info;

    resource_t::pointer res_p {};
    auto capture_status = dup.next_frame(frame_info, timeout, &res_p);
    resource_t res {res_p};

    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    const bool mouse_update_flag = frame_info.LastMouseUpdateTime.QuadPart != 0 || frame_info.PointerShapeBufferSize > 0;
    const bool frame_update_flag = frame_info.LastPresentTime.QuadPart != 0;
    const bool update_flag = mouse_update_flag || frame_update_flag;

    if (!update_flag) {
      return capture_e::timeout;
    }

    const auto host_processing_timestamp = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
    if (auto qpc_displayed = std::max(frame_info.LastPresentTime.QuadPart, frame_info.LastMouseUpdateTime.QuadPart)) {
      // Translate QueryPerformanceCounter() value to steady_clock time point
      frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), qpc_displayed);
    }

    if (frame_info.PointerShapeBufferSize > 0) {
      DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info {};

      util::buffer_t<std::uint8_t> img_data {frame_info.PointerShapeBufferSize};

      UINT dummy;
      status = dup.dup->GetFramePointerShape(img_data.size(), std::begin(img_data), &dummy, &shape_info);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to get new pointer shape [0x"sv << util::hex(status).to_string_view() << ']';

        return capture_e::error;
      }

      auto alpha_cursor_img = make_cursor_alpha_image(img_data, shape_info);
      auto xor_cursor_img = make_cursor_xor_image(img_data, shape_info);

      if (!set_cursor_texture(device.get(), cursor_alpha, std::move(alpha_cursor_img), shape_info) ||
          !set_cursor_texture(device.get(), cursor_xor, std::move(xor_cursor_img), shape_info)) {
        return capture_e::error;
      }
    }

    if (frame_info.LastMouseUpdateTime.QuadPart) {
      cursor_alpha.set_pos(frame_info.PointerPosition.Position.x, frame_info.PointerPosition.Position.y, width, height, display_rotation, frame_info.PointerPosition.Visible);

      cursor_xor.set_pos(frame_info.PointerPosition.Position.x, frame_info.PointerPosition.Position.y, width, height, display_rotation, frame_info.PointerPosition.Visible);
    }

    const bool blend_mouse_cursor_flag = (cursor_alpha.visible || cursor_xor.visible) && cursor_visible;

    texture2d_t src {};
    if (frame_update_flag) {
      // Get the texture object from this frame
      status = res->QueryInterface(IID_ID3D11Texture2D, (void **) &src);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't query interface [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }

      D3D11_TEXTURE2D_DESC desc;
      src->GetDesc(&desc);

      // It's possible for our display enumeration to race with mode changes and result in
      // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
      if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
        BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
        return capture_e::reinit;
      }

      // If we don't know the capture format yet, grab it from this texture
      if (capture_format == DXGI_FORMAT_UNKNOWN) {
        capture_format = desc.Format;
        BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';
      }

      // It's also possible for the capture format to change on the fly. If that happens,
      // reinitialize capture to try format detection again and create new images.
      if (capture_format != desc.Format) {
        BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
        return capture_e::reinit;
      }
    }

    enum class lfa {
      nothing,
      replace_surface_with_img,
      replace_img_with_surface,
      copy_src_to_img,
      copy_src_to_surface,
    };

    enum class ofa {
      forward_last_img,
      copy_last_surface_and_blend_cursor,
      dummy_fallback,
    };

    auto last_frame_action = lfa::nothing;
    auto out_frame_action = ofa::dummy_fallback;

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      // We don't know the final capture format yet, so we will encode a black dummy image
      last_frame_action = lfa::nothing;
      out_frame_action = ofa::dummy_fallback;
    } else {
      if (src) {
        // We got a new frame from DesktopDuplication...
        if (blend_mouse_cursor_flag) {
          // ...and we need to blend the mouse cursor onto it.
          // Copy the frame to intermediate surface so we can blend this and future mouse cursor updates
          // without new frames from DesktopDuplication. We use direct3d surface directly here and not
          // an image from pull_free_image_cb mainly because it's lighter (surface sharing between
          // direct3d devices produce significant memory overhead).
          last_frame_action = lfa::copy_src_to_surface;
          // Copy the intermediate surface to a new image from pull_free_image_cb and blend the mouse cursor onto it.
          out_frame_action = ofa::copy_last_surface_and_blend_cursor;
        } else {
          // ...and we don't need to blend the mouse cursor.
          // Copy the frame to a new image from pull_free_image_cb and save the shared pointer to the image
          // in case the mouse cursor appears without a new frame from DesktopDuplication.
          last_frame_action = lfa::copy_src_to_img;
          // Use saved last image shared pointer as output image evading copy.
          out_frame_action = ofa::forward_last_img;
        }
      } else if (!std::holds_alternative<std::monostate>(last_frame_variant)) {
        // We didn't get a new frame from DesktopDuplication...
        if (blend_mouse_cursor_flag) {
          // ...but we need to blend the mouse cursor.
          if (std::holds_alternative<std::shared_ptr<platf::img_t>>(last_frame_variant)) {
            // We have the shared pointer of the last image, replace it with intermediate surface
            // while copying contents so we can blend this and future mouse cursor updates.
            last_frame_action = lfa::replace_img_with_surface;
          }
          // Copy the intermediate surface which contains last DesktopDuplication frame
          // to a new image from pull_free_image_cb and blend the mouse cursor onto it.
          out_frame_action = ofa::copy_last_surface_and_blend_cursor;
        } else {
          // ...and we don't need to blend the mouse cursor.
          // This happens when the mouse cursor disappears from screen,
          // or there's mouse cursor on screen, but its drawing is disabled in sunshine.
          if (std::holds_alternative<texture2d_t>(last_frame_variant)) {
            // We have the intermediate surface that was used as the mouse cursor blending base.
            // Replace it with an image from pull_free_image_cb copying contents and freeing up the surface memory.
            // Save the shared pointer to the image in case the mouse cursor reappears.
            last_frame_action = lfa::replace_surface_with_img;
          }
          // Use saved last image shared pointer as output image evading copy.
          out_frame_action = ofa::forward_last_img;
        }
      }
    }

    auto create_surface = [&](texture2d_t &surface) -> bool {
      // Try to reuse the old surface if it hasn't been destroyed yet.
      if (old_surface_delayed_destruction) {
        surface.reset(old_surface_delayed_destruction.release());
        return true;
      }

      // Otherwise create a new surface.
      D3D11_TEXTURE2D_DESC t {};
      t.Width = width_before_rotation;
      t.Height = height_before_rotation;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_DEFAULT;
      t.Format = capture_format;
      t.BindFlags = 0;
      status = device->CreateTexture2D(&t, nullptr, &surface);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create frame copy texture [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }

      return true;
    };

    auto get_locked_d3d_img = [&](std::shared_ptr<platf::img_t> &img, bool dummy = false) -> std::tuple<std::shared_ptr<img_d3d_t>, texture_lock_helper> {
      auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);

      // Finish creating the image (if it hasn't happened already),
      // also creates synchronization primitives for shared access from multiple direct3d devices.
      if (complete_img(d3d_img.get(), dummy)) {
        return {nullptr, nullptr};
      }

      // This image is shared between capture direct3d device and encoders direct3d devices,
      // we must acquire lock before doing anything to it.
      texture_lock_helper lock_helper(d3d_img->capture_mutex.get());
      if (!lock_helper.lock()) {
        BOOST_LOG(error) << "Failed to lock capture texture";
        return {nullptr, nullptr};
      }

      // Clear the blank flag now that we're ready to capture into the image
      d3d_img->blank = false;

      return {std::move(d3d_img), std::move(lock_helper)};
    };

    switch (last_frame_action) {
      case lfa::nothing:
        {
          break;
        }

      case lfa::replace_surface_with_img:
        {
          auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!p_surface) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }

          std::shared_ptr<platf::img_t> img;
          if (!pull_free_image_cb(img)) {
            return capture_e::interrupted;
          }

          auto [d3d_img, lock] = get_locked_d3d_img(img);
          if (!d3d_img) {
            return capture_e::error;
          }

          device_ctx->CopyResource(d3d_img->capture_texture.get(), p_surface->get());

          // We delay the destruction of intermediate surface in case the mouse cursor reappears shortly.
          old_surface_delayed_destruction.reset(p_surface->release());
          old_surface_timestamp = std::chrono::steady_clock::now();

          last_frame_variant = img;
          break;
        }

      case lfa::replace_img_with_surface:
        {
          auto p_img = std::get_if<std::shared_ptr<platf::img_t>>(&last_frame_variant);
          if (!p_img) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }
          auto [d3d_img, lock] = get_locked_d3d_img(*p_img);
          if (!d3d_img) {
            return capture_e::error;
          }

          p_img = nullptr;
          last_frame_variant = texture2d_t {};
          auto &surface = std::get<texture2d_t>(last_frame_variant);
          if (!create_surface(surface)) {
            return capture_e::error;
          }

          device_ctx->CopyResource(surface.get(), d3d_img->capture_texture.get());
          break;
        }

      case lfa::copy_src_to_img:
        {
          last_frame_variant = {};

          std::shared_ptr<platf::img_t> img;
          if (!pull_free_image_cb(img)) {
            return capture_e::interrupted;
          }

          auto [d3d_img, lock] = get_locked_d3d_img(img);
          if (!d3d_img) {
            return capture_e::error;
          }

          device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
          last_frame_variant = img;
          break;
        }

      case lfa::copy_src_to_surface:
        {
          auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!p_surface) {
            last_frame_variant = texture2d_t {};
            p_surface = std::get_if<texture2d_t>(&last_frame_variant);
            if (!create_surface(*p_surface)) {
              return capture_e::error;
            }
          }
          device_ctx->CopyResource(p_surface->get(), src.get());
          break;
        }
    }

    auto blend_cursor = [&](img_d3d_t &d3d_img) {
      device_ctx->VSSetShader(cursor_vs.get(), nullptr, 0);
      device_ctx->PSSetShader(cursor_ps.get(), nullptr, 0);
      device_ctx->OMSetRenderTargets(1, &d3d_img.capture_rt, nullptr);

      if (cursor_alpha.texture.get()) {
        // Perform an alpha blending operation
        device_ctx->OMSetBlendState(blend_alpha.get(), nullptr, 0xFFFFFFFFu);

        device_ctx->PSSetShaderResources(0, 1, &cursor_alpha.input_res);
        device_ctx->RSSetViewports(1, &cursor_alpha.cursor_view);
        device_ctx->Draw(3, 0);
      }

      if (cursor_xor.texture.get()) {
        // Perform an invert blending without touching alpha values
        device_ctx->OMSetBlendState(blend_invert.get(), nullptr, 0x00FFFFFFu);

        device_ctx->PSSetShaderResources(0, 1, &cursor_xor.input_res);
        device_ctx->RSSetViewports(1, &cursor_xor.cursor_view);
        device_ctx->Draw(3, 0);
      }

      device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);

      ID3D11RenderTargetView *emptyRenderTarget = nullptr;
      device_ctx->OMSetRenderTargets(1, &emptyRenderTarget, nullptr);
      device_ctx->RSSetViewports(0, nullptr);
      ID3D11ShaderResourceView *emptyShaderResourceView = nullptr;
      device_ctx->PSSetShaderResources(0, 1, &emptyShaderResourceView);
    };

    switch (out_frame_action) {
      case ofa::forward_last_img:
        {
          auto p_img = std::get_if<std::shared_ptr<platf::img_t>>(&last_frame_variant);
          if (!p_img) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }
          img_out = *p_img;
          break;
        }

      case ofa::copy_last_surface_and_blend_cursor:
        {
          auto p_surface = std::get_if<texture2d_t>(&last_frame_variant);
          if (!p_surface) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }
          if (!blend_mouse_cursor_flag) {
            BOOST_LOG(error) << "Logical error at " << __FILE__ << ":" << __LINE__;
            return capture_e::error;
          }

          if (!pull_free_image_cb(img_out)) {
            return capture_e::interrupted;
          }

          auto [d3d_img, lock] = get_locked_d3d_img(img_out);
          if (!d3d_img) {
            return capture_e::error;
          }

          device_ctx->CopyResource(d3d_img->capture_texture.get(), p_surface->get());
          blend_cursor(*d3d_img);
          break;
        }

      case ofa::dummy_fallback:
        {
          if (!pull_free_image_cb(img_out)) {
            return capture_e::interrupted;
          }

          // Clear the image if it has been used as a dummy.
          // It can have the mouse cursor blended onto it.
          auto old_d3d_img = (img_d3d_t *) img_out.get();
          bool reclear_dummy = !old_d3d_img->blank && old_d3d_img->capture_texture;

          auto [d3d_img, lock] = get_locked_d3d_img(img_out, true);
          if (!d3d_img) {
            return capture_e::error;
          }

          if (reclear_dummy) {
            const float rgb_black[] = {0.0f, 0.0f, 0.0f, 0.0f};
            device_ctx->ClearRenderTargetView(d3d_img->capture_rt.get(), rgb_black);
          }

          if (blend_mouse_cursor_flag) {
            blend_cursor(*d3d_img);
          }

          break;
        }
    }

    // Perform delayed destruction of the unused surface if the time is due.
    if (old_surface_delayed_destruction && old_surface_timestamp + 10s < std::chrono::steady_clock::now()) {
      old_surface_delayed_destruction.reset();
    }

    if (img_out) {
      img_out->frame_timestamp = frame_timestamp;
      img_out->host_processing_timestamp = host_processing_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_ddup_vram_t::release_snapshot() {
    return dup.release_frame();
  }

  int display_ddup_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    D3D11_SAMPLER_DESC sampler_desc {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    auto status = device->CreateSamplerState(&sampler_desc, &sampler_linear);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create point sampler state [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    status = device->CreateVertexShader(cursor_vs_hlsl->GetBufferPointer(), cursor_vs_hlsl->GetBufferSize(), nullptr, &cursor_vs);
    if (status) {
      BOOST_LOG(error) << "Failed to create scene vertex shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    {
      int32_t rotation_modifier = display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? 0 : display_rotation - 1;
      int32_t rotation_data[16 / sizeof(int32_t)] {rotation_modifier};  // aligned to 16-byte
      auto rotation = make_buffer(device.get(), rotation_data);
      if (!rotation) {
        BOOST_LOG(error) << "Failed to create display rotation vertex constant buffer";
        return -1;
      }
      device_ctx->VSSetConstantBuffers(2, 1, &rotation);
    }

    if (config.dynamicRange && is_hdr()) {
      // This shader will normalize scRGB white levels to a user-defined white level
      status = device->CreatePixelShader(cursor_ps_normalize_white_hlsl->GetBufferPointer(), cursor_ps_normalize_white_hlsl->GetBufferSize(), nullptr, &cursor_ps);
      if (status) {
        BOOST_LOG(error) << "Failed to create cursor blending (normalized white) pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      // Use a 300 nit target for the mouse cursor. We should really get
      // the user's SDR white level in nits, but there is no API that
      // provides that information to Win32 apps.
      float white_multiplier_data[16 / sizeof(float)] {300.0f / 80.f};  // aligned to 16-byte
      auto white_multiplier = make_buffer(device.get(), white_multiplier_data);
      if (!white_multiplier) {
        BOOST_LOG(warning) << "Failed to create cursor blending (normalized white) white multiplier constant buffer";
        return -1;
      }

      device_ctx->PSSetConstantBuffers(1, 1, &white_multiplier);
    } else {
      status = device->CreatePixelShader(cursor_ps_hlsl->GetBufferPointer(), cursor_ps_hlsl->GetBufferSize(), nullptr, &cursor_ps);
      if (status) {
        BOOST_LOG(error) << "Failed to create cursor blending pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }

    blend_alpha = make_blend(device.get(), true, false);
    blend_invert = make_blend(device.get(), true, true);
    blend_disable = make_blend(device.get(), false, false);

    if (!blend_disable || !blend_alpha || !blend_invert) {
      return -1;
    }

    device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
    device_ctx->PSSetSamplers(0, 1, &sampler_linear);
    device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    return 0;
  }

  std::shared_ptr<display_t> display_wgc_vram_t::create(const ::video::config_t &config, const std::string &display_name) {
    return display_wgc_ipc_vram_t::create(config, display_name);
  }

  std::shared_ptr<platf::img_t> display_vram_t::alloc_img() {
    auto img = std::make_shared<img_d3d_t>();

    // Initialize format-independent fields
    img->width = width_before_rotation;
    img->height = height_before_rotation;
    img->id = next_image_id++;
    img->blank = true;

    return img;
  }

  // This cannot use ID3D11DeviceContext because it can be called concurrently by the encoding thread
  int display_vram_t::complete_img(platf::img_t *img_base, bool dummy) {
    auto img = (img_d3d_t *) img_base;

    // If this already has a capture texture and it's not switching dummy state, nothing to do
    if (img->capture_texture && img->dummy == dummy) {
      return 0;
    }

    // If this is not a dummy image, we must know the format by now
    if (!dummy && capture_format == DXGI_FORMAT_UNKNOWN) {
      BOOST_LOG(error) << "display_vram_t::complete_img() called with unknown capture format!";
      return -1;
    }

    // Reset the image (in case this was previously a dummy)
    img->capture_texture.reset();
    img->capture_rt.reset();
    img->capture_mutex.reset();
    img->data = nullptr;
    if (img->encoder_texture_handle) {
      CloseHandle(img->encoder_texture_handle);
      img->encoder_texture_handle = nullptr;
    }

    // Initialize format-dependent fields
    img->pixel_pitch = get_pixel_pitch();
    img->row_pitch = img->pixel_pitch * img->width;
    img->dummy = dummy;
    img->format = (capture_format == DXGI_FORMAT_UNKNOWN) ? DXGI_FORMAT_B8G8R8A8_UNORM : capture_format;

    D3D11_TEXTURE2D_DESC t {};
    t.Width = img->width;
    t.Height = img->height;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.Format = img->format;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    t.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    auto status = device->CreateTexture2D(&t, nullptr, &img->capture_texture);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create img buf texture [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    status = device->CreateRenderTargetView(img->capture_texture.get(), nullptr, &img->capture_rt);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create render target view [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Get the keyed mutex to synchronize with the encoding code
    status = img->capture_texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &img->capture_mutex);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIKeyedMutex [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    resource1_t resource;
    status = img->capture_texture->QueryInterface(__uuidof(IDXGIResource1), (void **) &resource);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to query IDXGIResource1 [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    // Create a handle for the encoder device to use to open this texture
    status = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &img->encoder_texture_handle);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create shared texture handle [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    img->data = (std::uint8_t *) img->capture_texture.get();

    return 0;
  }

  // This cannot use ID3D11DeviceContext because it can be called concurrently by the encoding thread
  /**
   * @memberof platf::dxgi::display_vram_t
   */
  int display_vram_t::dummy_img(platf::img_t *img_base) {
    return complete_img(img_base, true);
  }

  std::vector<DXGI_FORMAT> display_vram_t::get_supported_capture_formats() {
    return {
      // scRGB FP16 is the ideal format for Wide Color Gamut and Advanced Color
      // displays (both SDR and HDR). This format uses linear gamma, so we will
      // use a linear->PQ shader for HDR and a linear->sRGB shader for SDR.
      DXGI_FORMAT_R16G16B16A16_FLOAT,

      // DXGI_FORMAT_R10G10B10A2_UNORM seems like it might give us frames already
      // converted to SMPTE 2084 PQ, however it seems to actually just clamp the
      // scRGB FP16 values that DWM is using when the desktop format is scRGB FP16.
      //
      // If there is a case where the desktop format is really SMPTE 2084 PQ, it
      // might make sense to support capturing it without conversion to scRGB,
      // but we avoid it for now.

      // We include the 8-bit modes too for when the display is in SDR mode,
      // while the client stream is HDR-capable. These UNORM formats can
      // use our normal pixel shaders that expect sRGB input.
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_B8G8R8X8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UNORM,
    };
  }

  /**
   * @brief Check that a given codec is supported by the display device.
   * @param name The FFmpeg codec name (or similar for non-FFmpeg codecs).
   * @param config The codec configuration.
   * @return `true` if supported, `false` otherwise.
   */
  bool display_vram_t::is_codec_supported(std::string_view name, const ::video::config_t &config) {
    DXGI_ADAPTER_DESC adapter_desc;
    adapter->GetDesc(&adapter_desc);

    if (adapter_desc.VendorId == 0x1002) {  // AMD
      // If it's not an AMF encoder, it's not compatible with an AMD GPU
      if (!boost::algorithm::ends_with(name, "_amf")) {
        return false;
      }

      // Perform AMF version checks if we're using an AMD GPU. This check is placed in display_vram_t
      // to avoid hitting the display_ram_t path which uses software encoding and doesn't touch AMF.
      HMODULE amfrt = LoadLibraryW(AMF_DLL_NAME);
      if (amfrt) {
        auto unload_amfrt = util::fail_guard([amfrt]() {
          FreeLibrary(amfrt);
        });

        auto fnAMFQueryVersion = (AMFQueryVersion_Fn) GetProcAddress(amfrt, AMF_QUERY_VERSION_FUNCTION_NAME);
        if (fnAMFQueryVersion) {
          amf_uint64 version;
          auto result = fnAMFQueryVersion(&version);
          if (result == AMF_OK) {
            if (config.videoFormat == 2 && version < AMF_MAKE_FULL_VERSION(1, 4, 30, 0)) {
              // AMF 1.4.30 adds ultra low latency mode for AV1. Don't use AV1 on earlier versions.
              // This corresponds to driver version 23.5.2 (23.10.01.45) or newer.
              BOOST_LOG(warning) << "AV1 encoding is disabled on AMF version "sv
                                 << AMF_GET_MAJOR_VERSION(version) << '.'
                                 << AMF_GET_MINOR_VERSION(version) << '.'
                                 << AMF_GET_SUBMINOR_VERSION(version) << '.'
                                 << AMF_GET_BUILD_VERSION(version);
              BOOST_LOG(warning) << "If your AMD GPU supports AV1 encoding, update your graphics drivers!"sv;
              return false;
            } else if (config.dynamicRange && version < AMF_MAKE_FULL_VERSION(1, 4, 23, 0)) {
              // Older versions of the AMD AMF runtime can crash when fed P010 surfaces.
              // Fail if AMF version is below 1.4.23 where HEVC Main10 encoding was introduced.
              // AMF 1.4.23 corresponds to driver version 21.12.1 (21.40.11.03) or newer.
              BOOST_LOG(warning) << "HDR encoding is disabled on AMF version "sv
                                 << AMF_GET_MAJOR_VERSION(version) << '.'
                                 << AMF_GET_MINOR_VERSION(version) << '.'
                                 << AMF_GET_SUBMINOR_VERSION(version) << '.'
                                 << AMF_GET_BUILD_VERSION(version);
              BOOST_LOG(warning) << "If your AMD GPU supports HEVC Main10 encoding, update your graphics drivers!"sv;
              return false;
            }
          } else {
            BOOST_LOG(warning) << "AMFQueryVersion() failed: "sv << result;
          }
        } else {
          BOOST_LOG(warning) << "AMF DLL missing export: "sv << AMF_QUERY_VERSION_FUNCTION_NAME;
        }
      } else {
        BOOST_LOG(warning) << "Detected AMD GPU but AMF failed to load"sv;
      }
    } else if (adapter_desc.VendorId == 0x8086) {  // Intel
      // If it's not a QSV encoder, it's not compatible with an Intel GPU
      if (!boost::algorithm::ends_with(name, "_qsv")) {
        return false;
      }
      if (config.chromaSamplingType == 1) {
        if (config.videoFormat == 0 || config.videoFormat == 2) {
          // QSV doesn't support 4:4:4 in H.264 or AV1
          return false;
        }
        // TODO: Blacklist HEVC 4:4:4 based on adapter model
      }
    } else if (adapter_desc.VendorId == 0x10de) {  // Nvidia
      // If it's not an NVENC encoder, it's not compatible with an Nvidia GPU
      if (!boost::algorithm::ends_with(name, "_nvenc")) {
        return false;
      }
    } else if (adapter_desc.VendorId == 0x4D4F4351 ||  // Qualcomm (QCOM as MOQC reversed)
               adapter_desc.VendorId == 0x5143) {  // Qualcomm alternate ID
      // If it's not a MediaFoundation encoder, it's not compatible with a Qualcomm GPU
      if (!boost::algorithm::ends_with(name, "_mf")) {
        return false;
      }
    } else {
      BOOST_LOG(warning) << "Unknown GPU vendor ID: " << util::hex(adapter_desc.VendorId).to_string_view();
    }

    return true;
  }

  std::unique_ptr<avcodec_encode_device_t> display_vram_t::make_avcodec_encode_device(pix_fmt_e pix_fmt) {
    auto device = std::make_unique<d3d_avcodec_encode_device_t>();
    if (device->init(shared_from_this(), adapter.get(), pix_fmt) != 0) {
      return nullptr;
    }
    return device;
  }

  std::unique_ptr<nvenc_encode_device_t> display_vram_t::make_nvenc_encode_device(pix_fmt_e pix_fmt) {
    auto device = std::make_unique<d3d_nvenc_encode_device_t>();
    if (!device->init_device(shared_from_this(), adapter.get(), pix_fmt)) {
      return nullptr;
    }
    return device;
  }

  int init() {
    BOOST_LOG(info) << "Compiling shaders..."sv;

#define compile_vertex_shader_helper(x) \
  if (!(x##_hlsl = compile_vertex_shader(SUNSHINE_SHADERS_DIR "/" #x ".hlsl"))) \
    return -1;
#define compile_pixel_shader_helper(x) \
  if (!(x##_hlsl = compile_pixel_shader(SUNSHINE_SHADERS_DIR "/" #x ".hlsl"))) \
    return -1;

    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_sdr_to_pq);
    compile_vertex_shader_helper(convert_yuv420_packed_uv_type0_vs);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_sdr_to_pq);
    compile_vertex_shader_helper(convert_yuv420_packed_uv_type0s_vs);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_linear);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_sdr_to_pq);
    compile_vertex_shader_helper(convert_yuv420_planar_y_vs);
    compile_pixel_shader_helper(convert_yuv444_packed_ayuv_ps);
    compile_pixel_shader_helper(convert_yuv444_packed_ayuv_ps_linear);
    compile_vertex_shader_helper(convert_yuv444_packed_vs);
    compile_pixel_shader_helper(convert_yuv444_planar_ps);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_linear);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_sdr_to_pq);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_linear);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_perceptual_quantizer);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_sdr_to_pq);
    compile_vertex_shader_helper(convert_yuv444_planar_vs);
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0_ps_truehdr_peak);
    compile_pixel_shader_helper(convert_yuv420_packed_uv_type0s_ps_truehdr_peak);
    compile_pixel_shader_helper(convert_yuv420_planar_y_ps_truehdr_peak);
    compile_pixel_shader_helper(convert_yuv444_planar_ps_truehdr_peak);
    compile_pixel_shader_helper(convert_yuv444_packed_y410_ps_truehdr_peak);
#endif
    compile_pixel_shader_helper(cursor_ps);
    compile_pixel_shader_helper(cursor_ps_normalize_white);
    compile_vertex_shader_helper(cursor_vs);

    BOOST_LOG(info) << "Compiled shaders"sv;

#undef compile_vertex_shader_helper
#undef compile_pixel_shader_helper

    return 0;
  }
}  // namespace platf::dxgi
