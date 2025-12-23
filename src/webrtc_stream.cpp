/**
 * @file src/webrtc_stream.cpp
 * @brief Definitions for WebRTC session tracking and frame handoff.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
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

#ifdef SUNSHINE_ENABLE_WEBRTC
#include <libwebrtc.h>
#include <rtc_ice_candidate.h>
#include <rtc_mediaconstraints.h>
#include <rtc_peerconnection.h>
#include <rtc_peerconnection_factory.h>
#endif

// local includes
#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"
#include "uuid.h"
#include "webrtc_stream.h"

namespace webrtc_stream {
  namespace {
    constexpr std::size_t kMaxVideoFrames = 2;
    constexpr std::size_t kMaxAudioFrames = 8;
    constexpr int kDefaultWidth = 1920;
    constexpr int kDefaultHeight = 1080;
    constexpr int kDefaultFps = 60;
    constexpr int kDefaultBitrateKbps = 20000;
    constexpr short kAbsCoordinateMax = 32767;

    struct CaptureState {
      std::shared_ptr<safe::mail_raw_t> mail;
      safe::mail_raw_t::event_t<bool> shutdown_event;
      std::shared_ptr<input::input_t> input;
      std::thread video_thread;
      std::thread audio_thread;
      std::string config_session_id;
      bool audio_running = false;
      bool video_running = false;
      bool running = false;
    };

    std::mutex capture_mutex;
    CaptureState capture_state;
    std::atomic_bool rtsp_sessions_active {false};

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

    private:
      std::size_t _max_items;
      std::deque<T> _queue;
    };

    struct Session {
      SessionState state;
      ring_buffer_t<EncodedVideoFrame> video_frames {kMaxVideoFrames};
      ring_buffer_t<EncodedAudioFrame> audio_frames {kMaxAudioFrames};
      std::string remote_offer_sdp;
      std::string remote_offer_type;
      std::string local_answer_sdp;
      std::string local_answer_type;
#ifdef SUNSHINE_ENABLE_WEBRTC
      libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> peer;
      std::unique_ptr<libwebrtc::RTCPeerConnectionObserver> observer;
      libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel> input_channel;
      std::unique_ptr<libwebrtc::RTCDataChannelObserver> input_observer;
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

#ifdef SUNSHINE_ENABLE_WEBRTC
    class InputDataChannelObserver final : public libwebrtc::RTCDataChannelObserver {
    public:
      explicit InputDataChannelObserver(std::string session_id):
          session_id_(std::move(session_id)) {}

      void OnStateChange(libwebrtc::RTCDataChannelState) override {}

      void OnMessage(const char *buffer, int length, bool binary) override {
        if (binary || length <= 0 || !buffer) {
          return;
        }
        handle_input_message(std::string_view {buffer, static_cast<std::size_t>(length)});
      }

    private:
      std::string session_id_;
    };

    void register_input_channel(
      const std::string &session_id,
      const libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel> &channel
    ) {
      if (!channel) {
        return;
      }
      std::string label = channel->label().c_str();
      if (label != "input") {
        return;
      }

      std::lock_guard lg {session_mutex};
      auto it = sessions.find(session_id);
      if (it == sessions.end()) {
        return;
      }

      auto observer = std::make_unique<InputDataChannelObserver>(session_id);
      channel->RegisterObserver(observer.get());
      it->second.input_channel = channel;
      it->second.input_observer = std::move(observer);
    }

    class PeerObserver final : public libwebrtc::RTCPeerConnectionObserver {
    public:
      explicit PeerObserver(std::string session_id):
          session_id_(std::move(session_id)) {}

      void OnSignalingState(libwebrtc::RTCSignalingState) override {}

      void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState) override {}

      void OnIceGatheringState(libwebrtc::RTCIceGatheringState) override {}

      void OnIceConnectionState(libwebrtc::RTCIceConnectionState) override {}

      void OnIceCandidate(libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate> candidate) override {
        if (!candidate) {
          return;
        }
        std::string mid = candidate->sdp_mid().c_str();
        std::string cand = candidate->candidate().c_str();
        int mline_index = candidate->sdp_mline_index();
        add_local_candidate(session_id_, std::move(mid), mline_index, std::move(cand));
      }

      void OnAddStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}

      void OnRemoveStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}

      void OnDataChannel(libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel> channel) override {
        register_input_channel(session_id_, channel);
      }

      void OnRenegotiationNeeded() override {}

      void OnTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver>) override {}

      void OnAddTrack(
        libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>>,
        libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>
      ) override {}

      void OnRemoveTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}

    private:
      std::string session_id_;
    };

    std::mutex webrtc_mutex;
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> webrtc_factory;

    bool ensure_webrtc_factory() {
      std::lock_guard lg {webrtc_mutex};
      if (webrtc_factory) {
        return true;
      }

      if (!libwebrtc::LibWebRTC::Initialize()) {
        BOOST_LOG(error) << "WebRTC: failed to initialize";
        return false;
      }

      webrtc_factory = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
      if (!webrtc_factory || !webrtc_factory->Initialize()) {
        BOOST_LOG(error) << "WebRTC: failed to create peer connection factory";
        webrtc_factory.reset();
        return false;
      }

      return true;
    }

    libwebrtc::scoped_refptr<libwebrtc::RTCMediaConstraints> create_constraints() {
      auto constraints = libwebrtc::RTCMediaConstraints::Create();
      constraints->AddMandatoryConstraint(
        libwebrtc::RTCMediaConstraints::kEnableRtpDataChannels,
        libwebrtc::RTCMediaConstraints::kValueTrue
      );
      return constraints;
    }

    std::vector<libwebrtc::IceServer> load_ice_servers() {
      auto env = std::getenv("SUNSHINE_WEBRTC_ICE_SERVERS");
      if (!env || !*env) {
        return {};
      }

      nlohmann::json parsed = nlohmann::json::parse(env, nullptr, false);
      if (!parsed.is_array()) {
        return {};
      }

      std::vector<libwebrtc::IceServer> servers;
      for (const auto &entry : parsed) {
        if (entry.is_string()) {
          libwebrtc::IceServer server;
          server.uri = entry.get<std::string>().c_str();
          servers.push_back(std::move(server));
          continue;
        }

        if (!entry.is_object()) {
          continue;
        }

        std::vector<std::string> urls;
        if (entry.contains("urls")) {
          const auto &urls_node = entry["urls"];
          if (urls_node.is_string()) {
            urls.push_back(urls_node.get<std::string>());
          } else if (urls_node.is_array()) {
            for (const auto &url : urls_node) {
              if (url.is_string()) {
                urls.push_back(url.get<std::string>());
              }
            }
          }
        } else if (entry.contains("url") && entry["url"].is_string()) {
          urls.push_back(entry["url"].get<std::string>());
        }

        auto username = entry.value("username", "");
        auto credential = entry.value("credential", "");
        for (const auto &url : urls) {
          libwebrtc::IceServer server;
          server.uri = url.c_str();
          server.username = username.c_str();
          server.password = credential.c_str();
          servers.push_back(std::move(server));
        }
      }

      return servers;
    }

    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> create_peer_connection(
      const SessionState &state
    ) {
      if (!ensure_webrtc_factory()) {
        return nullptr;
      }

      libwebrtc::RTCConfiguration config {};
      config.offer_to_receive_audio = state.audio;
      config.offer_to_receive_video = state.video;
      auto ice_servers = load_ice_servers();
      for (std::size_t i = 0; i < ice_servers.size() && i < libwebrtc::kMaxIceServerSize; ++i) {
        config.ice_servers[i] = ice_servers[i];
      }
      auto constraints = create_constraints();
      return webrtc_factory->Create(config, constraints);
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

    int resolve_video_format(const SessionState &state) {
      std::string codec = state.codec.value_or("h264");
      if (codec == "hevc") {
        if (video::active_hevc_mode > 1) {
          return 1;
        }
        BOOST_LOG(warning) << "WebRTC: HEVC requested but unsupported, falling back to H.264";
      } else if (codec == "av1") {
        if (video::active_av1_mode > 1) {
          return 2;
        }
        BOOST_LOG(warning) << "WebRTC: AV1 requested but unsupported, falling back to H.264";
      }
      return 0;
    }

    video::config_t build_video_config(const SessionState &state) {
      video::config_t config {};
      config.width = state.width.value_or(kDefaultWidth);
      config.height = state.height.value_or(kDefaultHeight);
      config.framerate = state.fps.value_or(kDefaultFps);
      config.bitrate = state.bitrate_kbps.value_or(kDefaultBitrateKbps);
      if (config::video.max_bitrate > 0) {
        config.bitrate = std::min(config.bitrate, config::video.max_bitrate);
      }
      config.slicesPerFrame = 1;
      config.numRefFrames = 1;
      config.encoderCscMode = 0;
      config.videoFormat = resolve_video_format(state);
      config.dynamicRange = state.hdr.value_or(false) ? 1 : 0;
      config.prefer_sdr_10bit = false;
      config.chromaSamplingType = 0;
      config.enableIntraRefresh = 0;
      return config;
    }

    audio::config_t build_audio_config(const SessionState &state) {
      audio::config_t config {};
      config.packetDuration = 5;
      int channels = state.audio_channels.value_or(2);
      if (channels != 2 && channels != 6 && channels != 8) {
        channels = 2;
      }
      config.channels = channels;
      config.mask = 0;
      config.flags[audio::config_t::HIGH_QUALITY] = true;
      config.flags[audio::config_t::HOST_AUDIO] = true;
      config.flags[audio::config_t::CUSTOM_SURROUND_PARAMS] = false;
      if (state.audio_codec && *state.audio_codec != "opus") {
        BOOST_LOG(warning) << "WebRTC: audio codec " << *state.audio_codec << " requested but unsupported";
      }
      return config;
    }

    void stop_capture_locked() {
      if (!capture_state.running) {
        capture_state.config_session_id.clear();
        capture_state.input.reset();
        capture_state.mail.reset();
        capture_state.shutdown_event.reset();
        capture_state.audio_running = false;
        capture_state.video_running = false;
        return;
      }

      if (capture_state.shutdown_event) {
        capture_state.shutdown_event->raise(true);
      }
      if (capture_state.video_thread.joinable()) {
        capture_state.video_thread.join();
      }
      if (capture_state.audio_thread.joinable()) {
        capture_state.audio_thread.join();
      }
      if (capture_state.input) {
        input::reset(capture_state.input);
      }
      capture_state.input.reset();
      capture_state.mail.reset();
      capture_state.shutdown_event.reset();
      capture_state.running = false;
      capture_state.audio_running = false;
      capture_state.video_running = false;
      capture_state.config_session_id.clear();
      platf::streaming_will_stop();
    }

    void start_capture_locked(const SessionState &state, bool wants_audio, bool wants_video) {
      if (capture_state.running) {
        return;
      }

      capture_state.mail = std::make_shared<safe::mail_raw_t>();
      capture_state.shutdown_event = capture_state.mail->event<bool>(mail::shutdown);
      capture_state.input = input::alloc(capture_state.mail);

      auto video_config = build_video_config(state);
      auto audio_config = build_audio_config(state);

      if (wants_video) {
        capture_state.video_thread = std::thread([mail = capture_state.mail, video_config]() {
          video::capture(mail, video_config, nullptr);
        });
        capture_state.video_running = true;
      }
      if (wants_audio) {
        capture_state.audio_thread = std::thread([mail = capture_state.mail, audio_config]() {
          audio::capture(mail, audio_config, nullptr);
        });
        capture_state.audio_running = true;
      }

      capture_state.running = capture_state.video_running || capture_state.audio_running;
      if (capture_state.running) {
        platf::streaming_will_start();
      }
    }

    void update_capture_state() {
      std::unique_lock lock {capture_mutex};

      bool wants_audio = false;
      bool wants_video = false;
      bool should_run = false;
      bool restart = false;
      SessionState primary {};
      bool have_primary = false;

      {
        std::lock_guard lg {session_mutex};
        if (!sessions.empty()) {
          for (const auto &entry : sessions) {
            wants_audio = wants_audio || entry.second.state.audio;
            wants_video = wants_video || entry.second.state.video;
          }
          should_run = (wants_audio || wants_video) && !rtsp_sessions_active.load(std::memory_order_relaxed);
          if (should_run) {
            auto it = sessions.find(capture_state.config_session_id);
            if (it == sessions.end()) {
              it = sessions.begin();
              capture_state.config_session_id = it->first;
              restart = capture_state.running;
            }
            if (it != sessions.end()) {
              primary = it->second.state;
              have_primary = true;
            }
          } else {
            capture_state.config_session_id.clear();
          }
        } else {
          capture_state.config_session_id.clear();
        }
      }

      if (!should_run || !have_primary) {
        stop_capture_locked();
        return;
      }

      if (!capture_state.running) {
        start_capture_locked(primary, wants_audio, wants_video);
        return;
      }

      if (restart || wants_audio != capture_state.audio_running || wants_video != capture_state.video_running) {
        stop_capture_locked();
        start_capture_locked(primary, wants_audio, wants_video);
      }
    }

    std::shared_ptr<input::input_t> current_input_context() {
      std::lock_guard lg {capture_mutex};
      return capture_state.input;
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

    {
      std::lock_guard lg {session_mutex};
      sessions.emplace(session.state.id, std::move(session));
      active_sessions.fetch_add(1, std::memory_order_relaxed);
    }
    update_capture_state();
    return snapshot;
  }

  bool close_session(std::string_view id) {
#ifdef SUNSHINE_ENABLE_WEBRTC
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> peer;
#endif
    {
      std::lock_guard lg {session_mutex};
      auto it = sessions.find(std::string {id});
      if (it == sessions.end()) {
        return false;
      }
#ifdef SUNSHINE_ENABLE_WEBRTC
      peer = it->second.peer;
      if (it->second.input_channel) {
        it->second.input_channel->UnregisterObserver();
      }
#endif
      sessions.erase(it);
      active_sessions.fetch_sub(1, std::memory_order_relaxed);
    }
#ifdef SUNSHINE_ENABLE_WEBRTC
    if (peer) {
      peer->Close();
    }
#endif
    update_capture_state();
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

  void set_rtsp_sessions_active(bool active) {
    rtsp_sessions_active.store(active, std::memory_order_relaxed);
    update_capture_state();
  }

  bool set_remote_offer(std::string_view id, const std::string &sdp, const std::string &type) {
    std::string session_id {id};
#ifdef SUNSHINE_ENABLE_WEBRTC
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> peer;
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
        auto new_peer = create_peer_connection(it->second.state);
        if (!new_peer) {
          return false;
        }
        auto observer = std::make_unique<PeerObserver>(session_id);
        new_peer->RegisterRTCPeerConnectionObserver(observer.get());
        it->second.peer = new_peer;
        it->second.observer = std::move(observer);
      }
      peer = it->second.peer;
#endif
    }

#ifdef SUNSHINE_ENABLE_WEBRTC
    if (!peer) {
      return false;
    }

    peer->SetRemoteDescription(
      sdp,
      type,
      [session_id, peer]() {
        auto constraints = create_constraints();
        peer->CreateAnswer(
          [session_id, peer](const libwebrtc::string sdp_out, const libwebrtc::string type_out) {
            std::string sdp_copy = sdp_out.c_str();
            std::string type_copy = type_out.c_str();
            peer->SetLocalDescription(
              sdp_copy,
              type_copy,
              [session_id, sdp_copy, type_copy]() {
                set_local_answer(session_id, sdp_copy, type_copy);
              },
              [session_id](const char *error) {
                BOOST_LOG(error) << "WebRTC: failed to set local description for " << session_id
                                 << ": " << (error ? error : "unknown");
              }
            );
          },
          [session_id](const char *error) {
            BOOST_LOG(error) << "WebRTC: failed to create answer for " << session_id
                             << ": " << (error ? error : "unknown");
          },
          constraints
        );
      },
      [session_id](const char *error) {
        BOOST_LOG(error) << "WebRTC: failed to set remote description for " << session_id
                         << ": " << (error ? error : "unknown");
      }
    );
    return true;
#else
    BOOST_LOG(error) << "WebRTC: support is disabled at build time";
    return false;
#endif
  }

  bool add_ice_candidate(std::string_view id, std::string mid, int mline_index, std::string candidate) {
#ifdef SUNSHINE_ENABLE_WEBRTC
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> peer;
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
      peer->AddCandidate(stored_mid, stored_mline_index, stored_candidate);
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
