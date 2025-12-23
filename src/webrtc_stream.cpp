/**
 * @file src/webrtc_stream.cpp
 * @brief Definitions for WebRTC session tracking and frame handoff.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <unordered_map>

// lib includes
#include <openssl/evp.h>
#include <openssl/x509.h>

// local includes
#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "uuid.h"
#include "webrtc_stream.h"

namespace webrtc_stream {
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

      struct IceCandidate {
        std::string mid;
        int mline_index = -1;
        std::string candidate;
      };
      std::vector<IceCandidate> candidates;
    };

    std::mutex session_mutex;
    std::unordered_map<std::string, Session> sessions;
    std::atomic_uint active_sessions {0};

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

    std::lock_guard lg {session_mutex};
    sessions.emplace(session.state.id, std::move(session));
    active_sessions.fetch_add(1, std::memory_order_relaxed);
    return session.state;
  }

  bool close_session(std::string_view id) {
    std::lock_guard lg {session_mutex};
    auto it = sessions.find(std::string {id});
    if (it == sessions.end()) {
      return false;
    }
    sessions.erase(it);
    active_sessions.fetch_sub(1, std::memory_order_relaxed);
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

  bool set_remote_offer(std::string_view id, const std::string &sdp, const std::string &type) {
    std::lock_guard lg {session_mutex};
    auto it = sessions.find(std::string {id});
    if (it == sessions.end()) {
      return false;
    }

    it->second.remote_offer_sdp = sdp;
    it->second.remote_offer_type = type;
    it->second.state.has_remote_offer = true;
    return true;
  }

  bool add_ice_candidate(std::string_view id, std::string mid, int mline_index, std::string candidate) {
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
    return true;
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
