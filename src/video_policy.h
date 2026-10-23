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
  /**
   * What an encode loop does with an image after convert(): whether to encode it,
   * and the timestamps to encode it with.
   */
  struct converted_input_t {
    bool encode = true;
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
    std::optional<std::chrono::steady_clock::time_point> host_processing_timestamp;
  };

  /**
   * A conversion that read pixels this session already sent is skipped, not
   * encoded again. Otherwise the frame the conversion actually read decides the
   * timestamps; an image whose pixels were not replaced keeps the stamps its
   * capture claimed. `Converted` is platf::converted_frame_t.
   */
  template<class Converted>
  converted_input_t resolve_converted_input(
    const Converted &converted,
    const std::optional<std::chrono::steady_clock::time_point> &claimed_frame_timestamp,
    const std::optional<std::chrono::steady_clock::time_point> &claimed_host_processing_timestamp
  ) {
    if (converted.repeats_previous) {
      return {false, std::nullopt, std::nullopt};
    }
    return {
      true,
      converted.frame_timestamp ? converted.frame_timestamp : claimed_frame_timestamp,
      converted.host_processing_timestamp ? converted.host_processing_timestamp : claimed_host_processing_timestamp,
    };
  }

  /**
   * A repeated input is skipped, which must not swallow a keyframe requested in
   * the same pass: the request is carried forward so the last input is encoded as
   * the IDR without waiting for another capture. A repeat with no request is a
   * plain skip, and an input that was converted never needs a retry.
   */
  inline bool retry_idr_after_repeat(const converted_input_t &input, bool idr_requested) {
    return !input.encode && idr_requested;
  }

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

  std::optional<std::string> select_manual_display_output(
    capture_selection_e selection,
    int requested_index,
    std::span<const std::string> display_names
  );

  std::optional<int> resolve_display_output(
    std::string_view output_identity,
    std::span<const std::string> display_names
  );

  /** Delay repeated display discovery without busy-looping during a persistent topology failure. */
  [[nodiscard]] std::chrono::milliseconds display_retry_delay(std::size_t consecutive_failures);

  /** Log the first failure and exponentially sparse reminders for a persistent retry loop. */
  [[nodiscard]] bool should_log_display_retry(std::size_t consecutive_failures);

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
