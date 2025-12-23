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

#ifdef SUNSHINE_ENABLE_WEBRTC
#include <libwebrtc_c.h>
#endif

// local includes
#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "logging.h"
#include "uuid.h"
#include "webrtc_stream.h"

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

    struct Session {
      SessionState state;
      ring_buffer_t<EncodedVideoFrame> video_frames {kMaxVideoFrames};
      ring_buffer_t<EncodedAudioFrame> audio_frames {kMaxAudioFrames};
      std::string remote_offer_sdp;
      std::string remote_offer_type;
      std::string local_answer_sdp;
      std::string local_answer_type;
#ifdef SUNSHINE_ENABLE_WEBRTC
      lwrtc_peer_t *peer = nullptr;
      std::shared_ptr<SessionIceContext> ice_context;
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
