/**
 * @file src/stream_broadcast_runtime.h
 * @brief Broadcast queue lifetime and existing-session control operations.
 */
#pragma once

#include "thread_safe.h"

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace stream::broadcast_runtime {
  enum class worker_e {
    receive,
    video,
    audio,
    control
  };

  // The platform boundary owns sockets and thread handles, not packet queues.
  class transport_t {
  public:
    virtual ~transport_t() = default;
    virtual void stop_receiving() = 0;
    virtual void close_sockets() = 0;
    virtual void join(worker_e worker) = 0;
  };

  class packet_queue_t {
  public:
    virtual ~packet_queue_t() = default;
    virtual void reset() = 0;
    virtual void stop() = 0;
  };

  // Payload erasure is confined to queue lifecycle operations. Both production
  // and tests use this adapter to the real mail registry and safe::queue_t.
  template<class Packet>
  class mail_packet_queue_t final: public packet_queue_t {
  public:
    mail_packet_queue_t(const safe::mail_t &mail, std::string_view id):
        _queue {mail->queue<Packet>(id)} {
    }

    void reset() override {
      _queue->reset();
    }

    void stop() override {
      _queue->stop();
    }

  private:
    safe::mail_raw_t::queue_t<Packet> _queue;
  };

  struct packet_queue_state_t {
    safe::mail_raw_t::event_t<bool> shutdown;
    std::unique_ptr<packet_queue_t> video;
    std::unique_ptr<packet_queue_t> audio;
  };

  class packet_queues_t {
  public:
    using acquire_t = std::function<std::unique_ptr<packet_queue_t>()>;

    packet_queues_t(safe::mail_t mail, std::string shutdown_id, acquire_t video, acquire_t audio);
    [[nodiscard]] packet_queue_state_t reset_for_start() const;
    void stop_and_join(transport_t &transport) const;

  private:
    safe::mail_t _mail;
    std::string _shutdown_id;
    acquire_t _video;
    acquire_t _audio;
  };

  template<class VideoPacket, class AudioPacket>
  packet_queues_t packet_queues(safe::mail_t mail, std::string_view shutdown, std::string_view video, std::string_view audio) {
    return {
      mail,
      std::string {shutdown},
      [mail, id = std::string {video}] {
        return std::make_unique<mail_packet_queue_t<VideoPacket>>(mail, id);
      },
      [mail, id = std::string {audio}] {
        return std::make_unique<mail_packet_queue_t<AudioPacket>>(mail, id);
      }
    };
  }

  // Views are valid only while the caller holds the session registry lock.
  struct session_control_t {
    std::string_view client_uuid;
    std::reference_wrapper<int> bitrate;
    std::reference_wrapper<int> client_requested_bitrate;
    std::reference_wrapper<std::atomic<int>> pacing_bitrate_kbps;
    safe::mail_raw_t::event_t<bool> idr_events;
    safe::mail_raw_t::event_t<int> bitrate_events;
  };

  using sessions_t = std::span<session_control_t>;
  using session_action_t = std::function<int(sessions_t)>;
  void request_idr(sessions_t sessions);
  int set_bitrate(sessions_t sessions, const std::string &client_uuid, int bitrate_kbps, const std::function<bool()> &admit);

  // The real shared_t protects construction/destruction. A session adapter only
  // exposes locked field views; client matching, admission ordering, metadata
  // changes and event publication belong to the component implementation.
  template<class Context>
  class controls_t {
  public:
    using locked_sessions_t = std::function<int(Context &, const session_action_t &)>;
    using admission_t = std::function<bool(Context &, const std::string &, int)>;

    controls_t(safe::shared_t<Context> &broadcast, locked_sessions_t sessions, admission_t admit = {}):
        _broadcast {broadcast},
        _sessions {std::move(sessions)},
        _admit {std::move(admit)} {
    }

    void request_idr_for_all_sessions() {
      auto ref = _broadcast.try_ref();
      if (ref) {
        _sessions(*ref.get(), [](sessions_t sessions) {
          request_idr(sessions);
          return 0;
        });
      }
    }

    int set_bitrate_for_sessions(const std::string &client_uuid, int bitrate_kbps) {
      if (bitrate_kbps <= 0) {
        return 0;
      }
      auto ref = _broadcast.try_ref();
      if (!ref) {
        return 0;
      }
      return _sessions(*ref.get(), [&](sessions_t sessions) {
        return set_bitrate(sessions, client_uuid, bitrate_kbps, [&] {
          return !_admit || _admit(*ref.get(), client_uuid, bitrate_kbps);
        });
      });
    }

  private:
    safe::shared_t<Context> &_broadcast;
    locked_sessions_t _sessions;
    admission_t _admit;
  };
}  // namespace stream::broadcast_runtime
