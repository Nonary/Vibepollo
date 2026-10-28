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

  int set_bitrate(sessions_t sessions, const std::string &client_uuid, int bitrate_kbps, const std::function<bool()> &admit) {
    if (bitrate_kbps <= 0) {
      return 0;
    }
    const auto matches = [&](const auto &session) {
      return session.bitrate_events && (client_uuid.empty() || session.client_uuid == client_uuid);
    };
    if (std::none_of(sessions.begin(), sessions.end(), matches)) {
      return 0;
    }
    // Keep the downstream transport/pacing admission check before all updates
    // and inside the caller's session lock. Rejection changes no metadata.
    if (admit && !admit()) {
      return -1;
    }
    int updated = 0;
    for (auto &session : sessions) {
      if (matches(session)) {
        session.bitrate.get() = bitrate_kbps;
        session.client_requested_bitrate.get() = bitrate_kbps;
        session.pacing_bitrate_kbps.get().store(bitrate_kbps, std::memory_order_relaxed);
        session.bitrate_events->raise(bitrate_kbps);
        ++updated;
      }
    }
    return updated;
  }
}  // namespace stream::broadcast_runtime
