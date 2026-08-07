/**
 * @file src/platform/windows/rtx_hdr_policy.h
 * @brief Dependency-free RTX HDR eligibility and profile precedence policy.
 */
#pragma once

#include <cstdint>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace platf::rtx_hdr {

  enum class profile_source_e { none, application, global, config };

  struct profile_values_t {
    std::optional<bool> enabled;
    std::optional<int> contrast;
    std::optional<int> saturation;
    std::optional<int> middle_gray;
    std::optional<int> peak_brightness;
    bool has_any() const { return enabled || contrast || saturation || middle_gray || peak_brightness; }
  };

  struct resolved_profile_t {
    bool lookup_available {false};
    profile_source_e source {profile_source_e::none};
    profile_values_t application;
    profile_values_t global;
    std::string executable;
    std::string profile_name;
  };

  struct runtime_values_t {
    bool enabled {false};
    int contrast {100};
    int saturation {100};
    int middle_gray {50};
    int sdr_brightness {0};
    int peak_brightness {1000};
    profile_source_e source {profile_source_e::none};
  };

  namespace policy {
    struct overrides_t {
      bool enable {false};
      bool contrast {false};
      bool saturation {false};
      bool middle_gray {false};
      bool peak_brightness {false};
    };

    runtime_values_t materialize(const resolved_profile_t &resolved, const runtime_values_t &config, const overrides_t &overrides);
    runtime_values_t desktop_values(const runtime_values_t &config, bool runtime_enabled);
    std::optional<bool> decode_activation(std::optional<std::uint32_t> driver_flags, std::optional<std::uint32_t> profile_enable);
    std::optional<int> decode_percent_units(std::uint32_t raw);
    float sdr_brightness_to_white_nits(int brightness);
    bool playnite_foreground_matches(std::string_view active_playnite_id, std::string_view status_id, std::string_view status_exe, std::string_view status_install_dir, std::string_view foreground_exe);

    struct foreground_state_t {
      bool has_active_app {false};
      bool matches_active_app {false};
      std::string foreground_exe;
      std::string active_app_exe;
      std::string active_app_name;
      std::string source;
    };

    struct scheduler_frame_t: runtime_values_t {
      bool has_active_app {false};
      bool foreground_matches {false};
      bool lookup_available {false};
      std::string foreground_exe;
      std::string active_app_exe;
    };

    class scheduler_t {
    public:
      void observe_foreground(const foreground_state_t &foreground, std::chrono::milliseconds now, const runtime_values_t &config, const overrides_t &overrides);
      bool complete_profile_lookup(const resolved_profile_t &profile, std::chrono::milliseconds elapsed, std::chrono::milliseconds now, const runtime_values_t &config, const overrides_t &overrides);
      void refresh_live_settings(const runtime_values_t &config, const overrides_t &overrides);
      const scheduler_frame_t &frame() const { return frame_; }
      bool lookup_pending() const { return pending_; }
      std::chrono::milliseconds refresh_interval() const { return refresh_interval_; }

    private:
      void apply(const runtime_values_t &values);
      std::string identity_;
      std::uint64_t generation_ {0};
      std::uint64_t pending_generation_ {0};
      bool pending_ {false};
      std::chrono::milliseconds next_refresh_ {0};
      std::chrono::milliseconds refresh_interval_ {std::chrono::seconds(5)};
      std::optional<resolved_profile_t> last_profile_;
      scheduler_frame_t frame_;
    };
  }
}
