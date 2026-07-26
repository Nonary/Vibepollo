/**
 * @file tests/unit/platform/windows/test_native_amf_review.cpp
 * @brief Behavioral tests for native AMF ownership and lifecycle policy.
 */

#include "src/amf/amf_lifecycle.h"

#include <array>
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

  bool native_selection_never_routes_to_legacy() {
    const auto automatic = amf::lifecycle::encoder_selection_policy("");
    const auto native = amf::lifecycle::encoder_selection_policy("amdvce");
    const auto legacy = amf::lifecycle::encoder_selection_policy("amdvce_legacy");

    return !automatic.include_legacy && !automatic.fail_closed &&
           !native.include_legacy && native.fail_closed &&
           legacy.include_legacy && !legacy.fail_closed;
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
           !amf::lifecycle::should_disarm_output_poll(queried_through, 10, true, 0);
  }

  bool asynchronous_pipeline_catches_up_to_current_output() {
    using amf::lifecycle::output_coalesce_budget;
    using amf::lifecycle::driver_wait_budget;
    using amf::lifecycle::output_coalesce_target_reached;

    return output_coalesce_budget(30) == std::chrono::milliseconds(32) &&
           output_coalesce_budget(60) == std::chrono::milliseconds(16) &&
           output_coalesce_budget(120) == std::chrono::milliseconds(8) &&
           output_coalesce_budget(240) == std::chrono::milliseconds(4) &&
           output_coalesce_budget(1000) == std::chrono::milliseconds(1) &&
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
      ~slow_resource_t() {
        std::this_thread::sleep_for(150ms);
        destroyed->store(true, std::memory_order_release);
      }
    };

    std::atomic<bool> destroyed {false};
    auto resource = std::make_unique<slow_resource_t>();
    resource->destroyed = &destroyed;
    const auto start = std::chrono::steady_clock::now();
    const bool completed = amf::lifecycle::run_with_timeout(
      [resource = std::move(resource)]() mutable { resource.reset(); },
      5ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const auto cleanup_deadline = std::chrono::steady_clock::now() + 1s;
    while (!destroyed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < cleanup_deadline) {
      std::this_thread::sleep_for(1ms);
    }
    // Generous bound: the point is "returned promptly instead of blocking on the
    // wedged destructor", not a scheduler-sensitive exact latency.
    return !completed && elapsed < 2s && destroyed.load(std::memory_order_acquire);
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
    std::thread worker {[&]() {
      worker_id = std::this_thread::get_id();
      std::this_thread::sleep_for(20ms);
      auto resource = std::make_unique<resource_t>();
      resource->destroyed_on = &destroyed_on;
      handoff->publish(std::move(resource));
    }};
    const auto accepted = handoff->accept_until(
      std::chrono::steady_clock::now() + 2ms,
      []() { return false; });
    worker.join();
    return !accepted && destroyed_on == worker_id;
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
  return synchronous_release_during_submit_is_reentrant_safe() &&
             backpressure_retries_the_same_submission_until_accepted() &&
             exhausted_backpressure_reinitializes_without_owned_surfaces() &&
             recovery_state_changes_only_after_accepted_input() &&
             preanalysis_dependent_rate_control_is_planned_natively() &&
             preanalysis_pipeline_primes_and_drains_in_order() &&
             automatic_h264_coder_preserves_driver_default() &&
             native_selection_never_routes_to_legacy() &&
             xbox_intra_refresh_maps_to_native_amf() &&
             effective_reference_frame_limit_matches_configure_and_verify() &&
             repeated_input_rotates_away_from_a_lookahead_owned_surface() &&
             surface_pool_can_prime_a_retaining_driver() &&
             driver_submit_capacity_bounds_are_inclusive() &&
             saturation_wait_requires_an_actual_surface_release() &&
             output_poll_rearm_survives_concurrent_submission() &&
             asynchronous_pipeline_catches_up_to_current_output() &&
             preanalysis_target_tracks_accepted_indices_with_gaps() &&
             teardown_timeout_returns_control_before_a_wedged_destructor() &&
             runtime_gate_fences_initialization_against_teardown_and_quarantine() &&
             timed_out_teardown_quarantines_and_retains_the_runtime_fence() &&
             timed_out_worker_handoff_reaps_on_the_producer_thread() &&
             normal_cancellation_does_not_quarantine_the_runtime() &&
             teardown_gate_contention_respects_its_deadline() &&
             smart_access_video_avoids_aggressive_low_latency_driver_path() &&
             concurrent_native_session_remains_usable() ?
           0 :
           1;
}

#else

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

TEST(SunshineNativeAmfReview, NativeSelectionNeverRoutesToLegacy) {
  EXPECT_TRUE(native_selection_never_routes_to_legacy());
}

TEST(SunshineNativeAmfReview, XboxIntraRefreshMapsToNativeAmf) {
  EXPECT_TRUE(xbox_intra_refresh_maps_to_native_amf());
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
