/**
 * @file tests/unit/platform/windows/test_native_amf_review.cpp
 * @brief Behavioral tests for native AMF ownership and lifecycle policy.
 */

#include "src/amf/amf_lifecycle.h"
#include "src/amf/amf_config_policy.h"

#include <array>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifndef SUNSHINE_AMF_LIFECYCLE_STANDALONE
  #include "../../../tests_common.h"
#endif

using namespace std::chrono_literals;

namespace {

  struct ready_output_session_t {
    struct frame_t {
      int index;
      bool fatal = false;
    };
    struct result_t {
      std::vector<frame_t> frames;
      bool fatal = false;
    } queued;
    int drains = 0;
    std::chrono::milliseconds timeout {99};

    bool has_completed_output() const {
      return !queued.frames.empty();
    }
    result_t drain_frames(std::chrono::milliseconds wait) {
      ++drains;
      timeout = wait;
      return std::exchange(queued, {});
    }
  };

  bool capture_wait_completion_is_delivered_before_conversion() {
    ready_output_session_t session;
    std::vector<int> events;
    auto deliver = [&](auto &frames) {
      for (auto &frame : frames) events.push_back(frame.index);
    };
    if (!amf::lifecycle::deliver_ready_output(&session, deliver) || session.drains != 0) return false;
    // Output arrives during capture's wait; a catch-up batch must retain order.
    session.queued.frames = {{41}, {42}};
    if (!amf::lifecycle::deliver_ready_output(&session, deliver)) return false;
    events.push_back(43);  // Convert the newly captured image only after delivery.
    if (!amf::lifecycle::deliver_ready_output(&session, deliver)) return false;
    return events == std::vector<int> {41, 42, 43} && session.drains == 1 && session.timeout == 0ms;
  }

  bool ready_output_drain_is_optional_and_propagates_failures() {
    bool delivered = false;
    auto deliver = [&](auto &) { delivered = true; };
    if (!amf::lifecycle::deliver_ready_output<ready_output_session_t>(nullptr, deliver)) return false;
    for (bool batch_fatal : {false, true}) {
      ready_output_session_t session;
      session.queued = {{{41, !batch_fatal}}, batch_fatal};
      if (amf::lifecycle::deliver_ready_output(&session, deliver) || delivered ||
          session.drains != 1 || session.timeout != 0ms) return false;
    }
    return !delivered;
  }

  bool av1_tiles_auto_preserves_client_and_preset_behavior() {
    using amf::config_policy::av1_tiles_request;
    return !av1_tiles_request(0, 0) && !av1_tiles_request(0, 1) &&
           av1_tiles_request(0, 2) == 2 && av1_tiles_request(0, 8) == 8;
  }

  bool av1_tiles_override_includes_explicit_single_tile() {
    using amf::config_policy::av1_tiles_request;
    return av1_tiles_request(1, 4) == 1 && av1_tiles_request(2, 1) == 2 &&
           av1_tiles_request(4, 1) == 4;
  }

  bool av1_tiles_rejects_invalid_host_overrides() {
    using namespace amf::config_policy;
    return valid_av1_tiles_override(0) && valid_av1_tiles_override(1) &&
           valid_av1_tiles_override(2) && valid_av1_tiles_override(4) &&
           !valid_av1_tiles_override(-1) && !valid_av1_tiles_override(3) &&
           !valid_av1_tiles_override(8) && !av1_tiles_request(3, 1) &&
           av1_tiles_request(-1, 8) == 8;
  }

  enum class fake_amf_result_e {
    ok,
    input_full,
    failed
  };

  bool synchronous_release_during_submit_is_reentrant_safe() {
    std::mutex state_mutex;
    amf::lifecycle::input_surface_state_t slot;
    slot.state = amf::lifecycle::input_surface_state_e::reserved;

    bool observer_acquired_state = false;
    const auto result = amf::lifecycle::submit_with_bounded_retry(
      [&]() {
        // Fake AMF invokes OnSurfaceDataRelease synchronously from SubmitInput.
        if (state_mutex.try_lock()) {
          observer_acquired_state = true;
          amf::lifecycle::on_surface_released(slot);
          state_mutex.unlock();
        }
        return fake_amf_result_e::ok;
      },
      []() { return false; },
      [](fake_amf_result_e value) { return value == fake_amf_result_e::input_full; },
      20);

    std::lock_guard lock(state_mutex);
    const bool immediately_reusable = amf::lifecycle::on_input_accepted(slot, 7);
    return result == fake_amf_result_e::ok && observer_acquired_state && immediately_reusable &&
           slot.state == amf::lifecycle::input_surface_state_e::free && slot.frame_index == 0;
  }

  bool backpressure_retries_the_same_submission_until_accepted() {
    const std::vector<fake_amf_result_e> responses {
      fake_amf_result_e::input_full,
      fake_amf_result_e::input_full,
      fake_amf_result_e::ok,
    };
    std::size_t submit_count = 0;
    int wait_count = 0;
    const auto result = amf::lifecycle::submit_with_bounded_retry(
      [&]() { return responses.at(submit_count++); },
      [&]() {
        ++wait_count;
        return false;
      },
      [](fake_amf_result_e value) { return value == fake_amf_result_e::input_full; },
      20);
    return result == fake_amf_result_e::ok && submit_count == 3 && wait_count == 2;
  }

  bool exhausted_backpressure_reinitializes_without_owned_surfaces() {
    using amf::lifecycle::submit_backpressure_requires_reinit;
    return !submit_backpressure_requires_reinit(1, 120, false, 10s) &&
           !submit_backpressure_requires_reinit(119, 120, true, 1999ms) &&
           submit_backpressure_requires_reinit(120, 120, false, 0ms) &&
           submit_backpressure_requires_reinit(1, 120, true, 2s);
  }

  bool recovery_state_changes_only_after_accepted_input() {
    std::array<bool, 4> slots_valid {true, true, false, false};
    std::array<uint64_t, 4> slot_frames {10, 20, 0, 0};
    int current_slot = 1;
    bool rfi_pending = true;
    uint64_t flagged_frame = 0;

    auto commit = [&](bool accepted) {
      amf::lifecycle::commit_recovery_state(
        accepted,
        2,
        true,
        0,
        -1,
        1,
        true,
        true,
        42,
        slots_valid,
        slot_frames,
        current_slot,
        rfi_pending,
        [&](uint64_t frame_index) { flagged_frame = frame_index; });
    };

    commit(false);
    const bool unchanged_while_rejected = slots_valid[0] && slots_valid[1] &&
                                          slot_frames[0] == 10 && slot_frames[1] == 20 &&
                                          rfi_pending && flagged_frame == 0;
    commit(true);
    return unchanged_while_rejected && slots_valid[0] && !slots_valid[1] &&
           slot_frames[0] == 10 && slot_frames[1] == 0 && !rfi_pending && flagged_frame == 42;
  }

  bool preanalysis_dependent_rate_control_is_planned_natively() {
    const auto normal = amf::lifecycle::resolve_preanalysis(3, 0);
    const auto explicit_pa = amf::lifecycle::resolve_preanalysis(3, 1);
    const auto qvbr = amf::lifecycle::resolve_preanalysis(4, 0);
    const auto hqvbr = amf::lifecycle::resolve_preanalysis(5, 0);
    const auto hqcbr = amf::lifecycle::resolve_preanalysis(6, 0);
    std::vector<int> property_order;
    const bool applied = amf::lifecycle::apply_rate_control_and_preanalysis(
      4,
      true,
      qvbr,
      qvbr.lookahead_depth,
      [&](int mode) {
        property_order.push_back(100 + mode);
        return true;
      },
      [&](bool enabled) {
        property_order.push_back(200 + static_cast<int>(enabled));
        return true;
      },
      [&](int depth) {
        property_order.push_back(300 + depth);
        return true;
      });

    return !normal.enabled && normal.lookahead_depth == 0 &&
           !amf::lifecycle::rate_control_supports_adaptive_quantization(0) &&
           amf::lifecycle::rate_control_supports_adaptive_quantization(3) &&
           amf::lifecycle::rate_control_supports_adaptive_quantization(std::nullopt) &&
           explicit_pa.enabled && explicit_pa.lookahead_depth == 1 && !explicit_pa.enabled_for_rate_control &&
           qvbr.enabled && qvbr.lookahead_depth == 1 && qvbr.enabled_for_rate_control &&
           hqvbr.enabled && hqvbr.lookahead_depth == 1 && hqvbr.enabled_for_rate_control &&
           hqcbr.enabled && hqcbr.lookahead_depth == 1 && hqcbr.enabled_for_rate_control &&
           applied && property_order == std::vector<int> {104, 201, 301};
  }

  bool preanalysis_pipeline_primes_and_drains_in_order() {
    struct fake_delayed_encoder_t {
      explicit fake_delayed_encoder_t(int depth):
          lookahead_depth(depth) {
      }

      std::optional<uint64_t> submit(uint64_t frame_index) {
        pending.push_back(frame_index);
        if (!amf::lifecycle::delayed_output_is_expected(
              static_cast<int>(pending.size()),
              lookahead_depth)) {
          return std::nullopt;
        }
        const auto output = pending.front();
        pending.pop_front();
        return output;
      }

      std::vector<uint64_t> drain() {
        std::vector<uint64_t> output;
        while (!pending.empty()) {
          output.push_back(pending.front());
          pending.pop_front();
        }
        return output;
      }

      int lookahead_depth;
      std::deque<uint64_t> pending;
    };

    fake_delayed_encoder_t encoder {amf::lifecycle::low_latency_preanalysis_lookahead_depth};
    const auto first = encoder.submit(100);
    const auto second = encoder.submit(101);
    const auto tail = encoder.drain();
    return !first && second && *second == 100 && tail == std::vector<uint64_t> {101} &&
           !amf::lifecycle::delayed_output_is_expected(1, 1) &&
           amf::lifecycle::delayed_output_is_expected(2, 1) &&
           amf::lifecycle::delayed_output_is_expected(1, 0);
  }

  bool automatic_h264_coder_preserves_driver_default() {
    const auto automatic = amf::lifecycle::resolve_h264_cabac(0);
    const auto cabac = amf::lifecycle::resolve_h264_cabac(1);
    const auto cavlc = amf::lifecycle::resolve_h264_cabac(2);
    return !automatic && cabac && *cabac == 1 && cavlc && *cavlc == 0;
  }

  bool automatic_prefers_native_and_explicit_choices_are_strict() {
    const auto automatic = amf::lifecycle::encoder_selection_policy("");
    const auto stable = amf::lifecycle::encoder_selection_policy("amdvce_ffmpeg");
    const auto experimental = amf::lifecycle::encoder_selection_policy("amdvce_experimental");
    const auto stable_alias = amf::lifecycle::encoder_selection_policy("amdvce_legacy");
    const auto experimental_alias = amf::lifecycle::encoder_selection_policy("amdvce");

    return automatic.include_experimental && !automatic.fail_closed &&
           !stable.include_experimental && stable.fail_closed &&
           experimental.include_experimental && experimental.fail_closed &&
           !stable_alias.include_experimental && stable_alias.fail_closed &&
           experimental_alias.include_experimental && experimental_alias.fail_closed;
  }

  bool initialization_fallback_respects_success_and_explicit_choices() {
    using namespace amf::lifecycle;
    using result_t = initialization_result_t<std::unique_ptr<int>>;
    int native_calls = 0, legacy_calls = 0, logs = 0;
    initialization_fallback_t automatic;
    auto success = [&]() -> result_t { ++native_calls; return {std::make_unique<int>(42), initialization_failure_e::none}; };
    auto legacy = [&]() -> result_t { ++legacy_calls; return {std::make_unique<int>(17), initialization_failure_e::none}; };
    auto log = [&](auto) { ++logs; };
    auto result = automatic.initialize(true, true, success, legacy, log);
    if (!result.value || **result.value != 42 || automatic.legacy || native_calls != 1 || legacy_calls || logs) return false;
    initialization_fallback_t explicit_native;
    auto failure = []() -> result_t { return {std::nullopt, initialization_failure_e::encoder_init}; };
    if (explicit_native.initialize(false, true, failure, legacy, log).value || explicit_native.legacy || legacy_calls || logs) return false;
    initialization_fallback_t explicit_legacy {.legacy = true};
    result = explicit_legacy.initialize(false, true, success, legacy, log);
    return result.value && **result.value == 17 && native_calls == 1 && legacy_calls == 1 && logs == 0;
  }

  bool successful_rollback_uses_the_same_stream_format() {
    using namespace amf::lifecycle;
    using format_t = std::array<int, 4>; // codec, depth, chroma, HDR
    using result_t = initialization_result_t<format_t>;
    const format_t requested {2, 10, 0, 1};
    initialization_fallback_t stream;
    int native_calls = 0, legacy_calls = 0, logs = 0;
    auto native = [&]() -> result_t { ++native_calls; return {std::nullopt, initialization_failure_e::encoder_init}; };
    auto legacy = [&]() -> result_t { ++legacy_calls; return {requested, initialization_failure_e::none}; };
    auto result = stream.initialize(true, true, native, legacy, [&](auto) { ++logs; });
    if (!result.value || *result.value != requested || !stream.legacy || native_calls != 1 || legacy_calls != 1 || logs != 1) return false;
    result = stream.initialize(true, true, native, legacy, [&](auto) { ++logs; });
    return result.value && *result.value == requested && native_calls == 1 && legacy_calls == 2 && logs == 1;
  }

  bool encoder_initialization_fallback_is_bounded_and_owned() {
    using namespace amf::lifecycle;
    struct owned_t {
      int &live;
      explicit owned_t(int &count): live(count) { ++live; }
      ~owned_t() { --live; }
    };
    using result_t = initialization_result_t<std::unique_ptr<owned_t>>;
    for (auto failure : {initialization_failure_e::encoder_create, initialization_failure_e::encoder_init}) {
      int live = 0, native_calls = 0, legacy_calls = 0, logs = 0;
      initialization_fallback_t stream;
      auto native = [&]() -> result_t {
        ++native_calls;
        auto partial = std::make_unique<owned_t>(live);
        return {std::nullopt, failure};
      };
      auto failed_legacy = [&]() -> result_t {
        ++legacy_calls;
        if (live != 0) return {std::make_unique<owned_t>(live), initialization_failure_e::none};
        auto partial = std::make_unique<owned_t>(live);
        return {std::nullopt, initialization_failure_e::encoder_init};
      };
      auto log = [&](auto reason) { if (live == 0 && reason == failure) ++logs; };
      auto result = stream.initialize(true, true, native, failed_legacy, log);
      if (result.value || live || !stream.legacy || native_calls != 1 || legacy_calls != 1 || logs != 1) return false;
      // A failed rollback is terminal: neither implementation is tried again.
      result = stream.initialize(true, true, native, failed_legacy, log);
      if (result.value || live || native_calls != 1 || legacy_calls != 1 || logs != 1) return false;
      initialization_fallback_t next_stream;
      auto success = [&]() -> result_t { ++native_calls; return {std::make_unique<owned_t>(live), initialization_failure_e::none}; };
      result = next_stream.initialize(true, true, success, failed_legacy, log);
      if (!result.value || next_stream.legacy || native_calls != 2 || legacy_calls != 1 || live != 1) return false;
      result.value.reset();
      if (live) return false;
    }
    return true;
  }

  bool unrelated_or_unsupported_failures_never_fallback() {
    using namespace amf::lifecycle;
    using result_t = initialization_result_t<int>;
    for (auto failure : {initialization_failure_e::unsupported_format, initialization_failure_e::device_or_capture,
                         initialization_failure_e::conversion, initialization_failure_e::network,
                         initialization_failure_e::cancelled, initialization_failure_e::busy,
                         initialization_failure_e::unsafe_runtime}) {
      initialization_fallback_t stream;
      int rollbacks = 0;
      auto result = stream.initialize(true, true,
        [&]() -> result_t { return {std::nullopt, failure}; },
        [&]() -> result_t { ++rollbacks; return {17, initialization_failure_e::none}; }, [](auto) {});
      if (result.value || result.failure != failure || rollbacks || stream.legacy) return false;
    }
    initialization_fallback_t incompatible;
    auto result = incompatible.initialize(true, false,
      []() -> result_t { return {std::nullopt, initialization_failure_e::encoder_init}; },
      []() -> result_t { return {17, initialization_failure_e::none}; }, [](auto) {});
    return !result.value && !incompatible.legacy;
  }

  bool rollback_preserves_negotiated_format_requirements() {
    using amf::lifecycle::legacy_stream_format_compatible;
    return legacy_stream_format_compatible(0, 0, false, false, false) &&
           legacy_stream_format_compatible(1, 0, true, false, false) &&
           legacy_stream_format_compatible(2, 0, true, false, false) &&
           !legacy_stream_format_compatible(0, 0, true, false, false) &&
           !legacy_stream_format_compatible(1, 1, false, false, false) &&
           !legacy_stream_format_compatible(3, 0, false, false, false) &&
           !legacy_stream_format_compatible(1, 0, false, true, false) &&
           !legacy_stream_format_compatible(2, 0, false, false, true) &&
           legacy_stream_format_compatible(1, 0, true, false, false, 1, true) &&
           !legacy_stream_format_compatible(1, 0, true, false, false, 1, false);
  }

  bool xbox_intra_refresh_maps_to_native_amf() {
    const auto disabled = amf::lifecycle::resolve_intra_refresh(false, 0, 1920, 1080);
    const auto invalid = amf::lifecycle::resolve_intra_refresh(true, 3, 1920, 1080);
    const auto h264 = amf::lifecycle::resolve_intra_refresh(true, 0, 1920, 1080);
    const auto h264_odd = amf::lifecycle::resolve_intra_refresh(true, 0, 1919, 1079);
    const auto hevc = amf::lifecycle::resolve_intra_refresh(true, 1, 1920, 1080);
    const auto av1 = amf::lifecycle::resolve_intra_refresh(true, 2, 1920, 1080);

    return !disabled.enabled && !disabled.blocks_per_slot &&
           !invalid.enabled &&
           h264.enabled && h264.blocks_per_slot && *h264.blocks_per_slot == 28 &&
           h264.minimum_reference_frames == 2 && h264.disable_ltr &&
           h264_odd.blocks_per_slot && *h264_odd.blocks_per_slot == 28 &&
           hevc.enabled && hevc.blocks_per_slot && *hevc.blocks_per_slot == 2 &&
           hevc.minimum_reference_frames == 0 && hevc.disable_ltr &&
           av1.enabled && !av1.blocks_per_slot &&
           av1.av1_mode && *av1.av1_mode == amf::lifecycle::av1_continuous_intra_refresh_mode &&
           av1.av1_cycle_frames &&
           *av1.av1_cycle_frames == amf::lifecycle::intra_refresh_period_frames &&
           av1.disable_ltr;
  }

  bool hevc_gdr_uses_negotiated_codec_and_dimensions() {
    using amf::lifecycle::hevc_gdr_ctbs_per_slot;
    if (hevc_gdr_ctbs_per_slot(false, 1, 3840, 2160) ||
        hevc_gdr_ctbs_per_slot(true, 0, 3840, 2160) ||
        hevc_gdr_ctbs_per_slot(true, 2, 3840, 2160) ||
        hevc_gdr_ctbs_per_slot(true, 3, 3840, 2160) ||
        hevc_gdr_ctbs_per_slot(true, 1, 0, 2160) ||
        hevc_gdr_ctbs_per_slot(true, 1, 3840, -1)) {
      return false;
    }

    // Each ordinary stream refreshes one actual CTB row per slot, rather than
    // inheriting 60 CTBs from an assumed 4K surface before encoder Init().
    return hevc_gdr_ctbs_per_slot(true, 1, 1280, 720) == 20 &&
           hevc_gdr_ctbs_per_slot(true, 1, 1920, 1080) == 30 &&
           hevc_gdr_ctbs_per_slot(true, 1, 2560, 1440) == 40 &&
           hevc_gdr_ctbs_per_slot(true, 1, 3840, 2160) == 60 &&
           hevc_gdr_ctbs_per_slot(true, 1, 1921, 1081) == 31 &&
           hevc_gdr_ctbs_per_slot(true, 1, 1, 1) == 1 &&
           hevc_gdr_ctbs_per_slot(true, 1, 64, 7680) == 1 &&
           hevc_gdr_ctbs_per_slot(true, 1, 64, 7681) == 2;
  }

  bool effective_reference_frame_limit_matches_configure_and_verify() {
    using amf::lifecycle::effective_reference_frame_limit;

    // No client limit and no intra-refresh minimum: leave the driver default.
    const auto untouched = effective_reference_frame_limit(0, 0);
    const auto untouched_negative = effective_reference_frame_limit(-1, 0);
    // "No limit" clients still get the H.264 intra-refresh floor.
    const auto floored = effective_reference_frame_limit(0, 2);
    // The non-RFI single-reference client is raised to the floor — and the
    // post-Init verify must expect the raised value, not the client value.
    const auto raised = effective_reference_frame_limit(1, 2);
    // A client above the floor keeps its own limit.
    const auto client_wins = effective_reference_frame_limit(4, 2);
    // Without intra refresh the client limit passes through untouched.
    const auto passthrough = effective_reference_frame_limit(3, 0);

    return !untouched && !untouched_negative &&
           floored && *floored == 2 &&
           raised && *raised == 2 &&
           client_wins && *client_wins == 4 &&
           passthrough && *passthrough == 3;
  }

  bool repeated_input_rotates_away_from_a_lookahead_owned_surface() {
    std::array<amf::lifecycle::input_surface_state_t, 3> slots;
    slots[0].state = amf::lifecycle::input_surface_state_e::in_flight;
    slots[1].state = amf::lifecycle::input_surface_state_e::free;
    slots[2].state = amf::lifecycle::input_surface_state_e::in_flight;
    const auto rotated = amf::lifecycle::select_repeat_surface(slots, 0, 1);

    slots[0].state = amf::lifecycle::input_surface_state_e::free;
    const auto reused = amf::lifecycle::select_repeat_surface(slots, 0, 2);

    for (auto &slot : slots) slot.state = amf::lifecycle::input_surface_state_e::in_flight;
    const auto unavailable = amf::lifecycle::select_repeat_surface(slots, 0, 1);
    return rotated && *rotated == 1 && reused && *reused == 0 && !unavailable;
  }

  bool surface_pool_can_prime_a_retaining_driver() {
    constexpr auto active_slots = amf::lifecycle::initial_input_surface_count(1);
    static_assert(active_slots == 6);

    std::array<amf::lifecycle::input_surface_state_t, amf::lifecycle::maximum_input_surface_count> slots;
    std::deque<std::size_t> retained;
    std::size_t accepted = 0;
    std::size_t outputs = 0;

    // This fake driver retains four inputs before producing its first output.
    // A three-surface pool deadlocks before submission four even without PA; the
    // queue-aware pool must keep ownership exact and make enough forward progress.
    for (uint64_t frame = 1; frame <= 8; ++frame) {
      std::optional<std::size_t> free_slot;
      for (std::size_t slot = 0; slot < active_slots; ++slot) {
        if (slots[slot].state == amf::lifecycle::input_surface_state_e::free) {
          free_slot = slot;
          break;
        }
      }
      if (!free_slot) {
        return false;
      }

      auto &input = slots[*free_slot];
      input.state = amf::lifecycle::input_surface_state_e::reserved;
      input.release_notified = false;
      amf::lifecycle::on_input_accepted(input, frame);
      retained.push_back(*free_slot);
      ++accepted;

      if (retained.size() >= 4) {
        auto &released = slots[retained.front()];
        retained.pop_front();
        if (!amf::lifecycle::on_surface_released(released)) {
          return false;
        }
        ++outputs;
      }
    }

    return accepted == 8 && outputs == 5 &&
           amf::lifecycle::input_surface_count_for_lookahead(0) == 4 &&
           amf::lifecycle::input_surface_count_for_lookahead(2) == 8 &&
           amf::lifecycle::initial_input_surface_count(0) == 4 &&
           amf::lifecycle::initial_input_surface_count(1) == 6 &&
           amf::lifecycle::initial_input_surface_count(100) ==
             amf::lifecycle::maximum_input_surface_count;
  }

  bool driver_submit_capacity_bounds_are_inclusive() {
    return amf::lifecycle::driver_submit_capacity_available(4, 4) &&
           !amf::lifecycle::driver_submit_capacity_available(5, 4) &&
           amf::lifecycle::driver_submit_capacity_available(16, 16) &&
           !amf::lifecycle::driver_submit_capacity_available(17, 16);
  }

  bool saturation_wait_requires_an_actual_surface_release() {
    using amf::lifecycle::saturation_wait_should_finish;
    return !saturation_wait_should_finish(false, false) &&
           saturation_wait_should_finish(false, true) &&
           saturation_wait_should_finish(true, false);
  }

  bool output_poll_rearm_survives_concurrent_submission() {
    const uint64_t queried_through = 10;
    return amf::lifecycle::should_disarm_output_poll(queried_through, 10, false, 0) &&
           !amf::lifecycle::should_disarm_output_poll(queried_through, 10, false, 1) &&
           !amf::lifecycle::should_disarm_output_poll(queried_through, 11, false, 0) &&
           !amf::lifecycle::should_disarm_output_poll(queried_through, 10, true, 0) &&
           !amf::lifecycle::output_poll_requires_fixed_backoff(1, true, true) &&
           !amf::lifecycle::output_poll_requires_fixed_backoff(1, true, false) &&
           amf::lifecycle::output_poll_requires_fixed_backoff(0, true, true) &&
           amf::lifecycle::output_poll_requires_fixed_backoff(0, false, false) &&
           !amf::lifecycle::output_poll_requires_fixed_backoff(0, false, true);
  }

  bool strict_application_depth_preserves_driver_headroom() {
    using amf::lifecycle::low_latency_submit_capacity_available;

    // Cold start may prime four retained native surfaces, independent of the
    // driver's larger configured input-queue capacity.
    const bool cold_start = low_latency_submit_capacity_available(0, 0, 0, 4, std::nullopt) &&
                            low_latency_submit_capacity_available(1, 0, 0, 4, std::nullopt) &&
                            low_latency_submit_capacity_available(3, 0, 0, 4, std::nullopt) &&
                            !low_latency_submit_capacity_available(4, 0, 0, 4, std::nullopt);

    // A normal no-PA encoder learns that it retains no output after completion,
    // making the application depth one. PA preserves exactly its lookahead.
    const auto direct_depth = amf::lifecycle::refine_low_latency_pipeline_depth(
      std::nullopt, 1, 1, 0);
    const auto pa_depth = amf::lifecycle::refine_low_latency_pipeline_depth(
      std::nullopt, 2, 1, 1);
    const bool steady_state = low_latency_submit_capacity_available(4, 4, 0, 4, direct_depth) &&
                              !low_latency_submit_capacity_available(5, 4, 0, 4, direct_depth) &&
                              low_latency_submit_capacity_available(5, 4, 1, 6, pa_depth) &&
                              !low_latency_submit_capacity_available(6, 4, 1, 6, pa_depth);

    // If a driver needs four inputs before its first output, retain that observed
    // three-frame baseline so the gate cannot deadlock. Later catch-up can only
    // reduce the learned depth; it can never let latency drift upward.
    const auto retained_depth = amf::lifecycle::refine_low_latency_pipeline_depth(
      std::nullopt, 4, 1, 0);
    const auto caught_up_depth = amf::lifecycle::refine_low_latency_pipeline_depth(
      retained_depth, 4, 3, 0);
    const bool retained_driver = retained_depth == 3 &&
                                 low_latency_submit_capacity_available(4, 1, 0, 4, retained_depth) &&
                                 !low_latency_submit_capacity_available(5, 1, 0, 4, retained_depth) &&
                                 caught_up_depth == 1;
    return cold_start && steady_state && retained_driver;
  }

  bool dropped_submission_preserves_latest_static_and_idr_image() {
    using namespace amf::lifecycle;
    struct slot_t: input_surface_state_t { int pixels = 0; };
    std::array<slot_t, 2> slots;
    slots[0].state = input_surface_state_e::in_flight;
    slots[0].frame_index = 41;
    slots[0].pixels = 17; // Previous frame can still be owned by AMF.
    slots[1].state = input_surface_state_e::reserved;
    slots[1].pixels = 42; // Last real conversion before capture goes static.
    std::optional<std::size_t> prepared {1}, last_rendered {0};
    release_unsubmitted_capture(slots, prepared, last_rendered);
    if (prepared || last_rendered != 1 || slots[1].state != input_surface_state_e::free ||
        slots[0].state != input_surface_state_e::in_flight || slots[0].frame_index != 41) return false;
    const auto repeat = select_repeat_surface(slots, *last_rendered, 0, slots.size());
    if (repeat != 1 || slots[*repeat].pixels != 42) return false;
    // Missing or incorrectly owned reservations cannot steal driver ownership.
    release_unsubmitted_capture(slots, prepared, last_rendered);
    prepared = 0;
    release_unsubmitted_capture(slots, prepared, last_rendered);
    return !prepared && last_rendered == 1 && slots[0].state == input_surface_state_e::in_flight;
  }

  bool asynchronous_pipeline_catches_up_to_current_output() {
    using amf::lifecycle::output_coalesce_budget;
    using amf::lifecycle::driver_wait_budget;
    using amf::lifecycle::output_coalesce_target_reached;

    return output_coalesce_budget(30) == std::chrono::milliseconds(50) &&
           output_coalesce_budget(60) == std::chrono::milliseconds(33) &&
           output_coalesce_budget(120) == std::chrono::milliseconds(17) &&
           output_coalesce_budget(240) == std::chrono::milliseconds(9) &&
           output_coalesce_budget(1000) == std::chrono::milliseconds(2) &&
           driver_wait_budget(30) == std::chrono::milliseconds(20) &&
           driver_wait_budget(60) == std::chrono::milliseconds(16) &&
           driver_wait_budget(120) == std::chrono::milliseconds(8) &&
           driver_wait_budget(240) == std::chrono::milliseconds(4) &&
           !output_coalesce_target_reached(11, 10, 10, 10) &&
           !output_coalesce_target_reached(11, 10, 11, 10) &&
           output_coalesce_target_reached(11, 10, 11, 11) &&
           output_coalesce_target_reached(11, 10, 12, 12) &&
           output_coalesce_target_reached(10, 10, 10, 10) &&
           !output_coalesce_target_reached(10, 10, 11, 9) &&
           output_coalesce_target_reached(10, 10, 11, 10) &&
           !amf::lifecycle::output_delivery_is_due(1, 0, 1, false) &&
           amf::lifecycle::output_delivery_is_due(2, 0, 1, false) &&
           !amf::lifecycle::output_delivery_is_due(2, 1, 1, false) &&
           amf::lifecycle::output_delivery_is_due(2, 1, 1, true) &&
           !amf::lifecycle::retained_preanalysis_tail_exists(1, 0, 0) &&
           amf::lifecycle::retained_preanalysis_tail_exists(1, 0, 1) &&
           amf::lifecycle::retained_preanalysis_tail_exists(2, 1, 1) &&
           !amf::lifecycle::retained_preanalysis_tail_exists(3, 1, 1) &&
           !amf::lifecycle::retained_preanalysis_tail_exists(2, 2, 1) &&
           amf::lifecycle::preanalysis_tail_flush_is_due(true, false) &&
           !amf::lifecycle::preanalysis_tail_flush_is_due(true, true) &&
           !amf::lifecycle::preanalysis_tail_flush_is_due(false, false);
  }

  bool preanalysis_target_tracks_accepted_indices_with_gaps() {
    std::deque<uint64_t> accepted;
    const auto first = amf::lifecycle::record_accepted_frame(accepted, 1, 1);
    // Frame 2 was rejected and is intentionally absent from this sequence.
    const auto third = amf::lifecycle::record_accepted_frame(accepted, 3, 1);
    const auto fourth = amf::lifecycle::record_accepted_frame(accepted, 4, 1);
    return !first && third && *third == 1 && fourth && *fourth == 3 &&
           amf::lifecycle::output_coalesce_target_reached(*third, 0, 1, 1) &&
           !amf::lifecycle::output_coalesce_target_reached(*fourth, 1, 2, 1) &&
           amf::lifecycle::output_coalesce_target_reached(*fourth, 1, 2, 3);
  }

  bool teardown_timeout_returns_control_before_a_wedged_destructor() {
    struct slow_resource_t {
      std::atomic<bool> *destroyed;
      std::atomic<bool> *release;
      ~slow_resource_t() {
        // Keep destruction blocked until the watchdog returns, independent of
        // thread-launch cost or Windows timer granularity.
        release->wait(false, std::memory_order_acquire);
        destroyed->store(true, std::memory_order_release);
      }
    };

    std::atomic<bool> destroyed {false};
    std::atomic<bool> release {false};
    auto resource = std::make_unique<slow_resource_t>();
    resource->destroyed = &destroyed;
    resource->release = &release;
    const auto start = std::chrono::steady_clock::now();
    const bool completed = amf::lifecycle::run_with_timeout(
      [resource = std::move(resource)]() mutable { resource.reset(); },
      5ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const bool returned_before_destruction = !destroyed.load(std::memory_order_acquire);
    release.store(true, std::memory_order_release);
    release.notify_one();

    const auto cleanup_deadline = std::chrono::steady_clock::now() + 1s;
    while (!destroyed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < cleanup_deadline) {
      std::this_thread::sleep_for(1ms);
    }
    // Generous bound: the point is "returned promptly instead of blocking on the
    // wedged destructor", not a scheduler-sensitive exact latency.
    return !completed && returned_before_destruction && elapsed < 2s && destroyed.load(std::memory_order_acquire);
  }

  bool runtime_gate_fences_initialization_against_teardown_and_quarantine() {
    amf::lifecycle::native_runtime_gate_t gate;
    if (!gate.try_begin_initialization()) {
      return false;
    }
    std::atomic<bool> teardown_entered {false};
    std::thread teardown {[&]() {
      if (gate.begin_teardown()) {
        teardown_entered.store(true, std::memory_order_release);
        gate.finish_teardown(true);
      }
    }};
    std::this_thread::sleep_for(5ms);
    if (teardown_entered.load(std::memory_order_acquire) || gate.runtime_is_idle()) {
      gate.cancel_initialization();
      teardown.join();
      return false;
    }
    gate.cancel_initialization();
    teardown.join();
    if (!gate.runtime_is_idle() || !gate.try_begin_initialization()) {
      return false;
    }
    gate.quarantine_initialization();
    return !gate.finish_initialization() && gate.is_quarantined() &&
           !gate.runtime_is_idle() && !gate.try_begin_initialization();
  }

  bool timed_out_teardown_quarantines_and_retains_the_runtime_fence() {
    amf::lifecycle::native_runtime_gate_t gate;
    if (!gate.begin_teardown()) return false;
    gate.finish_teardown(false);
    const auto start = std::chrono::steady_clock::now();
    const bool second_teardown_entered = gate.begin_teardown();
    return !second_teardown_entered &&
           std::chrono::steady_clock::now() - start < 50ms &&
           gate.is_quarantined() && gate.operation_in_progress() &&
           !gate.try_begin_initialization() && !gate.runtime_is_idle();
  }

  bool timed_out_worker_handoff_reaps_on_the_producer_thread() {
    struct resource_t {
      std::thread::id *destroyed_on;
      ~resource_t() { *destroyed_on = std::this_thread::get_id(); }
    };

    auto handoff = std::make_shared<amf::lifecycle::worker_handoff_t<std::unique_ptr<resource_t>>>();
    std::thread::id destroyed_on;
    std::thread::id worker_id;
    std::atomic<bool> publish_after_timeout {false};
    std::thread worker {[&]() {
      worker_id = std::this_thread::get_id();
      publish_after_timeout.wait(false, std::memory_order_acquire);
      auto resource = std::make_unique<resource_t>();
      resource->destroyed_on = &destroyed_on;
      handoff->publish(std::move(resource));
    }};
    const auto accepted = handoff->accept_until(
      std::chrono::steady_clock::now() + 2ms,
      []() { return false; });
    // A slow thread constructor must not make the producer's result ready
    // before the test establishes its deadline.
    publish_after_timeout.store(true, std::memory_order_release);
    publish_after_timeout.notify_one();
    worker.join();
    return !accepted && destroyed_on == worker_id;
  }

  bool ready_handoff_rechecks_deadline_and_cancellation() {
    struct resource_t {
      std::thread::id &destroyed_on;
      ~resource_t() { destroyed_on = std::this_thread::get_id(); }
    };
    struct candidate_t {
      std::unique_ptr<resource_t> resource;
      std::atomic<bool> &stored;
      candidate_t(std::unique_ptr<resource_t> owned, std::atomic<bool> &ready): resource(std::move(owned)), stored(ready) {}
      candidate_t(candidate_t &&other) noexcept: resource(std::move(other.resource)), stored(other.stored) {
        stored.store(true, std::memory_order_release);
        stored.notify_one();
      }
    };
    for (bool cancelled : {false, true}) {
      std::atomic<bool> stored {false};
      std::thread::id destroyed_on, producer_id;
      auto handoff = std::make_shared<amf::lifecycle::worker_handoff_t<candidate_t>>();
      std::thread producer {[&]() {
        producer_id = std::this_thread::get_id();
        handoff->publish(candidate_t {std::make_unique<resource_t>(destroyed_on), stored});
      }};
      // The candidate move runs under the handoff mutex. Once we acquire that
      // mutex, publish has set ready and parked the producer: no timing sleeps.
      stored.wait(false, std::memory_order_acquire);
      bool was_cancelled = false;
      auto accepted = handoff->accept_until(
        std::chrono::steady_clock::now() + (cancelled ? 1s : -1s),
        [&]() { return cancelled; }, &was_cancelled);
      producer.join();
      if (accepted || was_cancelled != cancelled || destroyed_on != producer_id) return false;
    }
    return true;
  }

  bool normal_cancellation_does_not_quarantine_the_runtime() {
    amf::lifecycle::native_runtime_gate_t gate;
    if (!gate.try_begin_initialization()) return false;
    auto handoff = std::make_shared<amf::lifecycle::worker_handoff_t<int>>();
    bool was_cancelled = false;
    const auto accepted = handoff->accept_until(
      std::chrono::steady_clock::now() + 1s,
      []() { return true; },
      &was_cancelled);
    gate.cancel_initialization();
    return !accepted && was_cancelled && !gate.is_quarantined() &&
           gate.runtime_is_idle();
  }

  bool teardown_gate_contention_respects_its_deadline() {
    amf::lifecycle::native_runtime_gate_t gate;
    if (!gate.try_begin_initialization()) return false;
    const auto start = std::chrono::steady_clock::now();
    const auto admission = gate.begin_teardown_until(start + 2ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    gate.cancel_initialization();
    return admission == amf::lifecycle::teardown_admission_e::contended &&
           elapsed < 2s && !gate.is_quarantined() &&
           gate.runtime_is_idle();
  }

  bool smart_access_video_avoids_aggressive_low_latency_driver_path() {
    const auto automatic = amf::lifecycle::resolve_smart_access_video(std::nullopt, std::nullopt);
    const auto sav_only = amf::lifecycle::resolve_smart_access_video(true, std::nullopt);
    const auto low_latency_only = amf::lifecycle::resolve_smart_access_video(std::nullopt, true);
    const auto unsafe_combination = amf::lifecycle::resolve_smart_access_video(true, true);
    const auto explicit_opt_out = amf::lifecycle::resolve_smart_access_video(false, true);

    return !automatic.enabled && !automatic.disabled_for_low_latency &&
           sav_only.enabled && *sav_only.enabled && !sav_only.disabled_for_low_latency &&
           !low_latency_only.enabled && !low_latency_only.disabled_for_low_latency &&
           unsafe_combination.enabled && !*unsafe_combination.enabled && unsafe_combination.disabled_for_low_latency &&
           explicit_opt_out.enabled && !*explicit_opt_out.enabled && !explicit_opt_out.disabled_for_low_latency;
  }

  bool concurrent_native_session_remains_usable() {
    const auto first = amf::lifecycle::resolve_concurrent_session_features(1, true, true);
    const auto second = amf::lifecycle::resolve_concurrent_session_features(2, true, true);
    const auto automatic = amf::lifecycle::resolve_concurrent_session_features(
      2,
      std::nullopt,
      std::nullopt);
    const auto explicit_opt_out = amf::lifecycle::resolve_concurrent_session_features(2, false, false);

    return first.low_latency_mode && *first.low_latency_mode &&
           first.high_motion_quality_boost && *first.high_motion_quality_boost &&
           !first.overrides_suppressed &&
           !second.low_latency_mode && !second.high_motion_quality_boost &&
           second.overrides_suppressed &&
           !automatic.low_latency_mode && !automatic.high_motion_quality_boost &&
           !automatic.overrides_suppressed &&
           explicit_opt_out.low_latency_mode && !*explicit_opt_out.low_latency_mode &&
           explicit_opt_out.high_motion_quality_boost && !*explicit_opt_out.high_motion_quality_boost &&
           !explicit_opt_out.overrides_suppressed;
  }

}  // namespace

#ifdef SUNSHINE_AMF_LIFECYCLE_STANDALONE

int main() {
  const std::pair<const char *, bool (*)()> cases[] {
    {"capture_wait_completion_is_delivered_before_conversion", capture_wait_completion_is_delivered_before_conversion},
    {"ready_output_drain_is_optional_and_propagates_failures", ready_output_drain_is_optional_and_propagates_failures},
    {"av1_tiles_auto_preserves_client_and_preset_behavior", av1_tiles_auto_preserves_client_and_preset_behavior},
    {"av1_tiles_override_includes_explicit_single_tile", av1_tiles_override_includes_explicit_single_tile},
    {"av1_tiles_rejects_invalid_host_overrides", av1_tiles_rejects_invalid_host_overrides},
    {"synchronous_release_during_submit_is_reentrant_safe", synchronous_release_during_submit_is_reentrant_safe},
    {"backpressure_retries_the_same_submission_until_accepted", backpressure_retries_the_same_submission_until_accepted},
    {"exhausted_backpressure_reinitializes_without_owned_surfaces", exhausted_backpressure_reinitializes_without_owned_surfaces},
    {"recovery_state_changes_only_after_accepted_input", recovery_state_changes_only_after_accepted_input},
    {"preanalysis_dependent_rate_control_is_planned_natively", preanalysis_dependent_rate_control_is_planned_natively},
    {"preanalysis_pipeline_primes_and_drains_in_order", preanalysis_pipeline_primes_and_drains_in_order},
    {"automatic_h264_coder_preserves_driver_default", automatic_h264_coder_preserves_driver_default},
    {"automatic_prefers_native_and_explicit_choices_are_strict", automatic_prefers_native_and_explicit_choices_are_strict},
    {"initialization_fallback_respects_success_and_explicit_choices", initialization_fallback_respects_success_and_explicit_choices},
    {"successful_rollback_uses_the_same_stream_format", successful_rollback_uses_the_same_stream_format},
    {"encoder_initialization_fallback_is_bounded_and_owned", encoder_initialization_fallback_is_bounded_and_owned},
    {"unrelated_or_unsupported_failures_never_fallback", unrelated_or_unsupported_failures_never_fallback},
    {"rollback_preserves_negotiated_format_requirements", rollback_preserves_negotiated_format_requirements},
    {"xbox_intra_refresh_maps_to_native_amf", xbox_intra_refresh_maps_to_native_amf},
    {"hevc_gdr_uses_negotiated_codec_and_dimensions", hevc_gdr_uses_negotiated_codec_and_dimensions},
    {"effective_reference_frame_limit_matches_configure_and_verify", effective_reference_frame_limit_matches_configure_and_verify},
    {"repeated_input_rotates_away_from_a_lookahead_owned_surface", repeated_input_rotates_away_from_a_lookahead_owned_surface},
    {"surface_pool_can_prime_a_retaining_driver", surface_pool_can_prime_a_retaining_driver},
    {"driver_submit_capacity_bounds_are_inclusive", driver_submit_capacity_bounds_are_inclusive},
    {"saturation_wait_requires_an_actual_surface_release", saturation_wait_requires_an_actual_surface_release},
    {"output_poll_rearm_survives_concurrent_submission", output_poll_rearm_survives_concurrent_submission},
    {"strict_application_depth_preserves_driver_headroom", strict_application_depth_preserves_driver_headroom},
    {"dropped_submission_preserves_latest_static_and_idr_image", dropped_submission_preserves_latest_static_and_idr_image},
    {"asynchronous_pipeline_catches_up_to_current_output", asynchronous_pipeline_catches_up_to_current_output},
    {"preanalysis_target_tracks_accepted_indices_with_gaps", preanalysis_target_tracks_accepted_indices_with_gaps},
    {"teardown_timeout_returns_control_before_a_wedged_destructor", teardown_timeout_returns_control_before_a_wedged_destructor},
    {"runtime_gate_fences_initialization_against_teardown_and_quarantine", runtime_gate_fences_initialization_against_teardown_and_quarantine},
    {"timed_out_teardown_quarantines_and_retains_the_runtime_fence", timed_out_teardown_quarantines_and_retains_the_runtime_fence},
    {"timed_out_worker_handoff_reaps_on_the_producer_thread", timed_out_worker_handoff_reaps_on_the_producer_thread},
    {"ready_handoff_rechecks_deadline_and_cancellation", ready_handoff_rechecks_deadline_and_cancellation},
    {"normal_cancellation_does_not_quarantine_the_runtime", normal_cancellation_does_not_quarantine_the_runtime},
    {"teardown_gate_contention_respects_its_deadline", teardown_gate_contention_respects_its_deadline},
    {"smart_access_video_avoids_aggressive_low_latency_driver_path", smart_access_video_avoids_aggressive_low_latency_driver_path},
    {"concurrent_native_session_remains_usable", concurrent_native_session_remains_usable},
  };
  int failed = 0;
  for (const auto &[name, run] : cases) {
    if (!run()) {
      std::fprintf(stderr, "FAILED: %s\n", name);
      ++failed;
    }
  }
  std::fprintf(stdout, "%zu native AMF policy cases, %d failures\n", std::size(cases), failed);
  return failed ? 1 : 0;
}

#else

TEST(SunshineNativeAmfReview, CaptureWaitCompletionIsDeliveredBeforeConversion) {
  EXPECT_TRUE(capture_wait_completion_is_delivered_before_conversion());
}

TEST(SunshineNativeAmfReview, ReadyOutputDrainIsOptionalAndPropagatesFailures) {
  EXPECT_TRUE(ready_output_drain_is_optional_and_propagates_failures());
}

TEST(SunshineNativeAmfReview, Av1TilesAutoPreservesClientAndPresetBehavior) {
  EXPECT_TRUE(av1_tiles_auto_preserves_client_and_preset_behavior());
}

TEST(SunshineNativeAmfReview, Av1TilesOverrideIncludesExplicitSingleTile) {
  EXPECT_TRUE(av1_tiles_override_includes_explicit_single_tile());
}

TEST(SunshineNativeAmfReview, Av1TilesRejectsInvalidHostOverrides) {
  EXPECT_TRUE(av1_tiles_rejects_invalid_host_overrides());
}

TEST(SunshineNativeAmfReview, InitializationFallbackRespectsSuccessAndExplicitChoices) {
  EXPECT_TRUE(initialization_fallback_respects_success_and_explicit_choices());
}
TEST(SunshineNativeAmfReview, SuccessfulRollbackUsesTheSameStreamFormat) {
  EXPECT_TRUE(successful_rollback_uses_the_same_stream_format());
}
TEST(SunshineNativeAmfReview, EncoderInitializationFallbackIsBoundedAndOwned) {
  EXPECT_TRUE(encoder_initialization_fallback_is_bounded_and_owned());
}
TEST(SunshineNativeAmfReview, UnrelatedOrUnsupportedFailuresNeverFallback) {
  EXPECT_TRUE(unrelated_or_unsupported_failures_never_fallback());
}
TEST(SunshineNativeAmfReview, RollbackPreservesNegotiatedFormatRequirements) {
  EXPECT_TRUE(rollback_preserves_negotiated_format_requirements());
}

TEST(SunshineNativeAmfReview, SynchronousReleaseDuringSubmitIsReentrantSafe) {
  EXPECT_TRUE(synchronous_release_during_submit_is_reentrant_safe());
}

TEST(SunshineNativeAmfReview, BackpressureRetriesUntilAccepted) {
  EXPECT_TRUE(backpressure_retries_the_same_submission_until_accepted());
}

TEST(SunshineNativeAmfReview, ExhaustedBackpressureReinitializesWithoutOwnedSurfaces) {
  EXPECT_TRUE(exhausted_backpressure_reinitializes_without_owned_surfaces());
}

TEST(SunshineNativeAmfReview, RecoveryStateChangesOnlyAfterAcceptance) {
  EXPECT_TRUE(recovery_state_changes_only_after_accepted_input());
}

TEST(SunshineNativeAmfReview, PreAnalysisDependentRateControlIsPlannedNatively) {
  EXPECT_TRUE(preanalysis_dependent_rate_control_is_planned_natively());
}

TEST(SunshineNativeAmfReview, PreAnalysisPipelinePrimesAndDrainsInOrder) {
  EXPECT_TRUE(preanalysis_pipeline_primes_and_drains_in_order());
}

TEST(SunshineNativeAmfReview, AutomaticH264CoderPreservesDriverDefault) {
  EXPECT_TRUE(automatic_h264_coder_preserves_driver_default());
}

TEST(SunshineNativeAmfReview, AutomaticPrefersNativeAndExplicitChoicesAreStrict) {
  EXPECT_TRUE(automatic_prefers_native_and_explicit_choices_are_strict());
}

TEST(SunshineNativeAmfReview, XboxIntraRefreshMapsToNativeAmf) {
  EXPECT_TRUE(xbox_intra_refresh_maps_to_native_amf());
}

TEST(SunshineNativeAmfReview, HevcGdrUsesNegotiatedCodecAndDimensions) {
  EXPECT_TRUE(hevc_gdr_uses_negotiated_codec_and_dimensions());
}

TEST(SunshineNativeAmfReview, EffectiveReferenceFrameLimitMatchesConfigureAndVerify) {
  EXPECT_TRUE(effective_reference_frame_limit_matches_configure_and_verify());
}


TEST(SunshineNativeAmfReview, RepeatedInputRotatesAwayFromLookaheadOwnedSurface) {
  EXPECT_TRUE(repeated_input_rotates_away_from_a_lookahead_owned_surface());
}

TEST(SunshineNativeAmfReview, SurfacePoolPrimesRetainingDriver) {
  EXPECT_TRUE(surface_pool_can_prime_a_retaining_driver());
}

TEST(SunshineNativeAmfReview, PreanalysisPreservesLowLatencyQueue) {
  EXPECT_TRUE(driver_submit_capacity_bounds_are_inclusive());
}

TEST(SunshineNativeAmfReview, SaturationWaitRequiresActualSurfaceRelease) {
  EXPECT_TRUE(saturation_wait_requires_an_actual_surface_release());
}

TEST(SunshineNativeAmfReview, OutputPollRearmSurvivesConcurrentSubmission) {
  EXPECT_TRUE(output_poll_rearm_survives_concurrent_submission());
}

TEST(SunshineNativeAmfReview, StrictApplicationDepthPreservesDriverHeadroom) {
  EXPECT_TRUE(strict_application_depth_preserves_driver_headroom());
}

TEST(SunshineNativeAmfReview, DroppedSubmissionPreservesLatestStaticAndIdrImage) {
  EXPECT_TRUE(dropped_submission_preserves_latest_static_and_idr_image());
}

TEST(SunshineNativeAmfReview, AsynchronousPipelineCatchesUpToCurrentOutput) {
  EXPECT_TRUE(asynchronous_pipeline_catches_up_to_current_output());
}

TEST(SunshineNativeAmfReview, PreanalysisTargetTracksAcceptedIndicesWithGaps) {
  EXPECT_TRUE(preanalysis_target_tracks_accepted_indices_with_gaps());
}

TEST(SunshineNativeAmfReview, TeardownTimeoutReturnsControl) {
  EXPECT_TRUE(teardown_timeout_returns_control_before_a_wedged_destructor());
}

TEST(SunshineNativeAmfReview, RuntimeGateFencesInitializationAgainstTeardownAndQuarantine) {
  EXPECT_TRUE(runtime_gate_fences_initialization_against_teardown_and_quarantine());
}

TEST(SunshineNativeAmfReview, TimedOutTeardownQuarantinesAndRetainsRuntimeFence) {
  EXPECT_TRUE(timed_out_teardown_quarantines_and_retains_the_runtime_fence());
}

TEST(SunshineNativeAmfReview, TimedOutWorkerHandoffReapsOnProducerThread) {
  EXPECT_TRUE(timed_out_worker_handoff_reaps_on_the_producer_thread());
}

TEST(SunshineNativeAmfReview, ReadyHandoffRechecksDeadlineAndCancellation) {
  EXPECT_TRUE(ready_handoff_rechecks_deadline_and_cancellation());
}

TEST(SunshineNativeAmfReview, NormalCancellationDoesNotQuarantineRuntime) {
  EXPECT_TRUE(normal_cancellation_does_not_quarantine_the_runtime());
}

TEST(SunshineNativeAmfReview, TeardownGateContentionRespectsDeadline) {
  EXPECT_TRUE(teardown_gate_contention_respects_its_deadline());
}

TEST(SunshineNativeAmfReview, SmartAccessVideoAvoidsAggressiveLowLatencyDriverPath) {
  EXPECT_TRUE(smart_access_video_avoids_aggressive_low_latency_driver_path());
}

TEST(SunshineNativeAmfReview, ConcurrentNativeSessionRemainsUsable) {
  EXPECT_TRUE(concurrent_native_session_remains_usable());
}

#endif
