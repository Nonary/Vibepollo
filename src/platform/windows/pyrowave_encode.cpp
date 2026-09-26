/**
 * @file src/platform/windows/pyrowave_encode.cpp
 * @brief Windows PyroWave host encoder: Sunshine adapter around pyrowave_d3d11_core.
 */
// standard includes
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>

// platform includes
#include <winsock2.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

// local includes
#include "display.h"
#include "display_vram.h"
#include "pyrowave_d3d11_core.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/pyrowave_host.h"
#include "src/pyrowave_policy.h"
#include "src/utility.h"
#include "src/video_colorspace.h"
#include "utf_utils.h"

#if !defined(SUNSHINE_SHADERS_DIR)  // for testing this needs to be defined in cmake as we don't do an install
  #define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/directx"
#endif

using Microsoft::WRL::ComPtr;
using namespace std::literals;

namespace pyrowave::host {
  namespace {
    void log_core_message(int level, const std::string &message) {
      switch (level) {
        case 0:
          BOOST_LOG(debug) << message;
          break;
        case 1:
          BOOST_LOG(info) << message;
          break;
        case 2:
          BOOST_LOG(warning) << message;
          break;
        default:
          BOOST_LOG(error) << message;
          break;
      }
    }

    ComPtr<IDXGIAdapter> adapter_of(ID3D11Texture2D *texture) {
      ComPtr<ID3D11Device> device;
      texture->GetDevice(&device);
      ComPtr<IDXGIDevice> dxgi_device;
      ComPtr<IDXGIAdapter> adapter;
      if (device && SUCCEEDED(device.As(&dxgi_device))) {
        dxgi_device->GetAdapter(&adapter);
      }
      return adapter;
    }

    d3d11::color_matrix_t color_matrix_for(const video::sunshine_colorspace_t &colorspace) {
      d3d11::color_matrix_t matrix {};
      // UNORM plane outputs: normalized code values, like the NV12/P010 converters.
      if (const auto *vectors = video::color_vectors_from_colorspace(colorspace, true)) {
        static_assert(sizeof(*vectors) == sizeof(matrix), "video::color_t layout changed");
        std::memcpy(&matrix, vectors, sizeof(matrix));
      }
      return matrix;
    }

    class encoder_impl_t final: public encoder_t {
    public:
      encoder_impl_t(const session_params_t &params, std::shared_ptr<platf::display_t> display):
          params {params},
          display {std::move(display)},
          budget {params.framerate, params.bitrate_kbps, policy::max_bitstream_bytes(params.packetsize, params.framing == policy::framing_e::length_prefixed, params.critical_fec)},
          encode_logger {debug, "PyroWave: encode (GPU wait)", "ms"},
          frame_size_logger {debug, "PyroWave: frame size", "KiB"},
          padding_logger {debug, "PyroWave: record padding", "%"},
          critical_logger {debug, "PyroWave: critical (coarsest level) share", "%"} {
        framing.framing = params.framing;
        framing.shard_payload = policy::shard_payload_bytes(params.packetsize);
        framing.max_frame_bytes = policy::max_frame_bytes(params.packetsize, params.critical_fec);

        BOOST_LOG(info) << "PyroWave encoder session: "sv << params.width << 'x' << params.height
                        << (params.yuv444 ? " 4:4:4"sv : " 4:2:0"sv) << ", "sv << params.colorspace.bit_depth << "-bit "sv
                        << (video::colorspace_is_hdr(params.colorspace) ? "HDR"sv : "SDR"sv)
                        << ", framing "sv << (params.framing == policy::framing_e::records ? "records"sv : "length-prefixed"sv)
                        << " (shard payload "sv << framing.shard_payload << "), budget "sv << budget.bytes_per_frame() << " bytes/frame"sv;
      }

      int encode(platf::img_t &img_base, std::vector<std::uint8_t> &out, std::size_t &critical_bytes) override {
        critical_bytes = 0;
        if (failed) {
          return -1;
        }
        if (budget.bytes_per_frame() == 0) {
          return 1;
        }

        auto *img = dynamic_cast<platf::dxgi::img_d3d_t *>(&img_base);
        if (!img) {
          BOOST_LOG(error) << "PyroWave requires GPU (VRAM) capture; the selected capture method delivers system-memory frames"sv;
          failed = true;
          return -1;
        }
        if (!img->capture_texture) {
          // Nothing to encode yet and nothing that tells us the adapter.
          return 1;
        }

        if (!core && !create_core(*img)) {
          failed = true;
          return -1;
        }

        d3d11::source_t source;
        if (!img->blank) {
          auto *ctx = open_image(*img);
          if (!ctx) {
            failed = true;
            return -1;
          }
          source.srv = ctx->srv.Get();
          source.mutex = ctx->mutex.Get();
          source.width = unsigned(img->width);
          source.height = unsigned(img->height);
          source.transfer = transfer_for(img->format);
          source.rotate_texture_steps = rotate_texture_steps();
        }

        out.clear();
        d3d11::frame_stats_t stats;
        const auto result = core->encode(source, budget.bytes_per_frame(), framing, out, &stats);
        if (result == d3d11::result_e::failed) {
          failed = true;
          return -1;
        }
        if (result == d3d11::result_e::skipped) {
          return 1;
        }

        encode_logger.collect_and_log(stats.encode_ms);
        frame_size_logger.collect_and_log(double(stats.frame_bytes) / 1024.0);
        if (stats.frame_bytes) {
          padding_logger.collect_and_log(100.0 * double(stats.records.padding_bytes) / double(stats.frame_bytes));
        }
        if (params.framing == policy::framing_e::records && framing.shard_payload) {
          critical_bytes = stats.records.critical_bytes;
          if (stats.frame_bytes) {
            critical_logger.collect_and_log(100.0 * double(critical_bytes) / double(stats.frame_bytes));
          }
        }
        return 0;
      }

      void set_bitrate(int bitrate_kbps) override {
        budget.set_bitrate(bitrate_kbps);
        BOOST_LOG(info) << "PyroWave: bitrate "sv << bitrate_kbps << " kbps, budget "sv << budget.bytes_per_frame() << " bytes/frame"sv;
      }

      void on_frame(std::chrono::steady_clock::time_point when) override {
        budget.on_frame(when);
        // Only log material changes in the per-frame allocation.
        const std::size_t current = budget.bytes_per_frame();
        if (logged_budget == 0 || current * 10 > logged_budget * 11 || current * 11 < logged_budget * 10) {
          logged_budget = current;
          BOOST_LOG(debug) << "PyroWave: encode rate "sv << budget.frame_fps() << " fps, budget "sv << current << " bytes/frame"sv;
        }
      }

    private:
      struct img_ctx_t {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIKeyedMutex> mutex;
        ComPtr<ID3D11ShaderResourceView> srv;
        ID3D11Texture2D *capture_texture_p = nullptr;
        std::weak_ptr<const platf::img_t> img_weak;
      };

      bool create_core(platf::dxgi::img_d3d_t &img) {
        auto adapter = adapter_of(img.capture_texture.get());
        if (!adapter) {
          BOOST_LOG(error) << "PyroWave: could not determine the capture adapter"sv;
          return false;
        }

        d3d11::core_config_t config;
        config.width = params.width;
        config.height = params.height;
        config.capture_wait = std::chrono::ceil<std::chrono::milliseconds>(std::chrono::duration<double>(1.0 / params.framerate));
        config.yuv444 = params.yuv444;
        config.ten_bit = params.colorspace.bit_depth > 8;
        config.color_matrix = color_matrix_for(params.colorspace);
        config.shader_path = utf_utils::from_utf8(SUNSHINE_SHADERS_DIR "/convert_pyrowave_cs.hlsl");

        core = d3d11::core_t::create(adapter.Get(), config, log_core_message);
        if (!core) {
          BOOST_LOG(error) << "PyroWave: encoder initialization failed"sv;
          return false;
        }
        BOOST_LOG(info) << "PyroWave encoder ready (Vulkan, D3D11 interop)"sv;
        return true;
      }

      img_ctx_t *open_image(platf::dxgi::img_d3d_t &img) {
        // Forget images the capture side has released.
        for (auto it = img_ctxs.begin(); it != img_ctxs.end();) {
          it = it->second.img_weak.expired() ? img_ctxs.erase(it) : std::next(it);
        }

        auto &ctx = img_ctxs[img.id];
        if (ctx.texture && ctx.capture_texture_p == img.capture_texture.get()) {
          return &ctx;
        }

        ctx = {};
        ComPtr<ID3D11Device1> device1;
        HRESULT hr = core->device()->QueryInterface(IID_PPV_ARGS(&device1));
        if (SUCCEEDED(hr)) {
          hr = device1->OpenSharedResource1(img.encoder_texture_handle, IID_PPV_ARGS(&ctx.texture));
        }
        if (SUCCEEDED(hr)) {
          hr = ctx.texture.As(&ctx.mutex);
        }
        if (SUCCEEDED(hr)) {
          hr = core->device()->CreateShaderResourceView(ctx.texture.Get(), nullptr, &ctx.srv);
        }
        if (FAILED(hr)) {
          BOOST_LOG(error) << "PyroWave: opening the captured frame failed [0x"sv << util::hex(hr).to_string_view() << ']';
          img_ctxs.erase(img.id);
          return nullptr;
        }
        ctx.capture_texture_p = img.capture_texture.get();
        ctx.img_weak = img.weak_from_this();
        return &ctx;
      }

      d3d11::transfer_e transfer_for(DXGI_FORMAT format) {
        const bool hdr = video::colorspace_is_hdr(params.colorspace);
        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
          return hdr ? d3d11::transfer_e::pq : d3d11::transfer_e::linear_sdr;
        }
        if (hdr && !warned_unorm_hdr) {
          BOOST_LOG(warning) << "PyroWave: HDR stream with an SDR capture format; mapping SDR white to 100 nits"sv;
          warned_unorm_hdr = true;
        }
        return hdr ? d3d11::transfer_e::sdr_to_pq : d3d11::transfer_e::unorm;
      }

      int rotate_texture_steps() const {
        // Same convention as the vertex shaders of the other D3D11 converters.
        const auto base = std::dynamic_pointer_cast<platf::dxgi::display_base_t>(display);
        if (!base || base->display_rotation == DXGI_MODE_ROTATION_UNSPECIFIED) {
          return 0;
        }
        return -int(base->display_rotation - 1);
      }

      session_params_t params;
      std::shared_ptr<platf::display_t> display;
      d3d11::framing_params_t framing;
      policy::budget_t budget;
      std::size_t logged_budget = 0;
      std::unique_ptr<d3d11::core_t> core;
      std::map<std::uint32_t, img_ctx_t> img_ctxs;
      bool failed = false;
      bool warned_unorm_hdr = false;
      logging::min_max_avg_periodic_logger<double> encode_logger;
      logging::min_max_avg_periodic_logger<double> frame_size_logger;
      logging::min_max_avg_periodic_logger<double> padding_logger;
      logging::min_max_avg_periodic_logger<double> critical_logger;
    };
  }  // namespace

  std::unique_ptr<encoder_t> make_encoder(const session_params_t &params, std::shared_ptr<platf::display_t> display) {
    if (params.framerate <= 0 || params.bitrate_kbps <= 0 || policy::max_frame_bytes(params.packetsize) == 0) {
      BOOST_LOG(error) << "PyroWave: invalid negotiated frame rate, bitrate or packet size"sv;
      return nullptr;
    }
    if (params.width <= 0 || params.height <= 0 ||
        (!params.yuv444 && ((params.width & 1) || (params.height & 1)))) {
      BOOST_LOG(error) << "PyroWave: invalid stream size "sv << params.width << 'x' << params.height;
      return nullptr;
    }
    return std::make_unique<encoder_impl_t>(params, std::move(display));
  }

  bool probe(const std::optional<platf::adapter_id_t> &adapter, std::string &detail) {
    if (adapter) {
      LUID luid {};
      luid.HighPart = adapter->high_part;
      luid.LowPart = adapter->low_part;
      return d3d11::probe(&luid, detail);
    }
    return d3d11::probe(nullptr, detail);
  }
}  // namespace pyrowave::host
