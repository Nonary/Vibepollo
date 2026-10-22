/**
 * @file src/platform/linux/private_display_diagnostic_policy.h
 * @brief Stage-specific outcome labels for Linux private-display diagnostics.
 */
#pragma once

#include <optional>
#include <string_view>

namespace platf::linux_private_display::diagnostic_policy {
  enum class configuration_e {
    primary,
    isolated_layout_retry,
    hdr_activation,
  };

  enum class configuration_result_e {
    not_attempted,
    succeeded,
    failed,
  };

  enum class readback_result_e {
    not_observed,
    unavailable,
    output_missing,
    disconnected,
    disabled,
    incomplete,
    mode_mismatch,
    size_mismatch,
    refresh_mismatch,
    scale_mismatch,
    hdr_mismatch,
    matched,
  };

  enum class fallback_e {
    not_used,
    current_settings,
    no_capture_output,
  };

  struct trace_t {
    configuration_result_e primary {configuration_result_e::not_attempted};
    configuration_result_e isolated_layout_retry {configuration_result_e::not_attempted};
    configuration_result_e hdr_activation {configuration_result_e::not_attempted};
    readback_result_e readback {readback_result_e::not_observed};
    readback_result_e pre_fallback_readback {readback_result_e::not_observed};
    readback_result_e fallback_readback {readback_result_e::not_observed};
    fallback_e fallback {fallback_e::not_used};
  };

  struct readback_facts_t {
    bool query_available = false;
    bool requested_mode_known = false;
    bool output_present = false;
    bool connection_observed = false;
    bool output_connected = false;
    bool enabled_observed = false;
    bool output_enabled = false;
    bool mode_id_observed = false;
    bool size_required = false;
    bool size_observed = false;
    bool refresh_required = false;
    bool refresh_expected = false;
    bool refresh_observed = false;
    bool scale_required = false;
    bool scale_observed = false;
    bool hdr_required = false;
    bool hdr_observed = false;
    bool mode_id_matches = true;
    bool size_matches = true;
    bool refresh_matches = true;
    bool scale_matches = true;
    bool hdr_matches = true;
  };

  inline void record_configuration_result(
    trace_t &trace,
    const bool succeeded,
    const configuration_e operation
  ) noexcept {
    const auto result = succeeded ? configuration_result_e::succeeded : configuration_result_e::failed;
    switch (operation) {
      case configuration_e::primary:
        trace.primary = result;
        break;
      case configuration_e::isolated_layout_retry:
        trace.isolated_layout_retry = result;
        break;
      case configuration_e::hdr_activation:
        trace.hdr_activation = result;
        break;
    }
  }

  [[nodiscard]] inline std::optional<double> expected_refresh_hz(
    const std::optional<double> requested_hz,
    const std::optional<double> selected_mode_hz,
    const bool prefer_highest
  ) noexcept {
    return prefer_highest ? selected_mode_hz : requested_hz;
  }

  [[nodiscard]] inline readback_result_e classify_readback(const readback_facts_t &facts) noexcept {
    if (!facts.query_available) {
      return readback_result_e::unavailable;
    }
    if (!facts.output_present) {
      return readback_result_e::output_missing;
    }
    if (facts.connection_observed && !facts.output_connected) {
      return readback_result_e::disconnected;
    }
    if (facts.enabled_observed && !facts.output_enabled) {
      return readback_result_e::disabled;
    }
    if (!facts.requested_mode_known) {
      return readback_result_e::not_observed;
    }
    if (facts.mode_id_observed && !facts.mode_id_matches) {
      return readback_result_e::mode_mismatch;
    }
    if (facts.size_required && facts.size_observed && !facts.size_matches) {
      return readback_result_e::size_mismatch;
    }
    if (facts.refresh_required && facts.refresh_expected && facts.refresh_observed && !facts.refresh_matches) {
      return readback_result_e::refresh_mismatch;
    }
    if (facts.scale_required && facts.scale_observed && !facts.scale_matches) {
      return readback_result_e::scale_mismatch;
    }
    if (facts.hdr_required && facts.hdr_observed && !facts.hdr_matches) {
      return readback_result_e::hdr_mismatch;
    }
    if (!facts.connection_observed || !facts.enabled_observed || !facts.mode_id_observed || (facts.size_required && !facts.size_observed) || (facts.refresh_required && (!facts.refresh_expected || !facts.refresh_observed)) || (facts.scale_required && !facts.scale_observed) || (facts.hdr_required && !facts.hdr_observed)) {
      return readback_result_e::incomplete;
    }
    return readback_result_e::matched;
  }

  inline void begin_fallback(trace_t &trace) noexcept {
    trace.pre_fallback_readback = trace.readback;
    trace.readback = readback_result_e::not_observed;
    trace.fallback_readback = readback_result_e::not_observed;
  }

  inline void record_readback(
    trace_t &trace,
    const readback_facts_t &facts,
    const bool during_fallback = false
  ) noexcept {
    trace.readback = classify_readback(facts);
    if (during_fallback) {
      trace.fallback_readback = trace.readback;
    }
  }

  [[nodiscard]] inline fallback_e fallback_choice(const bool capture_published) noexcept {
    return capture_published ? fallback_e::current_settings : fallback_e::no_capture_output;
  }

  [[nodiscard]] inline std::string_view name(const configuration_result_e result) noexcept {
    switch (result) {
      case configuration_result_e::not_attempted:
        return "not_attempted";
      case configuration_result_e::succeeded:
        return "succeeded";
      case configuration_result_e::failed:
        return "failed";
    }
    return "unknown";
  }

  [[nodiscard]] inline std::string_view name(const readback_result_e result) noexcept {
    switch (result) {
      case readback_result_e::not_observed:
        return "not_observed";
      case readback_result_e::unavailable:
        return "unavailable";
      case readback_result_e::output_missing:
        return "output_missing";
      case readback_result_e::disconnected:
        return "disconnected";
      case readback_result_e::disabled:
        return "disabled";
      case readback_result_e::incomplete:
        return "incomplete";
      case readback_result_e::mode_mismatch:
        return "mode_mismatch";
      case readback_result_e::size_mismatch:
        return "size_mismatch";
      case readback_result_e::refresh_mismatch:
        return "refresh_mismatch";
      case readback_result_e::scale_mismatch:
        return "scale_mismatch";
      case readback_result_e::hdr_mismatch:
        return "hdr_mismatch";
      case readback_result_e::matched:
        return "matched";
    }
    return "unknown";
  }

  [[nodiscard]] inline std::string_view name(const fallback_e result) noexcept {
    switch (result) {
      case fallback_e::not_used:
        return "not_used";
      case fallback_e::current_settings:
        return "current_settings";
      case fallback_e::no_capture_output:
        return "no_capture_output";
    }
    return "unknown";
  }
}  // namespace platf::linux_private_display::diagnostic_policy
