#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace video::policy {
  template<class Queue, class Context, class ShutdownSignal, class JoinSignal>
  bool try_admit_capture_session(
    Queue &queue,
    Context &&context,
    ShutdownSignal &shutdown_signal,
    JoinSignal &join_signal
  ) {
    if (queue.try_raise(std::forward<Context>(context))) {
      return true;
    }

    shutdown_signal.raise(true);
    join_signal.raise(true);
    return false;
  }

  enum class capture_selection_e : std::uint8_t { process_preferred, exact_output, synthetic_black };

  [[nodiscard]] bool may_apply_process_display_preference(capture_selection_e selection);

  /** Delay repeated display discovery without busy-looping during a persistent topology failure. */
  [[nodiscard]] std::chrono::milliseconds display_retry_delay(std::size_t consecutive_failures);

  /** Log the first failure and exponentially sparse reminders for a persistent retry loop. */
  [[nodiscard]] bool should_log_display_retry(std::size_t consecutive_failures);

  std::optional<std::string> select_manual_display_output(
    capture_selection_e selection,
    int requested_index,
    std::span<const std::string> display_names
  );

  std::optional<int> resolve_display_output(
    std::string_view output_identity,
    std::span<const std::string> display_names
  );

  struct rational_t {
    int numerator;
    int denominator;
    friend bool operator==(const rational_t &, const rational_t &) = default;
  };

  rational_t framerate_x100_to_rational(std::int32_t value);

  struct encoder_requirements_t {
    bool hdr = false;
    bool yuv444 = false;
  };
  struct encoder_capabilities_t {
    bool available = false;
    bool hdr = false;
    bool yuv444 = false;
  };
  class encoder_capability_provider_t {
  public:
    virtual ~encoder_capability_provider_t() = default;
    virtual encoder_capabilities_t capabilities(std::string_view encoder) const = 0;
  };

  std::optional<std::string> select_encoder(
    std::span<const std::string_view> preference,
    encoder_requirements_t requirements,
    const encoder_capability_provider_t &provider
  );

  std::optional<std::string> select_preferred_virtual_output(
    std::string_view configured_output,
    std::span<const std::string> active_virtual_outputs,
    std::span<const std::string> all_virtual_outputs
  );
}  // namespace video::policy
