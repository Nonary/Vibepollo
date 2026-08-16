/**
 * @file src/audio_policy.cpp
 * @brief Platform-neutral audio stream and capture decisions.
 */
#include "audio_policy.h"

#include <algorithm>

namespace audio::policy {
  int stream_index(int channels, bool high_quality) {
    const int quality_offset = high_quality ? 1 : 0;
    switch (channels) {
      case 2:
        return quality_offset;
      case 6:
        return 2 + quality_offset;
      case 8:
        return 4 + quality_offset;
      default:
        return 0;
    }
  }

  stream_layout_t apply_custom_layout(stream_layout_t base, const std::optional<stream_layout_t> &custom) {
    return custom.value_or(base);
  }

  std::string select_sink(const sink_catalog_t &catalog,
                          const std::string &configured_sink,
                          int channels,
                          bool host_audio_enabled) {
    std::string selected = configured_sink.empty() ? catalog.host : configured_sink;
    if (!host_audio_enabled || selected.empty()) {
      const std::optional<std::string> *virtual_sink = nullptr;
      switch (channels) {
        case 2:
          virtual_sink = &catalog.stereo;
          break;
        case 6:
          virtual_sink = &catalog.surround51;
          break;
        case 8:
          virtual_sink = &catalog.surround71;
          break;
      }
      if (virtual_sink && *virtual_sink) {
        selected = **virtual_sink;
      }
    }
    return selected;
  }

  bool is_steam_streaming_render_adapter(std::string_view adapter_name) {
    return adapter_name == "Steam Streaming Speakers" ||
           adapter_name == "Steam Streaming Microphone";
  }

  render_endpoint_catalog_t build_render_endpoint_catalog(
    bool discovery_complete,
    const std::vector<render_endpoint_t> &endpoints
  ) {
    render_endpoint_catalog_t catalog {
      discovery_complete,
      endpoints,
      {},
      {},
    };
    for (const auto &endpoint : endpoints) {
      if (!endpoint.active) {
        continue;
      }
      if (endpoint.id.empty() || endpoint.adapter_name.empty()) {
        catalog.complete = false;
        continue;
      }
      if (is_steam_streaming_render_adapter(endpoint.adapter_name)) {
        catalog.steam_endpoint_ids.push_back(endpoint.id);
      } else {
        catalog.eligible_non_steam_endpoint_ids.push_back(endpoint.id);
      }
    }
    return catalog;
  }

  std::optional<std::string> select_eligible_non_steam_render_endpoint(
    const render_endpoint_catalog_t &catalog,
    const std::vector<std::string> &preferred_ids
  ) {
    if (!catalog.complete) {
      return std::nullopt;
    }

    for (const auto &preferred_id : preferred_ids) {
      if (!preferred_id.empty() &&
          std::find(
            catalog.eligible_non_steam_endpoint_ids.begin(),
            catalog.eligible_non_steam_endpoint_ids.end(),
            preferred_id
          ) != catalog.eligible_non_steam_endpoint_ids.end()) {
        return preferred_id;
      }
    }

    if (!catalog.eligible_non_steam_endpoint_ids.empty()) {
      return catalog.eligible_non_steam_endpoint_ids.front();
    }
    return std::nullopt;
  }

  std::optional<std::string> select_eligible_non_steam_render_endpoint(
    const std::vector<render_endpoint_t> &endpoints,
    const std::vector<std::string> &preferred_ids
  ) {
    return select_eligible_non_steam_render_endpoint(
      build_render_endpoint_catalog(true, endpoints),
      preferred_ids
    );
  }

  sample_action_e sample_action(sample_status_e status) {
    switch (status) {
      case sample_status_e::ok:
        return sample_action_e::emit;
      case sample_status_e::timeout:
        return sample_action_e::retry;
      case sample_status_e::reinitialize:
        return sample_action_e::reacquire;
      case sample_status_e::interrupted:
      case sample_status_e::error:
        return sample_action_e::stop;
    }
    return sample_action_e::stop;
  }

  capture_summary_t drive_capture(sample_source_t &source, std::size_t event_limit) {
    capture_summary_t summary;
    for (std::size_t event = 0; event < event_limit; ++event) {
      switch (sample_action(source.sample())) {
        case sample_action_e::emit:
          ++summary.emitted;
          break;
        case sample_action_e::retry:
          ++summary.timeouts;
          break;
        case sample_action_e::reacquire:
          ++summary.reacquisitions;
          if (!source.reacquire()) {
            summary.stopped = true;
            return summary;
          }
          break;
        case sample_action_e::stop:
          summary.stopped = true;
          return summary;
      }
    }
    return summary;
  }
}  // namespace audio::policy
