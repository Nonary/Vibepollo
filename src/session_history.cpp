/**
 * @file src/session_history.cpp
 * @brief Public facade for SQLite-backed session history persistence.
 */

// standard includes
#include <algorithm>
#include <chrono>

// local includes
#include "session_history.h"
#include "config.h"
#include "host_stats.h"
#include "logging.h"
#include "rtsp.h"
#include "stream.h"
#include "session_history_sampler.h"
#include "session_history_storage.h"
#include "session_history_writer.h"

namespace session_history {

  namespace {

    writer::settings_t current_writer_settings() {
      writer::settings_t settings;
      settings.enabled = config::sunshine.session_history_enabled;
      settings.ttl_days = std::max(config::sunshine.session_history_ttl_days, 0);
      settings.max_db_size_bytes =
        config::sunshine.session_history_db_size_limit_mb > 0 ?
          static_cast<std::uint64_t>(config::sunshine.session_history_db_size_limit_mb) * 1024ull * 1024ull :
          0ull;
      return settings;
    }

    bool history_available() {
      return writer::is_enabled() && writer::is_available();
    }

  }  // namespace

  void init(const std::string &db_path) {
    writer::update_settings(current_writer_settings());
    writer::init(db_path);
    if (writer::is_available()) {
      sampler::init();
    }
  }

  void shutdown() {
    BOOST_LOG(info) << "session_history: shutting down";
    sampler::shutdown();
    writer::shutdown();
    BOOST_LOG(info) << "session_history: shut down";
  }

  void begin_session(const session_metadata_t &metadata) {
    if (!history_available()) {
      return;
    }

    BOOST_LOG(info) << "session_history: begin_session uuid=" << metadata.uuid
                    << " protocol=" << metadata.protocol;

    session_metadata_t enriched = metadata;
    if (enriched.host_cpu_model.empty() || enriched.host_gpu_model.empty()) {
      const auto &info = host_stats::info();
      if (enriched.host_cpu_model.empty()) {
        enriched.host_cpu_model = info.cpu_model;
      }
      if (enriched.host_gpu_model.empty()) {
        enriched.host_gpu_model = info.gpu_model;
      }
    }
    enriched.codec = stream::canonical_codec_name(enriched.codec);

    sampler::register_session(enriched);

    if (!writer::enqueue_begin(enriched)) {
      BOOST_LOG(error) << "session_history: failed to queue begin_session for uuid=" << enriched.uuid;
    }

    session_event_t event;
    event.session_uuid = enriched.uuid;
    event.timestamp_unix = storage::now_unix();
    event.event_type = "stream_started";
    if (!writer::enqueue_event(event)) {
      BOOST_LOG(error) << "session_history: failed to queue stream_started event for uuid=" << enriched.uuid;
    }
  }

  void end_session(const std::string &uuid) {
    if (!history_available()) {
      return;
    }

    BOOST_LOG(info) << "session_history: end_session uuid=" << uuid;
    sampler::unregister_session(uuid);

    session_event_t event;
    event.session_uuid = uuid;
    event.timestamp_unix = storage::now_unix();
    event.event_type = "stream_ended";
    if (!writer::enqueue_event(event)) {
      BOOST_LOG(error) << "session_history: failed to queue stream_ended event for uuid=" << uuid;
    }

    if (!writer::enqueue_end(uuid)) {
      BOOST_LOG(error) << "session_history: failed to queue end_session for uuid=" << uuid;
    }

    (void) writer::enqueue_prune();
  }

  void record_event(const std::string &uuid, const std::string &event_type, const std::string &payload) {
    if (!history_available()) {
      return;
    }

    session_event_t event;
    event.session_uuid = uuid;
    event.timestamp_unix = storage::now_unix();
    event.event_type = event_type;
    event.payload = payload;
    if (!writer::enqueue_event(event)) {
      BOOST_LOG(error) << "session_history: failed to queue event '" << event_type << "' for uuid=" << uuid;
    }
  }

  std::vector<session_summary_t> list_sessions(int limit, int offset) {
    return writer::list_sessions(limit, offset);
  }

  std::optional<session_detail_t> get_session_detail(const std::string &uuid, bool include_all) {
    return writer::get_session_detail(uuid, include_all);
  }

  std::vector<active_session_t> get_active_sessions() {
    return sampler::get_active_sessions();
  }

  delete_result_e delete_session(const std::string &uuid) {
    if (!history_available()) {
      return delete_result_e::unavailable;
    }
    if (sampler::is_active(uuid)) {
      return delete_result_e::active_session;
    }
    return writer::delete_session(uuid);
  }

#ifdef SUNSHINE_TESTS
  bool record_sample_for_tests(const session_sample_t &sample) {
    return writer::enqueue_sample(sample);
  }

  bool set_session_end_time_for_tests(const std::string &uuid, double end_time_unix) {
    return writer::set_session_end_time_for_tests(uuid, end_time_unix);
  }

  void configure_retention_for_tests(bool enabled, int ttl_days, std::uint64_t max_db_size_bytes) {
    writer::settings_t settings;
    settings.enabled = enabled;
    settings.ttl_days = ttl_days;
    settings.max_db_size_bytes = max_db_size_bytes;
    writer::update_settings(settings);
  }

  bool prune_now_for_tests() {
    return writer::prune_now_for_tests();
  }
#endif

}  // namespace session_history
