/**
 * @file src/amf/amf_lifecycle.h
 * @brief Small, driver-independent AMF lifecycle primitives.
 *
 * These helpers intentionally contain no AMF or D3D types. Production uses them
 * for input ownership, bounded retry, and teardown; tests can therefore exercise
 * the same state transitions with a fake AMF implementation.
 */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace amf::lifecycle {

  enum class bitrate_update_result_e {
    applied,
    ignored_for_mode,
    temporarily_busy,
    requires_rebuild,
    vendor_timed_out
  };

  enum class bitrate_loop_action_e {
    complete,
    defer_latest,
    rebuild_once,
    abandon_generation
  };

  inline constexpr bitrate_loop_action_e bitrate_loop_action(
    bitrate_update_result_e result) noexcept {
    switch (result) {
      case bitrate_update_result_e::applied:
      case bitrate_update_result_e::ignored_for_mode:
        return bitrate_loop_action_e::complete;
      case bitrate_update_result_e::temporarily_busy:
        return bitrate_loop_action_e::defer_latest;
      case bitrate_update_result_e::requires_rebuild:
        return bitrate_loop_action_e::rebuild_once;
      case bitrate_update_result_e::vendor_timed_out:
        return bitrate_loop_action_e::abandon_generation;
    }
    return bitrate_loop_action_e::abandon_generation;
  }

  // Frame/coalescing deadlines protect stream latency. Once a call has actually
  // entered the vendor runtime, use a separate watchdog so scheduler jitter can
  // never be mistaken for a wedged AMD driver.
  inline constexpr auto driver_call_watchdog_timeout = std::chrono::seconds(5);

  template<typename Clock, typename Duration>
  constexpr std::chrono::time_point<Clock, Duration> caller_acceptance_deadline(
    const std::chrono::time_point<Clock, Duration> &overall_deadline,
    const std::chrono::time_point<Clock, Duration> &vendor_deadline) noexcept {
    return std::min(overall_deadline, vendor_deadline);
  }

  template<typename Clock, typename Duration, typename Rep, typename Period>
  constexpr bool full_watchdog_interval_fits(
    const std::chrono::time_point<Clock, Duration> &overall_deadline,
    const std::chrono::time_point<Clock, Duration> &now,
    const std::chrono::duration<Rep, Period> &watchdog_interval) noexcept {
    return now <= overall_deadline && overall_deadline - now >= watchdog_interval;
  }

  inline constexpr bool capture_generation_requires_preservation(
    bool capture_uses_amf,
    bool runtime_quarantined,
    bool runtime_operation_in_progress,
    std::uint32_t scheduled_workers,
    bool encoder_still_owns_generation = false) noexcept {
    return capture_uses_amf &&
           (runtime_quarantined || runtime_operation_in_progress || scheduled_workers != 0 ||
            encoder_still_owns_generation);
  }

  inline constexpr std::chrono::steady_clock::duration teardown_watchdog_within_deadline(
    const std::chrono::steady_clock::time_point &overall_deadline,
    const std::chrono::steady_clock::time_point &now,
    const std::chrono::steady_clock::duration &maximum_watchdog) noexcept {
    if (now >= overall_deadline) {
      return std::chrono::steady_clock::duration::zero();
    }
    return std::min(maximum_watchdog, overall_deadline - now);
  }

  inline constexpr bool logical_input_requires_immediate_retry(
    bool input_accepted,
    bool fresh_conversion_pending,
    bool force_idr_pending) noexcept {
    return !input_accepted && (fresh_conversion_pending || force_idr_pending);
  }

  inline constexpr std::chrono::steady_clock::time_point output_or_image_wait_deadline(
    const std::chrono::steady_clock::time_point &image_deadline,
    bool preanalysis_tail_retained,
    const std::chrono::steady_clock::time_point &tail_started,
    const std::chrono::steady_clock::duration &tail_budget) noexcept {
    return preanalysis_tail_retained ?
             std::min(image_deadline, tail_started + tail_budget) :
             image_deadline;
  }

  class native_runtime_gate_t {
  public:
    template<typename Rep, typename Period>
    bool begin_initialization_for(const std::chrono::duration<Rep, Period> &timeout) {
      std::unique_lock lock(mutex);
      if (!state_changed.wait_for(lock, timeout, [&]() {
            return quarantined || (!initialization_in_progress &&
                                   pending_teardowns == 0 &&
                                   teardowns_in_progress == 0);
          }) || quarantined) {
        return false;
      }
      initialization_in_progress = true;
      return true;
    }

    bool try_begin_initialization() {
      return begin_initialization_for(std::chrono::steady_clock::duration::zero());
    }

    bool finish_initialization() {
      std::lock_guard lock(mutex);
      const bool publish = initialization_in_progress && !quarantined &&
                           pending_teardowns == 0 && teardowns_in_progress == 0;
      initialization_in_progress = false;
      state_changed.notify_all();
      return publish;
    }

    void cancel_initialization() {
      std::lock_guard lock(mutex);
      initialization_in_progress = false;
      state_changed.notify_all();
    }

    void quarantine_initialization() {
      std::lock_guard lock(mutex);
      quarantined = true;
      state_changed.notify_all();
    }

    bool reserve_teardown() {
      std::lock_guard lock(mutex);
      if (quarantined) return false;
      ++pending_teardowns;
      state_changed.notify_all();
      return true;
    }

    void cancel_teardown_reservation() {
      std::lock_guard lock(mutex);
      if (pending_teardowns != 0) {
        --pending_teardowns;
      }
      state_changed.notify_all();
    }

    template<typename Clock, typename Duration>
    bool begin_reserved_teardown_until(const std::chrono::time_point<Clock, Duration> &deadline) {
      std::unique_lock lock(mutex);
      if (pending_teardowns == 0) return false;
      const bool available = state_changed.wait_until(lock, deadline, [&]() {
        return quarantined || (!initialization_in_progress && teardowns_in_progress == 0);
      });
      if (!available) {
        // The caller still owns the reservation and may transfer it to a
        // deferred worker without opening an initialization window.
        return false;
      }
      --pending_teardowns;
      if (quarantined) {
        state_changed.notify_all();
        return false;
      }
      ++teardowns_in_progress;
      return true;
    }

    bool begin_reserved_teardown() {
      return begin_reserved_teardown_until(std::chrono::steady_clock::time_point::max());
    }

    template<typename Clock, typename Duration>
    bool begin_teardown_until(const std::chrono::time_point<Clock, Duration> &deadline) {
      if (!reserve_teardown()) return false;
      if (begin_reserved_teardown_until(deadline)) return true;
      if (!is_quarantined()) cancel_teardown_reservation();
      return false;
    }

    bool begin_teardown() {
      return begin_teardown_until(std::chrono::steady_clock::time_point::max());
    }

    void finish_teardown(bool completed) {
      std::lock_guard lock(mutex);
      if (completed && teardowns_in_progress != 0) {
        --teardowns_in_progress;
      }
      if (!completed) {
        // The owning worker may still be executing vendor code after its
        // watchdog expires. Keep the teardown fence occupied until process
        // restart as well as quarantining new initialization; decrementing here
        // would permit another teardown to overlap the abandoned call.
        quarantined = true;
      }
      state_changed.notify_all();
    }

    bool is_quarantined() const {
      std::lock_guard lock(mutex);
      return quarantined;
    }

    bool legacy_fallback_is_safe() const {
      std::lock_guard lock(mutex);
      return !quarantined && !initialization_in_progress &&
             pending_teardowns == 0 && teardowns_in_progress == 0;
    }

    bool operation_in_progress() const {
      std::lock_guard lock(mutex);
      return initialization_in_progress || pending_teardowns != 0 || teardowns_in_progress != 0;
    }

  private:
    mutable std::mutex mutex;
    std::condition_variable state_changed;
    bool quarantined = false;
    bool initialization_in_progress = false;
    std::size_t pending_teardowns = 0;
    std::size_t teardowns_in_progress = 0;
  };

  // A timeout-safe, two-party ownership transfer. The producing worker cannot
  // exit after publishing until the caller explicitly accepts or abandons the
  // value. Consequently a timeout always leaves candidate destruction on the
  // worker, never on the caller through a last-owner future race.
  template<typename T>
  class worker_handoff_t {
  public:
    bool publish(T value) {
      std::unique_lock lock(mutex);
      if (abandoned) {
        // Destruction is part of producer completion. Move the incoming value to
        // a local holder and reap it without the handoff mutex; the cancellation
        // supervisor must remain able to acquire the mutex, expire its deadline,
        // and quarantine the runtime if that destructor wedges.
        auto abandoned_value = std::make_unique<T>(std::move(value));
        lock.unlock();
        abandoned_value.reset();
        lock.lock();
        producer_finished = true;
        changed.notify_all();
        return false;
      }
      candidate.emplace(std::move(value));
      ready = true;
      changed.notify_all();
      changed.wait(lock, [&]() { return accepted || abandoned; });
      if (abandoned) {
        // Explicitly destroy on the producer before it can drop its shared state
        // reference; otherwise the caller may become the last handoff owner at
        // the timeout boundary and run the candidate destructor itself. Reap
        // outside the mutex so deadline supervision cannot be blocked by it.
        auto abandoned_value = std::make_unique<T>(std::move(*candidate));
        candidate.reset();
        lock.unlock();
        abandoned_value.reset();
        lock.lock();
        producer_finished = true;
        changed.notify_all();
        return false;
      }
      producer_finished = true;
      changed.notify_all();
      return accepted;
    }

    template<typename Clock, typename Duration, typename Cancelled>
    std::optional<T> accept_until(
      const std::chrono::time_point<Clock, Duration> &deadline,
      Cancelled &&cancelled,
      bool *was_cancelled = nullptr) {
      if (was_cancelled) *was_cancelled = false;
      std::unique_lock lock(mutex);
      while (!ready && !abandoned) {
        if (cancelled()) {
          if (was_cancelled) *was_cancelled = true;
          abandoned = true;
          changed.notify_all();
          return std::nullopt;
        }
        if (Clock::now() >= deadline) {
          abandoned = true;
          changed.notify_all();
          return std::nullopt;
        }
        const auto poll_interval = std::chrono::duration_cast<typename Clock::duration>(
          std::chrono::milliseconds(50));
        changed.wait_until(lock, std::min(deadline, Clock::now() + poll_interval));
      }
      if (!ready || abandoned) return std::nullopt;
      auto result = std::move(candidate);
      candidate.reset();
      accepted = true;
      changed.notify_all();
      return result;
    }

    template<typename Clock, typename Duration>
    bool producer_finished_until(const std::chrono::time_point<Clock, Duration> &deadline) {
      std::unique_lock lock(mutex);
      return changed.wait_until(lock, deadline, [&]() { return producer_finished; });
    }

  private:
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<T> candidate;
    bool ready = false;
    bool accepted = false;
    bool abandoned = false;
    bool producer_finished = false;
  };

  enum class input_surface_state_e {
    free,
    reserved,
    in_flight,
    // A released, application-owned texture pinned as the source for a future
    // static refresh. AMF never owns a slot in this state and the converter may
    // not select it for rendering until a newer replay source replaces it.
    replay_source
  };

  struct input_surface_state_t {
    uint64_t frame_index = 0;
    input_surface_state_e state = input_surface_state_e::free;
    bool release_notified = false;
  };

  inline bool rate_control_supports_adaptive_quantization(const std::optional<int> &rate_control) noexcept {
    // All three AMF encoders use zero for CONSTANT_QP. AMD documents VBAQ/CAQ
    // as incompatible with CQP, so an enabled-by-default AQ setting must not turn
    // an otherwise valid CQP request into a driver-dependent configuration.
    return !rate_control || *rate_control != 0;
  }

  enum class rate_control_mode_e : int {
    cqp = 0,
    cbr = 1,
    peak_constrained_vbr = 2,
    latency_constrained_vbr = 3,
    qvbr = 4,
    hqvbr = 5,
    hqcbr = 6
  };

  struct rate_control_policy_t {
    bool valid = true;
    bool requires_preanalysis = false;
    bool uses_target_bitrate = false;
    bool uses_peak_bitrate = false;
    bool uses_vbv = false;
    bool supports_live_bitrate = false;
  };

  struct live_bitrate_plan_t {
    bool ignored_for_mode = false;
    bool write_target = false;
    bool write_peak = false;
    bool write_vbv = false;
    bool write_maximum_frame_size = false;
  };

  inline constexpr rate_control_policy_t rate_control_policy(
    const std::optional<int> &rate_control) noexcept {
    if (!rate_control) {
      // Preserve the AMF preset controller. Sunshine historically overrides only
      // its dynamic target in automatic mode.
      return {.uses_target_bitrate = true, .supports_live_bitrate = true};
    }

    switch (static_cast<rate_control_mode_e>(*rate_control)) {
      case rate_control_mode_e::cqp:
        return {};
      case rate_control_mode_e::cbr:
        return {.uses_target_bitrate = true, .uses_peak_bitrate = true, .uses_vbv = true, .supports_live_bitrate = true};
      case rate_control_mode_e::peak_constrained_vbr:
      case rate_control_mode_e::latency_constrained_vbr:
        return {.uses_target_bitrate = true, .uses_peak_bitrate = true, .uses_vbv = true, .supports_live_bitrate = true};
      case rate_control_mode_e::qvbr:
        // AMD defines QVBR by its quality level and explicitly does not define a
        // target bitrate. Bitrate/VBV fields are therefore inactive in this mode.
        return {.requires_preanalysis = true};
      case rate_control_mode_e::hqvbr:
        return {.requires_preanalysis = true, .uses_target_bitrate = true, .uses_peak_bitrate = true, .uses_vbv = true, .supports_live_bitrate = true};
      case rate_control_mode_e::hqcbr:
        return {.requires_preanalysis = true, .uses_target_bitrate = true, .uses_peak_bitrate = true, .uses_vbv = true, .supports_live_bitrate = true};
    }
    return {.valid = false};
  }

  inline bool rate_control_uses_bitrate_updates(const std::optional<int> &rate_control) noexcept {
    return rate_control_policy(rate_control).supports_live_bitrate;
  }

  inline bool rate_control_requires_preanalysis(int mode) noexcept {
    return rate_control_policy(mode).requires_preanalysis;
  }

  inline constexpr live_bitrate_plan_t live_bitrate_plan(
    const rate_control_policy_t &policy,
    int video_format,
    bool enforce_hrd) noexcept {
    if (!policy.supports_live_bitrate) {
      return {.ignored_for_mode = true};
    }
    return {
      .write_target = policy.uses_target_bitrate,
      .write_peak = policy.uses_peak_bitrate,
      // HEVC exposes VBV as a static property; H.264 and AV1 allow the live write.
      .write_vbv = policy.uses_vbv && video_format != 1,
      .write_maximum_frame_size = enforce_hrd
    };
  }

  inline constexpr bool runtime_supports_advanced_rate_control(
    int video_format,
    unsigned major,
    unsigned minor,
    unsigned subminor) noexcept {
    if (video_format < 0 || video_format > 2) return false;
    // AMF 1.4.28 independently introduced the new AVC/HEVC controllers and the
    // AV1 encoder/API which defines the corresponding QVBR/HQVBR/HQCBR modes.
    // Keep the codec argument explicit so future per-codec version changes do
    // not silently inherit another codec's gate.
    return major > 1 ||
           (major == 1 && (minor > 4 || (minor == 4 && subminor >= 28)));
  }

  inline constexpr bool native_pa_queue_retry_should_run(
    bool first_packet_delivered,
    bool compatibility_failure_detected,
    bool already_using_default_queue,
    bool retry_already_attempted) noexcept {
    return !first_packet_delivered && compatibility_failure_detected &&
           !already_using_default_queue && !retry_already_attempted;
  }

  inline constexpr bool normalized_contract_value_is_acceptable(
    std::int64_t applied,
    std::int64_t requested,
    std::int64_t absolute_tolerance,
    std::int64_t relative_tolerance_per_mille) noexcept {
    if (applied <= 0 || requested <= 0) return false;
    const auto relative_tolerance = requested * relative_tolerance_per_mille / 1000;
    const auto tolerance = std::max(absolute_tolerance, relative_tolerance);
    return applied >= requested - tolerance && applied <= requested + tolerance;
  }

  inline constexpr bool bitrate_contract_is_acceptable(
    std::int64_t target,
    std::int64_t peak,
    std::int64_t requested) noexcept {
    constexpr std::int64_t bitrate_rounding_tolerance = 64'000;
    constexpr std::int64_t bitrate_relative_tolerance_per_mille = 50;  // 5%
    return normalized_contract_value_is_acceptable(
             target, requested, bitrate_rounding_tolerance, bitrate_relative_tolerance_per_mille) &&
           normalized_contract_value_is_acceptable(
             peak, requested, bitrate_rounding_tolerance, bitrate_relative_tolerance_per_mille) &&
           peak >= target;
  }

  inline constexpr bool one_frame_vbv_is_acceptable(
    std::int64_t applied,
    std::int64_t requested) noexcept {
    // Drivers commonly round VBV to a hardware granularity. A materially larger
    // value changes the latency/FEC contract, while a similar-size value is the
    // intended one-frame buffer.
    return normalized_contract_value_is_acceptable(applied, requested, 4'096, 100);  // 10%
  }

  // AMD documents a lookahead depth of one for the ultra-low-latency usage
  // preset. Keep native streaming on that bounded pipeline instead of inheriting
  // the 11-frame default used by transcoding/high-quality presets.
  inline constexpr int low_latency_preanalysis_lookahead_depth = 1;

  // Native encoders can retain several inputs before the first output even
  // without B-frames or PreAnalysis. Three surfaces proved too small on current
  // Radeon drivers: once every wrapper was retained, the host could no longer
  // submit the input needed to advance the VCN queue. Keep two transit surfaces
  // beyond AMF's configured queue and additional room for PA lookahead. Production
  // can grow lazily to AMF's documented maximum queue plus that transit headroom.
  inline constexpr std::size_t minimum_input_surface_count = 4;
  inline constexpr std::size_t input_surface_transit_count = 2;
  inline constexpr std::size_t default_amf_input_queue_size = 16;
  inline constexpr std::size_t maximum_amf_input_queue_size = 32;
  inline constexpr std::size_t maximum_input_surface_count =
    maximum_amf_input_queue_size + input_surface_transit_count;

  inline constexpr std::size_t input_surface_count_for_lookahead(int lookahead_depth) noexcept {
    const auto requested = minimum_input_surface_count +
                           static_cast<std::size_t>(std::max(0, lookahead_depth)) * 2;
    return std::clamp(requested, minimum_input_surface_count, maximum_input_surface_count);
  }

  inline constexpr std::size_t initial_input_surface_count(int lookahead_depth) noexcept {
    // Start with only the lookahead/transit working set. The native encoder grows
    // this pool one texture at a time if a driver actually retains more inputs;
    // eagerly mirroring a 16/32-frame AMF queue wastes hundreds of MiB at 4K.
    return input_surface_count_for_lookahead(lookahead_depth);
  }

  inline std::optional<int> input_queue_size_for_preanalysis(
    const std::optional<int> &requested_queue_size,
    bool preanalysis_enabled) noexcept {
    (void) preanalysis_enabled;
    // PA needs shader-readable inputs and lookahead surface headroom, not a deep
    // submission queue. Preserve the user's low-latency queue choice; the surface
    // pool expands independently if a runtime retains more work than usual.
    return requested_queue_size;
  }

  inline constexpr bool driver_submit_capacity_available(
    std::size_t input_surfaces_in_flight,
    std::size_t encoder_input_queue_size) noexcept {
    // AMF's queue depth is a capacity, not the last safe occupied index. Once the
    // runtime owns exactly `queue_size` inputs, entering one more SubmitInput can
    // block in the driver before it has a chance to return AMF_INPUT_FULL.
    return input_surfaces_in_flight < encoder_input_queue_size;
  }

  inline constexpr bool normalized_input_queue_is_compatible(
    std::optional<int> requested,
    int applied,
    bool preanalysis_enabled) noexcept {
    if (applied < 1 || applied > static_cast<int>(maximum_amf_input_queue_size)) return false;
    if (!requested) return true;
    if (applied == *requested) return true;
    return preanalysis_enabled && *requested < static_cast<int>(default_amf_input_queue_size) &&
           applied == static_cast<int>(default_amf_input_queue_size);
  }

  inline constexpr bool cached_pa_queue_may_override(
    std::optional<int> explicitly_requested,
    int cached_queue) noexcept {
    if (cached_queue != static_cast<int>(default_amf_input_queue_size)) return false;
    // A learned queue-16 workaround may replace automatic or low-depth settings,
    // but must never clobber a later explicit queue that already provides at least
    // that much headroom (for example 24 or 32).
    return !explicitly_requested ||
           *explicitly_requested < static_cast<int>(default_amf_input_queue_size);
  }

  inline constexpr bool fresh_conversion_requires_replay_snapshot(
    bool preanalysis_enabled) noexcept {
    return preanalysis_enabled;
  }

  inline constexpr bool unaccepted_fresh_input_must_remain_reserved(
    bool repeated_input,
    bool fatal,
    bool input_accepted) noexcept {
    return !repeated_input && !fatal && !input_accepted;
  }

  enum class output_pts_result_e {
    accepted,
    pending_confirmation,
    missing,
    not_from_accepted_input,
    duplicate_or_regressing
  };

  class output_pts_tracker_t {
  public:
    void clear() {
      accepted_indices.clear();
      candidate_indices.clear();
      accepted_order.clear();
      last_completed.reset();
    }

    void note_input_candidate(std::uint64_t frame_index) {
      candidate_indices.emplace(frame_index);
    }

    void confirm_input_accepted(std::uint64_t frame_index) {
      candidate_indices.erase(frame_index);
      if (accepted_indices.emplace(frame_index).second) {
        accepted_order.push_back(frame_index);
      }
      while (accepted_order.size() > 1024) {
        accepted_indices.erase(accepted_order.front());
        accepted_order.pop_front();
      }
    }

    void discard_input_candidate(std::uint64_t frame_index) {
      candidate_indices.erase(frame_index);
    }

    output_pts_result_e classify(std::optional<std::uint64_t> frame_index) {
      if (!frame_index) return output_pts_result_e::missing;
      if (candidate_indices.contains(*frame_index)) {
        return output_pts_result_e::pending_confirmation;
      }
      const auto found = accepted_indices.find(*frame_index);
      if (found == accepted_indices.end()) {
        return output_pts_result_e::not_from_accepted_input;
      }
      accepted_indices.erase(found);
      if (last_completed && *frame_index <= *last_completed) {
        return output_pts_result_e::duplicate_or_regressing;
      }
      last_completed = *frame_index;
      return output_pts_result_e::accepted;
    }

    std::optional<std::uint64_t> last() const noexcept {
      return last_completed;
    }

  private:
    std::unordered_set<std::uint64_t> accepted_indices;
    std::unordered_set<std::uint64_t> candidate_indices;
    std::deque<std::uint64_t> accepted_order;
    std::optional<std::uint64_t> last_completed;
  };

  class monotonic_delivery_tracker_t {
  public:
    bool accept(std::uint64_t frame_index) noexcept {
      if (last_delivered && frame_index <= *last_delivered) return false;
      last_delivered = frame_index;
      return true;
    }

    std::optional<std::uint64_t> last() const noexcept {
      return last_delivered;
    }

  private:
    std::optional<std::uint64_t> last_delivered;
  };

  inline constexpr bool startup_budget_reset_allowed(
    bool client_visible_packet_delivered) noexcept {
    return client_visible_packet_delivered;
  }

  inline constexpr bool preanalysis_queue_fallback_is_recommended(
    bool preanalysis_enabled,
    bool saturation_observed,
    std::size_t encoder_input_queue_size,
    uint64_t completed_output_count) noexcept {
    // Retry only the compatibility failure observed on RDNA4: PA filled a
    // user-selected queue below AMF's documented default without producing its
    // first packet. Do not penalize adapters that make progress at queue 4.
    return preanalysis_enabled && saturation_observed &&
           encoder_input_queue_size < default_amf_input_queue_size &&
           completed_output_count == 0;
  }

  inline constexpr bool saturation_wait_should_finish(bool output_fatal,
                                                       bool capacity_available,
                                                       bool completed_output_ready = false) noexcept {
    // A packet that is already ready for the caller takes priority over waiting
    // for room for a newer input, preventing one frame of head-of-line blocking.
    return output_fatal || capacity_available || completed_output_ready;
  }

  inline constexpr bool output_wait_should_finish(bool output_fatal,
                                                   bool target_reached,
                                                   bool completed_output_ready) noexcept {
    return output_fatal || target_reached || completed_output_ready;
  }

  template<typename Clock, typename Duration>
  constexpr bool query_failure_is_persistent(
    const std::chrono::time_point<Clock, Duration> &failure_started,
    const std::chrono::time_point<Clock, Duration> &now,
    const std::chrono::milliseconds &threshold = std::chrono::seconds(1)) noexcept {
    return failure_started.time_since_epoch().count() != 0 &&
           now >= failure_started && now - failure_started >= threshold;
  }

  struct preanalysis_plan_t {
    bool enabled = false;
    int lookahead_depth = 0;
    bool enabled_for_rate_control = false;
  };

  inline preanalysis_plan_t resolve_preanalysis(const std::optional<int> &rate_control,
                                                const std::optional<int> &explicit_preanalysis) noexcept {
    const bool required_by_rate_control = rate_control && rate_control_requires_preanalysis(*rate_control);
    const bool explicitly_enabled = explicit_preanalysis && *explicit_preanalysis != 0;
    const bool enabled = required_by_rate_control || explicitly_enabled;
    return {
      enabled,
      enabled ? low_latency_preanalysis_lookahead_depth : 0,
      required_by_rate_control,
    };
  }

  // AMD's encoder sample requires this exact dependency order: rate control
  // first, then PA enable, then PA's dynamic lookahead properties.
  template<typename ApplyRateControl, typename ApplyPreanalysis, typename ApplyLookahead>
  bool apply_rate_control_and_preanalysis(
    const std::optional<int> &rate_control,
    bool configure_preanalysis,
    const preanalysis_plan_t &plan,
    int lookahead_depth,
    ApplyRateControl &&apply_rate_control,
    ApplyPreanalysis &&apply_preanalysis,
    ApplyLookahead &&apply_lookahead) {
    if (rate_control && !apply_rate_control(*rate_control)) {
      return false;
    }
    if (configure_preanalysis && !apply_preanalysis(plan.enabled)) {
      return false;
    }
    if (plan.enabled && !apply_lookahead(std::max(1, lookahead_depth))) {
      return false;
    }
    return true;
  }

  // A lookahead encoder can only be expected to emit once it has accepted more
  // inputs than it keeps for future-frame analysis. This gates waits/watchdogs;
  // it does not prevent QueryOutput from returning earlier if a driver can do so.
  inline bool delayed_output_is_expected(uint64_t accepted_input_count, int lookahead_depth) noexcept {
    return accepted_input_count > static_cast<uint64_t>(std::max(0, lookahead_depth));
  }

  inline constexpr bool should_disarm_output_poll(uint64_t queried_through_input,
                                                  uint64_t accepted_input_count,
                                                  bool drain_requested,
                                                  std::size_t active_poll_waiters) noexcept {
    // A no-data QueryOutput result only describes that call. It does not guarantee
    // that an already-submitted hardware job cannot complete a moment later. A
    // bounded encode-side waiter therefore owns a polling lease; disarming
    // underneath that waiter strands the completion until the next input.
    return !drain_requested && active_poll_waiters == 0 &&
           queried_through_input == accepted_input_count;
  }

  // Keep submission coalescing within one negotiated frame period. Sparse/static
  // delivery gets its longer, image-interruptible grace in the outer capture loop;
  // carrying that grace into encode_frame lets a fresh capture inherit a 32 ms wait.
  inline constexpr std::chrono::milliseconds output_coalesce_budget(int framerate) noexcept {
    const auto frame_period = framerate > 0 ?
                                std::chrono::milliseconds((1000 + framerate - 1) / framerate) :
                                std::chrono::milliseconds(17);
    return std::clamp(
      frame_period > std::chrono::milliseconds(1) ?
        frame_period - std::chrono::milliseconds(1) :
        std::chrono::milliseconds(1),
      std::chrono::milliseconds(1),
      std::chrono::milliseconds(32));
  }

  inline constexpr std::chrono::milliseconds driver_wait_budget(int framerate) noexcept {
    return std::min(std::chrono::milliseconds(20), output_coalesce_budget(framerate));
  }

  inline constexpr bool output_delivery_is_due(uint64_t accepted_input_count,
                                                uint64_t completed_output_count,
                                                int lookahead_depth,
                                                bool completed_output_queued) noexcept {
    const auto delay = static_cast<uint64_t>(std::max(0, lookahead_depth));
    const auto expected_output_count = accepted_input_count > delay ?
                                         accepted_input_count - delay :
                                         0;
    return completed_output_queued || completed_output_count < expected_output_count;
  }

  inline constexpr bool retained_preanalysis_tail_exists(uint64_t accepted_input_count,
                                                         uint64_t completed_output_count,
                                                         int lookahead_depth) noexcept {
    const auto depth = static_cast<uint64_t>(std::max(0, lookahead_depth));
    if (depth == 0 || accepted_input_count <= completed_output_count) {
      return false;
    }
    return accepted_input_count - completed_output_count <= depth;
  }

  inline constexpr bool preanalysis_tail_flush_is_due(bool retained_tail_exists,
                                                       bool suppressed_until_fresh_conversion) noexcept {
    return retained_tail_exists && !suppressed_until_fresh_conversion;
  }

  inline bool submit_backpressure_requires_reinit(
    int consecutive_exhaustions,
    int failure_threshold,
    bool sequence_start_known,
    std::chrono::steady_clock::duration time_since_sequence_start) noexcept {
    return consecutive_exhaustions >= std::max(1, failure_threshold) ||
           (sequence_start_known && time_since_sequence_start >= std::chrono::seconds(2));
  }

  inline constexpr bool output_coalesce_target_reached(uint64_t required_frame_index,
                                                        uint64_t completed_before_submission,
                                                        uint64_t completed_after_submission,
                                                        uint64_t last_completed_frame_index) noexcept {
    // encode_frame drains the completed queue before taking its completion-count
    // snapshot. The packet required by this submission may therefore already be
    // in the caller's result even though the count cannot increase again. Frame
    // generation, rather than count movement, is the authoritative condition.
    if (last_completed_frame_index >= required_frame_index) return true;

    if (completed_after_submission <= completed_before_submission) return false;

    // Require progress from the generation that became due with this input. A PA
    // completion observed during surface preparation may be newer than the count
    // snapshot yet still belong to an older generation; treating it as catch-up
    // silently adds another frame of latency.
    return false;
  }

  inline std::optional<uint64_t> record_accepted_frame(
    std::deque<uint64_t> &accepted_frame_indices,
    uint64_t frame_index,
    int lookahead_depth) {
    const auto retained_depth = static_cast<std::size_t>(std::max(0, lookahead_depth));
    accepted_frame_indices.push_back(frame_index);
    while (accepted_frame_indices.size() > retained_depth + 1) {
      accepted_frame_indices.pop_front();
    }
    if (accepted_frame_indices.size() <= retained_depth) {
      return std::nullopt;
    }
    return accepted_frame_indices.front();
  }

  inline std::optional<int> resolve_h264_cabac(int coder_mode) noexcept {
    if (coder_mode == 1) return 1;  // CABAC
    if (coder_mode == 2) return 0;  // CAVLC
    return std::nullopt;  // auto: preserve the driver default
  }

  template<typename Slot, std::size_t SlotCount>
  std::optional<std::size_t> select_free_surface(
    const std::array<Slot, SlotCount> &slots,
    std::size_t next_slot,
    std::size_t active_slot_count = SlotCount) noexcept {
    static_assert(SlotCount > 0);
    const auto bounded_slot_count = std::clamp<std::size_t>(active_slot_count, 1, SlotCount);
    for (std::size_t offset = 0; offset < bounded_slot_count; ++offset) {
      const auto candidate = (next_slot + offset) % bounded_slot_count;
      if (slots[candidate].state == input_surface_state_e::free) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  // Returns true when the slot became reusable.
  inline bool on_surface_released(input_surface_state_t &slot) noexcept {
    slot.release_notified = true;
    if (slot.state != input_surface_state_e::in_flight) {
      return false;
    }

    slot.state = input_surface_state_e::free;
    slot.frame_index = 0;
    return true;
  }

  // Commit ownership only after SubmitInput accepted the surface. If AMF
  // synchronously released it from inside SubmitInput, the observer has already
  // set release_notified and the slot is immediately reusable.
  inline bool on_input_accepted(input_surface_state_t &slot, uint64_t frame_index) noexcept {
    slot.state = input_surface_state_e::in_flight;
    slot.frame_index = frame_index;
    if (!slot.release_notified) {
      return false;
    }

    slot.state = input_surface_state_e::free;
    slot.frame_index = 0;
    return true;
  }

  template<std::size_t SlotCount, typename FlagRecoveredFrame>
  void commit_recovery_state(bool input_accepted,
                             int effective_slots,
                             bool reset_cache,
                             int slot_to_preserve,
                             int slot_to_mark,
                             int next_mark_slot,
                             bool consume_pending_rfi,
                             bool frame_after_rfi,
                             uint64_t frame_index,
                             std::array<bool, SlotCount> &slots_valid,
                             std::array<uint64_t, SlotCount> &slot_frame_indices,
                             int &current_mark_slot,
                             bool &rfi_pending,
                             FlagRecoveredFrame &&flag_recovered_frame) {
    if (!input_accepted) {
      return;
    }

    const int bounded_slots = std::min(effective_slots, static_cast<int>(SlotCount));
    if (reset_cache) {
      for (int slot = 0; slot < bounded_slots; ++slot) {
        if (slot != slot_to_preserve) {
          slots_valid[slot] = false;
          slot_frame_indices[slot] = 0;
        }
      }
    }
    if (slot_to_mark >= 0 && slot_to_mark < bounded_slots) {
      slots_valid[slot_to_mark] = true;
      slot_frame_indices[slot_to_mark] = frame_index;
      current_mark_slot = next_mark_slot;
    }
    if (consume_pending_rfi) {
      rfi_pending = false;
    }
    if (frame_after_rfi) {
      flag_recovered_frame(frame_index);
    }
  }

  template<typename Submit, typename WaitForProgress, typename Retryable>
  auto submit_with_bounded_retry(Submit &&submit,
                                 WaitForProgress &&wait_for_progress,
                                 Retryable &&retryable,
                                 int max_retries) {
    auto result = submit();
    for (int retry = 0; retry < max_retries && retryable(result); ++retry) {
      // A true result asks us to abort (fatal state/shutdown). The caller owns
      // policy for the still-retryable result.
      if (wait_for_progress()) {
        break;
      }
      result = submit();
    }
    return result;
  }

  struct smart_access_video_plan_t {
    std::optional<bool> enabled;
    bool disabled_for_low_latency = false;
  };

  // Smart Access Video distributes media work for throughput; the encoder-level
  // LOWLATENCY_MODE override selects a different, aggressive driver path. A
  // confirmed HEVC 4K120 HDR run with both enabled wedged AMF, triggered two
  // LiveKernelEvent 141 GPU resets, and faulted amdxx64.dll. Keep either explicit
  // setting independently, but resolve their unsafe conjunction in favor of the
  // streaming-oriented low-latency setting.
  inline smart_access_video_plan_t resolve_smart_access_video(
    std::optional<bool> smart_access_video,
    std::optional<bool> low_latency_mode) {
    if (smart_access_video && *smart_access_video &&
        low_latency_mode && *low_latency_mode) {
      return {false, true};
    }
    return {smart_access_video, false};
  }

  struct concurrent_session_feature_plan_t {
    std::optional<bool> low_latency_mode;
    std::optional<bool> high_motion_quality_boost;
    bool overrides_suppressed = false;
  };

  // Current Radeon drivers can wedge VCN when a second high-rate session enables
  // either of these optional driver paths. The AMF ultra-low-latency usage preset
  // remains active without them, so preserve a usable native session and suppress
  // only explicitly enabled overrides on concurrent sessions. Explicit false and
  // automatic settings are preserved exactly.
  inline concurrent_session_feature_plan_t resolve_concurrent_session_features(
    int active_encoder_count,
    std::optional<bool> low_latency_mode,
    std::optional<bool> high_motion_quality_boost) noexcept {
    const bool suppress_low_latency = active_encoder_count > 1 &&
                                      low_latency_mode && *low_latency_mode;
    const bool suppress_high_motion = active_encoder_count > 1 &&
                                      high_motion_quality_boost && *high_motion_quality_boost;
    if (suppress_low_latency) {
      low_latency_mode = std::nullopt;
    }
    if (suppress_high_motion) {
      high_motion_quality_boost = std::nullopt;
    }
    return {
      low_latency_mode,
      high_motion_quality_boost,
      suppress_low_latency || suppress_high_motion,
    };
  }

  // Executes potentially driver-blocking cleanup away from the caller. A timed
  // out worker deliberately retains ownership of its resources until it exits;
  // abandoning that session is safer than wedging the host process.
  template<typename Work>
  bool run_detached(Work &&work) {
    using work_t = std::decay_t<Work>;

    // Keep ownership out of the temporary std::thread closure. If thread
    // creation fails, destroying that closure on the caller could synchronously
    // enter the very vendor destructor this helper is meant to isolate. The
    // caller treats false as a quarantine boundary, so deliberately retain the
    // heap work item in that exceptional path.
    auto *owned_work = new work_t(std::forward<Work>(work));
    try {
      std::thread {
        [owned_work]() mutable {
          std::unique_ptr<work_t> work_owner {owned_work};
          try {
            (*work_owner)();
          } catch (...) {
            // Detached cleanup is best-effort.
          }
        }
      }.detach();
      return true;
    } catch (...) {
      (void) owned_work;
      return false;
    }
  }

  template<typename Work, typename Rep, typename Period>
  bool run_with_timeout(Work &&work, std::chrono::duration<Rep, Period> timeout) {
    std::promise<void> done;
    auto done_future = done.get_future();
    using work_t = std::decay_t<Work>;
    auto *owned_work = new work_t(std::forward<Work>(work));
    std::thread worker;
    try {
      worker = std::thread {
        [owned_work, done = std::move(done)]() mutable {
          std::unique_ptr<work_t> work_owner {owned_work};
          try {
            (*work_owner)();
          } catch (...) {
            // Cleanup is best-effort; completion still releases the waiter.
          }
          try {
            done.set_value();
          } catch (...) {
            // The waiter may already have abandoned the shared state.
          }
        }
      };
    } catch (...) {
      // See run_detached(): retain the vendor resource graph instead of running
      // its destructor synchronously on this caller.
      (void) owned_work;
      return false;
    }

    if (done_future.wait_for(timeout) == std::future_status::ready) {
      worker.join();
      return true;
    }

    worker.detach();
    return false;
  }

}  // namespace amf::lifecycle
