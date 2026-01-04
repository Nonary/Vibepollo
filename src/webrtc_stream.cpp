/**
 * @file src/webrtc_stream.cpp
 * @brief Definitions for WebRTC session tracking and frame handoff.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <unordered_map>

// lib includes
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
#include <boost/algorithm/string.hpp>

#ifdef SUNSHINE_ENABLE_WEBRTC
#include <libwebrtc_c.h>
#endif

// local includes
#include "audio.h"
#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "process.h"
#include "rtsp.h"
#include "stream.h"
#include "utility.h"
#include "httpcommon.h"
#include "video.h"
#include "video_colorspace.h"
#include "uuid.h"
#include "webrtc_stream.h"

#ifdef _WIN32
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "src/platform/common.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/frame_limiter.h"
#include "src/platform/windows/display_helper_integration.h"
#include "src/platform/windows/display_helper_request_helpers.h"
#include "src/platform/windows/virtual_display.h"
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
    constexpr std::size_t kMaxAudioFrames = 4;
    constexpr short kAbsCoordinateMax = 32767;
    constexpr int kDefaultWidth = 1920;
    constexpr int kDefaultHeight = 1080;
    constexpr int kDefaultFps = 60;
    constexpr int kDefaultAudioChannels = 2;
    constexpr int kDefaultAudioPacketMs = 10;
    constexpr std::size_t kEncodedPrefixLogLimit = 5;
    constexpr auto kKeyframeRequestInterval = std::chrono::milliseconds {500};
    constexpr auto kKeyframeResyncInterval = std::chrono::seconds {2};
    constexpr auto kVideoPacingSlackLatency = std::chrono::milliseconds {0};
    constexpr auto kVideoPacingSlackBalanced = std::chrono::milliseconds {2};
    constexpr auto kVideoPacingSlackSmooth = std::chrono::milliseconds {3};
    constexpr auto kVideoPacingSlackMin = std::chrono::milliseconds {0};
    constexpr auto kVideoPacingSlackMax = std::chrono::milliseconds {10};
    constexpr auto kVideoMaxFrameAgeMin = std::chrono::milliseconds {5};
    constexpr auto kVideoMaxFrameAgeMax = std::chrono::milliseconds {100};
    constexpr auto kAudioMaxFrameAge = std::chrono::milliseconds {kDefaultAudioPacketMs * kMaxAudioFrames};
    constexpr std::size_t kVideoInflightFramesMin = 2;
    constexpr std::size_t kVideoInflightFramesMax = 6;
    constexpr std::size_t kVideoInflightKeyframeExtra = 2;
    constexpr auto kWebrtcIdleGracePeriod = std::chrono::minutes {5};

    struct SharedEncodedPayloadReleaseContext {
      std::shared_ptr<std::vector<std::uint8_t>> payload;
      std::shared_ptr<std::atomic_uint32_t> inflight;
    };

    void release_shared_encoded_payload(void* user) noexcept {
      auto* context = static_cast<SharedEncodedPayloadReleaseContext*>(user);
      if (!context) {
        return;
      }
      if (context->inflight) {
        context->inflight->fetch_sub(1, std::memory_order_relaxed);
      }
      delete context;
    }

    struct WebRtcCaptureConfigKey {
      int app_id = 0;
      int width = 0;
      int height = 0;
      int framerate = 0;
      int bitrate = 0;
      int video_format = 0;
      int dynamic_range = 0;
      int chroma_sampling_type = 0;
      bool prefer_sdr_10bit = false;
      int audio_channels = 0;
      bool host_audio = false;

      bool operator==(const WebRtcCaptureConfigKey &) const = default;
    };

#ifdef _WIN32
    void prepare_virtual_display_for_webrtc_session(
      const std::shared_ptr<rtsp_stream::launch_session_t> &session
    ) {
      if (!session) {
        return;
      }

      std::optional<std::string> app_output_override;
      if (session->output_name_override && !session->output_name_override->empty()) {
        app_output_override = boost::algorithm::trim_copy(*session->output_name_override);
      }

      if (app_output_override &&
          boost::iequals(*app_output_override, VDISPLAY::SUDOVDA_VIRTUAL_DISPLAY_SELECTION)) {
        session->virtual_display = true;
        app_output_override.reset();
        session->output_name_override.reset();
      }

      bool config_requests_virtual =
        config::video.virtual_display_mode != config::video_t::virtual_display_mode_e::disabled;
      if (session->virtual_display_mode_override) {
        config_requests_virtual =
          *session->virtual_display_mode_override != config::video_t::virtual_display_mode_e::disabled;
      }
      const bool metadata_requests_virtual = session->app_metadata && session->app_metadata->virtual_screen;
      bool request_virtual_display =
        session->virtual_display || config_requests_virtual || metadata_requests_virtual;
      const bool has_app_output_override = app_output_override.has_value();
      if (has_app_output_override) {
        request_virtual_display = false;
      }

      if (!request_virtual_display) {
        return;
      }

      if (proc::vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
        proc::initVDisplayDriver();
        if (proc::vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
          BOOST_LOG(warning)
            << "SudaVDA driver unavailable (status=" << static_cast<int>(proc::vDisplayDriverStatus)
            << "). Continuing with best-effort virtual display creation.";
        }
      }

      if (!config::video.adapter_name.empty()) {
        (void) VDISPLAY::setRenderAdapterByName(platf::from_utf8(config::video.adapter_name));
      } else {
        (void) VDISPLAY::setRenderAdapterWithMostDedicatedMemory();
      }

      if (auto existing_device = VDISPLAY::resolveAnyVirtualDisplayDeviceId()) {
        session->virtual_display = true;
        session->virtual_display_failed = false;
        session->virtual_display_device_id = *existing_device;
        session->virtual_display_ready_since = std::chrono::steady_clock::now();
        config::set_runtime_output_name_override(session->virtual_display_device_id);
        return;
      }

      auto parse_uuid = [](const std::string &value) -> std::optional<uuid_util::uuid_t> {
        if (value.empty()) {
          return std::nullopt;
        }
        try {
          return uuid_util::uuid_t::parse(value);
        } catch (...) {
          return std::nullopt;
        }
      };

      const auto effective_virtual_display_mode =
        session->virtual_display_mode_override.value_or(config::video.virtual_display_mode);
      const bool shared_mode =
        (effective_virtual_display_mode == config::video_t::virtual_display_mode_e::shared);

      uuid_util::uuid_t session_uuid;
      if (shared_mode) {
        if (auto parsed = parse_uuid(http::shared_virtual_display_guid)) {
          session_uuid = *parsed;
        } else {
          session_uuid = VDISPLAY::persistentVirtualDisplayUuid();
          http::shared_virtual_display_guid = session_uuid.string();
        }
      } else if (auto parsed = parse_uuid(session->unique_id)) {
        session_uuid = *parsed;
      } else {
        session_uuid = uuid_util::uuid_t::generate();
      }

      session->unique_id = session_uuid.string();
      session->client_uuid = session->unique_id;

      GUID virtual_display_guid {};
      std::memcpy(&virtual_display_guid, session_uuid.b8, sizeof(virtual_display_guid));
      session->virtual_display_guid_bytes.fill(0);
      std::copy_n(
        std::cbegin(session_uuid.b8),
        sizeof(session_uuid.b8),
        session->virtual_display_guid_bytes.begin()
      );

      const auto desired_layout =
        session->virtual_display_layout_override.value_or(config::video.virtual_display_layout);
      const bool wants_extended_layout =
        desired_layout != config::video_t::virtual_display_layout_e::exclusive;
      if (wants_extended_layout) {
        if (auto topology_snapshot = display_helper_integration::capture_current_topology()) {
          session->virtual_display_topology_snapshot = *topology_snapshot;
        } else {
          session->virtual_display_topology_snapshot.reset();
        }
      } else {
        session->virtual_display_topology_snapshot.reset();
      }

      uint32_t vd_width = session->width > 0 ? static_cast<uint32_t>(session->width) : 1920u;
      uint32_t vd_height = session->height > 0 ? static_cast<uint32_t>(session->height) : 1080u;
      uint32_t base_vd_fps = session->fps > 0 ? static_cast<uint32_t>(session->fps) : 0u;
      uint32_t base_vd_fps_millihz = base_vd_fps;
      if (base_vd_fps_millihz > 0 && base_vd_fps_millihz < 1000u) {
        base_vd_fps_millihz *= 1000u;
      }
      uint32_t vd_fps = 0;
      if (session->framegen_refresh_rate && *session->framegen_refresh_rate > 0) {
        vd_fps = static_cast<uint32_t>(*session->framegen_refresh_rate);
      } else if (base_vd_fps > 0) {
        vd_fps = base_vd_fps;
      } else {
        vd_fps = 60000u;
      }
      if (vd_fps < 1000u) {
        vd_fps *= 1000u;
      }
      const bool framegen_refresh_active =
        session->framegen_refresh_rate && *session->framegen_refresh_rate > 0;

      std::string client_label = session->client_name;
      if (client_label.empty()) {
        client_label = session->device_name;
      }
      if (client_label.empty()) {
        client_label = "WebRTC";
      }

      VDISPLAY::setWatchdogFeedingEnabled(true);
      auto display_info = VDISPLAY::createVirtualDisplay(
        session->unique_id.c_str(),
        client_label.c_str(),
        session->hdr_profile ? session->hdr_profile->c_str() : nullptr,
        vd_width,
        vd_height,
        vd_fps,
        virtual_display_guid,
        base_vd_fps_millihz,
        framegen_refresh_active
      );

      if (display_info) {
        session->virtual_display = true;
        session->virtual_display_failed = false;
        if (display_info->device_id && !display_info->device_id->empty()) {
          session->virtual_display_device_id = *display_info->device_id;
        } else if (auto resolved_device = VDISPLAY::resolveAnyVirtualDisplayDeviceId()) {
          session->virtual_display_device_id = *resolved_device;
        } else {
          session->virtual_display_device_id.clear();
        }
        session->virtual_display_ready_since = display_info->ready_since;
        if (!session->virtual_display_device_id.empty()) {
          config::set_runtime_output_name_override(session->virtual_display_device_id);
        }
        return;
      }

      session->virtual_display = false;
      session->virtual_display_failed = true;
      session->virtual_display_guid_bytes.fill(0);
      session->virtual_display_device_id.clear();
      session->virtual_display_ready_since.reset();
    }
#endif

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
      std::shared_ptr<std::vector<int16_t>> samples;
      int sample_rate = 0;
      int channels = 0;
      int frames = 0;
      std::chrono::steady_clock::time_point timestamp;
    };

    enum class video_pacing_mode_e {
      latency,
      balanced,
      smoothness
    };

    int max_age_frames_for_mode(video_pacing_mode_e mode) {
      switch (mode) {
        case video_pacing_mode_e::latency:
          return 2;
        case video_pacing_mode_e::balanced:
          return 2;
        case video_pacing_mode_e::smoothness:
          return 3;
        default:
          return 2;
      }
    }

    int max_age_ms_from_frames(int fps, int frames) {
      if (fps <= 0) {
        fps = kDefaultFps;
      }
      const double interval_ms = 1000.0 / static_cast<double>(fps);
      return static_cast<int>(std::lround(interval_ms * static_cast<double>(frames)));
    }

    struct VideoPacingConfig {
      video_pacing_mode_e mode = video_pacing_mode_e::balanced;
      std::chrono::nanoseconds slack = std::chrono::duration_cast<std::chrono::nanoseconds>(kVideoPacingSlackBalanced);
      std::chrono::nanoseconds max_frame_age = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::milliseconds {max_age_ms_from_frames(kDefaultFps, 2)});
      int max_age_frames = 2;
      bool max_frame_age_override = false;
      bool pace = true;
      bool drop_old_frames = true;
      bool prefer_latest = false;
    };

    bool starts_with_annexb(const std::vector<std::uint8_t> &data) {
      if (data.size() < 3) {
        return false;
      }
      if (data[0] == 0 && data[1] == 0 && data[2] == 1) {
        return true;
      }
      return data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
    }

    std::string hex_prefix(const std::vector<std::uint8_t> &data, std::size_t max_bytes = 8) {
      std::ostringstream oss;
      const std::size_t count = std::min(data.size(), max_bytes);
      for (std::size_t i = 0; i < count; ++i) {
        if (i) {
          oss << ' ';
        }
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(data[i]);
      }
      return oss.str();
    }

    video_pacing_mode_e video_pacing_mode_from_string(const std::optional<std::string> &mode) {
      if (!mode) {
        return video_pacing_mode_e::balanced;
      }
      if (boost::iequals(*mode, "latency")) {
        return video_pacing_mode_e::latency;
      }
      if (boost::iequals(*mode, "smooth") || boost::iequals(*mode, "smoothness")) {
        return video_pacing_mode_e::smoothness;
      }
      return video_pacing_mode_e::balanced;
    }

    const char *video_pacing_mode_to_string(video_pacing_mode_e mode) {
      switch (mode) {
        case video_pacing_mode_e::latency:
          return "latency";
        case video_pacing_mode_e::smoothness:
          return "smoothness";
        case video_pacing_mode_e::balanced:
        default:
          return "balanced";
      }
    }

    struct WebRtcCaptureState {
      std::mutex mutex;
      std::atomic_bool active {false};
      std::atomic_bool idle_shutdown_pending {false};
      std::shared_ptr<safe::mail_raw_t> mail;
      std::thread video_thread;
      std::thread audio_thread;
      std::thread feedback_thread;
      safe::mail_raw_t::queue_t<platf::gamepad_feedback_msg_t> feedback_queue;
      std::atomic_bool feedback_shutdown {false};
      std::optional<int> app_id;
      std::optional<WebRtcCaptureConfigKey> config_key;
    };

    template<class T>
    class ring_buffer_t {
    public:
      explicit ring_buffer_t(std::size_t max_items):
          _max_items {max_items} {
      }

      bool push(T item, bool keep_keyframes = false) {
        bool dropped = false;
        if (_queue.size() >= _max_items) {
          if (keep_keyframes) {
            if constexpr (requires(const T &value) { value.idr; }) {
              auto it = std::find_if(_queue.begin(), _queue.end(), [](const T &frame) {
                return !frame.idr;
              });
              if (it != _queue.end()) {
                _queue.erase(it);
              } else {
                _queue.pop_front();
              }
            } else {
              _queue.pop_front();
            }
          } else {
            _queue.pop_front();
          }
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

      bool drop_oldest(bool keep_keyframes = false) {
        if (_queue.empty()) {
          return false;
        }
        if (keep_keyframes) {
          if constexpr (requires(const T &value) { value.idr; }) {
            auto it = std::find_if(_queue.begin(), _queue.end(), [](const T &frame) {
              return !frame.idr;
            });
            if (it != _queue.end()) {
              _queue.erase(it);
              return true;
            }
          }
        }
        _queue.pop_front();
        return true;
      }

      bool empty() const {
        return _queue.empty();
      }

      std::size_t size() const {
        return _queue.size();
      }

      const T *front() const {
        if (_queue.empty()) {
          return nullptr;
        }
        return &_queue.front();
      }

    private:
      std::size_t _max_items;
      std::deque<T> _queue;
    };

    class AudioSamplePool {
    public:
      std::shared_ptr<std::vector<int16_t>> acquire(std::size_t samples) {
        std::vector<int16_t> *buffer = nullptr;
        {
          std::lock_guard lg {mutex_};
          if (!pool_.empty()) {
            buffer = pool_.back();
            pool_.pop_back();
          }
        }
        if (!buffer) {
          buffer = new std::vector<int16_t>();
        }
        buffer->resize(samples);
        return std::shared_ptr<std::vector<int16_t>>(buffer, [this](std::vector<int16_t> *ptr) {
          if (!ptr) {
            return;
          }
          ptr->clear();
          std::lock_guard lg {mutex_};
          if (pool_.size() >= max_pool_size_) {
            delete ptr;
            return;
          }
          pool_.push_back(ptr);
        });
      }

    private:
      std::mutex mutex_;
      std::vector<std::vector<int16_t> *> pool_;
      std::size_t max_pool_size_ = 64;
    };

    AudioSamplePool &audio_sample_pool() {
      static auto *pool = new AudioSamplePool();
      return *pool;
    }

    std::mutex input_mutex;
    std::shared_ptr<safe::mail_raw_t> input_mail;
    std::shared_ptr<input::input_t> input_context;
    std::mutex gamepad_mutex;
    std::bitset<16> webrtc_gamepads;

    std::shared_ptr<safe::mail_raw_t> current_capture_mail();

    std::shared_ptr<input::input_t> current_input_context() {
      auto capture_mail = current_capture_mail();
      std::lock_guard lg {input_mutex};
      if (capture_mail && input_mail != capture_mail) {
        if (input_context) {
          input::reset(input_context);
        }
        input_context.reset();
        input_mail.reset();
        {
          std::lock_guard lg {gamepad_mutex};
          webrtc_gamepads.reset();
        }
      }
      if (!input_context) {
        input_mail = capture_mail ? capture_mail : std::make_shared<safe::mail_raw_t>();
        input_context = input::alloc(input_mail);

        // Set up a default touch port for WebRTC input when capture mail isn't available.
        if (!capture_mail) {
          auto touch_port_event = input_mail->event<input::touch_port_t>(mail::touch_port);
#ifdef _WIN32
          int screen_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
          int screen_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
#else
          // For non-Windows platforms, use a default resolution
          // This will be updated when actual capture dimensions are known
          int screen_width = 1920;
          int screen_height = 1080;
#endif
          if (screen_width <= 0) screen_width = 1920;
          if (screen_height <= 0) screen_height = 1080;

          input::touch_port_t port {};
          port.offset_x = 0;
          port.offset_y = 0;
          port.width = screen_width;
          port.height = screen_height;
          port.env_width = screen_width;
          port.env_height = screen_height;
          port.client_offsetX = 0.0f;
          port.client_offsetY = 0.0f;
          port.scalar_inv = 1.0f;
          touch_port_event->raise(port);
        }
      }
      return input_context;
    }

    void reset_input_context() {
      std::lock_guard lg {input_mutex};
      if (input_context) {
        input::reset(input_context);
      }
      input_context.reset();
      input_mail.reset();
      {
        std::lock_guard gamepad_lock {gamepad_mutex};
        webrtc_gamepads.reset();
      }
    }

    uint8_t modifiers_from_json(const nlohmann::json &input) {
      uint8_t modifiers = 0;
      if (input.value("shift", false)) {
        modifiers |= MODIFIER_SHIFT;
      }
      if (input.value("ctrl", false)) {
        modifiers |= MODIFIER_CTRL;
      }
      if (input.value("alt", false)) {
        modifiers |= MODIFIER_ALT;
      }
      if (input.value("meta", false)) {
        modifiers |= MODIFIER_META;
      }
      return modifiers;
    }

    std::optional<short> map_dom_code_to_vk(std::string_view code, std::string_view key) {
      if (code.size() == 4 && code.rfind("Key", 0) == 0) {
        char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(code[3])));
        return static_cast<short>(letter);
      }
      if (code.size() == 6 && code.rfind("Digit", 0) == 0) {
        char digit = code[5];
        if (digit >= '0' && digit <= '9') {
          return static_cast<short>(digit);
        }
      }
      if (code == "Space") return 0x20;
      if (code == "Enter") return 0x0D;
      if (code == "Tab") return 0x09;
      if (code == "Escape") return 0x1B;
      if (code == "Backspace") return 0x08;
      if (code == "Delete") return 0x2E;
      if (code == "Insert") return 0x2D;
      if (code == "Home") return 0x24;
      if (code == "End") return 0x23;
      if (code == "PageUp") return 0x21;
      if (code == "PageDown") return 0x22;
      if (code == "ArrowLeft") return 0x25;
      if (code == "ArrowUp") return 0x26;
      if (code == "ArrowRight") return 0x27;
      if (code == "ArrowDown") return 0x28;
      if (code == "CapsLock") return 0x14;
      if (code == "ShiftLeft") return 0xA0;
      if (code == "ShiftRight") return 0xA1;
      if (code == "ControlLeft") return 0xA2;
      if (code == "ControlRight") return 0xA3;
      if (code == "AltLeft") return 0xA4;
      if (code == "AltRight") return 0xA5;
      if (code == "MetaLeft") return 0x5B;
      if (code == "MetaRight") return 0x5C;
      if (code == "ContextMenu") return 0x5D;
      if (code == "PrintScreen") return 0x2C;
      if (code == "ScrollLock") return 0x91;
      if (code == "Pause") return 0x13;
      if (code == "NumLock") return 0x90;
      if (code.rfind("F", 0) == 0 && code.size() >= 2 && code.size() <= 3) {
        int fn = std::atoi(std::string(code.substr(1)).c_str());
        if (fn >= 1 && fn <= 24) {
          return static_cast<short>(0x70 + (fn - 1));
        }
      }
      if (code.rfind("Numpad", 0) == 0) {
        if (code.size() == 7) {
          char digit = code[6];
          if (digit >= '0' && digit <= '9') {
            return static_cast<short>(0x60 + (digit - '0'));
          }
        }
        if (code == "NumpadAdd") return 0x6B;
        if (code == "NumpadSubtract") return 0x6D;
        if (code == "NumpadMultiply") return 0x6A;
        if (code == "NumpadDivide") return 0x6F;
        if (code == "NumpadDecimal") return 0x6E;
        if (code == "NumpadEnter") return 0x0D;
      }
      if (code == "Minus") return 0xBD;
      if (code == "Equal") return 0xBB;
      if (code == "BracketLeft") return 0xDB;
      if (code == "BracketRight") return 0xDD;
      if (code == "Backslash") return 0xDC;
      if (code == "Semicolon") return 0xBA;
      if (code == "Quote") return 0xDE;
      if (code == "Backquote") return 0xC0;
      if (code == "Comma") return 0xBC;
      if (code == "Period") return 0xBE;
      if (code == "Slash") return 0xBF;

      if (key.size() == 1) {
        unsigned char ch = static_cast<unsigned char>(key[0]);
        if (std::isalnum(ch)) {
          return static_cast<short>(std::toupper(ch));
        }
      }
      return std::nullopt;
    }

    int map_mouse_button(int button) {
      switch (button) {
        case 0: return BUTTON_LEFT;
        case 1: return BUTTON_MIDDLE;
        case 2: return BUTTON_RIGHT;
        case 3: return BUTTON_X1;
        case 4: return BUTTON_X2;
        default: return -1;
      }
    }

    std::vector<uint8_t> make_abs_mouse_move_packet(double x_norm, double y_norm) {
      NV_ABS_MOUSE_MOVE_PACKET packet {};
      packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet) - sizeof(packet.header.size));
      packet.header.magic = util::endian::little<std::uint32_t>(MOUSE_MOVE_ABS_MAGIC);

      auto clamped_x = std::clamp(x_norm, 0.0, 1.0);
      auto clamped_y = std::clamp(y_norm, 0.0, 1.0);
      int x = static_cast<int>(std::lround(clamped_x * kAbsCoordinateMax));
      int y = static_cast<int>(std::lround(clamped_y * kAbsCoordinateMax));
      packet.x = util::endian::big(static_cast<int16_t>(std::clamp(x, 0, (int) kAbsCoordinateMax)));
      packet.y = util::endian::big(static_cast<int16_t>(std::clamp(y, 0, (int) kAbsCoordinateMax)));
      packet.unused = 0;
      packet.width = util::endian::big(static_cast<int16_t>(kAbsCoordinateMax));
      packet.height = util::endian::big(static_cast<int16_t>(kAbsCoordinateMax));

      std::vector<uint8_t> data(sizeof(packet));
      std::memcpy(data.data(), &packet, sizeof(packet));
      return data;
    }

    std::vector<uint8_t> make_mouse_button_packet(int button, bool release) {
      NV_MOUSE_BUTTON_PACKET packet {};
      packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet) - sizeof(packet.header.size));
      packet.header.magic = util::endian::little<std::uint32_t>(
        release ? MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5 : MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5
      );
      packet.button = static_cast<std::uint8_t>(button);

      std::vector<uint8_t> data(sizeof(packet));
      std::memcpy(data.data(), &packet, sizeof(packet));
      return data;
    }

    std::vector<uint8_t> make_scroll_packet(int amount) {
      NV_SCROLL_PACKET packet {};
      packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet) - sizeof(packet.header.size));
      packet.header.magic = util::endian::little<std::uint32_t>(SCROLL_MAGIC_GEN5);
      packet.scrollAmt1 = util::endian::big(static_cast<int16_t>(std::clamp(amount, -32768, 32767)));
      packet.scrollAmt2 = 0;
      packet.zero3 = 0;

      std::vector<uint8_t> data(sizeof(packet));
      std::memcpy(data.data(), &packet, sizeof(packet));
      return data;
    }

    std::vector<uint8_t> make_hscroll_packet(int amount) {
      SS_HSCROLL_PACKET packet {};
      packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet) - sizeof(packet.header.size));
      packet.header.magic = util::endian::little<std::uint32_t>(SS_HSCROLL_MAGIC);
      packet.scrollAmount = util::endian::big(static_cast<int16_t>(std::clamp(amount, -32768, 32767)));

      std::vector<uint8_t> data(sizeof(packet));
      std::memcpy(data.data(), &packet, sizeof(packet));
      return data;
    }

    std::vector<uint8_t> make_keyboard_packet(short key_code, bool release, uint8_t modifiers) {
      NV_KEYBOARD_PACKET packet {};
      packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet) - sizeof(packet.header.size));
      packet.header.magic = util::endian::little<std::uint32_t>(release ? KEY_UP_EVENT_MAGIC : KEY_DOWN_EVENT_MAGIC);
      packet.flags = 0;
      packet.keyCode = key_code;
      packet.modifiers = static_cast<char>(modifiers);
      packet.zero2 = 0;

      std::vector<uint8_t> data(sizeof(packet));
      std::memcpy(data.data(), &packet, sizeof(packet));
      return data;
    }

    void write_netfloat(netfloat &out, float value) {
      auto le_value = util::endian::little(value);
      std::memcpy(out, &le_value, sizeof(le_value));
    }

    std::vector<uint8_t> make_gamepad_arrival_packet(
      int controller_number,
      uint8_t controller_type,
      uint16_t capabilities,
      uint32_t supported_buttons
    ) {
      SS_CONTROLLER_ARRIVAL_PACKET packet {};
      packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet) - sizeof(packet.header.size));
      packet.header.magic = util::endian::little<std::uint32_t>(SS_CONTROLLER_ARRIVAL_MAGIC);
      packet.controllerNumber = static_cast<uint8_t>(controller_number);
      packet.type = controller_type;
      packet.capabilities = util::endian::little(static_cast<uint16_t>(capabilities));
      packet.supportedButtonFlags = util::endian::little(static_cast<uint32_t>(supported_buttons));

      std::vector<uint8_t> data(sizeof(packet));
      std::memcpy(data.data(), &packet, sizeof(packet));
      return data;
    }

    std::vector<uint8_t> make_gamepad_state_packet(
      int controller_number,
      uint16_t active_mask,
      uint32_t buttons,
      uint8_t left_trigger,
      uint8_t right_trigger,
      int16_t ls_x,
      int16_t ls_y,
      int16_t rs_x,
      int16_t rs_y
    ) {
      NV_MULTI_CONTROLLER_PACKET packet {};
      packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet) - sizeof(packet.header.size));
      packet.header.magic = util::endian::little<std::uint32_t>(MULTI_CONTROLLER_MAGIC_GEN5);
      packet.headerB = MC_HEADER_B;
      packet.controllerNumber = static_cast<int16_t>(controller_number);
      packet.activeGamepadMask = static_cast<int16_t>(active_mask);
      packet.midB = MC_MID_B;
      packet.buttonFlags = static_cast<int16_t>(buttons & 0xFFFF);
      packet.leftTrigger = left_trigger;
      packet.rightTrigger = right_trigger;
      packet.leftStickX = ls_x;
      packet.leftStickY = ls_y;
      packet.rightStickX = rs_x;
      packet.rightStickY = rs_y;
      packet.tailA = MC_TAIL_A;
      packet.buttonFlags2 = static_cast<int16_t>((buttons >> 16) & 0xFFFF);
      packet.tailB = MC_TAIL_B;

      std::vector<uint8_t> data(sizeof(packet));
      std::memcpy(data.data(), &packet, sizeof(packet));
      return data;
    }

    std::vector<uint8_t> make_gamepad_motion_packet(
      int controller_number,
      uint8_t motion_type,
      float x,
      float y,
      float z
    ) {
      SS_CONTROLLER_MOTION_PACKET packet {};
      packet.header.size = util::endian::big<std::uint32_t>(sizeof(packet) - sizeof(packet.header.size));
      packet.header.magic = util::endian::little<std::uint32_t>(SS_CONTROLLER_MOTION_MAGIC);
      packet.controllerNumber = static_cast<uint8_t>(controller_number);
      packet.motionType = motion_type;
      write_netfloat(packet.x, x);
      write_netfloat(packet.y, y);
      write_netfloat(packet.z, z);

      std::vector<uint8_t> data(sizeof(packet));
      std::memcpy(data.data(), &packet, sizeof(packet));
      return data;
    }

    uint8_t parse_gamepad_type(const nlohmann::json &message) {
      if (!message.contains("gamepadType")) {
        return LI_CTYPE_UNKNOWN;
      }
      const auto &value = message["gamepadType"];
      if (value.is_number_integer()) {
        return static_cast<uint8_t>(value.get<int>());
      }
      if (value.is_string()) {
        const auto type_str = value.get<std::string>();
        if (type_str == "xbox") {
          return LI_CTYPE_XBOX;
        }
        if (type_str == "playstation" || type_str == "ps") {
          return LI_CTYPE_PS;
        }
        if (type_str == "nintendo" || type_str == "switch") {
          return LI_CTYPE_NINTENDO;
        }
      }
      return LI_CTYPE_UNKNOWN;
    }

    uint8_t parse_motion_type(const nlohmann::json &message) {
      if (!message.contains("motionType")) {
        return 0;
      }
      const auto &value = message["motionType"];
      if (value.is_number_integer()) {
        return static_cast<uint8_t>(value.get<int>());
      }
      if (value.is_string()) {
        const auto type_str = value.get<std::string>();
        if (type_str == "gyro") {
          return LI_MOTION_TYPE_GYRO;
        }
        if (type_str == "accel") {
          return LI_MOTION_TYPE_ACCEL;
        }
      }
      return 0;
    }

    void handle_input_message(std::string_view payload) {
      if (payload.empty()) {
        return;
      }

      auto message = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
      if (message.is_discarded()) {
        return;
      }

      auto input_ctx = current_input_context();
      if (!input_ctx) {
        return;
      }

      const auto type = message.value("type", "");
      if (type == "mouse_move") {
        const double x = message.value("x", 0.0);
        const double y = message.value("y", 0.0);
        input::passthrough(input_ctx, make_abs_mouse_move_packet(x, y));
        return;
      }
      if (type == "mouse_down" || type == "mouse_up") {
        if (message.contains("x") && message.contains("y")) {
          const double x = message.value("x", 0.0);
          const double y = message.value("y", 0.0);
          input::passthrough(input_ctx, make_abs_mouse_move_packet(x, y));
        }

        int mapped_button = map_mouse_button(message.value("button", -1));
        if (mapped_button > 0) {
          input::passthrough(input_ctx, make_mouse_button_packet(mapped_button, type == "mouse_up"));
        }
        return;
      }
      if (type == "wheel") {
        const double dx = message.value("dx", 0.0);
        const double dy = message.value("dy", 0.0);
        const int vscroll = static_cast<int>(std::lround(-dy * 120.0));
        const int hscroll = static_cast<int>(std::lround(dx * 120.0));
        if (vscroll != 0) {
          input::passthrough(input_ctx, make_scroll_packet(vscroll));
        }
        if (hscroll != 0) {
          input::passthrough(input_ctx, make_hscroll_packet(hscroll));
        }
        return;
      }
      if (type == "key_down" || type == "key_up") {
        const auto code = message.value("code", "");
        const auto key = message.value("key", "");
        auto key_code = map_dom_code_to_vk(code, key);
        if (!key_code) {
          return;
        }
        uint8_t mods = 0;
        if (message.contains("modifiers") && message["modifiers"].is_object()) {
          mods = modifiers_from_json(message["modifiers"]);
        }
        input::passthrough(input_ctx, make_keyboard_packet(*key_code, type == "key_up", mods));
        return;
      }
      if (type == "gamepad_connect") {
        const int controller = message.value("id", -1);
        if (controller < 0 || controller >= 16) {
          return;
        }
        bool should_send = false;
        {
          std::lock_guard lg {gamepad_mutex};
          if (!webrtc_gamepads.test(controller)) {
            webrtc_gamepads.set(controller);
            should_send = true;
          }
        }
        if (!should_send) {
          return;
        }
        const uint8_t controller_type = parse_gamepad_type(message);
        const auto capabilities = static_cast<uint16_t>(message.value("capabilities", 0));
        const auto supported_buttons = static_cast<uint32_t>(message.value("supportedButtons", 0));
        input::passthrough(input_ctx, make_gamepad_arrival_packet(controller, controller_type, capabilities, supported_buttons));
        return;
      }
      if (type == "gamepad_state") {
        const int controller = message.value("id", -1);
        if (controller < 0 || controller >= 16) {
          return;
        }
        bool should_send_arrival = false;
        {
          std::lock_guard lg {gamepad_mutex};
          if (!webrtc_gamepads.test(controller)) {
            webrtc_gamepads.set(controller);
            should_send_arrival = true;
          }
        }
        if (should_send_arrival) {
          const uint8_t controller_type = parse_gamepad_type(message);
          const auto capabilities = static_cast<uint16_t>(message.value("capabilities", 0));
          const auto supported_buttons = static_cast<uint32_t>(message.value("supportedButtons", 0));
          input::passthrough(
            input_ctx,
            make_gamepad_arrival_packet(controller, controller_type, capabilities, supported_buttons)
          );
        }
        const auto active_mask = static_cast<uint16_t>(message.value("activeMask", 0));
        const auto buttons = static_cast<uint32_t>(message.value("buttons", 0));
        const auto clamp_u8 = [](int value) -> uint8_t {
          return static_cast<uint8_t>(std::clamp(value, 0, 255));
        };
        const auto clamp_i16 = [](int value) -> int16_t {
          return static_cast<int16_t>(std::clamp(value, -32768, 32767));
        };
        const int lt = static_cast<int>(std::lround(message.value("lt", 0.0)));
        const int rt = static_cast<int>(std::lround(message.value("rt", 0.0)));
        const int ls_x = static_cast<int>(std::lround(message.value("lsX", 0.0)));
        const int ls_y = static_cast<int>(std::lround(message.value("lsY", 0.0)));
        const int rs_x = static_cast<int>(std::lround(message.value("rsX", 0.0)));
        const int rs_y = static_cast<int>(std::lround(message.value("rsY", 0.0)));
        input::passthrough(
          input_ctx,
          make_gamepad_state_packet(
            controller,
            active_mask,
            buttons,
            clamp_u8(lt),
            clamp_u8(rt),
            clamp_i16(ls_x),
            clamp_i16(ls_y),
            clamp_i16(rs_x),
            clamp_i16(rs_y)
          )
        );
        return;
      }
      if (type == "gamepad_disconnect") {
        const int controller = message.value("id", -1);
        if (controller < 0 || controller >= 16) {
          return;
        }
        {
          std::lock_guard lg {gamepad_mutex};
          webrtc_gamepads.reset(controller);
        }
        const auto active_mask = static_cast<uint16_t>(message.value("activeMask", 0));
        input::passthrough(
          input_ctx,
          make_gamepad_state_packet(controller, active_mask, 0, 0, 0, 0, 0, 0, 0)
        );
        return;
      }
      if (type == "gamepad_motion") {
        const int controller = message.value("id", -1);
        if (controller < 0 || controller >= 16) {
          return;
        }
        const uint8_t motion_type = parse_motion_type(message);
        if (motion_type != LI_MOTION_TYPE_GYRO && motion_type != LI_MOTION_TYPE_ACCEL) {
          return;
        }
        const float x = static_cast<float>(message.value("x", 0.0));
        const float y = static_cast<float>(message.value("y", 0.0));
        const float z = static_cast<float>(message.value("z", 0.0));
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
          return;
        }
        input::passthrough(input_ctx, make_gamepad_motion_packet(controller, motion_type, x, y, z));
        return;
      }
    }

#ifdef SUNSHINE_ENABLE_WEBRTC
    struct SessionIceContext {
      std::string id;
      std::atomic<bool> active {true};
    };

    struct SessionDataChannelContext {
      std::string id;
      std::atomic<bool> active {true};
      std::atomic<bool> mouse_move_seq_initialized {false};
      std::atomic<std::uint16_t> last_mouse_move_seq {0};
      std::atomic<std::int64_t> last_mouse_move_at_ms {0};
    };

    struct SessionPeerContext {
      std::string session_id;
      lwrtc_peer_t *peer = nullptr;
      int audio_channels = 2;
    };

    struct LocalDescriptionContext {
      std::string session_id;
      lwrtc_peer_t *peer = nullptr;
      std::string sdp;
      std::string type;
    };

    lwrtc_constraints_t *create_constraints();

    template <typename T, void (*Releaser)(T *)>
    std::shared_ptr<T> make_lwrtc_ptr(T *ptr) {
      return std::shared_ptr<T>(ptr, [](T *value) {
        if (value) {
          Releaser(value);
        }
      });
    }
#endif

#ifdef _WIN32
    class D3D11Nv12Converter;
#endif

    struct Session {
      SessionState state;
      video::config_t video_config;
      ring_buffer_t<EncodedVideoFrame> video_frames {kMaxVideoFrames};
      ring_buffer_t<EncodedAudioFrame> audio_frames {kMaxAudioFrames};
      ring_buffer_t<RawVideoFrame> raw_video_frames {kMaxVideoFrames};
      ring_buffer_t<RawAudioFrame> raw_audio_frames {kMaxAudioFrames};
      std::shared_ptr<std::atomic_uint32_t> video_inflight = std::make_shared<std::atomic_uint32_t>(0);
      std::string remote_offer_sdp;
      std::string remote_offer_type;
      std::string local_answer_sdp;
      std::string local_answer_type;
#ifdef SUNSHINE_ENABLE_WEBRTC
      lwrtc_peer_t *peer = nullptr;
      std::shared_ptr<SessionIceContext> ice_context;
      std::shared_ptr<lwrtc_audio_source_t> audio_source;
      std::shared_ptr<lwrtc_video_source_t> video_source;
      std::shared_ptr<lwrtc_encoded_video_source_t> encoded_video_source;
      lwrtc_audio_track_t *audio_track = nullptr;
      lwrtc_video_track_t *video_track = nullptr;
      lwrtc_data_channel_t *input_channel = nullptr;
      std::shared_ptr<SessionDataChannelContext> data_channel_context;
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

      std::shared_ptr<platf::img_t> last_video_frame;
      std::optional<std::chrono::steady_clock::time_point> last_video_push;
      bool needs_keyframe = false;
      std::optional<std::chrono::steady_clock::time_point> last_keyframe_request;
      std::optional<std::chrono::steady_clock::time_point> last_keyframe_sent;
      std::size_t encoded_prefix_logs = 0;

      struct VideoPacingState {
        std::optional<std::chrono::steady_clock::time_point> anchor_capture;
        std::optional<std::chrono::steady_clock::time_point> anchor_send;
        std::optional<std::chrono::steady_clock::time_point> last_drift_reset;
        std::optional<std::chrono::steady_clock::time_point> recovery_prefer_latest_until;
      };
      VideoPacingState video_pacing_state;
      VideoPacingConfig video_pacing;
    };

    std::mutex session_mutex;
    std::unordered_map<std::string, Session> sessions;
    std::condition_variable local_answer_cv;
    std::atomic<std::uint64_t> webrtc_idle_shutdown_token {0};
    std::atomic_uint active_sessions {0};
#ifdef SUNSHINE_ENABLE_WEBRTC
    std::atomic_uint active_peers {0};
#endif
    std::atomic_bool rtsp_sessions_active {false};
    struct RtspCaptureConfig {
      video::config_t video;
      audio::config_t audio;
    };
    std::mutex rtsp_config_mutex;
    std::optional<RtspCaptureConfig> rtsp_capture_config;
    std::atomic_uint32_t webrtc_launch_session_id {0};
    WebRtcCaptureState webrtc_capture;

    std::shared_ptr<safe::mail_raw_t> current_capture_mail() {
      std::lock_guard<std::mutex> lock(webrtc_capture.mutex);
      return webrtc_capture.mail;
    }

    std::optional<std::string> build_gamepad_feedback_payload(const platf::gamepad_feedback_msg_t &msg) {
      nlohmann::json payload;
      payload["type"] = "gamepad_feedback";
      payload["id"] = msg.id;
      switch (msg.type) {
        case platf::gamepad_feedback_e::rumble:
          payload["event"] = "rumble";
          payload["lowfreq"] = msg.data.rumble.lowfreq;
          payload["highfreq"] = msg.data.rumble.highfreq;
          break;
        case platf::gamepad_feedback_e::rumble_triggers:
          payload["event"] = "rumble_triggers";
          payload["left"] = msg.data.rumble_triggers.left_trigger;
          payload["right"] = msg.data.rumble_triggers.right_trigger;
          break;
        case platf::gamepad_feedback_e::set_motion_event_state:
          payload["event"] = "motion_event_state";
          payload["motionType"] = msg.data.motion_event_state.motion_type;
          payload["reportRate"] = msg.data.motion_event_state.report_rate;
          break;
        default:
          return std::nullopt;
      }
      return payload.dump();
    }

    void send_gamepad_feedback_payload(const std::string &payload) {
      std::lock_guard lg {session_mutex};
      for (auto &[_, session] : sessions) {
        if (!session.input_channel) {
          continue;
        }
        if (lwrtc_data_channel_state(session.input_channel) != LWRTC_DATA_CHANNEL_OPEN) {
          continue;
        }
        lwrtc_data_channel_send(
          session.input_channel,
          reinterpret_cast<const uint8_t *>(payload.data()),
          payload.size(),
          0
        );
      }
    }

    void feedback_thread_main(safe::mail_raw_t::queue_t<platf::gamepad_feedback_msg_t> queue) {
      using namespace std::chrono_literals;
      while (!webrtc_capture.feedback_shutdown.load(std::memory_order_acquire)) {
        auto next = queue->pop(50ms);
        if (!next) {
          continue;
        }
        auto payload = build_gamepad_feedback_payload(*next);
        if (!payload) {
          continue;
        }
        send_gamepad_feedback_payload(*payload);
      }
    }

    void request_keyframe(std::string_view reason) {
      auto mail = current_capture_mail();
      if (!mail) {
        if (rtsp_sessions_active.load(std::memory_order_relaxed)) {
          stream::request_idr_for_all_sessions();
          BOOST_LOG(debug) << "WebRTC: keyframe requested via RTSP (" << reason << ')';
        } else {
          BOOST_LOG(debug) << "WebRTC: keyframe request skipped (" << reason << ") - no capture mail";
        }
        return;
      }

      auto idr_events = mail->event<bool>(mail::idr);
      if (!idr_events) {
        BOOST_LOG(debug) << "WebRTC: keyframe request skipped (" << reason << ") - no idr event";
        return;
      }

      idr_events->raise(true);
      BOOST_LOG(debug) << "WebRTC: keyframe requested (" << reason << ')';
    }

    int codec_to_video_format(const std::optional<std::string> &codec) {
      if (!codec) {
        return 0;
      }
      if (*codec == "hevc") {
        return 1;
      }
      if (*codec == "av1") {
        return 2;
      }
      return 0;
    }

    std::optional<std::string> video_format_to_codec(int video_format) {
      switch (video_format) {
        case 1:
          return "hevc";
        case 2:
          return "av1";
        case 0:
        default:
          return "h264";
      }
    }

    std::optional<RtspCaptureConfig> snapshot_rtsp_capture_config() {
      std::lock_guard<std::mutex> lock(rtsp_config_mutex);
      return rtsp_capture_config;
    }

    void clear_rtsp_capture_config() {
      std::lock_guard<std::mutex> lock(rtsp_config_mutex);
      rtsp_capture_config.reset();
    }

    void apply_rtsp_video_overrides(
      video::config_t &config,
      const std::optional<RtspCaptureConfig> &rtsp_config
    ) {
      if (!rtsp_config) {
        return;
      }
      config.framerate = rtsp_config->video.framerate;
    }

    struct Av1FmtpParams {
      std::string profile {"0"};
      std::string level_idx {"5"};
      std::string tier {"0"};
    };

    bool av1_params_equal(
      const std::optional<Av1FmtpParams> &left,
      const std::optional<Av1FmtpParams> &right
    ) {
      if (left.has_value() != right.has_value()) {
        return false;
      }
      if (!left) {
        return true;
      }
      return left->profile == right->profile &&
             left->level_idx == right->level_idx &&
             left->tier == right->tier;
    }

    std::string_view trim_ascii(std::string_view value) {
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
      }
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
      }
      return value;
    }

    struct Av1OfferInfo {
      bool offered = false;
      std::optional<Av1FmtpParams> fmtp;
    };

    Av1OfferInfo parse_av1_offer(std::string_view sdp) {
      std::unordered_map<int, Av1FmtpParams> fmtp_params;
      std::vector<int> av1_payloads;

      std::size_t line_start = 0;
      while (line_start < sdp.size()) {
        std::size_t line_end = sdp.find('\n', line_start);
        if (line_end == std::string_view::npos) {
          line_end = sdp.size();
        }
        auto line = sdp.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
          line.remove_suffix(1);
        }

        if (line.rfind("a=rtpmap:", 0) == 0) {
          auto rest = line.substr(9);
          auto space = rest.find_first_of(" \t");
          if (space != std::string_view::npos) {
            auto pt_str = trim_ascii(rest.substr(0, space));
            auto codec = trim_ascii(rest.substr(space + 1));
            auto slash = codec.find('/');
            auto codec_name = slash == std::string_view::npos ? codec : codec.substr(0, slash);
            if (boost::istarts_with(codec_name, "AV1")) {
              int pt = -1;
              auto result = std::from_chars(pt_str.data(), pt_str.data() + pt_str.size(), pt);
              if (result.ec == std::errc() && pt >= 0) {
                av1_payloads.push_back(pt);
              }
            }
          }
        } else if (line.rfind("a=fmtp:", 0) == 0) {
          auto rest = line.substr(7);
          auto space = rest.find_first_of(" \t");
          if (space != std::string_view::npos) {
            auto pt_str = trim_ascii(rest.substr(0, space));
            auto params_str = rest.substr(space + 1);
            int pt = -1;
            auto result = std::from_chars(pt_str.data(), pt_str.data() + pt_str.size(), pt);
            if (result.ec == std::errc() && pt >= 0) {
              auto &params = fmtp_params[pt];
              std::size_t param_start = 0;
              while (param_start < params_str.size()) {
                std::size_t param_end = params_str.find(';', param_start);
                if (param_end == std::string_view::npos) {
                  param_end = params_str.size();
                }
                auto token = trim_ascii(params_str.substr(param_start, param_end - param_start));
                if (!token.empty()) {
                  auto eq = token.find('=');
                  if (eq != std::string_view::npos) {
                    auto key = trim_ascii(token.substr(0, eq));
                    auto value = trim_ascii(token.substr(eq + 1));
                    std::string key_lower {key};
                    boost::algorithm::to_lower(key_lower);
                    if (key_lower == "profile") {
                      params.profile.assign(value.begin(), value.end());
                    } else if (key_lower == "level-idx") {
                      params.level_idx.assign(value.begin(), value.end());
                    } else if (key_lower == "tier") {
                      params.tier.assign(value.begin(), value.end());
                    }
                  }
                }
                if (param_end >= params_str.size()) {
                  break;
                }
                param_start = param_end + 1;
              }
            }
          }
        }

        if (line_end >= sdp.size()) {
          break;
        }
        line_start = line_end + 1;
      }

      Av1OfferInfo info;
      if (av1_payloads.empty()) {
        return info;
      }
      info.offered = true;
      const int pt = av1_payloads.front();
      auto it = fmtp_params.find(pt);
      if (it != fmtp_params.end()) {
        info.fmtp = it->second;
      }
      return info;
    }

    struct HevcOfferInfo {
      bool offered = false;
      std::optional<std::string> fmtp;
    };

    HevcOfferInfo parse_hevc_offer(std::string_view sdp) {
      std::unordered_map<int, std::string> fmtp_params;
      std::vector<int> h265_payloads;

      std::size_t line_start = 0;
      bool in_video = false;
      while (line_start < sdp.size()) {
        std::size_t line_end = sdp.find('\n', line_start);
        if (line_end == std::string_view::npos) {
          line_end = sdp.size();
        }
        auto line = sdp.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
          line.remove_suffix(1);
        }

        if (line.rfind("m=", 0) == 0) {
          in_video = line.rfind("m=video", 0) == 0;
        } else if (in_video && line.rfind("a=rtpmap:", 0) == 0) {
          auto rest = line.substr(9);
          auto space = rest.find_first_of(" \t");
          if (space != std::string_view::npos) {
            auto pt_str = trim_ascii(rest.substr(0, space));
            auto codec = trim_ascii(rest.substr(space + 1));
            auto slash = codec.find('/');
            auto codec_name = slash == std::string_view::npos ? codec : codec.substr(0, slash);
            if (boost::istarts_with(codec_name, "H265") || boost::istarts_with(codec_name, "HEVC")) {
              int pt = -1;
              auto result = std::from_chars(pt_str.data(), pt_str.data() + pt_str.size(), pt);
              if (result.ec == std::errc() && pt >= 0) {
                h265_payloads.push_back(pt);
              }
            }
          }
        } else if (in_video && line.rfind("a=fmtp:", 0) == 0) {
          auto rest = line.substr(7);
          auto space = rest.find_first_of(" \t");
          if (space != std::string_view::npos) {
            auto pt_str = trim_ascii(rest.substr(0, space));
            auto params_str = trim_ascii(rest.substr(space + 1));
            int pt = -1;
            auto result = std::from_chars(pt_str.data(), pt_str.data() + pt_str.size(), pt);
            if (result.ec == std::errc() && pt >= 0) {
              fmtp_params[pt] = std::string {params_str};
            }
          }
        }

        line_start = line_end + 1;
      }

      HevcOfferInfo info;
      if (h265_payloads.empty()) {
        return info;
      }
      info.offered = true;
      const int pt = h265_payloads.front();
      auto it = fmtp_params.find(pt);
      if (it != fmtp_params.end()) {
        info.fmtp = it->second;
      }
      return info;
    }

    /**
     * @brief Modifies SDP to configure Opus encoder for high quality audio.
     *
     * Adds parameters to match Sunshine's native Opus encoder settings:
     * - maxaveragebitrate: High bitrate for quality (512kbps for stereo)
     * - stereo/sprop-stereo: Enable stereo
     * - cbr: Constant bitrate (matches Sunshine's VBR=0)
     * - usedtx: Disable discontinuous transmission
     *
     * @param sdp The SDP string to modify
     * @param channels Number of audio channels (2 for stereo, 6 for 5.1, 8 for 7.1)
     * @return Modified SDP string
     */
    std::string apply_opus_audio_params(std::string_view sdp, int channels) {
      // Determine bitrate based on channel count (matching audio.cpp stream_configs)
      // Using HIGH_QUALITY bitrates since WebRTC config sets HIGH_QUALITY = true
      int bitrate = 512000;  // stereo high quality
      if (channels == 6) {
        bitrate = 1536000;  // 5.1 high quality
      } else if (channels == 8) {
        bitrate = 2048000;  // 7.1 high quality
      }

      const bool is_stereo = (channels == 2);
      std::string result;
      result.reserve(sdp.size() + 256);

      int opus_payload_type = -1;
      bool in_audio = false;
      bool found_opus_fmtp = false;

      std::size_t line_start = 0;
      while (line_start < sdp.size()) {
        std::size_t line_end = sdp.find('\n', line_start);
        if (line_end == std::string_view::npos) {
          line_end = sdp.size();
        }
        auto line = sdp.substr(line_start, line_end - line_start);
        std::string_view line_content = line;
        bool has_cr = !line_content.empty() && line_content.back() == '\r';
        if (has_cr) {
          line_content.remove_suffix(1);
        }

        // Track audio section
        if (line_content.rfind("m=audio", 0) == 0) {
          in_audio = true;
        } else if (line_content.rfind("m=", 0) == 0) {
          in_audio = false;
        }

        // Find Opus payload type
        if (in_audio && line_content.rfind("a=rtpmap:", 0) == 0) {
          auto rest = line_content.substr(9);
          auto space = rest.find_first_of(" \t");
          if (space != std::string_view::npos) {
            auto pt_str = trim_ascii(rest.substr(0, space));
            auto codec = trim_ascii(rest.substr(space + 1));
            if (boost::istarts_with(codec, "opus/")) {
              int pt = -1;
              auto parse_result = std::from_chars(pt_str.data(), pt_str.data() + pt_str.size(), pt);
              if (parse_result.ec == std::errc() && pt >= 0) {
                opus_payload_type = pt;
              }
            }
          }
        }

        // Modify existing Opus fmtp line or add parameters
        if (in_audio && opus_payload_type >= 0 && line_content.rfind("a=fmtp:", 0) == 0) {
          auto rest = line_content.substr(7);
          auto space = rest.find_first_of(" \t");
          if (space != std::string_view::npos) {
            auto pt_str = trim_ascii(rest.substr(0, space));
            int pt = -1;
            auto parse_result = std::from_chars(pt_str.data(), pt_str.data() + pt_str.size(), pt);
            if (parse_result.ec == std::errc() && pt == opus_payload_type) {
              found_opus_fmtp = true;
              auto existing_params = rest.substr(space + 1);

              // Build new fmtp line with our parameters
              result += "a=fmtp:";
              result += pt_str;
              result += ' ';

              // Add existing params that we don't override
              std::size_t param_start = 0;
              bool first_param = true;
              while (param_start < existing_params.size()) {
                std::size_t param_end = existing_params.find(';', param_start);
                if (param_end == std::string_view::npos) {
                  param_end = existing_params.size();
                }
                auto token = trim_ascii(existing_params.substr(param_start, param_end - param_start));
                if (!token.empty()) {
                  auto eq = token.find('=');
                  std::string_view key = eq != std::string_view::npos ? token.substr(0, eq) : token;
                  std::string key_lower {key};
                  boost::algorithm::to_lower(key_lower);
                  // Skip parameters we're going to set ourselves
                  if (key_lower != "maxaveragebitrate" && key_lower != "stereo" &&
                      key_lower != "sprop-stereo" && key_lower != "cbr" && key_lower != "usedtx") {
                    if (!first_param) result += ';';
                    result += token;
                    first_param = false;
                  }
                }
                if (param_end >= existing_params.size()) break;
                param_start = param_end + 1;
              }

              // Add our parameters
              if (!first_param) result += ';';
              result += "maxaveragebitrate=";
              result += std::to_string(bitrate);
              if (is_stereo) {
                result += ";stereo=1;sprop-stereo=1";
              }
              result += ";cbr=1;usedtx=0";

              if (has_cr) result += '\r';
              result += '\n';

              if (line_end < sdp.size()) {
                line_start = line_end + 1;
              } else {
                break;
              }
              continue;
            }
          }
        }

        // Copy line as-is
        result += line;
        result += '\n';

        if (line_end >= sdp.size()) break;
        line_start = line_end + 1;
      }

      // If we found Opus but no fmtp line, we need to add one
      // This shouldn't normally happen as browsers include fmtp for Opus
      if (opus_payload_type >= 0 && !found_opus_fmtp) {
        BOOST_LOG(debug) << "WebRTC: No Opus fmtp found, adding one";
        // Insert before the first a= line in audio section
        // For simplicity, just log a warning - browsers should always have fmtp
      }

      return result;
    }

    const char *lwrtc_codec_name(lwrtc_video_codec_t codec) {
      switch (codec) {
        case LWRTC_VIDEO_CODEC_H264:
          return "H264";
        case LWRTC_VIDEO_CODEC_H265:
          return "H265";
        case LWRTC_VIDEO_CODEC_AV1:
          return "AV1";
        default:
          return "Unknown";
      }
    }

    video::config_t build_video_config(const SessionOptions &options) {
      video::config_t config {};
      config.width = options.width.value_or(kDefaultWidth);
      config.height = options.height.value_or(kDefaultHeight);
      config.framerate = options.fps.value_or(kDefaultFps);
      int bitrate = options.bitrate_kbps.value_or(0);
      if (bitrate <= 0) {
        bitrate = config::video.max_bitrate > 0 ? config::video.max_bitrate : 20000;
      }
      if (config::video.max_bitrate > 0) {
        bitrate = std::min(bitrate, config::video.max_bitrate);
      }
      config.bitrate = bitrate;
      config.slicesPerFrame = 1;
      config.numRefFrames = 1;
      config.encoderCscMode = 0;
      config.videoFormat = codec_to_video_format(options.codec);
      config.dynamicRange = options.hdr.value_or(false) ? 1 : 0;
      config.prefer_sdr_10bit = false;
      config.chromaSamplingType = 0;
      config.enableIntraRefresh = 0;

      const bool prefer_10bit_sdr = config::video.prefer_10bit_sdr;
      if (prefer_10bit_sdr && config.dynamicRange == 0) {
        const bool hevc_main10 = config.videoFormat == 1 && video::active_hevc_mode >= 3;
        const bool av1_main10 = config.videoFormat == 2 && video::active_av1_mode >= 3;
        if (hevc_main10 || av1_main10) {
          BOOST_LOG(info) << "Preferring 10-bit SDR encode for WebRTC capture";
          config.dynamicRange = 1;
          config.prefer_sdr_10bit = true;
        }
      }

      return config;
    }

    audio::config_t build_audio_config(const SessionOptions &options) {
      audio::config_t config {};
      config.packetDuration = kDefaultAudioPacketMs;
      config.channels = options.audio_channels.value_or(kDefaultAudioChannels);
      config.mask = 0;
      config.bypass_opus = true;
      config.flags[audio::config_t::HIGH_QUALITY] = true;
      config.flags[audio::config_t::HOST_AUDIO] = options.host_audio;
      config.flags[audio::config_t::CUSTOM_SURROUND_PARAMS] = false;
      return config;
    }

    VideoPacingConfig build_video_pacing_config(const SessionOptions &options) {
      VideoPacingConfig config;
      config.mode = video_pacing_mode_from_string(options.video_pacing_mode);
      config.max_age_frames = max_age_frames_for_mode(config.mode);
      config.max_frame_age_override = false;
      const int fps = options.fps.value_or(kDefaultFps);
      const int safe_fps = fps > 0 ? fps : kDefaultFps;
      const auto frame_interval =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)) / safe_fps;
      config.max_frame_age = frame_interval * std::max(1, config.max_age_frames);
      switch (config.mode) {
        case video_pacing_mode_e::latency:
          config.pace = false;
          config.drop_old_frames = true;
          config.prefer_latest = true;
          config.slack = std::chrono::duration_cast<std::chrono::nanoseconds>(kVideoPacingSlackLatency);
          break;
        case video_pacing_mode_e::smoothness:
          config.pace = true;
          config.drop_old_frames = false;
          config.prefer_latest = false;
          config.slack = std::chrono::duration_cast<std::chrono::nanoseconds>(kVideoPacingSlackSmooth);
          break;
        case video_pacing_mode_e::balanced:
        default:
          config.pace = true;
          config.drop_old_frames = true;
          config.prefer_latest = false;
          config.slack = std::chrono::duration_cast<std::chrono::nanoseconds>(kVideoPacingSlackBalanced);
          break;
      }

      if (options.video_pacing_slack_ms) {
        const int ms = std::clamp(*options.video_pacing_slack_ms,
                                  static_cast<int>(kVideoPacingSlackMin.count()),
                                  static_cast<int>(kVideoPacingSlackMax.count()));
        config.slack = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds {ms});
      }
      if (options.video_max_frame_age_ms) {
        const int ms = std::clamp(*options.video_max_frame_age_ms,
                                  static_cast<int>(kVideoMaxFrameAgeMin.count()),
                                  static_cast<int>(kVideoMaxFrameAgeMax.count()));
        config.max_frame_age = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds {ms});
        config.max_frame_age_override = true;
        const auto interval_ns = frame_interval.count();
        if (interval_ns > 0) {
          const auto max_age_ns = config.max_frame_age.count();
          const auto frames = static_cast<int>((max_age_ns + interval_ns / 2) / interval_ns);
          config.max_age_frames = std::max(1, frames);
        }
      }
      return config;
    }

    std::shared_ptr<rtsp_stream::launch_session_t> build_launch_session(
      const SessionOptions &options,
      int app_id,
      int audio_channels
    ) {
      auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
      launch_session->id = ++webrtc_launch_session_id;
      launch_session->device_name = "WebRTC";
      const auto requested_uuid = options.client_uuid.value_or(std::string {});
      if (!requested_uuid.empty()) {
        launch_session->unique_id = requested_uuid;
        launch_session->client_uuid = requested_uuid;
      } else {
        launch_session->unique_id = uuid_util::uuid_t::generate().string();
        launch_session->client_uuid = launch_session->unique_id;
      }
      const auto requested_name = options.client_name.value_or(std::string {});
      launch_session->client_name = requested_name.empty() ? launch_session->device_name : requested_name;
      launch_session->width = options.width.value_or(kDefaultWidth);
      launch_session->height = options.height.value_or(kDefaultHeight);
      launch_session->fps = options.fps.value_or(kDefaultFps);
      launch_session->appid = app_id;
      launch_session->host_audio = options.host_audio;
      launch_session->surround_info = audio_channels;
      launch_session->surround_params.clear();
      launch_session->gcmap = 0;
      launch_session->enable_sops = false;
      launch_session->enable_hdr = options.hdr.value_or(false);

#ifdef _WIN32
      {
        using override_e = config::video_t::dd_t::hdr_request_override_e;
        switch (config::video.dd.hdr_request_override) {
          case override_e::force_on:
            launch_session->enable_hdr = true;
            break;
          case override_e::force_off:
            launch_session->enable_hdr = false;
            break;
          case override_e::automatic:
            break;
        }
      }
#endif

      launch_session->virtual_display = false;
      launch_session->virtual_display_failed = false;
      launch_session->virtual_display_guid_bytes.fill(0);
      launch_session->virtual_display_device_id.clear();
      launch_session->virtual_display_ready_since.reset();
      launch_session->framegen_refresh_rate.reset();
      launch_session->lossless_scaling_target_fps.reset();
      launch_session->lossless_scaling_rtss_limit.reset();
      launch_session->frame_generation_provider = "lossless-scaling";

      if (launch_session->appid > 0) {
        try {
          auto apps_snapshot = proc::proc.get_apps();
          const std::string app_id_str = std::to_string(launch_session->appid);
          for (const auto &app_ctx : apps_snapshot) {
            if (app_ctx.id == app_id_str) {
              launch_session->gen1_framegen_fix = app_ctx.gen1_framegen_fix;
              launch_session->gen2_framegen_fix = app_ctx.gen2_framegen_fix;
              launch_session->lossless_scaling_framegen = app_ctx.lossless_scaling_framegen;
              launch_session->lossless_scaling_target_fps = app_ctx.lossless_scaling_target_fps;
              launch_session->lossless_scaling_rtss_limit = app_ctx.lossless_scaling_rtss_limit;
              launch_session->frame_generation_provider = app_ctx.frame_generation_provider;
              rtsp_stream::launch_session_t::app_metadata_t metadata;
              metadata.id = app_ctx.id;
              metadata.name = app_ctx.name;
              metadata.virtual_screen = app_ctx.virtual_screen;
              metadata.has_command = !app_ctx.cmd.empty();
              metadata.has_playnite = !app_ctx.playnite_id.empty();
              metadata.playnite_fullscreen = app_ctx.playnite_fullscreen;
              launch_session->virtual_display = app_ctx.virtual_screen;
              if (!launch_session->virtual_display_mode_override && app_ctx.virtual_display_mode_override) {
                launch_session->virtual_display_mode_override = app_ctx.virtual_display_mode_override;
              }
              if (!launch_session->virtual_display_layout_override && app_ctx.virtual_display_layout_override) {
                launch_session->virtual_display_layout_override = app_ctx.virtual_display_layout_override;
              }
              if (!launch_session->dd_config_option_override && app_ctx.dd_config_option_override) {
                launch_session->dd_config_option_override = app_ctx.dd_config_option_override;
              }
              if (!launch_session->output_name_override || launch_session->output_name_override->empty()) {
                if (!app_ctx.output.empty()) {
                  launch_session->output_name_override = app_ctx.output;
                }
              }
              launch_session->app_metadata = std::move(metadata);
              break;
            }
          }
        } catch (...) {
        }
      }

      const auto apply_refresh_override = [&](int candidate) {
        if (candidate <= 0) {
          return;
        }
        if (!launch_session->framegen_refresh_rate || candidate > *launch_session->framegen_refresh_rate) {
          launch_session->framegen_refresh_rate = candidate;
        }
      };

      if (launch_session->fps > 0) {
        const auto saturating_double = [](int value) -> int {
          if (value > std::numeric_limits<int>::max() / 2) {
            return std::numeric_limits<int>::max();
          }
          return value * 2;
        };

        if (launch_session->gen1_framegen_fix || launch_session->gen2_framegen_fix) {
          apply_refresh_override(saturating_double(launch_session->fps));
        }
      }

      auto key = crypto::rand(16);
      launch_session->gcm_key.assign(key.begin(), key.end());
      launch_session->iv.resize(16);
      auto iv = crypto::rand(16);
      std::copy(iv.begin(), iv.end(), launch_session->iv.begin());

      std::array<std::uint8_t, 8> ping_payload {};
      RAND_bytes(ping_payload.data(), static_cast<int>(ping_payload.size()));
      launch_session->av_ping_payload = util::hex_vec(ping_payload);
      RAND_bytes(
        reinterpret_cast<unsigned char *>(&launch_session->control_connect_data),
        sizeof(launch_session->control_connect_data)
      );

      return launch_session;
    }

    WebRtcCaptureConfigKey build_capture_config_key(
      int app_id,
      const video::config_t &video_config,
      const SessionOptions &options
    ) {
      WebRtcCaptureConfigKey key;
      key.app_id = app_id;
      key.width = video_config.width;
      key.height = video_config.height;
      key.framerate = video_config.framerate;
      key.bitrate = video_config.bitrate;
      key.video_format = video_config.videoFormat;
      key.dynamic_range = video_config.dynamicRange;
      key.chroma_sampling_type = video_config.chromaSamplingType;
      key.prefer_sdr_10bit = video_config.prefer_sdr_10bit;
      key.audio_channels = options.audio_channels.value_or(kDefaultAudioChannels);
      key.host_audio = options.host_audio;
      return key;
    }

    void stop_webrtc_capture_locked(bool allow_platform_teardown) {
      if (webrtc_capture.mail) {
        auto shutdown_event = webrtc_capture.mail->event<bool>(mail::shutdown);
        shutdown_event->raise(true);
      }
      webrtc_capture.feedback_shutdown.store(true, std::memory_order_release);
      if (webrtc_capture.feedback_queue) {
        webrtc_capture.feedback_queue->stop();
      }
      if (webrtc_capture.feedback_thread.joinable()) {
        webrtc_capture.feedback_thread.join();
      }
      if (webrtc_capture.video_thread.joinable()) {
        webrtc_capture.video_thread.join();
      }
      if (webrtc_capture.audio_thread.joinable()) {
        webrtc_capture.audio_thread.join();
      }
      webrtc_capture.feedback_queue.reset();
      webrtc_capture.mail.reset();
      webrtc_capture.app_id.reset();
      webrtc_capture.config_key.reset();
      webrtc_capture.idle_shutdown_pending.store(false, std::memory_order_release);
      webrtc_capture.active.store(false, std::memory_order_release);

#ifdef _WIN32
      if (allow_platform_teardown) {
        if (config::video.dd.config_revert_on_disconnect) {
          const bool reverted = display_helper_integration::revert();
          if (reverted) {
            display_helper_integration::stop_watchdog();
          }
        }
        VDISPLAY::restorePhysicalHdrProfiles();
        platf::frame_limiter_streaming_stop();
      }
#endif
      if (allow_platform_teardown) {
        platf::streaming_will_stop();
      }

      if (allow_platform_teardown) {
        config::set_runtime_output_name_override(std::nullopt);
        config::maybe_apply_deferred();
      }
    }

    std::optional<std::string> start_webrtc_capture(const SessionOptions &options) {
      webrtc_idle_shutdown_token.fetch_add(1, std::memory_order_acq_rel);
      std::lock_guard<std::mutex> lock(webrtc_capture.mutex);
      const bool rtsp_active = rtsp_sessions_active.load(std::memory_order_relaxed);
      const auto rtsp_config = rtsp_active ? snapshot_rtsp_capture_config() : std::nullopt;
      const bool was_idle_shutdown_pending =
        webrtc_capture.idle_shutdown_pending.exchange(false, std::memory_order_acq_rel);
      if (webrtc_capture.active.load(std::memory_order_acquire) && !was_idle_shutdown_pending) {
        return std::nullopt;
      }

      const int current_app_id = proc::proc.running();
      const int requested_app_id = options.app_id.value_or(0);
      const bool resume_only = options.resume.value_or(false);

      if (resume_only) {
        if (current_app_id == 0) {
          return std::string {"No running app to resume"};
        }
        if (requested_app_id > 0 && requested_app_id != current_app_id) {
          return std::string {"Requested app is not running"};
        }
      }

      if (requested_app_id <= 0 && current_app_id == 0) {
        return std::string {"No running app to resume"};
      }

      if (rtsp_active && requested_app_id > 0 && requested_app_id != current_app_id) {
        return std::string {"RTSP session already active"};
      }

      const int effective_app_id = requested_app_id > 0 ? requested_app_id : current_app_id;
      const int audio_channels = options.audio_channels.value_or(kDefaultAudioChannels);
      auto video_config = build_video_config(options);
      auto audio_config = build_audio_config(options);
      apply_rtsp_video_overrides(video_config, rtsp_config);
      const auto desired_key = build_capture_config_key(effective_app_id, video_config, options);

      if (webrtc_capture.active.load(std::memory_order_acquire) && webrtc_capture.config_key && *webrtc_capture.config_key == desired_key) {
        return std::nullopt;
      }

      if (webrtc_capture.active.load(std::memory_order_acquire)) {
        stop_webrtc_capture_locked(!rtsp_active);
      }

      auto launch_session = build_launch_session(options, effective_app_id, audio_channels);

      const bool allow_display_changes = !rtsp_active && !resume_only;
      if (allow_display_changes && launch_session->output_name_override && !launch_session->output_name_override->empty()) {
#ifdef _WIN32
        if (!boost::iequals(*launch_session->output_name_override, VDISPLAY::SUDOVDA_VIRTUAL_DISPLAY_SELECTION)) {
          config::set_runtime_output_name_override(*launch_session->output_name_override);
        }
#else
        config::set_runtime_output_name_override(*launch_session->output_name_override);
#endif
      }

      if (!rtsp_active && requested_app_id > 0 && requested_app_id != current_app_id) {
        auto result = proc::proc.execute(requested_app_id, launch_session);
        if (result != 0) {
          return std::string {"Failed to launch application (code "} + std::to_string(result) + ")";
        }
      }

      if (!rtsp_active) {
        // Ensure the latest config is applied before starting capture.
        config::maybe_apply_deferred();
        auto _hot_apply_gate = config::acquire_apply_read_gate();

#ifdef _WIN32
        if (allow_display_changes) {
          prepare_virtual_display_for_webrtc_session(launch_session);
          if (launch_session->output_name_override && !launch_session->output_name_override->empty()) {
            config::set_runtime_output_name_override(*launch_session->output_name_override);
          }
          (void) display_helper_integration::disarm_pending_restore();
          auto request = display_helper_integration::helpers::build_request_from_session(config::video, *launch_session);
          if (!request) {
            BOOST_LOG(warning) << "Display helper: failed to build display configuration request; continuing with existing display.";
          } else if (!display_helper_integration::apply(*request)) {
            BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
          }
        }
#endif

#ifdef _WIN32
        if (allow_display_changes) {
          const auto verification_status =
            display_helper_integration::wait_for_apply_verification(std::chrono::milliseconds(6000));
          if (verification_status == display_helper_integration::ApplyVerificationStatus::Failed) {
            return std::string {"Display helper validation failed; refusing to start capture."};
          }
        }
#endif

        if (video::probe_encoders()) {
          return std::string {"Failed to initialize video capture/encoding. Is a display connected and turned on?"};
        }
      }

#ifdef _WIN32
      if (!rtsp_active) {
        std::optional<int> lossless_rtss_limit;
        const bool using_lossless_provider = launch_session->lossless_scaling_framegen &&
                                             boost::iequals(launch_session->frame_generation_provider, "lossless-scaling");
        const bool using_smooth_motion =
          boost::iequals(launch_session->frame_generation_provider, "nvidia-smooth-motion");
        if (using_lossless_provider) {
          if (launch_session->lossless_scaling_rtss_limit && *launch_session->lossless_scaling_rtss_limit > 0) {
            lossless_rtss_limit = launch_session->lossless_scaling_rtss_limit;
          } else if (launch_session->lossless_scaling_target_fps && *launch_session->lossless_scaling_target_fps > 0) {
            int computed = (int) std::lround(*launch_session->lossless_scaling_target_fps * 0.5);
            if (computed > 0) {
              lossless_rtss_limit = computed;
            }
          }
        }
        platf::frame_limiter_streaming_start(
          launch_session->fps,
          launch_session->gen1_framegen_fix,
          launch_session->gen2_framegen_fix,
          lossless_rtss_limit,
          using_smooth_motion
        );
      }
#endif

      if (!rtsp_active) {
        platf::streaming_will_start();
      }

      auto mail = std::make_shared<safe::mail_raw_t>();
      webrtc_capture.mail = mail;
      webrtc_capture.app_id = effective_app_id > 0 ? std::optional<int> {effective_app_id} : std::nullopt;
      webrtc_capture.config_key = desired_key;
      webrtc_capture.feedback_shutdown.store(false, std::memory_order_release);
      webrtc_capture.feedback_queue = mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);
      webrtc_capture.feedback_thread = std::thread([queue = webrtc_capture.feedback_queue]() {
        feedback_thread_main(queue);
      });
      webrtc_capture.active.store(true, std::memory_order_release);

      webrtc_capture.video_thread = std::thread([mail, video_config]() mutable {
        video::capture(mail, video_config, nullptr);
      });
      webrtc_capture.audio_thread = std::thread([mail, audio_config]() mutable {
        audio::capture(mail, audio_config, nullptr);
      });
      return std::nullopt;
    }

    void stop_webrtc_capture_if_idle() {
      std::lock_guard<std::mutex> lock(webrtc_capture.mutex);
      if (!webrtc_capture.active.load(std::memory_order_acquire)) {
        return;
      }
      if (rtsp_sessions_active.load(std::memory_order_relaxed)) {
        return;
      }
      stop_webrtc_capture_locked(true);
    }

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

    constexpr std::uint8_t kInputBinaryMouseMove = 1;
    constexpr std::size_t kInputBinaryMouseMoveSize = 1 + 2 + 2 + 2;
    constexpr auto kMouseMoveSeqResetIdle = std::chrono::milliseconds {1000};

    bool seq_newer_u16(std::uint16_t seq, std::uint16_t last) {
      return static_cast<std::int16_t>(seq - last) > 0;
    }

    double unit_from_u16(std::uint16_t value) {
      return static_cast<double>(value) / 65535.0;
    }

    void handle_input_message_binary(SessionDataChannelContext *ctx, const std::uint8_t *buffer, std::size_t length) {
      if (!ctx || !ctx->active.load(std::memory_order_acquire)) {
        return;
      }
      if (!buffer || length < 1) {
        return;
      }

      auto input_ctx = current_input_context();
      if (!input_ctx) {
        return;
      }

      const auto type = buffer[0];
      if (type != kInputBinaryMouseMove || length < kInputBinaryMouseMoveSize) {
        return;
      }

      const std::uint16_t seq = static_cast<std::uint16_t>(buffer[1]) |
                                (static_cast<std::uint16_t>(buffer[2]) << 8);
      const std::uint16_t x_u16 = static_cast<std::uint16_t>(buffer[3]) |
                                  (static_cast<std::uint16_t>(buffer[4]) << 8);
      const std::uint16_t y_u16 = static_cast<std::uint16_t>(buffer[5]) |
                                  (static_cast<std::uint16_t>(buffer[6]) << 8);
      const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
      ).count();

      const bool initialized = ctx->mouse_move_seq_initialized.load(std::memory_order_acquire);
      if (initialized) {
        const std::uint16_t last = ctx->last_mouse_move_seq.load(std::memory_order_acquire);
        if (!seq_newer_u16(seq, last)) {
          const auto last_ms = ctx->last_mouse_move_at_ms.load(std::memory_order_acquire);
          if (last_ms > 0) {
            const auto elapsed_ms = now_ms - last_ms;
            if (elapsed_ms >= 0 && elapsed_ms < kMouseMoveSeqResetIdle.count()) {
              return;
            }
          }
          ctx->mouse_move_seq_initialized.store(true, std::memory_order_release);
        }
      } else {
        ctx->mouse_move_seq_initialized.store(true, std::memory_order_release);
      }
      ctx->last_mouse_move_seq.store(seq, std::memory_order_release);
      ctx->last_mouse_move_at_ms.store(now_ms, std::memory_order_release);

      input::passthrough(input_ctx, make_abs_mouse_move_packet(unit_from_u16(x_u16), unit_from_u16(y_u16)));
    }

    void on_data_channel_message(void *user, const char *buffer, int length, int binary) {
      if (!buffer || length <= 0) {
        return;
      }
      if (binary) {
        handle_input_message_binary(
          static_cast<SessionDataChannelContext *>(user),
          reinterpret_cast<const std::uint8_t *>(buffer),
          static_cast<std::size_t>(length)
        );
        return;
      }
      handle_input_message(std::string_view {buffer, static_cast<std::size_t>(length)});
    }

    void on_data_channel(void *user, lwrtc_data_channel_t *channel) {
      auto *ctx = static_cast<SessionDataChannelContext *>(user);
      if (!ctx || !ctx->active.load(std::memory_order_acquire) || !channel) {
        if (channel) {
          lwrtc_data_channel_release(channel);
        }
        return;
      }

      const char *label = lwrtc_data_channel_label(channel);
      BOOST_LOG(debug) << "WebRTC: data channel opened label=" << (label ? label : "(null)");
      if (!label || std::string_view {label} != "input") {
        lwrtc_data_channel_release(channel);
        return;
      }

      {
        std::lock_guard lg {session_mutex};
        auto it = sessions.find(ctx->id);
        if (it == sessions.end()) {
          lwrtc_data_channel_release(channel);
          return;
        }

        if (it->second.input_channel) {
          lwrtc_data_channel_unregister_observer(it->second.input_channel);
          lwrtc_data_channel_release(it->second.input_channel);
        }
        it->second.input_channel = channel;
        if (it->second.data_channel_context) {
          it->second.data_channel_context->mouse_move_seq_initialized.store(false, std::memory_order_release);
          it->second.data_channel_context->last_mouse_move_seq.store(0, std::memory_order_release);
          it->second.data_channel_context->last_mouse_move_at_ms.store(0, std::memory_order_release);
        }
        lwrtc_data_channel_register_observer(channel, nullptr, &on_data_channel_message, ctx);
      }
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

      // Apply Opus audio parameters to match Sunshine's native encoder quality
      if (!sdp_copy.empty() && ctx->audio_channels > 0) {
        sdp_copy = apply_opus_audio_params(sdp_copy, ctx->audio_channels);
        BOOST_LOG(debug) << "WebRTC: applied Opus audio params for " << ctx->audio_channels << " channels";
      }

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
    std::optional<lwrtc_video_codec_t> webrtc_factory_codec;
    std::optional<Av1FmtpParams> webrtc_factory_av1_params;
    std::optional<Av1FmtpParams> webrtc_desired_av1_params;
    std::optional<std::string> webrtc_factory_hevc_fmtp;
    std::optional<std::string> webrtc_desired_hevc_fmtp;
    std::mutex webrtc_media_mutex;
    std::condition_variable webrtc_media_cv;
    std::thread webrtc_media_thread;
    std::atomic<bool> webrtc_media_running {false};
    std::atomic<bool> webrtc_media_shutdown {false};
    std::atomic<bool> webrtc_media_has_work {false};

    std::optional<lwrtc_video_codec_t> session_codec_to_lwrtc(const SessionState &state) {
      if (!state.codec) {
        return std::nullopt;
      }
      if (boost::iequals(*state.codec, "hevc")) {
        return LWRTC_VIDEO_CODEC_H265;
      }
      if (boost::iequals(*state.codec, "av1")) {
        return LWRTC_VIDEO_CODEC_AV1;
      }
      if (boost::iequals(*state.codec, "h264")) {
        return LWRTC_VIDEO_CODEC_H264;
      }
      return std::nullopt;
    }

    lwrtc_video_codec_t session_codec_to_encoded(const SessionState &state) {
      if (state.codec) {
        if (boost::iequals(*state.codec, "hevc")) {
          return LWRTC_VIDEO_CODEC_H265;
        }
        if (boost::iequals(*state.codec, "av1")) {
          return LWRTC_VIDEO_CODEC_AV1;
        }
      }
      return LWRTC_VIDEO_CODEC_H264;
    }

    bool ensure_webrtc_factory_locked(std::optional<lwrtc_video_codec_t> preferred_codec = std::nullopt) {
      const auto desired_av1_params = webrtc_desired_av1_params;
      const auto desired_hevc_fmtp = webrtc_desired_hevc_fmtp;
      if (webrtc_factory) {
        if (preferred_codec && webrtc_factory_codec && *preferred_codec != *webrtc_factory_codec) {
          if (active_peers.load(std::memory_order_acquire) == 0) {
            BOOST_LOG(info) << "WebRTC: resetting factory to switch codec";
            lwrtc_factory_release(webrtc_factory);
            webrtc_factory = nullptr;
            webrtc_factory_codec.reset();
            webrtc_factory_av1_params.reset();
            webrtc_factory_hevc_fmtp.reset();
          } else {
            BOOST_LOG(warning) << "WebRTC: codec switch requested while peers are active";
          }
        }
        if (webrtc_factory && preferred_codec &&
            *preferred_codec == LWRTC_VIDEO_CODEC_AV1 &&
            webrtc_factory_codec &&
            *webrtc_factory_codec == LWRTC_VIDEO_CODEC_AV1 &&
            !av1_params_equal(desired_av1_params, webrtc_factory_av1_params)) {
          if (active_peers.load(std::memory_order_acquire) == 0) {
            BOOST_LOG(info) << "WebRTC: resetting factory to update AV1 parameters";
            lwrtc_factory_release(webrtc_factory);
            webrtc_factory = nullptr;
            webrtc_factory_codec.reset();
            webrtc_factory_av1_params.reset();
            webrtc_factory_hevc_fmtp.reset();
          } else {
            BOOST_LOG(warning) << "WebRTC: AV1 parameter update requested while peers are active";
          }
        }
        if (webrtc_factory && preferred_codec &&
            *preferred_codec == LWRTC_VIDEO_CODEC_H265 &&
            webrtc_factory_codec &&
            *webrtc_factory_codec == LWRTC_VIDEO_CODEC_H265 &&
            desired_hevc_fmtp != webrtc_factory_hevc_fmtp) {
          if (active_peers.load(std::memory_order_acquire) == 0) {
            BOOST_LOG(info) << "WebRTC: resetting factory to update HEVC parameters";
            lwrtc_factory_release(webrtc_factory);
            webrtc_factory = nullptr;
            webrtc_factory_codec.reset();
            webrtc_factory_av1_params.reset();
            webrtc_factory_hevc_fmtp.reset();
          } else {
            BOOST_LOG(warning) << "WebRTC: HEVC parameter update requested while peers are active";
          }
        }
        if (webrtc_factory) {
          return true;
        }
      }

      const auto passthrough_codec = preferred_codec.value_or(LWRTC_VIDEO_CODEC_H264);
      BOOST_LOG(debug) << "WebRTC: initializing peer connection factory with passthrough encoder (codec="
                       << lwrtc_codec_name(passthrough_codec) << ")";
      webrtc_factory = lwrtc_factory_create();
      if (!webrtc_factory) {
        BOOST_LOG(error) << "WebRTC: failed to allocate peer connection factory";
        return false;
      }

      // Enable passthrough mode to use pre-encoded H.264/HEVC frames
      // This must be called BEFORE lwrtc_factory_initialize()
      if (!lwrtc_factory_enable_passthrough(webrtc_factory, passthrough_codec)) {
        BOOST_LOG(warning) << "WebRTC: failed to enable passthrough mode";
        // Continue anyway - will fall back to raw frame mode
      }

      if (passthrough_codec == LWRTC_VIDEO_CODEC_AV1 && desired_av1_params) {
        if (!lwrtc_factory_set_passthrough_av1_params(
              webrtc_factory,
              desired_av1_params->profile.c_str(),
              desired_av1_params->level_idx.c_str(),
              desired_av1_params->tier.c_str())) {
          BOOST_LOG(warning) << "WebRTC: failed to set AV1 fmtp parameters";
        } else {
          BOOST_LOG(debug) << "WebRTC: using AV1 fmtp parameters profile=" << desired_av1_params->profile
                           << " level-idx=" << desired_av1_params->level_idx
                           << " tier=" << desired_av1_params->tier;
        }
      }
      if (passthrough_codec == LWRTC_VIDEO_CODEC_H265 && desired_hevc_fmtp) {
        if (!lwrtc_factory_set_passthrough_hevc_fmtp(webrtc_factory, desired_hevc_fmtp->c_str())) {
          BOOST_LOG(warning) << "WebRTC: failed to set HEVC fmtp parameters";
        } else {
          BOOST_LOG(debug) << "WebRTC: using HEVC fmtp parameters " << *desired_hevc_fmtp;
        }
      }

      if (!lwrtc_factory_initialize(webrtc_factory)) {
        BOOST_LOG(error) << "WebRTC: failed to create peer connection factory";
        lwrtc_factory_release(webrtc_factory);
        webrtc_factory = nullptr;
        return false;
      }

      webrtc_factory_codec = passthrough_codec;
      webrtc_factory_av1_params =
        (passthrough_codec == LWRTC_VIDEO_CODEC_AV1) ? desired_av1_params : std::nullopt;
      webrtc_factory_hevc_fmtp =
        (passthrough_codec == LWRTC_VIDEO_CODEC_H265) ? desired_hevc_fmtp : std::nullopt;
      BOOST_LOG(debug) << "WebRTC: peer connection factory ready";
      return true;
    }

    bool ensure_webrtc_factory(std::optional<lwrtc_video_codec_t> preferred_codec = std::nullopt) {
      std::lock_guard lg {webrtc_mutex};
      return ensure_webrtc_factory_locked(preferred_codec);
    }

    lwrtc_constraints_t *create_constraints() {
      auto *constraints = lwrtc_constraints_create();
      if (!constraints) {
        return nullptr;
      }
      auto add_constraint = [&](const char *key, const char *value) {
        if (!lwrtc_constraints_add_mandatory(constraints, key, value)) {
          BOOST_LOG(warning) << "WebRTC: failed to add mandatory constraint " << key;
        }
      };

      add_constraint("googEchoCancellation", "false");
      add_constraint("googEchoCancellation2", "false");
      add_constraint("googDAEchoCancellation", "false");
      add_constraint("googAutoGainControl", "false");
      add_constraint("googNoiseSuppression", "false");
      add_constraint("googHighpassFilter", "false");
      add_constraint("googAudioMirroring", "false");
      add_constraint("googCpuOveruseDetection", "false");
      add_constraint("googSuspendBelowMinBitrate", "false");
      return constraints;
    }

    lwrtc_peer_t *create_peer_connection(
      const SessionState &state,
      SessionIceContext *ice_context
    ) {
      BOOST_LOG(debug) << "WebRTC: create_peer_connection enter";
      if (!ensure_webrtc_factory(session_codec_to_lwrtc(state))) {
        return nullptr;
      }

      lwrtc_config_t config {};
      config.offer_to_receive_audio = state.audio ? 1 : 0;
      config.offer_to_receive_video = state.video ? 1 : 0;

      BOOST_LOG(debug) << "WebRTC: create_peer_connection constraints";
      auto *constraints = create_constraints();
      if (!constraints) {
        BOOST_LOG(error) << "WebRTC: failed to create media constraints";
        return nullptr;
      }

      BOOST_LOG(debug) << "WebRTC: create_peer_connection lwrtc_factory_create_peer";
      auto *peer = lwrtc_factory_create_peer(
        webrtc_factory,
        &config,
        constraints,
        &on_ice_candidate,
        ice_context
      );
      lwrtc_constraints_release(constraints);
      if (!peer) {
        BOOST_LOG(error) << "WebRTC: create_peer_connection failed to create peer";
        return nullptr;
      }
      BOOST_LOG(debug) << "WebRTC: create_peer_connection exit";
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
        const video::sunshine_colorspace_t &colorspace,
        ID3D11Texture2D **out_texture,
        IDXGIKeyedMutex **out_mutex
      ) {
        if (!input_texture || width <= 0 || height <= 0 || !out_texture || !out_mutex) {
          BOOST_LOG(debug) << "WebRTC: Convert called with invalid params";
          return false;
        }

        const bool hdr_output = video::colorspace_is_hdr(colorspace);
        if (!ensure_device(input_texture)) {
          BOOST_LOG(error) << "WebRTC: ensure_device failed";
          return false;
        }
        if (!ensure_shaders()) {
          BOOST_LOG(error) << "WebRTC: ensure_shaders failed";
          return false;
        }
        if (!ensure_output(width, height)) {
          BOOST_LOG(error) << "WebRTC: ensure_output failed";
          return false;
        }
        if (!ensure_constant_buffers(width, height, colorspace)) {
          BOOST_LOG(error) << "WebRTC: ensure_constant_buffers failed";
          return false;
        }

        // Acquire input mutex BEFORE creating SRV to ensure texture content is stable
        if (input_mutex) {
          const HRESULT hr = input_mutex->AcquireSync(0, 3000);
          if (hr != S_OK && hr != WAIT_ABANDONED) {
            BOOST_LOG(warning) << "WebRTC: failed to acquire input mutex: 0x" << std::hex << hr;
            return false;
          }
        }
        auto release_input_mutex = util::fail_guard([&]() {
          if (input_mutex) {
            input_mutex->ReleaseSync(0);
          }
        });

        // Create SRV for the input texture (after acquiring mutex)
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> input_srv;
        D3D11_TEXTURE2D_DESC input_desc {};
        input_texture->GetDesc(&input_desc);

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc {};
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        srv_desc.Texture2D.MostDetailedMip = 0;
        // Map typeless formats to their typed equivalents for SRV
        switch (input_desc.Format) {
          case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
          case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
          case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            srv_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
            break;
          case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            srv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            break;
          default:
            srv_desc.Format = input_desc.Format;
            break;
        }

        HRESULT srv_hr = device_->CreateShaderResourceView(input_texture, &srv_desc, &input_srv);
        if (FAILED(srv_hr)) {
          BOOST_LOG(error) << "WebRTC: failed to create input SRV for NV12 conversion, hr=0x" << std::hex << srv_hr
                           << ", format=" << input_desc.Format;
          return false;
        }

        const HRESULT out_lock = output_mutex_->AcquireSync(0, 100);
        if (out_lock != S_OK && out_lock != WAIT_ABANDONED) {
          BOOST_LOG(warning) << "WebRTC: failed to acquire output mutex: 0x" << std::hex << out_lock;
          return false;
        }

        // Set up pipeline state
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ID3D11BlendState *blend = blend_disable_.Get();
        context_->OMSetBlendState(blend, nullptr, 0xFFFFFFFFu);

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
        context_->PSSetShader(select_ps_y(input_texture, hdr_output), nullptr, 0);
        context_->Draw(3, 0);

        // UV plane
        ID3D11Buffer *subsample_cb = subsample_cb_.Get();
        context_->VSSetConstantBuffers(0, 1, &subsample_cb);
        ID3D11RenderTargetView *rtv_uv = rtv_uv_.Get();
        context_->OMSetRenderTargets(1, &rtv_uv, nullptr);
        context_->RSSetViewports(1, &viewport_uv_);
        context_->VSSetShader(vs_uv_.Get(), nullptr, 0);
        context_->PSSetShader(select_ps_uv(input_texture, hdr_output), nullptr, 0);
        context_->Draw(3, 0);

        // Unbind resources
        ID3D11ShaderResourceView *empty_srv = nullptr;
        context_->PSSetShaderResources(0, 1, &empty_srv);
        ID3D11RenderTargetView *empty_rtv = nullptr;
        context_->OMSetRenderTargets(1, &empty_rtv, nullptr);

        // Flush to ensure commands are submitted before releasing mutex
        context_->Flush();

        output_mutex_->ReleaseSync(0);

        *out_texture = output_texture_.Get();
        *out_mutex = output_mutex_.Get();
        return true;
      }

      bool ReadbackNv12(
        ID3D11Texture2D *texture,
        IDXGIKeyedMutex *texture_mutex,
        int width,
        int height,
        const std::function<bool(const uint8_t *y, int stride_y, const uint8_t *uv, int stride_uv)> &push_cb
      ) {
        if (!texture || !push_cb || width <= 0 || height <= 0) {
          BOOST_LOG(debug) << "WebRTC: ReadbackNv12 invalid params";
          return false;
        }
        if (!ensure_staging(width, height)) {
          BOOST_LOG(error) << "WebRTC: ReadbackNv12 ensure_staging failed";
          return false;
        }

        // Acquire mutex to ensure GPU rendering has completed before copy
        if (texture_mutex) {
          const HRESULT hr = texture_mutex->AcquireSync(0, 1000);
          if (hr != S_OK && hr != WAIT_ABANDONED) {
            BOOST_LOG(warning) << "WebRTC: ReadbackNv12 failed to acquire mutex: 0x" << std::hex << hr;
            return false;
          }
        }
        auto release_mutex_guard = util::fail_guard([&]() {
          if (texture_mutex) {
            texture_mutex->ReleaseSync(0);
          }
        });

        context_->CopyResource(staging_texture_.Get(), texture);

        D3D11_MAPPED_SUBRESOURCE mapped {};
        const HRESULT hr = context_->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
          BOOST_LOG(error) << "WebRTC: ReadbackNv12 Map failed hr=0x" << std::hex << hr;
          return false;
        }
        auto unmap_guard = util::fail_guard([&]() {
          context_->Unmap(staging_texture_.Get(), 0);
        });

        const auto *y_plane = static_cast<const uint8_t *>(mapped.pData);
        const auto *uv_plane = y_plane + (mapped.RowPitch * height);

        return push_cb(y_plane, static_cast<int>(mapped.RowPitch), uv_plane, static_cast<int>(mapped.RowPitch));
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
        ps_y_pq_.Reset();
        ps_uv_pq_.Reset();
        sampler_.Reset();
        blend_disable_.Reset();
        color_matrix_cb_.Reset();
        rotation_cb_.Reset();
        subsample_cb_.Reset();
        output_texture_.Reset();
        rtv_y_.Reset();
        rtv_uv_.Reset();
        output_mutex_.Reset();
        colorspace_valid_ = false;
        return true;
      }

      bool ensure_shaders() {
        if (vs_y_ && ps_y_ && ps_y_linear_ && ps_y_pq_ && vs_uv_ && ps_uv_ && ps_uv_linear_ && ps_uv_pq_) {
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
        const std::string ps_y_pq_path = std::string {SUNSHINE_SHADERS_DIR} + "/convert_yuv420_planar_y_ps_perceptual_quantizer.hlsl";
        const std::string ps_uv_pq_path = std::string {SUNSHINE_SHADERS_DIR} + "/convert_yuv420_packed_uv_type0_ps_perceptual_quantizer.hlsl";

        auto vs_y_blob = compile_shader(vs_y_path, "main_vs", "vs_5_0");
        auto vs_uv_blob = compile_shader(vs_uv_path, "main_vs", "vs_5_0");
        auto ps_y_blob = compile_shader(ps_y_path, "main_ps", "ps_5_0");
        auto ps_uv_blob = compile_shader(ps_uv_path, "main_ps", "ps_5_0");
        auto ps_y_linear_blob = compile_shader(ps_y_linear_path, "main_ps", "ps_5_0");
        auto ps_uv_linear_blob = compile_shader(ps_uv_linear_path, "main_ps", "ps_5_0");
        auto ps_y_pq_blob = compile_shader(ps_y_pq_path, "main_ps", "ps_5_0");
        auto ps_uv_pq_blob = compile_shader(ps_uv_pq_path, "main_ps", "ps_5_0");
        if (!vs_y_blob || !vs_uv_blob || !ps_y_blob || !ps_uv_blob ||
            !ps_y_linear_blob || !ps_uv_linear_blob || !ps_y_pq_blob || !ps_uv_pq_blob) {
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
        if (FAILED(device_->CreatePixelShader(ps_y_pq_blob->GetBufferPointer(), ps_y_pq_blob->GetBufferSize(), nullptr, &ps_y_pq_))) {
          return false;
        }
        if (FAILED(device_->CreatePixelShader(ps_uv_pq_blob->GetBufferPointer(), ps_uv_pq_blob->GetBufferSize(), nullptr, &ps_uv_pq_))) {
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

      bool ensure_staging(int width, int height) {
        if (staging_texture_ && staging_width_ == width && staging_height_ == height) {
          return true;
        }

        staging_texture_.Reset();
        staging_width_ = width;
        staging_height_ = height;

        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_texture_))) {
          BOOST_LOG(error) << "WebRTC: failed to create staging NV12 texture";
          return false;
        }

        return true;
      }

      bool ensure_constant_buffers(int width, int height, const video::sunshine_colorspace_t &colorspace) {
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

        if (!blend_disable_) {
          D3D11_BLEND_DESC blend_desc {};
          blend_desc.RenderTarget[0].BlendEnable = FALSE;
          blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
          if (FAILED(device_->CreateBlendState(&blend_desc, &blend_disable_))) {
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

        const bool colorspace_changed = !colorspace_valid_ ||
                                        colorspace_.colorspace != colorspace.colorspace ||
                                        colorspace_.full_range != colorspace.full_range ||
                                        colorspace_.bit_depth != colorspace.bit_depth;
        if (!color_matrix_cb_ || colorspace_changed) {
          const video::color_t *colors = video::color_vectors_from_colorspace(colorspace);
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
          colorspace_ = colorspace;
          colorspace_valid_ = true;
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

      ID3D11PixelShader *select_ps_y(ID3D11Texture2D *input_texture, bool hdr_output) const {
        D3D11_TEXTURE2D_DESC desc {};
        input_texture->GetDesc(&desc);
        if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
          if (hdr_output && ps_y_pq_) {
            return ps_y_pq_.Get();
          }
          if (ps_y_linear_) {
            return ps_y_linear_.Get();
          }
        }
        return ps_y_.Get();
      }

      ID3D11PixelShader *select_ps_uv(ID3D11Texture2D *input_texture, bool hdr_output) const {
        D3D11_TEXTURE2D_DESC desc {};
        input_texture->GetDesc(&desc);
        if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
          if (hdr_output && ps_uv_pq_) {
            return ps_uv_pq_.Get();
          }
          if (ps_uv_linear_) {
            return ps_uv_linear_.Get();
          }
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
      Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_y_pq_;
      Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_uv_pq_;
      Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
      Microsoft::WRL::ComPtr<ID3D11BlendState> blend_disable_;
      Microsoft::WRL::ComPtr<ID3D11Buffer> color_matrix_cb_;
      Microsoft::WRL::ComPtr<ID3D11Buffer> rotation_cb_;
      Microsoft::WRL::ComPtr<ID3D11Buffer> subsample_cb_;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> output_texture_;
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_y_;
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_uv_;
      Microsoft::WRL::ComPtr<IDXGIKeyedMutex> output_mutex_;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;
      D3D11_VIEWPORT viewport_y_ {};
      D3D11_VIEWPORT viewport_uv_ {};
      int width_ = 0;
      int height_ = 0;
      int subsample_width_ = 0;
      int subsample_height_ = 0;
      int staging_width_ = 0;
      int staging_height_ = 0;
      bool colorspace_valid_ = false;
      video::sunshine_colorspace_t colorspace_ {};
    };

    bool try_push_d3d11_frame(
      lwrtc_video_source_t *source,
      const std::shared_ptr<platf::img_t> &image,
      const std::optional<std::chrono::steady_clock::time_point> &timestamp,
      std::unique_ptr<D3D11Nv12Converter> *converter,
      const video::config_t &video_config
    ) {
      auto d3d_img = std::dynamic_pointer_cast<platf::dxgi::img_d3d_t>(image);
      if (!d3d_img || !d3d_img->capture_texture) {
        BOOST_LOG(debug) << "WebRTC: try_push_d3d11_frame - no d3d image or capture_texture";
        return false;
      }

      // NV12 conversion path - libwebrtc encoder expects NV12 for direct D3D11
      if (!converter) {
        return false;
      }
      if (!(*converter)) {
        *converter = std::make_unique<D3D11Nv12Converter>();
      }

      ID3D11Texture2D *out_texture = nullptr;
      IDXGIKeyedMutex *out_mutex = nullptr;
      const bool hdr_display = d3d_img->format == DXGI_FORMAT_R16G16B16A16_FLOAT;
      auto colorspace = video::colorspace_from_client_config(video_config, hdr_display);
      if (video::colorspace_is_hdr(colorspace)) {
        colorspace.colorspace = video::colorspace_e::rec709;
        colorspace.bit_depth = 8;
      }
      if (!(*converter)->Convert(
            d3d_img->capture_texture.get(),
            d3d_img->capture_mutex.get(),
            d3d_img->width,
            d3d_img->height,
            colorspace,
            &out_texture,
            &out_mutex)) {
        return false;
      }

      return (*converter)->ReadbackNv12(
        out_texture,
        out_mutex,
        d3d_img->width,
        d3d_img->height,
        [&](const uint8_t *y, int stride_y, const uint8_t *uv, int stride_uv) {
          return lwrtc_video_source_push_nv12(
            source,
            y,
            stride_y,
            uv,
            stride_uv,
            d3d_img->width,
            d3d_img->height,
            timestamp_to_us(timestamp)
          ) != 0;
        }
      );
    }
#endif

    void ensure_media_thread();
    void stop_media_thread();

    void media_thread_main() {
      using namespace std::chrono_literals;
      platf::adjust_thread_priority(platf::thread_priority_e::high);
      auto timer = platf::create_high_precision_timer();
      const bool use_timer = timer && *timer;
      auto sleep_for = [&](const std::chrono::nanoseconds &duration) {
        if (duration <= 0ns) {
          return;
        }
        if (use_timer) {
          timer->sleep_for(duration);
        } else {
          std::this_thread::sleep_for(duration);
        }
      };

      if (!use_timer) {
        BOOST_LOG(warning) << "WebRTC: high precision timer unavailable; pacing may be less accurate";
      }

      while (!webrtc_media_shutdown.load(std::memory_order_acquire)) {
        {
          std::unique_lock<std::mutex> lock(webrtc_media_mutex);
          webrtc_media_cv.wait(lock, []() {
            return webrtc_media_shutdown.load(std::memory_order_acquire) ||
                   webrtc_media_has_work.load(std::memory_order_acquire);
          });
          webrtc_media_has_work.store(false, std::memory_order_release);
        }
        if (webrtc_media_shutdown.load(std::memory_order_acquire)) {
          break;
        }

        const auto now = std::chrono::steady_clock::now();
        // Encoded video work - for passthrough mode (pre-encoded frames)
        struct EncodedVideoWork {
          std::shared_ptr<lwrtc_encoded_video_source_t> source;
          std::shared_ptr<std::vector<std::uint8_t>> data;
          std::shared_ptr<std::atomic_uint32_t> inflight;
          bool is_keyframe = false;
          std::optional<std::chrono::steady_clock::time_point> timestamp;
          std::chrono::steady_clock::time_point target_send {};
          std::chrono::nanoseconds pacing_slack {};
          bool pace = true;
          std::string session_id;
          bool clear_keyframe_on_success = false;
          std::size_t max_inflight_frames = 0;
        };
        struct AudioWork {
          std::shared_ptr<lwrtc_audio_source_t> source;
          std::shared_ptr<std::vector<int16_t>> samples;
          int sample_rate = 0;
          int channels = 0;
          int frames = 0;
          std::chrono::steady_clock::time_point timestamp;
        };

        std::vector<EncodedVideoWork> video_work;
        const auto audio_drain_slice = std::chrono::duration_cast<std::chrono::nanoseconds>(2ms);

        auto drain_audio = [&]() {
          const auto drain_now = std::chrono::steady_clock::now();
          std::vector<AudioWork> audio_work;
          {
            std::lock_guard lg {session_mutex};
            for (auto &[_, session] : sessions) {
              if (session.audio_source) {
                while (auto *front = session.raw_audio_frames.front()) {
                  if (drain_now - front->timestamp <= kAudioMaxFrameAge) {
                    break;
                  }
                  if (!session.raw_audio_frames.drop_oldest()) {
                    break;
                  }
                  session.state.audio_dropped++;
                }
                while (session.raw_audio_frames.size() > 1) {
                  if (!session.raw_audio_frames.drop_oldest()) {
                    break;
                  }
                  session.state.audio_dropped++;
                }
                RawAudioFrame frame;
                if (session.raw_audio_frames.pop(frame)) {
                  audio_work.push_back(
                    {session.audio_source, std::move(frame.samples), frame.sample_rate, frame.channels, frame.frames, frame.timestamp}
                  );
                }
              }
            }
          }
          for (auto &work : audio_work) {
            if (!work.source || !work.samples || work.samples->empty()) {
              continue;
            }
            lwrtc_audio_source_push(
              work.source.get(),
              work.samples->data(),
              static_cast<int>(sizeof(int16_t) * 8),
              work.sample_rate,
              work.channels,
              work.frames
            );
          }
        };

        auto sleep_for_with_audio = [&](const std::chrono::nanoseconds &duration) {
          if (duration <= 0ns) {
            return;
          }
          auto remaining = duration;
          while (remaining > 0ns && !webrtc_media_shutdown.load(std::memory_order_acquire)) {
            const auto slice = remaining > audio_drain_slice ? audio_drain_slice : remaining;
            sleep_for(slice);
            remaining -= slice;
            drain_audio();
          }
        };

        {
          std::lock_guard lg {session_mutex};
          for (auto &[_, session] : sessions) {
            if (!session.peer) {
              continue;
            }
            // Use encoded video source for passthrough mode
            if (session.encoded_video_source) {
              const bool waiting_for_keyframe = session.needs_keyframe;
              bool queued_keyframe = false;
              const int fps = std::max(1, session.video_config.framerate);
              const auto frame_interval = std::chrono::nanoseconds(1s) / fps;
              const auto &pacing = session.video_pacing;
              const auto derived_max_frame_age = frame_interval * std::max(1, pacing.max_age_frames);
              const auto max_frame_age = pacing.max_frame_age_override ? pacing.max_frame_age : derived_max_frame_age;
              auto map_capture_to_send = [&](const std::chrono::steady_clock::time_point &capture_time)
                -> std::optional<std::chrono::steady_clock::time_point> {
                if (!session.video_pacing_state.anchor_capture || !session.video_pacing_state.anchor_send) {
                  return std::nullopt;
                }
                return *session.video_pacing_state.anchor_send +
                       (capture_time - *session.video_pacing_state.anchor_capture);
              };
              auto oldest_queued_age = [&]() -> std::optional<std::chrono::nanoseconds> {
                const auto *oldest = session.video_frames.front();
                if (!oldest || !oldest->timestamp) {
                  return std::nullopt;
                }
                auto mapped = map_capture_to_send(*oldest->timestamp);
                if (!mapped) {
                  return std::nullopt;
                }
                return now - *mapped;
              };
              auto drop_oldest_frame = [&](bool keep_keyframes) {
                if (session.video_frames.drop_oldest(keep_keyframes)) {
                  session.state.video_dropped++;
                }
              };
              if (session.video_pacing_state.recovery_prefer_latest_until &&
                  now >= *session.video_pacing_state.recovery_prefer_latest_until) {
                session.video_pacing_state.recovery_prefer_latest_until.reset();
              }
              if (auto age = oldest_queued_age(); age && *age > max_frame_age) {
                while (session.video_frames.size() > 0) {
                  auto current_age = oldest_queued_age();
                  if (!current_age || *current_age <= max_frame_age) {
                    break;
                  }
                  drop_oldest_frame(waiting_for_keyframe);
                }
                session.video_pacing_state.recovery_prefer_latest_until = now + max_frame_age;
              }
              const bool drift_reset_recently =
                session.video_pacing_state.last_drift_reset &&
                now - *session.video_pacing_state.last_drift_reset <= max_frame_age;
              auto oldest_age_after = oldest_queued_age();
              const bool behind =
                session.video_frames.size() >= kMaxVideoFrames ||
                (oldest_age_after && *oldest_age_after > max_frame_age) ||
                drift_reset_recently;
              if (pacing.mode == video_pacing_mode_e::balanced && behind) {
                while (session.video_frames.size() > 1) {
                  drop_oldest_frame(waiting_for_keyframe);
                }
                session.video_pacing_state.recovery_prefer_latest_until = now + max_frame_age;
              }
              const bool recovery_active =
                session.video_pacing_state.recovery_prefer_latest_until &&
                now < *session.video_pacing_state.recovery_prefer_latest_until;
              const bool lagging = behind || recovery_active;
              if (lagging && !waiting_for_keyframe) {
                const bool keyframe_stale =
                  !session.last_keyframe_sent ||
                  now - *session.last_keyframe_sent >= kKeyframeResyncInterval;
                if (keyframe_stale &&
                    (!session.last_keyframe_request ||
                     now - *session.last_keyframe_request >= kKeyframeRequestInterval)) {
                  session.last_keyframe_request = now;
                  request_keyframe("pacing recovery");
                }
              }
              auto handle_frame = [&](EncodedVideoFrame &&frame) {
                if (waiting_for_keyframe && !frame.idr) {
                  return;
                }
                if (waiting_for_keyframe) {
                  queued_keyframe = true;
                }
                const bool has_timestamp = frame.timestamp.has_value();
                if (frame.data && !frame.data->empty() && session.encoded_prefix_logs < kEncodedPrefixLogLimit) {
                  const bool annexb = starts_with_annexb(*frame.data);
                  if (annexb) {
                    BOOST_LOG(debug) << "WebRTC: encoded frame prefix ok (AnnexB) id=" << session.state.id
                                     << " size=" << frame.data->size() << " prefix=" << hex_prefix(*frame.data);
                  } else {
                    BOOST_LOG(warning) << "WebRTC: encoded frame prefix not AnnexB id=" << session.state.id
                                       << " size=" << frame.data->size() << " prefix=" << hex_prefix(*frame.data);
                  }
                  session.encoded_prefix_logs++;
                }
                std::chrono::steady_clock::time_point target_send = now;
                bool pace_frame = pacing.pace;
                const bool capture_in_future = has_timestamp && *frame.timestamp > now + max_frame_age;
                if (capture_in_future) {
                  session.video_pacing_state.anchor_capture.reset();
                  session.video_pacing_state.anchor_send.reset();
                  session.video_pacing_state.last_drift_reset = now;
                  session.last_video_push.reset();
                  target_send = now;
                  pace_frame = false;
                }
                if (pace_frame && frame.timestamp) {
                  const auto capture_time = *frame.timestamp;
                  if (!session.video_pacing_state.anchor_capture ||
                      !session.video_pacing_state.anchor_send ||
                      capture_time < *session.video_pacing_state.anchor_capture) {
                    session.video_pacing_state.anchor_capture = capture_time;
                    session.video_pacing_state.anchor_send = now;
                  }
                  target_send = *session.video_pacing_state.anchor_send +
                                (capture_time - *session.video_pacing_state.anchor_capture);
                }
                const bool target_too_old = target_send + max_frame_age < now;
                if (pacing.drop_old_frames && target_too_old && !session.video_frames.empty()) {
                  session.state.video_dropped++;
                  session.needs_keyframe = true;
                  return;
                }
                if (target_too_old) {
                  session.video_pacing_state.anchor_capture.reset();
                  session.video_pacing_state.anchor_send.reset();
                  session.video_pacing_state.last_drift_reset = now;
                  session.last_video_push.reset();
                  target_send = now;
                  pace_frame = false;
                } else if (pace_frame && session.last_video_push) {
                  // Ensure that we never schedule a send earlier than one frame interval after
                  // the previous send target. This avoids drift where late frames permanently
                  // bias the schedule and grow the receiver jitter buffer.
                  target_send = std::max(target_send, *session.last_video_push + frame_interval);
                }
                if (target_send > now + max_frame_age) {
                  session.video_pacing_state.anchor_capture.reset();
                  session.video_pacing_state.anchor_send.reset();
                  session.video_pacing_state.last_drift_reset = now;
                  session.last_video_push.reset();
                  target_send = now;
                  pace_frame = false;
                }
                if (pacing.drop_old_frames && pace_frame && target_send < now) {
                  const auto drift = now - target_send;
                  if (drift > frame_interval) {
                    session.video_pacing_state.anchor_capture = frame.timestamp.value_or(now);
                    session.video_pacing_state.anchor_send = now;
                    session.video_pacing_state.last_drift_reset = now;
                    session.last_video_push.reset();
                    target_send = now;
                    pace_frame = false;
                  }
                }
                EncodedVideoWork work;
                work.source = session.encoded_video_source;
                work.data = std::move(frame.data);
                work.inflight = session.video_inflight;
                work.is_keyframe = frame.idr;
                // Use the mapped send target as the capture timestamp for WebRTC.
                // This avoids the receiver treating frames as perpetually late due to
                // upstream capture/encode pipeline delay.
                work.timestamp = target_send;
                work.target_send = target_send;
                work.pacing_slack = pacing.slack;
                work.pace = pace_frame;
                work.session_id = session.state.id;
                work.clear_keyframe_on_success = waiting_for_keyframe && frame.idr;
                work.max_inflight_frames = std::clamp<std::size_t>(
                  static_cast<std::size_t>(std::max(1, session.video_pacing.max_age_frames)) * 2,
                  kVideoInflightFramesMin,
                  kVideoInflightFramesMax
                );
                session.last_video_push = target_send;
                video_work.push_back(std::move(work));
                if (waiting_for_keyframe) {
                  return;
                }
              };
              bool prefer_latest = false;
              if (!waiting_for_keyframe) {
                switch (pacing.mode) {
                  case video_pacing_mode_e::latency:
                    prefer_latest = true;
                    break;
                  case video_pacing_mode_e::balanced:
                    prefer_latest = behind || recovery_active;
                    break;
                  case video_pacing_mode_e::smoothness:
                    prefer_latest = recovery_active;
                    break;
                }
              }
              if (prefer_latest) {
                EncodedVideoFrame latest;
                if (session.video_frames.pop_latest(latest)) {
                  handle_frame(std::move(latest));
                }
              } else {
                EncodedVideoFrame frame;
                while (session.video_frames.pop(frame)) {
                  handle_frame(std::move(frame));
                  if (waiting_for_keyframe) {
                    break;
                  }
                }
              }
              if (waiting_for_keyframe && !queued_keyframe) {
                if (!session.last_keyframe_request ||
                    now - *session.last_keyframe_request >= kKeyframeRequestInterval) {
                  session.last_keyframe_request = now;
                  request_keyframe("waiting for video keyframe");
                }
              }
              // Note: For encoded passthrough, we don't repeat frames like raw mode
              // The encoder produces frames at the capture rate, and we just forward them
            }
          }
        }

        drain_audio();

        // Push encoded video frames to WebRTC
        if (!video_work.empty()) {
          std::sort(video_work.begin(), video_work.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.target_send < rhs.target_send;
          });
        }
        for (auto &work : video_work) {
          if (!work.source || !work.data || work.data->empty()) {
            continue;
          }
          if (webrtc_media_shutdown.load(std::memory_order_acquire)) {
            break;
          }
          auto send_now = std::chrono::steady_clock::now();
          if (work.inflight && work.max_inflight_frames > 0) {
            const auto inflight_now = work.inflight->load(std::memory_order_relaxed);
            const auto max_inflight =
              std::max<std::size_t>(work.max_inflight_frames, kVideoInflightFramesMin);
            const auto max_allowed =
              work.is_keyframe ? max_inflight + kVideoInflightKeyframeExtra : max_inflight;
            if (inflight_now >= max_allowed) {
              if (!work.session_id.empty()) {
                std::lock_guard lg {session_mutex};
                auto it = sessions.find(work.session_id);
                if (it != sessions.end()) {
                  it->second.state.video_dropped++;
                  it->second.needs_keyframe = true;
                  it->second.video_pacing_state.anchor_capture.reset();
                  it->second.video_pacing_state.anchor_send.reset();
                  it->second.video_pacing_state.last_drift_reset = send_now;
                  it->second.last_video_push.reset();
                  if (!it->second.last_keyframe_request ||
                      send_now - *it->second.last_keyframe_request >= kKeyframeRequestInterval) {
                    it->second.last_keyframe_request = send_now;
                    request_keyframe("sender backlog");
                  }
                }
              }
              continue;
            }
          }
          if (work.pace && work.target_send > send_now + work.pacing_slack) {
            sleep_for_with_audio(work.target_send - send_now - work.pacing_slack);
            send_now = std::chrono::steady_clock::now();
          }
          if (work.inflight) {
            work.inflight->fetch_add(1, std::memory_order_relaxed);
          }
          auto* payload_ref = new SharedEncodedPayloadReleaseContext {work.data, work.inflight};
          // Avoid passing a timestamp in the past relative to our actual send time. If encoding
          // throughput dips below the target framerate, the mapped schedule can lag slightly
          // behind real-time (without triggering a full drift reset), which encourages WebRTC to
          // grow its jitter buffer. Clamping prevents persistent "late frame" signaling.
          auto push_timestamp = work.timestamp.value_or(send_now);
          if (push_timestamp < send_now) {
            push_timestamp = send_now;
          }
          const int pushed = lwrtc_encoded_video_source_push_shared(
            work.source.get(),
            work.data->data(),
            work.data->size(),
            timestamp_to_us(push_timestamp),
            work.is_keyframe ? 1 : 0,
            release_shared_encoded_payload,
            payload_ref
          );
          if (pushed && !work.session_id.empty()) {
            std::lock_guard lg {session_mutex};
            auto it = sessions.find(work.session_id);
            if (it != sessions.end()) {
              if (work.is_keyframe) {
                it->second.last_keyframe_sent = std::chrono::steady_clock::now();
              }
              if (work.clear_keyframe_on_success) {
                it->second.needs_keyframe = false;
                // Reset pacing state on keyframe delivery to recover from any accumulated drift
                it->second.video_pacing_state.anchor_capture.reset();
                it->second.video_pacing_state.anchor_send.reset();
                it->second.last_video_push.reset();
              }
            }
          }
        }
      }
    }

    void ensure_media_thread() {
      bool expected = false;
      if (!webrtc_media_running.compare_exchange_strong(expected, true)) {
        return;
      }
      webrtc_media_shutdown.store(false, std::memory_order_release);
      BOOST_LOG(debug) << "WebRTC: starting media thread";
      webrtc_media_thread = std::thread(&media_thread_main);
    }

    void stop_media_thread() {
      if (!webrtc_media_running.load(std::memory_order_acquire)) {
        return;
      }
      BOOST_LOG(debug) << "WebRTC: stopping media thread";
      webrtc_media_shutdown.store(true, std::memory_order_release);
      webrtc_media_cv.notify_one();
      if (webrtc_media_thread.joinable()) {
        webrtc_media_thread.join();
      }
      webrtc_media_running.store(false, std::memory_order_release);
    }

    void reset_webrtc_factory() {
      std::lock_guard lg {webrtc_mutex};
      if (webrtc_factory) {
        BOOST_LOG(debug) << "WebRTC: releasing peer connection factory";
        lwrtc_factory_release(webrtc_factory);
        webrtc_factory = nullptr;
        webrtc_factory_codec.reset();
        webrtc_factory_av1_params.reset();
        webrtc_factory_hevc_fmtp.reset();
      }
    }

    void schedule_webrtc_idle_shutdown() {
      webrtc_capture.idle_shutdown_pending.store(true, std::memory_order_release);
      const auto token = webrtc_idle_shutdown_token.fetch_add(1, std::memory_order_acq_rel) + 1;
      task_pool.pushDelayed(
        [token]() {
          if (webrtc_idle_shutdown_token.load(std::memory_order_acquire) != token) {
            return;
          }
          if (has_active_sessions()) {
            return;
          }
          if (active_peers.load(std::memory_order_acquire) != 0) {
            return;
          }

          stop_webrtc_capture_if_idle();
          reset_webrtc_factory();
        },
        kWebrtcIdleGracePeriod
      );
    }

    bool attach_media_tracks(Session &session) {
      std::lock_guard webrtc_lock {webrtc_mutex};
      if (!session.peer || !ensure_webrtc_factory_locked(session_codec_to_lwrtc(session.state))) {
        return false;
      }

      BOOST_LOG(debug) << "WebRTC: attach_media_tracks enter id=" << session.state.id;
      const std::string audio_stream_id = "sunshine-audio-" + session.state.id;
      const std::string video_stream_id = "sunshine-video-" + session.state.id;
      bool requested_video_keyframe = false;

      if (session.state.audio && !session.audio_source) {
        session.audio_source = make_lwrtc_ptr<lwrtc_audio_source_t, lwrtc_audio_source_release>(
          lwrtc_audio_source_create(webrtc_factory)
        );
        if (!session.audio_source) {
          BOOST_LOG(error) << "WebRTC: failed to create audio source for " << session.state.id;
          return false;
        }

        // Immediately drain any audio that was buffered before audio_source was created.
        RawAudioFrame frame;
        while (session.raw_audio_frames.pop(frame)) {
          if (!frame.samples || frame.samples->empty()) {
            continue;
          }
          lwrtc_audio_source_push(
            session.audio_source.get(),
            frame.samples->data(),
            static_cast<int>(sizeof(int16_t) * 8),
            frame.sample_rate,
            frame.channels,
            frame.frames
          );
        }
      }

      if (session.state.audio && !session.audio_track) {
        const std::string track_id = "audio-" + session.state.id;
        session.audio_track =
          lwrtc_audio_track_create(webrtc_factory, session.audio_source.get(), track_id.c_str());
        if (!session.audio_track) {
          BOOST_LOG(error) << "WebRTC: failed to create audio track for " << session.state.id;
          return false;
        }
        if (!lwrtc_peer_add_audio_track(session.peer, session.audio_track, audio_stream_id.c_str())) {
          BOOST_LOG(error) << "WebRTC: failed to add audio track for " << session.state.id;
          return false;
        }
      }

      if (session.state.video && !session.encoded_video_source) {
        // Create encoded video source for passthrough mode based on session codec
        const auto codec = session_codec_to_encoded(session.state);
        session.encoded_video_source = make_lwrtc_ptr<lwrtc_encoded_video_source_t, lwrtc_encoded_video_source_release>(
          lwrtc_encoded_video_source_create(
            webrtc_factory,
            codec,
            session.video_config.width,
            session.video_config.height)
        );
        if (!session.encoded_video_source) {
          BOOST_LOG(error) << "WebRTC: failed to create encoded video source for " << session.state.id;
          return false;
        }

        // Set up keyframe request callback to trigger IDR from encoder
        lwrtc_encoded_video_source_set_keyframe_callback(
            session.encoded_video_source.get(),
            [](void *) {
              request_keyframe("PLI/FIR");
            },
            nullptr);
      }

      if (session.state.video && !session.video_track) {
        const std::string track_id = "video-" + session.state.id;
        session.video_track = lwrtc_encoded_video_track_create(
            webrtc_factory, session.encoded_video_source.get(), track_id.c_str());
        if (!session.video_track) {
          BOOST_LOG(error) << "WebRTC: failed to create video track for " << session.state.id;
          return false;
        }
        if (!lwrtc_peer_add_video_track(session.peer, session.video_track, video_stream_id.c_str())) {
          BOOST_LOG(error) << "WebRTC: failed to add video track for " << session.state.id;
          return false;
        }
        requested_video_keyframe = true;
      }

      if (session.state.video && requested_video_keyframe) {
        session.needs_keyframe = true;
        session.last_keyframe_request = std::chrono::steady_clock::now();
        request_keyframe("initial video track");
      }

      ensure_media_thread();
      BOOST_LOG(debug) << "WebRTC: attach_media_tracks exit id=" << session.state.id;
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
      SessionState snapshot = session.state;
      snapshot.video_queue_frames = session.video_frames.size();
      snapshot.audio_queue_frames = session.raw_audio_frames.size();
      if (session.video_inflight) {
        snapshot.video_inflight_frames = session.video_inflight->load(std::memory_order_relaxed);
      } else {
        snapshot.video_inflight_frames = 0;
      }
      return snapshot;
    }
  }  // namespace

  bool has_active_sessions() {
    return active_sessions.load(std::memory_order_relaxed) > 0;
  }

  std::optional<std::string> ensure_capture_started(const SessionOptions &options) {
    return start_webrtc_capture(options);
  }

  SessionState create_session(const SessionOptions &options) {
    BOOST_LOG(debug) << "WebRTC: create_session enter";
    const auto rtsp_config = rtsp_sessions_active.load(std::memory_order_relaxed)
      ? snapshot_rtsp_capture_config()
      : std::nullopt;
    Session session;
    session.state.id = uuid_util::uuid_t::generate().string();
    session.state.audio = options.audio;
    session.state.video = options.video;
    session.state.encoded = options.encoded;
    session.state.audio_channels = options.audio_channels;
    session.state.audio_codec = options.audio_codec;
    session.state.profile = options.profile;
    session.state.client_name = options.client_name;
    session.state.client_uuid = options.client_uuid;
    session.video_config = build_video_config(options);
    apply_rtsp_video_overrides(session.video_config, rtsp_config);
    session.state.width = session.video_config.width;
    session.state.height = session.video_config.height;
    session.state.fps = session.video_config.framerate;
    session.state.bitrate_kbps = session.video_config.bitrate;
    session.state.codec = video_format_to_codec(session.video_config.videoFormat);
    session.state.hdr = session.video_config.dynamicRange != 0;
    session.video_pacing = build_video_pacing_config(options);
    session.state.video_pacing_mode = video_pacing_mode_to_string(session.video_pacing.mode);
    session.state.video_pacing_slack_ms = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(session.video_pacing.slack).count()
    );
    session.state.video_max_frame_age_ms = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(session.video_pacing.max_frame_age).count()
    );

    SessionState snapshot = session.state;
    std::lock_guard lg {session_mutex};
    sessions.emplace(snapshot.id, std::move(session));
    active_sessions.fetch_add(1, std::memory_order_relaxed);
    BOOST_LOG(debug) << "WebRTC: create_session exit id=" << snapshot.id;
    return snapshot;
  }

  bool close_session(std::string_view id) {
#ifdef SUNSHINE_ENABLE_WEBRTC
    BOOST_LOG(debug) << "WebRTC: close_session enter id=" << id;
    lwrtc_peer_t *peer = nullptr;
    std::shared_ptr<SessionIceContext> ice_context;
    lwrtc_audio_track_t *audio_track = nullptr;
    lwrtc_video_track_t *video_track = nullptr;
    std::shared_ptr<lwrtc_audio_source_t> audio_source;
    std::shared_ptr<lwrtc_video_source_t> video_source;
    std::shared_ptr<lwrtc_encoded_video_source_t> encoded_video_source;
    lwrtc_data_channel_t *input_channel = nullptr;
    std::shared_ptr<SessionDataChannelContext> data_channel_context;
#endif
    bool removed = false;
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
      audio_source = std::move(it->second.audio_source);
      video_source = std::move(it->second.video_source);
      encoded_video_source = std::move(it->second.encoded_video_source);
      input_channel = it->second.input_channel;
      data_channel_context = it->second.data_channel_context;
#endif
      sessions.erase(it);
      removed = true;
      active_sessions.fetch_sub(1, std::memory_order_relaxed);
    }
    if (removed) {
      local_answer_cv.notify_all();
    }
#ifdef SUNSHINE_ENABLE_WEBRTC
    if (peer) {
      if (ice_context) {
        ice_context->active.store(false, std::memory_order_release);
      }
      if (data_channel_context) {
        data_channel_context->active.store(false, std::memory_order_release);
      }
      lwrtc_peer_close(peer);
      lwrtc_peer_release(peer);
      active_peers.fetch_sub(1, std::memory_order_relaxed);
    }
    if (input_channel) {
      lwrtc_data_channel_unregister_observer(input_channel);
      lwrtc_data_channel_close(input_channel);
      lwrtc_data_channel_release(input_channel);
    }
    if (audio_track) {
      lwrtc_audio_track_release(audio_track);
    }
    if (video_track) {
      lwrtc_video_track_release(video_track);
    }
    if (!has_active_sessions()) {
      stop_media_thread();
      reset_input_context();
      schedule_webrtc_idle_shutdown();
    }
#endif
    BOOST_LOG(debug) << "WebRTC: close_session exit id=" << id;
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

  void shutdown_all_sessions() {
    std::vector<std::string> ids;
    {
      std::lock_guard lg {session_mutex};
      ids.reserve(sessions.size());
      for (const auto &entry : sessions) {
        ids.push_back(entry.first);
      }
    }

    for (const auto &id : ids) {
      close_session(id);
    }

#ifdef SUNSHINE_ENABLE_WEBRTC
    {
      std::lock_guard lg {session_mutex};
      if (!sessions.empty()) {
        return;
      }
    }
    webrtc_idle_shutdown_token.fetch_add(1, std::memory_order_acq_rel);
    stop_media_thread();
    reset_input_context();
    reset_webrtc_factory();
#endif
    stop_webrtc_capture_if_idle();
  }

  void submit_video_packet(video::packet_raw_t &packet) {
    if (!has_active_sessions()) {
      return;
    }

    if (!webrtc_capture.active.load(std::memory_order_acquire) || packet.channel_data != nullptr) {
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

      const auto now = std::chrono::steady_clock::now();
      const bool recovery_active =
        session.video_pacing_state.recovery_prefer_latest_until &&
        now < *session.video_pacing_state.recovery_prefer_latest_until;
      bool prefer_latest_enqueue = false;
      if (!session.needs_keyframe) {
        switch (session.video_pacing.mode) {
          case video_pacing_mode_e::latency:
            prefer_latest_enqueue = true;
            break;
          case video_pacing_mode_e::balanced:
          case video_pacing_mode_e::smoothness:
            prefer_latest_enqueue = recovery_active;
            break;
        }
      }
      if (prefer_latest_enqueue) {
        while (!session.video_frames.empty()) {
          if (!session.video_frames.drop_oldest()) {
            break;
          }
          session.state.video_dropped++;
        }
      }

      bool dropped = session.video_frames.push(std::move(frame), session.needs_keyframe);
      session.state.video_packets++;
      session.state.last_video_time = now;
      session.state.last_video_bytes = payload->size();
      session.state.last_video_idr = packet.is_idr();
      session.state.last_video_frame_index = packet.frame_index();
      if (dropped) {
        session.state.video_dropped++;
      }
    }
#ifdef SUNSHINE_ENABLE_WEBRTC
    webrtc_media_has_work.store(true, std::memory_order_release);
    webrtc_media_cv.notify_one();
#endif
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
    webrtc_media_has_work.store(true, std::memory_order_release);
    webrtc_media_cv.notify_one();
#endif
  }

  void submit_audio_frame(const std::vector<float> &samples, int sample_rate, int channels, int frames) {
    if (!has_active_sessions() || samples.empty()) {
      return;
    }

    const bool downmix_to_stereo =
      rtsp_sessions_active.load(std::memory_order_relaxed) && channels > 2;
    const float *input_samples = samples.data();
    int output_channels = channels;
    std::vector<float> downmixed;
    if (downmix_to_stereo) {
      output_channels = 2;
      const std::size_t total_frames = static_cast<std::size_t>(std::max(frames, 0));
      downmixed.resize(total_frames * static_cast<std::size_t>(output_channels));
      for (std::size_t frame = 0; frame < total_frames; ++frame) {
        const std::size_t base = frame * static_cast<std::size_t>(channels);
        downmixed[frame * 2] = samples[base];
        downmixed[frame * 2 + 1] = samples[base + 1];
      }
      input_samples = downmixed.data();
    }

    const auto now = std::chrono::steady_clock::now();
    const std::size_t total_samples =
      static_cast<std::size_t>(std::max(frames, 0)) * static_cast<std::size_t>(output_channels);
    auto shared_samples = audio_sample_pool().acquire(total_samples);
    for (std::size_t i = 0; i < total_samples; ++i) {
      float value = input_samples[i];
      if (!std::isfinite(value)) {
        value = 0.0f;
      }
      value = std::clamp(value, -1.0f, 1.0f);
      (*shared_samples)[i] = static_cast<int16_t>(std::lround(value * 32767.0f));
    }
    const auto byte_count = shared_samples->size() * sizeof(int16_t);
#ifdef SUNSHINE_ENABLE_WEBRTC
    struct AudioPushWork {
      std::shared_ptr<lwrtc_audio_source_t> source;
      std::shared_ptr<std::vector<int16_t>> samples;
      int sample_rate = 0;
      int channels = 0;
      int frames = 0;
    };
    std::vector<AudioPushWork> direct_audio;
    bool queued_raw_audio = false;
#endif
    {
      std::lock_guard lg {session_mutex};
      for (auto &[_, session] : sessions) {
        if (!session.state.audio) {
          continue;
        }

        {
#ifdef SUNSHINE_ENABLE_WEBRTC
          if (session.audio_source) {
            direct_audio.push_back(AudioPushWork {
              session.audio_source,
              shared_samples,
              sample_rate,
              output_channels,
              frames
            });
          } else
#endif
          {
            RawAudioFrame raw;
            raw.samples = shared_samples;
            raw.sample_rate = sample_rate;
            raw.channels = output_channels;
            raw.frames = frames;
            raw.timestamp = now;
            bool dropped = session.raw_audio_frames.push(std::move(raw));
            if (dropped) {
              session.state.audio_dropped++;
            }
#ifdef SUNSHINE_ENABLE_WEBRTC
            queued_raw_audio = true;
#endif
          }
        }
        session.state.audio_packets++;
        session.state.last_audio_time = now;
        session.state.last_audio_bytes = byte_count;
      }
    }
#ifdef SUNSHINE_ENABLE_WEBRTC
    for (auto &work : direct_audio) {
      if (!work.source || !work.samples || work.samples->empty()) {
        continue;
      }
      lwrtc_audio_source_push(
        work.source.get(),
        work.samples->data(),
        static_cast<int>(sizeof(int16_t) * 8),
        work.sample_rate,
        work.channels,
        work.frames
      );
    }
    if (queued_raw_audio) {
      ensure_media_thread();
      webrtc_media_has_work.store(true, std::memory_order_release);
      webrtc_media_cv.notify_one();
    }
#endif
  }

  void set_rtsp_sessions_active(bool active) {
    rtsp_sessions_active.store(active, std::memory_order_relaxed);
    if (!active) {
      clear_rtsp_capture_config();
    }
  }

  void set_rtsp_capture_config(const video::config_t &video_config, const audio::config_t &audio_config) {
    std::lock_guard<std::mutex> lock(rtsp_config_mutex);
    rtsp_capture_config = RtspCaptureConfig {video_config, audio_config};
  }

  bool set_remote_offer(std::string_view id, const std::string &sdp, const std::string &type) {
    std::string session_id {id};
#ifdef SUNSHINE_ENABLE_WEBRTC
    BOOST_LOG(debug) << "WebRTC: set_remote_offer enter id=" << session_id;
    lwrtc_peer_t *peer = nullptr;
    int audio_channels = kDefaultAudioChannels;
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
      if (it->second.state.codec && boost::iequals(*it->second.state.codec, "av1")) {
        const auto av1_offer = parse_av1_offer(sdp);
        {
          std::lock_guard lg {webrtc_mutex};
          webrtc_desired_av1_params = av1_offer.fmtp;
        }
        if (!av1_offer.offered) {
          BOOST_LOG(error) << "WebRTC: AV1 requested but offer does not include AV1";
          return false;
        } else if (av1_offer.fmtp) {
          BOOST_LOG(debug) << "WebRTC: parsed AV1 fmtp params profile=" << av1_offer.fmtp->profile
                           << " level-idx=" << av1_offer.fmtp->level_idx
                           << " tier=" << av1_offer.fmtp->tier;
        } else {
          BOOST_LOG(warning) << "WebRTC: no AV1 fmtp params found in offer";
        }
      }
      if (it->second.state.codec && boost::iequals(*it->second.state.codec, "hevc")) {
        const auto hevc_offer = parse_hevc_offer(sdp);
        {
          std::lock_guard lg {webrtc_mutex};
          webrtc_desired_hevc_fmtp = hevc_offer.fmtp;
        }
        if (!hevc_offer.offered) {
          BOOST_LOG(error) << "WebRTC: HEVC requested but offer does not include H265";
          return false;
        } else if (hevc_offer.fmtp) {
          BOOST_LOG(debug) << "WebRTC: parsed HEVC fmtp params " << *hevc_offer.fmtp;
        } else {
          BOOST_LOG(warning) << "WebRTC: no HEVC fmtp params found in offer";
        }
      }

      bool created_peer = false;
      if (!it->second.peer) {
        if (!it->second.ice_context) {
          auto ice_context = std::make_shared<SessionIceContext>();
          ice_context->id = session_id;
          it->second.ice_context = std::move(ice_context);
        }
        BOOST_LOG(debug) << "WebRTC: creating peer id=" << session_id;
        auto new_peer = create_peer_connection(it->second.state, it->second.ice_context.get());
        if (!new_peer) {
          BOOST_LOG(error) << "WebRTC: failed to create peer id=" << session_id;
          return false;
        }
        it->second.peer = new_peer;
        created_peer = true;
        active_peers.fetch_add(1, std::memory_order_relaxed);
      }
      if (!it->second.data_channel_context) {
        auto data_context = std::make_shared<SessionDataChannelContext>();
        data_context->id = session_id;
        it->second.data_channel_context = std::move(data_context);
      }
      BOOST_LOG(debug) << "WebRTC: registering data channel id=" << session_id;
      lwrtc_peer_register_data_channel(
        it->second.peer,
        &on_data_channel,
        it->second.data_channel_context.get()
      );
      BOOST_LOG(debug) << "WebRTC: attaching media tracks id=" << session_id;
      if (!attach_media_tracks(it->second)) {
        BOOST_LOG(error) << "WebRTC: failed to attach media tracks id=" << session_id;
        if (created_peer && it->second.peer) {
          lwrtc_peer_close(it->second.peer);
          lwrtc_peer_release(it->second.peer);
          it->second.peer = nullptr;
          active_peers.fetch_sub(1, std::memory_order_relaxed);
        }
        return false;
      }
      peer = it->second.peer;
      audio_channels = it->second.state.audio_channels.value_or(kDefaultAudioChannels);
#endif
    }

#ifdef SUNSHINE_ENABLE_WEBRTC
    if (!peer) {
      return false;
    }

    auto *ctx = new SessionPeerContext {session_id, peer, audio_channels};
    lwrtc_peer_set_remote_description(
      peer,
      sdp.c_str(),
      type.c_str(),
      &on_set_remote_success,
      &on_set_remote_failure,
      ctx
    );
    BOOST_LOG(debug) << "WebRTC: set_remote_offer exit id=" << session_id;
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
    {
      std::lock_guard lg {session_mutex};
      auto it = sessions.find(std::string {id});
      if (it == sessions.end()) {
        return false;
      }

      it->second.local_answer_sdp = sdp;
      it->second.local_answer_type = type;
      it->second.state.has_local_answer = true;
    }
    local_answer_cv.notify_all();
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

  bool wait_for_local_answer(
    std::string_view id,
    std::string &sdp_out,
    std::string &type_out,
    std::chrono::milliseconds timeout
  ) {
    const std::string session_id {id};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock {session_mutex};
    while (true) {
      auto it = sessions.find(session_id);
      if (it == sessions.end()) {
        return false;
      }
      if (it->second.state.has_local_answer) {
        sdp_out = it->second.local_answer_sdp;
        type_out = it->second.local_answer_type;
        return true;
      }
      if (timeout <= std::chrono::milliseconds::zero()) {
        return false;
      }
      if (local_answer_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
        return false;
      }
    }
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
