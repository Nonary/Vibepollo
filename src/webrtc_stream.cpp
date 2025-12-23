/**
 * @file src/webrtc_stream.cpp
 * @brief Definitions for WebRTC session tracking and frame handoff.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <unordered_map>

// lib includes
#include <openssl/evp.h>
#include <openssl/x509.h>

#ifdef SUNSHINE_ENABLE_WEBRTC
#include <libwebrtc_c.h>
#endif

// local includes
#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "logging.h"
#include "utility.h"
#include "video_colorspace.h"
#include "uuid.h"
#include "webrtc_stream.h"

#ifdef _WIN32
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "src/platform/windows/display_vram.h"
#if !defined(SUNSHINE_SHADERS_DIR)
  #define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/directx"
#endif
#endif

#ifdef __APPLE__
#include "src/platform/macos/av_img_t.h"
#include <CoreVideo/CoreVideo.h>
#endif

namespace webrtc_stream {
  bool add_local_candidate(std::string_view id, std::string mid, int mline_index, std::string candidate);
  bool set_local_answer(std::string_view id, const std::string &sdp, const std::string &type);

  namespace {
    constexpr std::size_t kMaxVideoFrames = 2;
    constexpr std::size_t kMaxAudioFrames = 8;

    struct EncodedVideoFrame {
      std::shared_ptr<std::vector<std::uint8_t>> data;
      std::int64_t frame_index = 0;
      bool idr = false;
      bool after_ref_frame_invalidation = false;
      std::optional<std::chrono::steady_clock::time_point> timestamp;
    };

    struct EncodedAudioFrame {
      std::shared_ptr<std::vector<std::uint8_t>> data;
      std::optional<std::chrono::steady_clock::time_point> timestamp;
    };

    struct RawVideoFrame {
      std::shared_ptr<platf::img_t> image;
      std::optional<std::chrono::steady_clock::time_point> timestamp;
    };

    struct RawAudioFrame {
      std::shared_ptr<std::vector<float>> samples;
      int sample_rate = 0;
      int channels = 0;
      int frames = 0;
    };

    template<class T>
    class ring_buffer_t {
    public:
      explicit ring_buffer_t(std::size_t max_items):
          _max_items {max_items} {
      }

      bool push(T item) {
        bool dropped = false;
        if (_queue.size() >= _max_items) {
          _queue.pop_front();
          dropped = true;
        }
        _queue.emplace_back(std::move(item));
        return dropped;
      }

      bool pop(T &item) {
        if (_queue.empty()) {
          return false;
        }
        item = std::move(_queue.front());
        _queue.pop_front();
        return true;
      }

      bool pop_latest(T &item) {
        if (_queue.empty()) {
          return false;
        }
        item = std::move(_queue.back());
        _queue.clear();
        return true;
      }

      bool empty() const {
        return _queue.empty();
      }

    private:
      std::size_t _max_items;
      std::deque<T> _queue;
    };

#ifdef SUNSHINE_ENABLE_WEBRTC
    struct SessionIceContext {
      std::string id;
      std::atomic<bool> active {true};
    };

    struct SessionPeerContext {
      std::string session_id;
      lwrtc_peer_t *peer = nullptr;
    };

    struct LocalDescriptionContext {
      std::string session_id;
      lwrtc_peer_t *peer = nullptr;
      std::string sdp;
      std::string type;
    };

    lwrtc_constraints_t *create_constraints();
#endif

#ifdef _WIN32
    class D3D11Nv12Converter;
#endif

    struct Session {
      SessionState state;
      ring_buffer_t<EncodedVideoFrame> video_frames {kMaxVideoFrames};
      ring_buffer_t<EncodedAudioFrame> audio_frames {kMaxAudioFrames};
      ring_buffer_t<RawVideoFrame> raw_video_frames {kMaxVideoFrames};
      ring_buffer_t<RawAudioFrame> raw_audio_frames {kMaxAudioFrames};
      std::string remote_offer_sdp;
      std::string remote_offer_type;
      std::string local_answer_sdp;
      std::string local_answer_type;
#ifdef SUNSHINE_ENABLE_WEBRTC
      lwrtc_peer_t *peer = nullptr;
      std::shared_ptr<SessionIceContext> ice_context;
      lwrtc_audio_source_t *audio_source = nullptr;
      lwrtc_video_source_t *video_source = nullptr;
      lwrtc_audio_track_t *audio_track = nullptr;
      lwrtc_video_track_t *video_track = nullptr;
#ifdef _WIN32
      std::unique_ptr<D3D11Nv12Converter> d3d_converter;
#endif
#endif

      struct IceCandidate {
        std::string mid;
        int mline_index = -1;
        std::string candidate;
      };
      std::vector<IceCandidate> candidates;

      struct LocalCandidate {
        std::string mid;
        int mline_index = -1;
        std::string candidate;
        std::size_t index = 0;
      };
      std::vector<LocalCandidate> local_candidates;
      std::size_t local_candidate_counter = 0;
    };

    std::mutex session_mutex;
    std::unordered_map<std::string, Session> sessions;
    std::atomic_uint active_sessions {0};
    std::atomic_bool rtsp_sessions_active {false};

#ifdef SUNSHINE_ENABLE_WEBRTC
    void on_ice_candidate(
      void *user,
      const char *mid,
      int mline_index,
      const char *candidate
    ) {
      auto *ctx = static_cast<SessionIceContext *>(user);
      if (!ctx || !ctx->active.load(std::memory_order_acquire)) {
        return;
      }
      if (!mid || !candidate) {
        return;
      }
      add_local_candidate(ctx->id, std::string {mid}, mline_index, std::string {candidate});
    }

    void on_set_local_success(void *user) {
      auto *ctx = static_cast<LocalDescriptionContext *>(user);
      if (!ctx) {
        return;
      }
      if (!set_local_answer(ctx->session_id, ctx->sdp, ctx->type)) {
        BOOST_LOG(error) << "WebRTC: failed to store local description for " << ctx->session_id;
      }
      delete ctx;
    }

    void on_set_local_failure(void *user, const char *err) {
      auto *ctx = static_cast<LocalDescriptionContext *>(user);
      if (!ctx) {
        return;
      }
      BOOST_LOG(error) << "WebRTC: failed to set local description for " << ctx->session_id
                       << ": " << (err ? err : "unknown");
      delete ctx;
    }

    void on_create_answer_success(void *user, const char *sdp, const char *type) {
      auto *ctx = static_cast<SessionPeerContext *>(user);
      if (!ctx) {
        return;
      }
      if (!ctx->peer) {
        BOOST_LOG(error) << "WebRTC: missing peer connection for " << ctx->session_id;
        delete ctx;
        return;
      }
      std::string sdp_copy = sdp ? sdp : "";
      std::string type_copy = type ? type : "";
      auto *local_ctx = new LocalDescriptionContext {
        ctx->session_id,
        ctx->peer,
        std::move(sdp_copy),
        std::move(type_copy)
      };
      lwrtc_peer_set_local_description(
        ctx->peer,
        local_ctx->sdp.c_str(),
        local_ctx->type.c_str(),
        &on_set_local_success,
        &on_set_local_failure,
        local_ctx
      );
      delete ctx;
    }

    void on_create_answer_failure(void *user, const char *err) {
      auto *ctx = static_cast<SessionPeerContext *>(user);
      if (!ctx) {
        return;
      }
      BOOST_LOG(error) << "WebRTC: failed to create answer for " << ctx->session_id
                       << ": " << (err ? err : "unknown");
      delete ctx;
    }

    void on_set_remote_success(void *user) {
      auto *ctx = static_cast<SessionPeerContext *>(user);
      if (!ctx) {
        return;
      }
      if (!ctx->peer) {
        BOOST_LOG(error) << "WebRTC: missing peer connection for " << ctx->session_id;
        delete ctx;
        return;
      }
      auto *constraints = create_constraints();
      if (!constraints) {
        BOOST_LOG(error) << "WebRTC: failed to create media constraints";
        delete ctx;
        return;
      }
      lwrtc_peer_create_answer(
        ctx->peer,
        &on_create_answer_success,
        &on_create_answer_failure,
        constraints,
        ctx
      );
      lwrtc_constraints_release(constraints);
    }

    void on_set_remote_failure(void *user, const char *err) {
      auto *ctx = static_cast<SessionPeerContext *>(user);
      if (!ctx) {
        return;
      }
      BOOST_LOG(error) << "WebRTC: failed to set remote description for " << ctx->session_id
                       << ": " << (err ? err : "unknown");
      delete ctx;
    }

    std::mutex webrtc_mutex;
    lwrtc_factory_t *webrtc_factory = nullptr;
    std::mutex webrtc_media_mutex;
    std::condition_variable webrtc_media_cv;
    std::thread webrtc_media_thread;
    std::atomic<bool> webrtc_media_running {false};
    std::atomic<bool> webrtc_media_shutdown {false};

    bool ensure_webrtc_factory() {
      std::lock_guard lg {webrtc_mutex};
      if (webrtc_factory) {
        return true;
      }

      webrtc_factory = lwrtc_factory_create();
      if (!webrtc_factory) {
        BOOST_LOG(error) << "WebRTC: failed to allocate peer connection factory";
        return false;
      }
      if (!lwrtc_factory_initialize(webrtc_factory)) {
        BOOST_LOG(error) << "WebRTC: failed to create peer connection factory";
        lwrtc_factory_release(webrtc_factory);
        webrtc_factory = nullptr;
        return false;
      }

      return true;
    }

    lwrtc_constraints_t *create_constraints() {
      auto *constraints = lwrtc_constraints_create();
      if (!constraints) {
        return nullptr;
      }
      return constraints;
    }

    lwrtc_peer_t *create_peer_connection(
      const SessionState &state,
      SessionIceContext *ice_context
    ) {
      if (!ensure_webrtc_factory()) {
        return nullptr;
      }

      lwrtc_config_t config {};
      config.offer_to_receive_audio = state.audio ? 1 : 0;
      config.offer_to_receive_video = state.video ? 1 : 0;

      auto *constraints = create_constraints();
      if (!constraints) {
        BOOST_LOG(error) << "WebRTC: failed to create media constraints";
        return nullptr;
      }

      auto *peer = lwrtc_factory_create_peer(
        webrtc_factory,
        &config,
        constraints,
        &on_ice_candidate,
        ice_context
      );
      lwrtc_constraints_release(constraints);
      return peer;
    }

    int64_t timestamp_to_us(const std::optional<std::chrono::steady_clock::time_point> &timestamp) {
      const auto ts = timestamp.value_or(std::chrono::steady_clock::now());
      return std::chrono::duration_cast<std::chrono::microseconds>(ts.time_since_epoch()).count();
    }

#ifdef __APPLE__
    bool try_push_nv12_frame(
      lwrtc_video_source_t *source,
      const std::shared_ptr<platf::img_t> &image,
      const std::optional<std::chrono::steady_clock::time_point> &timestamp
    ) {
      auto av_img = std::dynamic_pointer_cast<platf::av_img_t>(image);
      if (!av_img || !av_img->pixel_buffer || !av_img->pixel_buffer->buf) {
        return false;
      }

      CVPixelBufferRef pixel_buffer = av_img->pixel_buffer->buf;
      const OSType fmt = CVPixelBufferGetPixelFormatType(pixel_buffer);
      if (fmt != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
          fmt != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        return false;
      }
      if (CVPixelBufferGetPlaneCount(pixel_buffer) < 2) {
        return false;
      }

      auto *y_plane = static_cast<std::uint8_t *>(
        CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
      auto *uv_plane = static_cast<std::uint8_t *>(
        CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));
      if (!y_plane || !uv_plane) {
        return false;
      }

      const int width = static_cast<int>(CVPixelBufferGetWidth(pixel_buffer));
      const int height = static_cast<int>(CVPixelBufferGetHeight(pixel_buffer));
      const int stride_y = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0));
      const int stride_uv = static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1));

      return lwrtc_video_source_push_nv12(
        source,
        y_plane,
        stride_y,
        uv_plane,
        stride_uv,
        width,
        height,
        timestamp_to_us(timestamp)
      ) != 0;
    }
#endif

#ifdef _WIN32
    class D3D11Nv12Converter {
    public:
      bool Convert(
        ID3D11Texture2D *input_texture,
        IDXGIKeyedMutex *input_mutex,
        int width,
        int height,
        bool hdr,
        ID3D11Texture2D **out_texture,
        IDXGIKeyedMutex **out_mutex
      ) {
        if (!input_texture || width <= 0 || height <= 0 || !out_texture || !out_mutex) {
          return false;
        }

        if (!ensure_device(input_texture) ||
            !ensure_shaders() ||
            !ensure_output(width, height) ||
            !ensure_constant_buffers(width, height, hdr)) {
          return false;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> input_srv;
        if (FAILED(device_->CreateShaderResourceView(input_texture, nullptr, &input_srv))) {
          BOOST_LOG(error) << "WebRTC: failed to create input SRV for NV12 conversion";
          return false;
        }

        if (input_mutex) {
          const HRESULT hr = input_mutex->AcquireSync(0, 3000);
          if (hr != S_OK && hr != WAIT_ABANDONED) {
            return false;
          }
        }
        auto release_input_mutex = util::fail_guard([&]() {
          if (input_mutex) {
            input_mutex->ReleaseSync(0);
          }
        });

        const HRESULT out_lock = output_mutex_->AcquireSync(0, 0);
        if (out_lock != S_OK && out_lock != WAIT_ABANDONED) {
          return false;
        }

        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ID3D11SamplerState *sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);

        ID3D11ShaderResourceView *srv = input_srv.Get();
        context_->PSSetShaderResources(0, 1, &srv);

        ID3D11Buffer *rotation_cb = rotation_cb_.Get();
        context_->VSSetConstantBuffers(1, 1, &rotation_cb);

        ID3D11Buffer *color_cb = color_matrix_cb_.Get();
        context_->PSSetConstantBuffers(0, 1, &color_cb);

        // Y plane
        ID3D11RenderTargetView *rtv_y = rtv_y_.Get();
        context_->OMSetRenderTargets(1, &rtv_y, nullptr);
        context_->RSSetViewports(1, &viewport_y_);
        context_->VSSetShader(vs_y_.Get(), nullptr, 0);
        context_->PSSetShader(select_ps_y(input_texture), nullptr, 0);
        context_->Draw(3, 0);

        // UV plane
        ID3D11Buffer *subsample_cb = subsample_cb_.Get();
        context_->VSSetConstantBuffers(0, 1, &subsample_cb);
        ID3D11RenderTargetView *rtv_uv = rtv_uv_.Get();
        context_->OMSetRenderTargets(1, &rtv_uv, nullptr);
        context_->RSSetViewports(1, &viewport_uv_);
        context_->VSSetShader(vs_uv_.Get(), nullptr, 0);
        context_->PSSetShader(select_ps_uv(input_texture), nullptr, 0);
        context_->Draw(3, 0);

        ID3D11ShaderResourceView *empty_srv = nullptr;
        context_->PSSetShaderResources(0, 1, &empty_srv);

        output_mutex_->ReleaseSync(0);

        *out_texture = output_texture_.Get();
        *out_mutex = output_mutex_.Get();
        return true;
      }

    private:
      bool ensure_device(ID3D11Texture2D *input_texture) {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        input_texture->GetDevice(&device);
        if (!device) {
          return false;
        }
        if (device_ == device) {
          return true;
        }

        device_ = std::move(device);
        device_->GetImmediateContext(&context_);
        vs_y_.Reset();
        ps_y_.Reset();
        vs_uv_.Reset();
        ps_uv_.Reset();
        ps_y_linear_.Reset();
        ps_uv_linear_.Reset();
        sampler_.Reset();
        color_matrix_cb_.Reset();
        rotation_cb_.Reset();
        subsample_cb_.Reset();
        output_texture_.Reset();
        rtv_y_.Reset();
        rtv_uv_.Reset();
        output_mutex_.Reset();
        return true;
      }

      bool ensure_shaders() {
        if (vs_y_ && ps_y_ && ps_y_linear_ && vs_uv_ && ps_uv_ && ps_uv_linear_) {
          return true;
        }

        auto compile_shader = [](const std::string &file, const char *entry, const char *model) -> Microsoft::WRL::ComPtr<ID3DBlob> {
          Microsoft::WRL::ComPtr<ID3DBlob> blob;
          Microsoft::WRL::ComPtr<ID3DBlob> errors;
          auto wfile = std::filesystem::path(file).wstring();
          const HRESULT hr = D3DCompileFromFile(
            wfile.c_str(),
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry,
            model,
            D3DCOMPILE_ENABLE_STRICTNESS,
            0,
            &blob,
            &errors
          );
          if (FAILED(hr)) {
            if (errors) {
              BOOST_LOG(error) << "WebRTC: shader compile failed: "
                               << std::string_view(
                                 static_cast<const char *>(errors->GetBufferPointer()),
                                 errors->GetBufferSize()
                               );
            }
            return {};
          }
          return blob;
        };

        const std::string vs_y_path = std::string {SUNSHINE_SHADERS_DIR} + "/convert_yuv420_planar_y_vs.hlsl";
        const std::string vs_uv_path = std::string {SUNSHINE_SHADERS_DIR} + "/convert_yuv420_packed_uv_type0_vs.hlsl";
        const std::string ps_y_path = std::string {SUNSHINE_SHADERS_DIR} + "/convert_yuv420_planar_y_ps.hlsl";
        const std::string ps_uv_path = std::string {SUNSHINE_SHADERS_DIR} + "/convert_yuv420_packed_uv_type0_ps.hlsl";
        const std::string ps_y_linear_path = std::string {SUNSHINE_SHADERS_DIR} + "/convert_yuv420_planar_y_ps_linear.hlsl";
        const std::string ps_uv_linear_path = std::string {SUNSHINE_SHADERS_DIR} + "/convert_yuv420_packed_uv_type0_ps_linear.hlsl";

        auto vs_y_blob = compile_shader(vs_y_path, "main_vs", "vs_5_0");
        auto vs_uv_blob = compile_shader(vs_uv_path, "main_vs", "vs_5_0");
        auto ps_y_blob = compile_shader(ps_y_path, "main_ps", "ps_5_0");
        auto ps_uv_blob = compile_shader(ps_uv_path, "main_ps", "ps_5_0");
        auto ps_y_linear_blob = compile_shader(ps_y_linear_path, "main_ps", "ps_5_0");
        auto ps_uv_linear_blob = compile_shader(ps_uv_linear_path, "main_ps", "ps_5_0");
        if (!vs_y_blob || !vs_uv_blob || !ps_y_blob || !ps_uv_blob ||
            !ps_y_linear_blob || !ps_uv_linear_blob) {
          return false;
        }

        if (FAILED(device_->CreateVertexShader(vs_y_blob->GetBufferPointer(), vs_y_blob->GetBufferSize(), nullptr, &vs_y_))) {
          return false;
        }
        if (FAILED(device_->CreateVertexShader(vs_uv_blob->GetBufferPointer(), vs_uv_blob->GetBufferSize(), nullptr, &vs_uv_))) {
          return false;
        }
        if (FAILED(device_->CreatePixelShader(ps_y_blob->GetBufferPointer(), ps_y_blob->GetBufferSize(), nullptr, &ps_y_))) {
          return false;
        }
        if (FAILED(device_->CreatePixelShader(ps_uv_blob->GetBufferPointer(), ps_uv_blob->GetBufferSize(), nullptr, &ps_uv_))) {
          return false;
        }
        if (FAILED(device_->CreatePixelShader(ps_y_linear_blob->GetBufferPointer(), ps_y_linear_blob->GetBufferSize(), nullptr, &ps_y_linear_))) {
          return false;
        }
        if (FAILED(device_->CreatePixelShader(ps_uv_linear_blob->GetBufferPointer(), ps_uv_linear_blob->GetBufferSize(), nullptr, &ps_uv_linear_))) {
          return false;
        }

        return true;
      }

      bool ensure_output(int width, int height) {
        if (output_texture_ && width_ == width && height_ == height) {
          return true;
        }

        width_ = width;
        height_ = height;

        output_texture_.Reset();
        rtv_y_.Reset();
        rtv_uv_.Reset();
        output_mutex_.Reset();

        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &output_texture_))) {
          BOOST_LOG(error) << "WebRTC: failed to create NV12 output texture";
          return false;
        }

        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc {};
        rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtv_desc.Texture2D.MipSlice = 0;

        rtv_desc.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(device_->CreateRenderTargetView(output_texture_.Get(), &rtv_desc, &rtv_y_))) {
          return false;
        }

        rtv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(device_->CreateRenderTargetView(output_texture_.Get(), &rtv_desc, &rtv_uv_))) {
          return false;
        }

        if (FAILED(output_texture_->QueryInterface(IID_PPV_ARGS(&output_mutex_)))) {
          return false;
        }

        viewport_y_ = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        viewport_uv_ = {0.0f, 0.0f, static_cast<float>(width) / 2.0f, static_cast<float>(height) / 2.0f, 0.0f, 1.0f};

        return true;
      }

      bool ensure_constant_buffers(int width, int height, bool hdr) {
        if (!sampler_) {
          D3D11_SAMPLER_DESC sampler_desc {};
          sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
          sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
          sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
          sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
          sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
          sampler_desc.MinLOD = 0;
          sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
          if (FAILED(device_->CreateSamplerState(&sampler_desc, &sampler_))) {
            return false;
          }
        }

        if (!rotation_cb_) {
          int rotation_data[4] = {0, 0, 0, 0};
          D3D11_BUFFER_DESC desc {};
          desc.ByteWidth = sizeof(rotation_data);
          desc.Usage = D3D11_USAGE_IMMUTABLE;
          desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
          D3D11_SUBRESOURCE_DATA init {};
          init.pSysMem = rotation_data;
          if (FAILED(device_->CreateBuffer(&desc, &init, &rotation_cb_))) {
            return false;
          }
        }

        const bool hdr_changed = hdr_ != hdr;
        if (!color_matrix_cb_ || hdr_changed) {
          const video::colorspace_e space = hdr ? video::colorspace_e::bt2020 : video::colorspace_e::rec709;
          const video::color_t *colors = video::color_vectors_from_colorspace(space, false);
          if (!colors) {
            return false;
          }
          D3D11_BUFFER_DESC desc {};
          desc.ByteWidth = sizeof(video::color_t);
          desc.Usage = D3D11_USAGE_DEFAULT;
          desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
          D3D11_SUBRESOURCE_DATA init {};
          init.pSysMem = colors;
          if (!color_matrix_cb_) {
            if (FAILED(device_->CreateBuffer(&desc, &init, &color_matrix_cb_))) {
              return false;
            }
          } else {
            context_->UpdateSubresource(color_matrix_cb_.Get(), 0, nullptr, colors, 0, 0);
          }
          hdr_ = hdr;
        }

        if (!subsample_cb_ || subsample_width_ != width || subsample_height_ != height) {
          float subsample_data[4] = {1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), 0.0f, 0.0f};
          D3D11_BUFFER_DESC desc {};
          desc.ByteWidth = sizeof(subsample_data);
          desc.Usage = D3D11_USAGE_DEFAULT;
          desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
          D3D11_SUBRESOURCE_DATA init {};
          init.pSysMem = subsample_data;
          if (!subsample_cb_) {
            if (FAILED(device_->CreateBuffer(&desc, &init, &subsample_cb_))) {
              return false;
            }
          } else {
            context_->UpdateSubresource(subsample_cb_.Get(), 0, nullptr, subsample_data, 0, 0);
          }
          subsample_width_ = width;
          subsample_height_ = height;
        }

        return true;
      }

      ID3D11PixelShader *select_ps_y(ID3D11Texture2D *input_texture) const {
        D3D11_TEXTURE2D_DESC desc {};
        input_texture->GetDesc(&desc);
        if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && ps_y_linear_) {
          return ps_y_linear_.Get();
        }
        return ps_y_.Get();
      }

      ID3D11PixelShader *select_ps_uv(ID3D11Texture2D *input_texture) const {
        D3D11_TEXTURE2D_DESC desc {};
        input_texture->GetDesc(&desc);
        if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && ps_uv_linear_) {
          return ps_uv_linear_.Get();
        }
        return ps_uv_.Get();
      }

      Microsoft::WRL::ComPtr<ID3D11Device> device_;
      Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
      Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_y_;
      Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_y_;
      Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_uv_;
      Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_uv_;
      Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_y_linear_;
      Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_uv_linear_;
      Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
      Microsoft::WRL::ComPtr<ID3D11Buffer> color_matrix_cb_;
      Microsoft::WRL::ComPtr<ID3D11Buffer> rotation_cb_;
      Microsoft::WRL::ComPtr<ID3D11Buffer> subsample_cb_;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> output_texture_;
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_y_;
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_uv_;
      Microsoft::WRL::ComPtr<IDXGIKeyedMutex> output_mutex_;
      D3D11_VIEWPORT viewport_y_ {};
      D3D11_VIEWPORT viewport_uv_ {};
      int width_ = 0;
      int height_ = 0;
      int subsample_width_ = 0;
      int subsample_height_ = 0;
      bool hdr_ = false;
    };

    bool try_push_d3d11_frame(
      lwrtc_video_source_t *source,
      const std::shared_ptr<platf::img_t> &image,
      const std::optional<std::chrono::steady_clock::time_point> &timestamp,
      std::unique_ptr<D3D11Nv12Converter> *converter,
      bool hdr
    ) {
      auto d3d_img = std::dynamic_pointer_cast<platf::dxgi::img_d3d_t>(image);
      if (!d3d_img || !d3d_img->capture_texture) {
        return false;
      }
      if (!converter) {
        return false;
      }
      if (!(*converter)) {
        *converter = std::make_unique<D3D11Nv12Converter>();
      }

      ID3D11Texture2D *out_texture = nullptr;
      IDXGIKeyedMutex *out_mutex = nullptr;
      if (!(*converter)->Convert(
            d3d_img->capture_texture.get(),
            d3d_img->capture_mutex.get(),
            d3d_img->width,
            d3d_img->height,
            hdr,
            &out_texture,
            &out_mutex)) {
        return false;
      }

      return lwrtc_video_source_push_d3d11(
        source,
        out_texture,
        out_mutex,
        d3d_img->width,
        d3d_img->height,
        timestamp_to_us(timestamp)
      ) != 0;
    }
#endif

    void ensure_media_thread();
    void stop_media_thread();

    void media_thread_main() {
      using namespace std::chrono_literals;
      while (!webrtc_media_shutdown.load(std::memory_order_acquire)) {
        {
          std::unique_lock<std::mutex> lock(webrtc_media_mutex);
          webrtc_media_cv.wait_for(lock, 10ms);
        }
        if (webrtc_media_shutdown.load(std::memory_order_acquire)) {
          break;
        }

        struct VideoWork {
          lwrtc_video_source_t *source = nullptr;
          std::shared_ptr<platf::img_t> image;
          std::optional<std::chrono::steady_clock::time_point> timestamp;
#ifdef _WIN32
          std::unique_ptr<D3D11Nv12Converter> *converter = nullptr;
          bool hdr = false;
#endif
        };
        struct AudioWork {
          lwrtc_audio_source_t *source = nullptr;
          std::shared_ptr<std::vector<float>> samples;
          int sample_rate = 0;
          int channels = 0;
          int frames = 0;
        };

        std::vector<VideoWork> video_work;
        std::vector<AudioWork> audio_work;

        {
          std::lock_guard lg {session_mutex};
          for (auto &[_, session] : sessions) {
            if (!session.peer) {
              continue;
            }
            if (session.video_source) {
              RawVideoFrame frame;
              if (session.raw_video_frames.pop_latest(frame)) {
                VideoWork work;
                work.source = session.video_source;
                work.image = std::move(frame.image);
                work.timestamp = frame.timestamp;
#ifdef _WIN32
                work.converter = &session.d3d_converter;
                work.hdr = session.state.hdr.value_or(false);
#endif
                video_work.push_back(std::move(work));
              }
            }
            if (session.audio_source) {
              RawAudioFrame frame;
              while (session.raw_audio_frames.pop(frame)) {
                audio_work.push_back({session.audio_source, std::move(frame.samples), frame.sample_rate, frame.channels, frame.frames});
              }
            }
          }
        }

        for (auto &work : video_work) {
          if (!work.source || !work.image || !work.image->data) {
            continue;
          }
          if (work.image->width <= 0 || work.image->height <= 0) {
            continue;
          }
          bool pushed = false;
#ifdef _WIN32
          pushed = try_push_d3d11_frame(
            work.source,
            work.image,
            work.timestamp,
            work.converter,
            work.hdr
          );
#endif
#ifdef __APPLE__
          if (!pushed) {
            pushed = try_push_nv12_frame(work.source, work.image, work.timestamp);
          }
#endif
          if (!pushed) {
            lwrtc_video_source_push_argb(
              work.source,
              work.image->data,
              work.image->row_pitch,
              work.image->width,
              work.image->height,
              timestamp_to_us(work.timestamp)
            );
          }
        }

        for (auto &work : audio_work) {
          if (!work.source || !work.samples || work.samples->empty()) {
            continue;
          }
          lwrtc_audio_source_push(
            work.source,
            work.samples->data(),
            static_cast<int>(sizeof(float) * 8),
            work.sample_rate,
            work.channels,
            work.frames
          );
        }
      }
    }

    void ensure_media_thread() {
      bool expected = false;
      if (!webrtc_media_running.compare_exchange_strong(expected, true)) {
        return;
      }
      webrtc_media_shutdown.store(false, std::memory_order_release);
      webrtc_media_thread = std::thread(&media_thread_main);
    }

    void stop_media_thread() {
      if (!webrtc_media_running.load(std::memory_order_acquire)) {
        return;
      }
      webrtc_media_shutdown.store(true, std::memory_order_release);
      webrtc_media_cv.notify_all();
      if (webrtc_media_thread.joinable()) {
        webrtc_media_thread.join();
      }
      webrtc_media_running.store(false, std::memory_order_release);
    }

    bool attach_media_tracks(Session &session) {
      if (!session.peer || !ensure_webrtc_factory()) {
        return false;
      }

      const std::string stream_id = "sunshine-" + session.state.id;

      if (session.state.audio && !session.audio_source) {
        session.audio_source = lwrtc_audio_source_create(webrtc_factory);
        if (!session.audio_source) {
          BOOST_LOG(error) << "WebRTC: failed to create audio source for " << session.state.id;
          return false;
        }
      }

      if (session.state.audio && !session.audio_track) {
        const std::string track_id = "audio-" + session.state.id;
        session.audio_track = lwrtc_audio_track_create(webrtc_factory, session.audio_source, track_id.c_str());
        if (!session.audio_track) {
          BOOST_LOG(error) << "WebRTC: failed to create audio track for " << session.state.id;
          return false;
        }
        if (!lwrtc_peer_add_audio_track(session.peer, session.audio_track, stream_id.c_str())) {
          BOOST_LOG(error) << "WebRTC: failed to add audio track for " << session.state.id;
          return false;
        }
      }

      if (session.state.video && !session.video_source) {
        session.video_source = lwrtc_video_source_create(webrtc_factory);
        if (!session.video_source) {
          BOOST_LOG(error) << "WebRTC: failed to create video source for " << session.state.id;
          return false;
        }
      }

      if (session.state.video && !session.video_track) {
        const std::string track_id = "video-" + session.state.id;
        session.video_track = lwrtc_video_track_create(webrtc_factory, session.video_source, track_id.c_str());
        if (!session.video_track) {
          BOOST_LOG(error) << "WebRTC: failed to create video track for " << session.state.id;
          return false;
        }
        if (!lwrtc_peer_add_video_track(session.peer, session.video_track, stream_id.c_str())) {
          BOOST_LOG(error) << "WebRTC: failed to add video track for " << session.state.id;
          return false;
        }
      }

      ensure_media_thread();
      return true;
    }
#endif

    std::vector<std::uint8_t> replace_payload(
      const std::string_view &original,
      const std::string_view &old,
      const std::string_view &_new
    ) {
      std::vector<std::uint8_t> replaced;
      replaced.reserve(original.size() + _new.size() - old.size());

      auto begin = std::begin(original);
      auto end = std::end(original);
      auto next = std::search(begin, end, std::begin(old), std::end(old));

      std::copy(begin, next, std::back_inserter(replaced));
      if (next != end) {
        std::copy(std::begin(_new), std::end(_new), std::back_inserter(replaced));
        std::copy(next + old.size(), end, std::back_inserter(replaced));
      }

      return replaced;
    }

    std::shared_ptr<std::vector<std::uint8_t>> copy_video_payload(video::packet_raw_t &packet) {
      std::vector<std::uint8_t> payload;
      std::string_view payload_view {
        reinterpret_cast<const char *>(packet.data()),
        packet.data_size()
      };

      std::optional<std::vector<std::uint8_t>> replaced_payload;
      if (packet.is_idr() && packet.replacements) {
        for (const auto &replacement : *packet.replacements) {
          auto next_payload = replace_payload(payload_view, replacement.old, replacement._new);
          replaced_payload = std::move(next_payload);
          payload_view = std::string_view {
            reinterpret_cast<const char *>(replaced_payload->data()),
            replaced_payload->size()
          };
        }
      }

      if (replaced_payload) {
        payload = std::move(*replaced_payload);
      } else {
        payload.assign(
          reinterpret_cast<const std::uint8_t *>(payload_view.data()),
          reinterpret_cast<const std::uint8_t *>(payload_view.data()) + payload_view.size()
        );
      }

      return std::make_shared<std::vector<std::uint8_t>>(std::move(payload));
    }

    std::shared_ptr<std::vector<std::uint8_t>> copy_audio_payload(const audio::buffer_t &packet) {
      std::vector<std::uint8_t> payload;
      payload.assign(packet.begin(), packet.end());
      return std::make_shared<std::vector<std::uint8_t>>(std::move(payload));
    }

    SessionState snapshot_session(const Session &session) {
      return session.state;
    }
  }  // namespace

  bool has_active_sessions() {
    return active_sessions.load(std::memory_order_relaxed) > 0;
  }

  SessionState create_session(const SessionOptions &options) {
    Session session;
    session.state.id = uuid_util::uuid_t::generate().string();
    session.state.audio = options.audio;
    session.state.video = options.video;
    session.state.encoded = options.encoded;
    session.state.width = options.width;
    session.state.height = options.height;
    session.state.fps = options.fps;
    session.state.bitrate_kbps = options.bitrate_kbps;
    session.state.codec = options.codec;
    session.state.hdr = options.hdr;
    session.state.audio_channels = options.audio_channels;
    session.state.audio_codec = options.audio_codec;
    session.state.profile = options.profile;

    SessionState snapshot = session.state;
    std::lock_guard lg {session_mutex};
    sessions.emplace(snapshot.id, std::move(session));
    active_sessions.fetch_add(1, std::memory_order_relaxed);
    return snapshot;
  }

  bool close_session(std::string_view id) {
#ifdef SUNSHINE_ENABLE_WEBRTC
    lwrtc_peer_t *peer = nullptr;
    std::shared_ptr<SessionIceContext> ice_context;
    lwrtc_audio_track_t *audio_track = nullptr;
    lwrtc_video_track_t *video_track = nullptr;
    lwrtc_audio_source_t *audio_source = nullptr;
    lwrtc_video_source_t *video_source = nullptr;
#endif
    {
      std::lock_guard lg {session_mutex};
      auto it = sessions.find(std::string {id});
      if (it == sessions.end()) {
        return false;
      }
#ifdef SUNSHINE_ENABLE_WEBRTC
      peer = it->second.peer;
      ice_context = it->second.ice_context;
      audio_track = it->second.audio_track;
      video_track = it->second.video_track;
      audio_source = it->second.audio_source;
      video_source = it->second.video_source;
#endif
      sessions.erase(it);
      active_sessions.fetch_sub(1, std::memory_order_relaxed);
    }
#ifdef SUNSHINE_ENABLE_WEBRTC
    if (peer) {
      if (ice_context) {
        ice_context->active.store(false, std::memory_order_release);
      }
      lwrtc_peer_close(peer);
      lwrtc_peer_release(peer);
    }
    if (audio_track) {
      lwrtc_audio_track_release(audio_track);
    }
    if (video_track) {
      lwrtc_video_track_release(video_track);
    }
    if (audio_source) {
      lwrtc_audio_source_release(audio_source);
    }
    if (video_source) {
      lwrtc_video_source_release(video_source);
    }
    if (!has_active_sessions()) {
      stop_media_thread();
    }
#endif
    return true;
  }

  std::optional<SessionState> get_session(std::string_view id) {
    std::lock_guard lg {session_mutex};
    auto it = sessions.find(std::string {id});
    if (it == sessions.end()) {
      return std::nullopt;
    }
    return snapshot_session(it->second);
  }

  std::vector<SessionState> list_sessions() {
    std::lock_guard lg {session_mutex};
    std::vector<SessionState> results;
    results.reserve(sessions.size());
    for (const auto &entry : sessions) {
      results.emplace_back(snapshot_session(entry.second));
    }
    return results;
  }

  void submit_video_packet(video::packet_raw_t &packet) {
    if (!has_active_sessions()) {
      return;
    }

    auto payload = copy_video_payload(packet);

    std::lock_guard lg {session_mutex};
    for (auto &[_, session] : sessions) {
      if (!session.state.video) {
        continue;
      }

      EncodedVideoFrame frame;
      frame.data = payload;
      frame.frame_index = packet.frame_index();
      frame.idr = packet.is_idr();
      frame.after_ref_frame_invalidation = packet.after_ref_frame_invalidation;
      frame.timestamp = packet.frame_timestamp;

      bool dropped = session.video_frames.push(std::move(frame));
      session.state.video_packets++;
      session.state.last_video_time = std::chrono::steady_clock::now();
      session.state.last_video_bytes = payload->size();
      session.state.last_video_idr = packet.is_idr();
      session.state.last_video_frame_index = packet.frame_index();
      if (dropped) {
        session.state.video_dropped++;
      }
    }
  }

  void submit_audio_packet(const audio::buffer_t &packet) {
    if (!has_active_sessions()) {
      return;
    }

    auto payload = copy_audio_payload(packet);

    std::lock_guard lg {session_mutex};
    for (auto &[_, session] : sessions) {
      if (!session.state.audio) {
        continue;
      }

      EncodedAudioFrame frame;
      frame.data = payload;
      frame.timestamp = std::chrono::steady_clock::now();

      bool dropped = session.audio_frames.push(std::move(frame));
      session.state.audio_packets++;
      session.state.last_audio_time = std::chrono::steady_clock::now();
      session.state.last_audio_bytes = payload->size();
      if (dropped) {
        session.state.audio_dropped++;
      }
    }
  }

  void submit_video_frame(const std::shared_ptr<platf::img_t> &frame) {
    if (!has_active_sessions() || !frame) {
      return;
    }

    std::lock_guard lg {session_mutex};
    for (auto &[_, session] : sessions) {
      if (!session.state.video) {
        continue;
      }

      RawVideoFrame raw;
      raw.image = frame;
      raw.timestamp = frame->frame_timestamp;
      session.raw_video_frames.push(std::move(raw));
    }
#ifdef SUNSHINE_ENABLE_WEBRTC
    webrtc_media_cv.notify_all();
#endif
  }

  void submit_audio_frame(const std::vector<float> &samples, int sample_rate, int channels, int frames) {
    if (!has_active_sessions() || samples.empty()) {
      return;
    }

    auto shared_samples = std::make_shared<std::vector<float>>(samples);
    std::lock_guard lg {session_mutex};
    for (auto &[_, session] : sessions) {
      if (!session.state.audio) {
        continue;
      }

      RawAudioFrame raw;
      raw.samples = shared_samples;
      raw.sample_rate = sample_rate;
      raw.channels = channels;
      raw.frames = frames;
      session.raw_audio_frames.push(std::move(raw));
    }
#ifdef SUNSHINE_ENABLE_WEBRTC
    webrtc_media_cv.notify_all();
#endif
  }

  void set_rtsp_sessions_active(bool active) {
    rtsp_sessions_active.store(active, std::memory_order_relaxed);
  }

  bool set_remote_offer(std::string_view id, const std::string &sdp, const std::string &type) {
    std::string session_id {id};
#ifdef SUNSHINE_ENABLE_WEBRTC
    lwrtc_peer_t *peer = nullptr;
#endif
    {
      std::lock_guard lg {session_mutex};
      auto it = sessions.find(session_id);
      if (it == sessions.end()) {
        return false;
      }

      it->second.remote_offer_sdp = sdp;
      it->second.remote_offer_type = type;
      it->second.state.has_remote_offer = true;

#ifdef SUNSHINE_ENABLE_WEBRTC
      if (!it->second.peer) {
        if (!it->second.ice_context) {
          auto ice_context = std::make_shared<SessionIceContext>();
          ice_context->id = session_id;
          it->second.ice_context = std::move(ice_context);
        }
        auto new_peer = create_peer_connection(it->second.state, it->second.ice_context.get());
        if (!new_peer) {
          return false;
        }
        it->second.peer = new_peer;
      }
      if (!attach_media_tracks(it->second)) {
        return false;
      }
      peer = it->second.peer;
#endif
    }

#ifdef SUNSHINE_ENABLE_WEBRTC
    if (!peer) {
      return false;
    }

    auto *ctx = new SessionPeerContext {session_id, peer};
    lwrtc_peer_set_remote_description(
      peer,
      sdp.c_str(),
      type.c_str(),
      &on_set_remote_success,
      &on_set_remote_failure,
      ctx
    );
    return true;
#else
    BOOST_LOG(error) << "WebRTC: support is disabled at build time";
    return false;
#endif
  }

  bool add_ice_candidate(std::string_view id, std::string mid, int mline_index, std::string candidate) {
#ifdef SUNSHINE_ENABLE_WEBRTC
    lwrtc_peer_t *peer = nullptr;
    std::string stored_mid;
    std::string stored_candidate;
    int stored_mline_index = -1;
#endif
    {
      std::lock_guard lg {session_mutex};
      auto it = sessions.find(std::string {id});
      if (it == sessions.end()) {
        return false;
      }

      Session::IceCandidate entry;
      entry.mid = std::move(mid);
      entry.mline_index = mline_index;
      entry.candidate = std::move(candidate);
      it->second.candidates.emplace_back(std::move(entry));
      it->second.state.ice_candidates = it->second.candidates.size();
#ifdef SUNSHINE_ENABLE_WEBRTC
      peer = it->second.peer;
      if (!it->second.candidates.empty()) {
        const auto &stored = it->second.candidates.back();
        stored_mid = stored.mid;
        stored_candidate = stored.candidate;
        stored_mline_index = stored.mline_index;
      }
#endif
    }
#ifdef SUNSHINE_ENABLE_WEBRTC
    if (peer && !stored_candidate.empty()) {
      lwrtc_peer_add_candidate(peer, stored_mid.c_str(), stored_mline_index, stored_candidate.c_str());
    }
#endif
    return true;
  }

  bool set_local_answer(std::string_view id, const std::string &sdp, const std::string &type) {
    std::lock_guard lg {session_mutex};
    auto it = sessions.find(std::string {id});
    if (it == sessions.end()) {
      return false;
    }

    it->second.local_answer_sdp = sdp;
    it->second.local_answer_type = type;
    it->second.state.has_local_answer = true;
    return true;
  }

  bool add_local_candidate(std::string_view id, std::string mid, int mline_index, std::string candidate) {
    std::lock_guard lg {session_mutex};
    auto it = sessions.find(std::string {id});
    if (it == sessions.end()) {
      return false;
    }

    Session::LocalCandidate entry;
    entry.mid = std::move(mid);
    entry.mline_index = mline_index;
    entry.candidate = std::move(candidate);
    entry.index = ++it->second.local_candidate_counter;
    it->second.local_candidates.emplace_back(std::move(entry));
    return true;
  }

  bool get_local_answer(std::string_view id, std::string &sdp_out, std::string &type_out) {
    std::lock_guard lg {session_mutex};
    auto it = sessions.find(std::string {id});
    if (it == sessions.end()) {
      return false;
    }
    if (!it->second.state.has_local_answer) {
      return false;
    }
    sdp_out = it->second.local_answer_sdp;
    type_out = it->second.local_answer_type;
    return true;
  }

  std::vector<IceCandidateInfo> get_local_candidates(std::string_view id, std::size_t since) {
    std::lock_guard lg {session_mutex};
    auto it = sessions.find(std::string {id});
    if (it == sessions.end()) {
      return {};
    }

    std::vector<IceCandidateInfo> results;
    for (const auto &candidate : it->second.local_candidates) {
      if (candidate.index <= since) {
        continue;
      }
      IceCandidateInfo info;
      info.mid = candidate.mid;
      info.mline_index = candidate.mline_index;
      info.candidate = candidate.candidate;
      info.index = candidate.index;
      results.emplace_back(std::move(info));
    }
    return results;
  }

  std::string get_server_cert_fingerprint() {
    std::string cert_pem = file_handler::read_file(config::nvhttp.cert.c_str());
    if (cert_pem.empty()) {
      return {};
    }

    auto cert = crypto::x509(cert_pem);
    if (!cert) {
      return {};
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (!X509_digest(cert.get(), EVP_sha256(), digest, &digest_len)) {
      return {};
    }

    std::string fingerprint;
    fingerprint.reserve(digest_len * 3);
    for (unsigned int i = 0; i < digest_len; ++i) {
      char buf[4];
      std::snprintf(buf, sizeof(buf), "%02X", digest[i]);
      fingerprint.append(buf);
      if (i + 1 < digest_len) {
        fingerprint.push_back(':');
      }
    }

    return fingerprint;
  }

  std::string get_server_cert_pem() {
    return file_handler::read_file(config::nvhttp.cert.c_str());
  }
}  // namespace webrtc_stream
