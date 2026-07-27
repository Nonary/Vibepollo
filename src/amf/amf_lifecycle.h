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
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace amf::lifecycle {

  /**
   * @brief Outcome of an attempt to acquire the process-wide AMF teardown fence.
   *
   * `quarantined` and `contended` are failures for very different reasons and
   * must not be collapsed. Under `quarantined` the runtime has an abandoned
   * vendor call and re-entering it can hang or crash, so leaking the session is
   * the lesser evil. Under `contended` nothing is wrong with the runtime at all
   * — another session's initialization or teardown simply still held the fence
   * when this caller's deadline expired — and leaking there permanently strands
   * an AMF hardware encode session, its D3D11 device, its input-surface ring and
   * its output-pump thread.
   */
  enum class teardown_admission_e {
    granted,      ///< Fence acquired; the caller owes a matching finish_teardown().
    quarantined,  ///< The AMD runtime has an abandoned vendor call; never re-enter it.
    contended,    ///< The fence was still held by a healthy peer at the deadline.
  };

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
      const bool publish = initialization_in_progress && !quarantined && teardowns_in_progress == 0;
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

    template<typename Clock, typename Duration>
    teardown_admission_e begin_teardown_until(const std::chrono::time_point<Clock, Duration> &deadline) {
      std::unique_lock lock(mutex);
      ++pending_teardowns;
      const bool available = state_changed.wait_until(lock, deadline, [&]() {
        return quarantined || (!initialization_in_progress && teardowns_in_progress == 0);
      });
      --pending_teardowns;
      if (quarantined) {
        state_changed.notify_all();
        return teardown_admission_e::quarantined;
      }
      if (!available) {
        // A healthy peer still owns the fence. This is NOT an abandoned vendor
        // call, and callers must not treat it as one: the session waiting here
        // is intact and destroying it is still both possible and necessary.
        state_changed.notify_all();
        return teardown_admission_e::contended;
      }
      ++teardowns_in_progress;
      return teardown_admission_e::granted;
    }

    bool begin_teardown() {
      return begin_teardown_until(std::chrono::steady_clock::time_point::max()) ==
             teardown_admission_e::granted;
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

    bool runtime_is_idle() const {
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
      if (abandoned) return false;
      candidate.emplace(std::move(value));
      ready = true;
      changed.notify_all();
      changed.wait(lock, [&]() { return accepted || abandoned; });
      if (abandoned) {
        // Explicitly destroy on the producer before it can drop its shared state
        // reference; otherwise the caller may become the last handoff owner at
        // the timeout boundary and run the candidate destructor itself.
        candidate.reset();
        return false;
      }
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

  private:
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<T> candidate;
    bool ready = false;
    bool accepted = false;
    bool abandoned = false;
  };

  enum class input_surface_state_e {
    free,
    reserved,
    in_flight
  };

  struct input_surface_state_t {
    uint64_t frame_index = 0;
    input_surface_state_e state = input_surface_state_e::free;
    bool release_notified = false;
  };

  struct encoder_selection_policy_t {
    bool include_legacy = false;
    bool fail_closed = false;
  };

  inline constexpr encoder_selection_policy_t encoder_selection_policy(
    std::string_view requested_encoder) noexcept {
    // Native AMF and the FFmpeg AMF implementation are separate, explicit
    // contracts. Automatic probing must never make a native feature/property
    // failure look successful by selecting the legacy encoder behind the user.
    return {
      requested_encoder == "amdvce_legacy",
      requested_encoder == "amdvce",
    };
  }

  inline constexpr int intra_refresh_period_frames = 300;
  inline constexpr int intra_refresh_duration_frames = intra_refresh_period_frames - 1;
  inline constexpr int av1_continuous_intra_refresh_mode = 2;

  struct intra_refresh_plan_t {
    bool enabled = false;
    std::optional<int> blocks_per_slot;
    std::optional<int> av1_mode;
    std::optional<int> av1_cycle_frames;
    int minimum_reference_frames = 0;
    bool disable_ltr = false;
  };

  inline constexpr intra_refresh_plan_t resolve_intra_refresh(
    bool requested,
    int video_format,
    int width,
    int height) noexcept {
    if (!requested || width <= 0 || height <= 0) {
      return {};
    }

    if (video_format == 2) {
      // AMF's AV1 stripe count is the number of frames in a complete refresh
      // cycle. Continuous mode therefore maps directly to NVENC's 300-frame
      // Xbox-compatible period.
      return {
        true,
        std::nullopt,
        av1_continuous_intra_refresh_mode,
        intra_refresh_period_frames,
        0,
        true,
      };
    }

    if (video_format != 0 && video_format != 1) {
      return {};
    }

    const int block_edge = video_format == 0 ? 16 : 64;
    const int64_t blocks_wide = (static_cast<int64_t>(width) + block_edge - 1) / block_edge;
    const int64_t blocks_high = (static_cast<int64_t>(height) + block_edge - 1) / block_edge;
    const int64_t block_count = blocks_wide * blocks_high;
    const int64_t refresh_frames = std::min<int64_t>(block_count, intra_refresh_duration_frames);
    const int blocks_per_slot = static_cast<int>((block_count + refresh_frames - 1) / refresh_frames);

    // User-controlled LTR is mutually exclusive with intra refresh on every
    // codec, not just H.264. AMF_Video_Encode_API.md:714 (AVC),
    // AMF_Video_Encode_HEVC_API.md:718 and AMF_Video_Encode_AV1_API.md:674 all
    // state "With user control of LTR, Intra-refresh features are not
    // supported." AMF stores both property sets verbatim, so a driver that
    // honours only one is invisible to the post-Init readback contract — the
    // interlock has to be applied here. Only the minimum-reference-frame floor
    // is genuinely H.264-specific.
    return {
      true,
      blocks_per_slot,
      std::nullopt,
      std::nullopt,
      video_format == 0 ? 2 : 0,
      true,
    };
  }

  // The client's reference-frame limit and the intra-refresh minimum must be
  // combined identically at configure time and at post-Init verification, or
  // the readback contract rejects its own raise. nullopt == leave the driver
  // default untouched (no limit requested and no minimum required).
  inline constexpr std::optional<int64_t> effective_reference_frame_limit(
    int client_num_ref_frames,
    int intra_refresh_minimum_reference_frames) noexcept {
    if (client_num_ref_frames <= 0) {
      if (intra_refresh_minimum_reference_frames <= 0) {
        return std::nullopt;
      }
      // "No limit" from the client still needs the intra-refresh floor applied,
      // otherwise H.264 intra refresh rides on a driver-default single reference.
      return intra_refresh_minimum_reference_frames;
    }
    return std::max<int64_t>(client_num_ref_frames, intra_refresh_minimum_reference_frames);
  }

  inline bool rate_control_requires_preanalysis(int mode) noexcept {
    // AMF uses these values consistently for AVC, HEVC, and AV1.
    return mode >= 4 && mode <= 6;  // QVBR, HQVBR, HQCBR
  }

  // PEAK_CONSTRAINED_VBR. Among the rate controls that do not require
  // PreAnalysis, this is the only one whose numeric value is identical across
  // all three AMF encoders — 2 in VideoEncoderVCE.h
  // (AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR),
  // VideoEncoderHEVC.h and VideoEncoderAV1.h alike. LATENCY_CONSTRAINED_VBR is
  // deliberately not used here: it is 3 for AVC but 1 for HEVC/AV1, so it would
  // require a per-codec map. PEAK_CONSTRAINED_VBR is also the mode AMD's own
  // PRE_ANALYSIS_ENABLE documentation names as PA's companion, so every runtime
  // that implements PA implements this mode too.
  inline constexpr int preanalysis_free_rate_control = 2;

  // AMD documents QVBR/HQVBR/HQCBR as legal only while PreAnalysis is enabled
  // (AMF_Video_Encode_API.md:1082, AMF_Video_Encode_HEVC_API.md:950 and
  // AMF_Video_Encode_AV1_API.md:952 all state "QVBR, HQVBR and HQCBR are only
  // supported if PreAnalysis is enabled"), while the PA component itself
  // "accepts raw input images in NV12 format" (AMF_Video_PreAnalysis_API.md:71)
  // — that is, 8-bit only. A 10-bit session that asks for one of those modes
  // therefore has no legal configuration at all: leaving PA on fails Init with
  // AMF_NOT_SUPPORTED, and turning PA off alone leaves a rate control the
  // runtime refuses for exactly the same reason. Demote the mode as well so the
  // HDR session can initialize instead of losing 10-bit entirely.
  // nullopt == no demotion required; keep the requested mode.
  inline std::optional<int> demoted_rate_control_for_10bit(
    const std::optional<int> &rate_control,
    bool suppress_preanalysis) noexcept {
    if (!suppress_preanalysis || !rate_control ||
        !rate_control_requires_preanalysis(*rate_control)) {
      return std::nullopt;
    }
    return preanalysis_free_rate_control;
  }

  // The single source of truth for the rate-control mode actually written to the
  // encoder. Configure-time application and post-Init readback verification must
  // both use this; comparing the readback against the raw request would fail the
  // session closed on our own substitution.
  inline std::optional<int> effective_rate_control(
    const std::optional<int> &rate_control,
    bool suppress_preanalysis) noexcept {
    if (const auto demoted = demoted_rate_control_for_10bit(rate_control, suppress_preanalysis)) {
      return demoted;
    }
    return rate_control;
  }

  inline bool rate_control_supports_adaptive_quantization(const std::optional<int> &rate_control) noexcept {
    // All three AMF encoders use zero for CONSTANT_QP. AMD documents VBAQ/CAQ
    // as incompatible with CQP, so an enabled-by-default AQ setting must not turn
    // an otherwise valid CQP request into a driver-dependent configuration.
    return !rate_control || *rate_control != 0;
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

  inline constexpr bool driver_submit_capacity_available(
    std::size_t input_surfaces_in_flight,
    std::size_t encoder_input_queue_size) noexcept {
    // Current AMF runtimes can retain one wrapper beyond the configured queue.
    // Do not enter SubmitInput again once ownership has already crossed that
    // boundary: on a stalled PA pipeline the call itself can block for tens of
    // seconds and cannot be bounded by the caller's probe deadline.
    return input_surfaces_in_flight <= encoder_input_queue_size;
  }

  inline constexpr bool saturation_wait_should_finish(bool output_fatal,
                                                       bool capacity_available) noexcept {
    return output_fatal || capacity_available;
  }

  struct preanalysis_plan_t {
    bool enabled = false;
    int lookahead_depth = 0;
    bool enabled_for_rate_control = false;
  };

  inline preanalysis_plan_t resolve_preanalysis(const std::optional<int> &rate_control,
                                                const std::optional<int> &explicit_preanalysis,
                                                bool suppress_preanalysis = false) noexcept {
    const bool required_by_rate_control = rate_control && rate_control_requires_preanalysis(*rate_control);
    // AMF cannot run PreAnalysis on a 10-bit pipeline: the PA component consumes
    // NV12 (AMF_Video_PreAnalysis_API.md:71). Force PA off regardless of whether
    // it was selected explicitly or implied by rate control. The rate-control
    // mode is NOT preserved in that case — a PA-requiring mode is equally
    // unsupported without PA, so callers must pair this with
    // effective_rate_control() and write the demoted mode instead.
    if (suppress_preanalysis) {
      return {};
    }
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
  std::optional<std::size_t> select_repeat_surface(
    const std::array<Slot, SlotCount> &slots,
    std::size_t source_slot,
    std::size_t next_slot,
    std::size_t active_slot_count = SlotCount) noexcept {
    static_assert(SlotCount > 0);
    const auto bounded_slot_count = std::clamp<std::size_t>(active_slot_count, 1, SlotCount);
    if (source_slot >= bounded_slot_count) {
      return std::nullopt;
    }
    if (slots[source_slot].state == input_surface_state_e::free) {
      return source_slot;
    }
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
  template<typename Work, typename Rep, typename Period>
  bool run_with_timeout(Work &&work, std::chrono::duration<Rep, Period> timeout) {
    // std::thread's constructor throws std::system_error when the OS cannot
    // create a thread, and two callers run inside a `noexcept` fail-guard
    // destructor, where that would be an unconditional std::terminate. Keep the
    // work in shared state so a failed thread launch can still fall back to the
    // caller's own stack: the callable is moved into std::thread's shared state
    // before it can throw, so it cannot be reused directly.
    struct pending_work_t {
      std::decay_t<Work> work;
      std::promise<void> done;
    };

    auto pending = std::make_shared<pending_work_t>(
      pending_work_t {std::forward<Work>(work), std::promise<void>()}
    );
    auto done_future = pending->done.get_future();

    std::thread worker;
    try {
      worker = std::thread {
        [pending]() mutable {
          try {
            pending->work();
          } catch (...) {
            // Cleanup is best-effort; completion still releases the waiter.
          }
          try {
            pending->done.set_value();
          } catch (...) {
            // The waiter may already have abandoned the shared state.
          }
        }
      };
    } catch (...) {
      // Thread exhaustion is a transient host condition, not a wedged driver.
      // Running the cleanup here can block, but that is what every unbounded
      // teardown path already does, and it neither aborts the process nor
      // quarantines the AMD runtime for the rest of its lifetime.
      try {
        pending->work();
      } catch (...) {
      }
      return true;
    }

    if (done_future.wait_for(timeout) == std::future_status::ready) {
      worker.join();
      return true;
    }

    worker.detach();
    return false;
  }

}  // namespace amf::lifecycle
