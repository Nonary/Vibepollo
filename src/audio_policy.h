/**
 * @file src/audio_policy.h
 * @brief Platform-neutral audio stream and capture decisions.
 */
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace audio::policy {
  struct stream_layout_t {
    int channels;
    int streams;
    int coupled_streams;
    std::array<unsigned char, 8> mapping;
  };

  int stream_index(int channels, bool high_quality);
  stream_layout_t apply_custom_layout(stream_layout_t base, const std::optional<stream_layout_t> &custom);

  struct sink_catalog_t {
    std::string host;
    std::optional<std::string> stereo;
    std::optional<std::string> surround51;
    std::optional<std::string> surround71;
  };

  std::string select_sink(const sink_catalog_t &catalog,
                          const std::string &configured_sink,
                          int channels,
                          bool host_audio_enabled);

  struct render_endpoint_t {
    std::string id;
    std::string adapter_name;
    bool active;
  };

  struct render_endpoint_catalog_t {
    bool complete;
    std::vector<render_endpoint_t> endpoints;
    std::vector<std::string> steam_endpoint_ids;
    std::vector<std::string> eligible_non_steam_endpoint_ids;
  };

  struct host_mute_visibility_plan_t {
    std::vector<std::string> show_on_connect;
    std::vector<std::string> hide_on_connect;
    std::vector<std::string> hide_on_teardown;
    std::vector<std::string> show_on_teardown;
  };

  bool is_steam_streaming_render_adapter(std::string_view adapter_name);
  render_endpoint_catalog_t build_render_endpoint_catalog(
    bool discovery_complete,
    const std::vector<render_endpoint_t> &endpoints
  );
  std::optional<std::string> select_eligible_non_steam_render_endpoint(
    const render_endpoint_catalog_t &catalog,
    const std::vector<std::string> &preferred_ids
  );
  std::optional<std::string> select_eligible_non_steam_render_endpoint(
    const std::vector<render_endpoint_t> &endpoints,
    const std::vector<std::string> &preferred_ids
  );

  std::optional<host_mute_visibility_plan_t> plan_host_mute_visibility(
    bool discovery_complete,
    const std::vector<render_endpoint_t> &endpoints,
    const std::vector<std::string> &virtual_ids
  );

  int sink_assignment_result(bool assignment_active, int role_failures);

  enum class sample_status_e {
    ok,
    timeout,
    reinitialize,
    interrupted,
    error,
  };

  enum class sample_action_e {
    emit,
    retry,
    reacquire,
    stop,
  };

  sample_action_e sample_action(sample_status_e status);

  class sample_source_t {
  public:
    virtual ~sample_source_t() = default;
    virtual sample_status_e sample() = 0;
    virtual bool reacquire() = 0;
  };

  struct capture_summary_t {
    std::size_t emitted = 0;
    std::size_t timeouts = 0;
    std::size_t reacquisitions = 0;
    bool stopped = false;
  };

  capture_summary_t drive_capture(sample_source_t &source, std::size_t event_limit);
}  // namespace audio::policy
