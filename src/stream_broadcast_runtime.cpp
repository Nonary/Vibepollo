/**
 * @file src/stream_broadcast_runtime.cpp
 * @brief Broadcast queue lifetime and existing-session control operations.
 */
#include "stream_broadcast_runtime.h"

#include <algorithm>

namespace stream::broadcast_runtime {
  packet_queues_t::packet_queues_t(safe::mail_t mail, std::string shutdown_id, acquire_t video, acquire_t audio):
      _mail {std::move(mail)},
      _shutdown_id {std::move(shutdown_id)},
      _video {std::move(video)},
      _audio {std::move(audio)} {
  }

  packet_queue_state_t packet_queues_t::reset_for_start() const {
    auto shutdown = _mail->event<bool>(_shutdown_id);
    shutdown->reset();
    auto video = _video();
    auto audio = _audio();
    video->reset();
    audio->reset();
    return {std::move(shutdown), std::move(video), std::move(audio)};
  }

  void packet_queues_t::stop_and_join(transport_t &transport) const {
    auto shutdown = _mail->event<bool>(_shutdown_id);
    shutdown->raise(true);
    // Retain both stopped queues through every join. Mail owns only weak
    // entries, so a late worker must still find the queue we stopped.
    auto video = _video();
    auto audio = _audio();
    video->stop();
    audio->stop();
    transport.stop_receiving();
    transport.close_sockets();
    transport.join(worker_e::receive);
    transport.join(worker_e::video);
    transport.join(worker_e::audio);
    transport.join(worker_e::control);
    shutdown->reset();
  }

  void request_idr(sessions_t sessions) {
    for (auto &session : sessions) {
      if (session.idr_events) {
        session.idr_events->raise(true);
      }
    }
  }

  bitrate_update_t set_bitrate(sessions_t sessions, const std::string &client_uuid, int bitrate_kbps, const std::function<bool(int)> &admit) {
    const auto matches = [&](const auto &session) {
      return session.bitrate_events && (client_uuid.empty() || session.client_uuid == client_uuid);
    };

    // The caller receives one applied rate. Use the highest rate all
    // matching sessions can accept, including matches with different codecs.
    int applied_kbps = bitrate_kbps;
    int matches_count = 0;
    for (const auto &session : sessions) {
      if (matches(session)) {
        applied_kbps = std::min(applied_kbps, session.bitrate_limit_kbps);
        ++matches_count;
      }
    }
    if (matches_count == 0 || applied_kbps <= 0) {
      return {};
    }

    // Admit the clamped rate before publishing any change, under the same
    // session lock used to select the matching sessions.
    if (admit && !admit(applied_kbps)) {
      return {-1, 0};
    }
    for (auto &session : sessions) {
      if (matches(session)) {
        session.bitrate.get() = applied_kbps;
        session.client_requested_bitrate.get() = applied_kbps;
        session.pacing_bitrate_kbps.get().store(applied_kbps, std::memory_order_relaxed);
        session.bitrate_events->raise(applied_kbps);
      }
    }
    return {matches_count, applied_kbps};
  }
}  // namespace stream::broadcast_runtime
