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

  bool every_rate_control_mode_has_a_bounded_first_packet_plan() {
    for (int mode = 0; mode <= 6; ++mode) {
      const auto plan = amf::lifecycle::resolve_preanalysis(mode, 0);
      const bool should_use_pa = mode >= 4;
      if (plan.enabled != should_use_pa ||
          plan.lookahead_depth != (should_use_pa ? 1 : 0)) {
        return false;
      }

      std::deque<int> retained;
      bool first_packet = false;
      const int bounded_submissions = plan.lookahead_depth + 1;
      for (int submission = 0; submission < bounded_submissions; ++submission) {
        retained.push_back(submission);
        if (static_cast<int>(retained.size()) > plan.lookahead_depth) {
          retained.pop_front();
          first_packet = true;
        }
      }
      if (!first_packet) return false;
    }

    return !amf::lifecycle::rate_control_uses_bitrate_updates(0) &&
           amf::lifecycle::rate_control_uses_bitrate_updates(1) &&
           amf::lifecycle::rate_control_uses_bitrate_updates(2) &&
           amf::lifecycle::rate_control_uses_bitrate_updates(3) &&
           !amf::lifecycle::rate_control_uses_bitrate_updates(4) &&
           amf::lifecycle::rate_control_uses_bitrate_updates(5) &&
           amf::lifecycle::rate_control_uses_bitrate_updates(6);
  }

  bool rate_control_policy_covers_all_semantic_modes() {
    for (int mode = 0; mode <= 6; ++mode) {
      const auto policy = amf::lifecycle::rate_control_policy(mode);
      const bool no_bitrate_contract = mode == 0 || mode == 4;
      const bool requires_pa = mode >= 4;
      if (!policy.valid || policy.requires_preanalysis != requires_pa ||
          policy.uses_target_bitrate == no_bitrate_contract ||
          policy.uses_peak_bitrate == no_bitrate_contract ||
          policy.uses_vbv == no_bitrate_contract ||
          policy.supports_live_bitrate == no_bitrate_contract) {
        return false;
      }
    }
    const auto automatic = amf::lifecycle::rate_control_policy(std::nullopt);
    return automatic.valid && automatic.uses_target_bitrate &&
           !automatic.uses_peak_bitrate && !automatic.uses_vbv &&
           automatic.supports_live_bitrate;
  }

  bool live_bitrate_plan_skips_qvbr_and_hevc_static_vbv() {
    for (int video_format = 0; video_format <= 2; ++video_format) {
      for (int mode = 0; mode <= 6; ++mode) {
        const auto policy = amf::lifecycle::rate_control_policy(mode);
        const auto plan = amf::lifecycle::live_bitrate_plan(policy, video_format, true);
        const bool ignored = mode == 0 || mode == 4;
        if (plan.ignored_for_mode != ignored) return false;
        if (ignored && (plan.write_target || plan.write_peak || plan.write_vbv ||
                        plan.rebuild_if_static_vbv_changes ||
                        plan.write_maximum_frame_size)) return false;
        if (!ignored && (!plan.write_target || !plan.write_peak ||
                         plan.write_vbv != (video_format != 1) ||
                         plan.rebuild_if_static_vbv_changes != (video_format == 1) ||
                         !plan.write_maximum_frame_size)) return false;
      }
    }
    const auto h264_plan = amf::lifecycle::live_bitrate_plan(
      amf::lifecycle::rate_control_policy(3), 0, false);
    const auto hevc_plan = amf::lifecycle::live_bitrate_plan(
      amf::lifecycle::rate_control_policy(3), 1, false);
    return !amf::lifecycle::static_vbv_change_requires_rebuild(
             h264_plan, 1'333'333, 666'667) &&
           !amf::lifecycle::static_vbv_change_requires_rebuild(
             hevc_plan, 1'333'333, 1'333'333) &&
           amf::lifecycle::static_vbv_change_requires_rebuild(
             hevc_plan, std::nullopt, 666'667) &&
           amf::lifecycle::static_vbv_change_requires_rebuild(
             hevc_plan, 1'333'333, 666'667);
  }

  bool bitrate_worker_contention_defers_and_timeout_abandons() {
    using amf::lifecycle::bitrate_loop_action;
    using amf::lifecycle::bitrate_loop_action_e;
    using amf::lifecycle::bitrate_update_result_e;
    return bitrate_loop_action(bitrate_update_result_e::applied) == bitrate_loop_action_e::complete &&
           bitrate_loop_action(bitrate_update_result_e::ignored_for_mode) == bitrate_loop_action_e::complete &&
           bitrate_loop_action(bitrate_update_result_e::temporarily_busy) == bitrate_loop_action_e::defer_latest &&
           bitrate_loop_action(bitrate_update_result_e::requires_rebuild) == bitrate_loop_action_e::rebuild_once &&
           bitrate_loop_action(bitrate_update_result_e::vendor_timed_out) == bitrate_loop_action_e::abandon_generation;
  }

  bool advanced_rate_control_is_gated_per_codec_runtime() {
    for (int video_format = 0; video_format <= 1; ++video_format) {
      if (amf::lifecycle::runtime_supports_advanced_rate_control(video_format, 4, 1, 4, 20) ||
          !amf::lifecycle::runtime_supports_advanced_rate_control(video_format, 4, 1, 4, 21) ||
          amf::lifecycle::runtime_supports_advanced_rate_control(video_format, 5, 1, 4, 27) ||
          amf::lifecycle::runtime_supports_advanced_rate_control(video_format, 6, 1, 4, 27) ||
          !amf::lifecycle::runtime_supports_advanced_rate_control(video_format, 5, 1, 4, 28) ||
          !amf::lifecycle::runtime_supports_advanced_rate_control(video_format, 6, 1, 4, 28)) {
        return false;
      }
    }
    for (int mode = 4; mode <= 6; ++mode) {
      if (amf::lifecycle::runtime_supports_advanced_rate_control(2, mode, 1, 4, 27) ||
          !amf::lifecycle::runtime_supports_advanced_rate_control(2, mode, 1, 4, 28) ||
          !amf::lifecycle::runtime_supports_advanced_rate_control(2, mode, 2, 0, 0)) {
        return false;
      }
    }
    return !amf::lifecycle::runtime_supports_advanced_rate_control(-1, 4, 2, 0, 0) &&
           !amf::lifecycle::runtime_supports_advanced_rate_control(3, 4, 2, 0, 0) &&
           !amf::lifecycle::runtime_supports_advanced_rate_control(0, 3, 2, 0, 0) &&
           !amf::lifecycle::runtime_supports_advanced_rate_control(0, 7, 2, 0, 0);
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

  bool repeated_input_uses_a_replay_source_never_owned_by_amf() {
    std::array<amf::lifecycle::input_surface_state_t, 3> slots;
    const uint64_t replay_frame = 77;
    std::vector<uint64_t> repeated_frames;

    // AMF has produced output for slot zero but has not released the input
    // surface. A repeat must not read that still-owned texture.
    slots[0].state = amf::lifecycle::input_surface_state_e::in_flight;
    slots[1].state = amf::lifecycle::input_surface_state_e::free;
    slots[2].state = amf::lifecycle::input_surface_state_e::free;
    const auto first_repeat = amf::lifecycle::select_free_surface(slots, 1);
    if (!first_repeat || *first_repeat != 1) return false;
    repeated_frames.push_back(replay_frame);
    slots[*first_repeat].state = amf::lifecycle::input_surface_state_e::in_flight;

    // A second consecutive repeat is valid while both earlier submissions are
    // still owned because the immutable source is outside the AMF ring.
    const auto second_repeat = amf::lifecycle::select_free_surface(slots, 2);
    if (!second_repeat || *second_repeat != 2) return false;
    repeated_frames.push_back(replay_frame);
    slots[*second_repeat].state = amf::lifecycle::input_surface_state_e::in_flight;

    const auto unavailable = amf::lifecycle::select_free_surface(slots, 0);
    amf::lifecycle::on_surface_released(slots[0]);
    const auto after_release = amf::lifecycle::select_free_surface(slots, 0);
    if (after_release) repeated_frames.push_back(replay_frame);
    return !unavailable && after_release && *after_release == 0 &&
           repeated_frames == std::vector<uint64_t> {77, 77, 77};
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

  bool preanalysis_preserves_low_latency_queue_with_compatibility_fallback() {
    const auto normal = amf::lifecycle::input_queue_size_for_preanalysis(4, false);
    const auto automatic = amf::lifecycle::input_queue_size_for_preanalysis(std::nullopt, true);
    const auto clamped = amf::lifecycle::input_queue_size_for_preanalysis(4, true);
    const auto documented_default = amf::lifecycle::input_queue_size_for_preanalysis(16, true);
    const auto larger = amf::lifecycle::input_queue_size_for_preanalysis(24, true);

    return normal && *normal == 4 && !automatic &&
           clamped && *clamped == 4 &&
           documented_default && *documented_default == 16 &&
           larger && *larger == 24 &&
           amf::lifecycle::driver_submit_capacity_available(3, 4) &&
           !amf::lifecycle::driver_submit_capacity_available(4, 4) &&
           amf::lifecycle::driver_submit_capacity_available(15, 16) &&
           !amf::lifecycle::driver_submit_capacity_available(16, 16) &&
           amf::lifecycle::preanalysis_queue_fallback_is_recommended(true, true, 4, 0) &&
           !amf::lifecycle::preanalysis_queue_fallback_is_recommended(false, true, 4, 0) &&
           !amf::lifecycle::preanalysis_queue_fallback_is_recommended(true, false, 4, 0) &&
           !amf::lifecycle::preanalysis_queue_fallback_is_recommended(true, true, 16, 0) &&
           !amf::lifecycle::preanalysis_queue_fallback_is_recommended(true, true, 4, 1);
  }

  bool queue_boundary_normalization_and_cache_policy_match_production() {
    std::size_t entered_submissions = 0;
    std::size_t owned = 0;
    while (amf::lifecycle::driver_submit_capacity_available(owned, 4)) {
      ++entered_submissions;
      ++owned;
    }
    return entered_submissions == 4 && owned == 4 &&
           amf::lifecycle::normalized_input_queue_is_compatible(4, 16, true) &&
           !amf::lifecycle::normalized_input_queue_is_compatible(4, 16, false) &&
           !amf::lifecycle::normalized_input_queue_is_compatible(4, 24, true) &&
           amf::lifecycle::cached_pa_queue_may_override(std::nullopt, 16) &&
           amf::lifecycle::cached_pa_queue_may_override(4, 16) &&
           !amf::lifecycle::cached_pa_queue_may_override(24, 16) &&
           !amf::lifecycle::cached_pa_queue_may_override(32, 16);
  }

  bool output_pts_filter_rejects_missing_duplicate_and_regressing_packets() {
    amf::lifecycle::output_pts_tracker_t tracker;
    const int64_t negative_pts = -1;
    tracker.confirm_input_accepted(9);
    tracker.note_input_candidate(10);
    if (tracker.classify(negative_pts >= 0 ?
                           std::optional<uint64_t> {static_cast<uint64_t>(negative_pts)} :
                           std::nullopt) != amf::lifecycle::output_pts_result_e::missing ||
        tracker.classify(10) != amf::lifecycle::output_pts_result_e::pending_confirmation) {
      return false;
    }
    tracker.confirm_input_accepted(10);
    if (tracker.classify(10) != amf::lifecycle::output_pts_result_e::accepted ||
        tracker.classify(9) != amf::lifecycle::output_pts_result_e::duplicate_or_regressing ||
        tracker.classify(10) != amf::lifecycle::output_pts_result_e::not_from_accepted_input ||
        tracker.last() != std::optional<uint64_t> {10}) {
      return false;
    }
    tracker.note_input_candidate(11);
    tracker.discard_input_candidate(11);
    if (tracker.classify(11) != amf::lifecycle::output_pts_result_e::not_from_accepted_input) {
      return false;
    }
    tracker.confirm_input_accepted(12);
    return tracker.classify(12) == amf::lifecycle::output_pts_result_e::accepted;
  }

  bool delivery_filter_and_startup_budget_follow_client_visible_packets() {
    amf::lifecycle::monotonic_delivery_tracker_t delivery;
    return delivery.accept(10) && !delivery.accept(9) && !delivery.accept(10) &&
           delivery.last() == std::optional<uint64_t> {10} && delivery.accept(11) &&
           !amf::lifecycle::startup_budget_reset_allowed(false) &&
           amf::lifecycle::startup_budget_reset_allowed(true);
  }

  bool replay_policy_avoids_continuous_copies_and_never_reuses_owned_sources() {
    std::array<amf::lifecycle::input_surface_state_t, 3> slots;
    slots[0].state = amf::lifecycle::input_surface_state_e::in_flight;
    slots[1].state = amf::lifecycle::input_surface_state_e::replay_source;
    slots[2].state = amf::lifecycle::input_surface_state_e::free;
    const auto selected = amf::lifecycle::select_free_surface(slots, 0);
    return selected && *selected == 2 &&
           // Non-PA needs a temporary snapshot before its first released source,
           // then avoids continuous copies. PA always snapshots its newest input.
           amf::lifecycle::fresh_conversion_requires_replay_snapshot(false, false) &&
           !amf::lifecycle::fresh_conversion_requires_replay_snapshot(false, true) &&
           amf::lifecycle::fresh_conversion_requires_replay_snapshot(true, false) &&
           amf::lifecycle::fresh_conversion_requires_replay_snapshot(true, true) &&
           amf::lifecycle::unaccepted_fresh_input_must_remain_reserved(false, false, false) &&
           !amf::lifecycle::unaccepted_fresh_input_must_remain_reserved(true, false, false) &&
           !amf::lifecycle::unaccepted_fresh_input_must_remain_reserved(false, true, false) &&
           !amf::lifecycle::unaccepted_fresh_input_must_remain_reserved(false, false, true);
  }

  bool queue_sixteen_retry_is_prepacket_native_only_and_single_shot() {
    using amf::lifecycle::native_pa_queue_retry_should_run;
    const bool real_session_retry_after_probe_pass = native_pa_queue_retry_should_run(
      false, true, false, false);
    return real_session_retry_after_probe_pass &&
           !native_pa_queue_retry_should_run(true, true, false, false) &&
           !native_pa_queue_retry_should_run(false, false, false, false) &&
           !native_pa_queue_retry_should_run(false, true, true, false) &&
           !native_pa_queue_retry_should_run(false, true, false, true);
  }

  bool saturation_wait_requires_an_actual_surface_release() {
    using amf::lifecycle::saturation_wait_should_finish;
    return !saturation_wait_should_finish(false, false) &&
           saturation_wait_should_finish(false, true) &&
           saturation_wait_should_finish(true, false) &&
           saturation_wait_should_finish(false, false, true) &&
           !amf::lifecycle::output_wait_should_finish(false, false, false) &&
           amf::lifecycle::output_wait_should_finish(false, false, true) &&
           amf::lifecycle::output_wait_should_finish(false, true, false);
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
    return !completed && elapsed < 100ms && destroyed.load(std::memory_order_acquire);
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
    if (teardown_entered.load(std::memory_order_acquire) || gate.legacy_fallback_is_safe()) {
      gate.cancel_initialization();
      teardown.join();
      return false;
    }
    gate.cancel_initialization();
    teardown.join();
    if (!gate.legacy_fallback_is_safe() || !gate.try_begin_initialization()) {
      return false;
    }
    gate.quarantine_initialization();
    return !gate.finish_initialization() && gate.is_quarantined() &&
           !gate.legacy_fallback_is_safe() && !gate.try_begin_initialization();
  }

  bool teardown_reservation_closes_the_worker_registration_window() {
    amf::lifecycle::native_runtime_gate_t gate;
    if (!gate.try_begin_initialization() || !gate.reserve_teardown()) return false;

    // Publication loses as soon as the live session's teardown is reserved,
    // even though the worker has not started running vendor destruction yet.
    if (gate.finish_initialization()) return false;
    if (gate.try_begin_initialization()) return false;
    if (!gate.begin_reserved_teardown()) return false;
    if (gate.try_begin_initialization()) return false;
    gate.finish_teardown(true);
    return gate.try_begin_initialization();
  }

  bool ordinary_disconnect_defers_a_slow_destructor_without_opening_reconnect() {
    amf::lifecycle::native_runtime_gate_t gate;
    if (!gate.reserve_teardown()) return false;
    std::atomic<bool> destructor_finished {false};
    const auto disconnect_started = std::chrono::steady_clock::now();
    const bool scheduled = amf::lifecycle::run_detached([&]() {
      if (!gate.begin_reserved_teardown()) return;
      std::this_thread::sleep_for(40ms);
      destructor_finished.store(true, std::memory_order_release);
      gate.finish_teardown(true);
    });
    const auto disconnect_elapsed = std::chrono::steady_clock::now() - disconnect_started;
    if (!scheduled || disconnect_elapsed >= 30ms || gate.try_begin_initialization()) return false;

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!destructor_finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    return destructor_finished.load(std::memory_order_acquire) && gate.try_begin_initialization();
  }

  bool bitrate_and_one_frame_vbv_tolerate_only_small_normalization() {
    using amf::lifecycle::bitrate_contract_is_acceptable;
    using amf::lifecycle::one_frame_vbv_is_acceptable;
    return bitrate_contract_is_acceptable(79'900'000, 80'100'000, 80'000'000) &&
           !bitrate_contract_is_acceptable(70'000'000, 80'000'000, 80'000'000) &&
           !bitrate_contract_is_acceptable(80'000'000, 79'000'000, 80'000'000) &&
           one_frame_vbv_is_acceptable(1'330'000, 1'333'333) &&
           !one_frame_vbv_is_acceptable(2'000'000, 1'333'333) &&
           !one_frame_vbv_is_acceptable(800'000, 1'333'333);
  }

  bool equivalent_framerates_preserve_the_one_frame_vbv_contract() {
    using amf::lifecycle::rational_rates_are_equivalent;
    return rational_rates_are_equivalent(60, 1, 6000, 100) &&
           rational_rates_are_equivalent(6000, 1001, 12000, 2002) &&
           !rational_rates_are_equivalent(60, 1, 30, 1) &&
           !rational_rates_are_equivalent(60, 1, 0, 1) &&
           !rational_rates_are_equivalent(60, 0, 60, 1);
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
           !gate.try_begin_initialization() && !gate.legacy_fallback_is_safe();
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
    std::thread worker {[handoff]() {
      std::this_thread::sleep_for(5ms);
      handoff->publish(7);
    }};
    bool was_cancelled = false;
    const auto accepted = handoff->accept_until(
      std::chrono::steady_clock::now() + 1s,
      []() { return true; },
      &was_cancelled);
    const bool producer_unwound = handoff->producer_finished_until(
      std::chrono::steady_clock::now() + 100ms);
    worker.join();
    gate.cancel_initialization();
    return !accepted && was_cancelled && producer_unwound && !gate.is_quarantined() &&
           gate.legacy_fallback_is_safe();
  }

  bool cancelled_hang_remains_deadline_supervised() {
    amf::lifecycle::native_runtime_gate_t gate;
    if (!gate.try_begin_initialization()) return false;
    auto handoff = std::make_shared<amf::lifecycle::worker_handoff_t<int>>();
    bool was_cancelled = false;
    const auto accepted = handoff->accept_until(
      std::chrono::steady_clock::now() + 1s,
      []() { return true; },
      &was_cancelled);
    const bool producer_unwound = handoff->producer_finished_until(
      std::chrono::steady_clock::now() + 2ms);
    if (!producer_unwound) gate.quarantine_initialization();
    return !accepted && was_cancelled && !producer_unwound &&
           gate.is_quarantined() && !gate.legacy_fallback_is_safe();
  }

  bool cancelled_handoff_reports_finished_only_after_reaping() {
    struct blocking_resource_t {
      std::atomic<bool> *destruction_started;
      std::atomic<bool> *allow_destruction;
      ~blocking_resource_t() {
        destruction_started->store(true, std::memory_order_release);
        while (!allow_destruction->load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
      }
    };

    auto handoff = std::make_shared<amf::lifecycle::worker_handoff_t<std::unique_ptr<blocking_resource_t>>>();
    bool was_cancelled = false;
    const auto accepted = handoff->accept_until(
      std::chrono::steady_clock::now() + 1s,
      []() { return true; },
      &was_cancelled);

    std::atomic<bool> destruction_started {false};
    std::atomic<bool> allow_destruction {false};
    std::thread worker {[&]() {
      auto resource = std::make_unique<blocking_resource_t>();
      resource->destruction_started = &destruction_started;
      resource->allow_destruction = &allow_destruction;
      handoff->publish(std::move(resource));
    }};

    const auto start_deadline = std::chrono::steady_clock::now() + 100ms;
    while (!destruction_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < start_deadline) {
      std::this_thread::yield();
    }
    const bool finished_before_reap = handoff->producer_finished_until(
      std::chrono::steady_clock::now() + 2ms);
    allow_destruction.store(true, std::memory_order_release);
    const bool finished_after_reap = handoff->producer_finished_until(
      std::chrono::steady_clock::now() + 100ms);
    worker.join();
    return !accepted && was_cancelled && destruction_started.load(std::memory_order_acquire) &&
           !finished_before_reap && finished_after_reap;
  }

  bool teardown_gate_contention_respects_its_deadline() {
    amf::lifecycle::native_runtime_gate_t gate;
    if (!gate.try_begin_initialization()) return false;
    const auto start = std::chrono::steady_clock::now();
    const bool entered = gate.begin_teardown_until(start + 2ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    gate.cancel_initialization();
    return !entered && elapsed < 100ms && !gate.is_quarantined() &&
           gate.legacy_fallback_is_safe();
  }

  bool startup_deadline_caps_the_caller_not_the_vendor_watchdog() {
    using clock = std::chrono::steady_clock;
    const auto start = clock::time_point {};
    const auto overall_deadline = start + 6100ms;
    const auto vendor_started = start + 4s;
    const auto vendor_deadline = vendor_started + 5s;
    const auto native_cleanup_completed = start + 4s;
    const auto legacy_first_packet = start + 6200ms;
    return amf::lifecycle::caller_acceptance_deadline(
             overall_deadline, vendor_deadline) == overall_deadline &&
           vendor_deadline == start + 9s &&
           amf::lifecycle::caller_acceptance_deadline(
             start + 20s, vendor_deadline) == vendor_deadline &&
           native_cleanup_completed < overall_deadline &&
           legacy_first_packet >= overall_deadline;
  }

  bool driver_call_deadlines_separate_latency_from_ownership() {
    using clock = std::chrono::steady_clock;
    const auto start = clock::time_point {};
    const auto scheduling_deadline = start + 4ms;
    const auto caller_deadline = start + 3ms;
    const auto ownership_deadline = start + 5s;
    return amf::lifecycle::driver_call_acceptance_deadline(
             scheduling_deadline,
             std::optional {caller_deadline},
             ownership_deadline) == caller_deadline &&
           amf::lifecycle::driver_call_acceptance_deadline(
             scheduling_deadline,
             std::optional<clock::time_point> {},
             ownership_deadline) == scheduling_deadline &&
           amf::lifecycle::driver_call_acceptance_deadline(
             start + 10s,
             std::optional<clock::time_point> {},
             ownership_deadline) == ownership_deadline &&
           amf::lifecycle::teardown_watchdog_within_deadline(
             ownership_deadline,
             start + 4s,
             amf::lifecycle::driver_call_watchdog_timeout) == 1s &&
           amf::lifecycle::late_driver_response_may_publish(false) &&
           !amf::lifecycle::late_driver_response_may_publish(true);
  }

  bool late_driver_return_is_reaped_without_permanent_quarantine() {
    auto gate = std::make_shared<amf::lifecycle::native_runtime_gate_t>();
    if (!gate->reserve_teardown()) return false;

    struct state_t {
      std::atomic<bool> allow_reap {false};
      std::atomic<bool> reaped {false};
      std::atomic<bool> supervisor_done {false};
    };
    auto state = std::make_shared<state_t>();
    const auto caller_started = std::chrono::steady_clock::now();
    const bool scheduled = amf::lifecycle::run_detached([gate, state]() {
      if (!gate->begin_reserved_teardown()) return;
      const bool completed = amf::lifecycle::run_with_timeout([state]() {
        while (!state->allow_reap.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        state->reaped.store(true, std::memory_order_release);
      }, 1s);
      gate->finish_teardown(completed);
      state->supervisor_done.store(true, std::memory_order_release);
    });
    const auto caller_elapsed = std::chrono::steady_clock::now() - caller_started;
    const bool initialization_blocked = !gate->try_begin_initialization();
    state->allow_reap.store(true, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (scheduled && !state->supervisor_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    return scheduled && caller_elapsed < 100ms && initialization_blocked &&
           state->supervisor_done.load(std::memory_order_acquire) &&
           state->reaped.load(std::memory_order_acquire) &&
           !gate->is_quarantined() && gate->try_begin_initialization();
  }

  bool ownership_deadline_expiry_quarantines_after_caller_return() {
    amf::lifecycle::native_runtime_gate_t gate;
    if (!gate.begin_teardown()) return false;

    struct state_t {
      std::atomic<bool> release_worker {false};
      std::atomic<bool> worker_finished {false};
    };
    auto state = std::make_shared<state_t>();
    const auto started = std::chrono::steady_clock::now();
    const bool completed = amf::lifecycle::run_with_timeout([state]() {
      while (!state->release_worker.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      state->worker_finished.store(true, std::memory_order_release);
    }, 2ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    gate.finish_teardown(completed);
    const bool quarantined_while_owned = !completed && elapsed < 100ms &&
                                         gate.is_quarantined() &&
                                         gate.operation_in_progress() &&
                                         !gate.try_begin_initialization();
    state->release_worker.store(true, std::memory_order_release);
    const auto finish_deadline = std::chrono::steady_clock::now() + 1s;
    while (!state->worker_finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < finish_deadline) {
      std::this_thread::yield();
    }
    return quarantined_while_owned && state->worker_finished.load(std::memory_order_acquire) &&
           gate.is_quarantined() && !gate.legacy_fallback_is_safe();
  }

  bool ownership_watchdog_outlives_frame_latency_budget() {
    return amf::lifecycle::driver_call_watchdog_timeout >
             amf::lifecycle::output_coalesce_budget(30) &&
           amf::lifecycle::driver_call_watchdog_timeout >
             amf::lifecycle::output_coalesce_budget(60) &&
           amf::lifecycle::driver_call_watchdog_timeout >
             amf::lifecycle::output_coalesce_budget(120) &&
           amf::lifecycle::driver_call_watchdog_timeout >
             amf::lifecycle::output_coalesce_budget(240);
  }

  bool output_watchdog_rearms_after_caught_up_idle() {
    using amf::lifecycle::output_delivery_is_due;
    return !output_delivery_is_due(100, 100, 0, false) &&
           output_delivery_is_due(101, 100, 0, false) &&
           !output_delivery_is_due(101, 100, 1, false) &&
           output_delivery_is_due(102, 100, 1, false);
  }

  bool query_output_failure_threshold_is_elapsed_time_not_frame_count() {
    using clock = std::chrono::steady_clock;
    const auto start = clock::time_point {} + 1ms;
    return !amf::lifecycle::query_failure_is_persistent(start, start + 999ms) &&
           amf::lifecycle::query_failure_is_persistent(start, start + 1s) &&
           !amf::lifecycle::query_failure_is_persistent({}, start + 5s);
  }

  bool capture_generation_preservation_is_amd_scoped_and_worker_aware() {
    using amf::lifecycle::capture_generation_requires_preservation;
    return !capture_generation_requires_preservation(false, true, true, 1, true) &&
           !capture_generation_requires_preservation(true, false, false, 0, false) &&
           capture_generation_requires_preservation(true, true, false, 0) &&
           capture_generation_requires_preservation(true, false, true, 0) &&
           capture_generation_requires_preservation(true, false, false, 1) &&
           capture_generation_requires_preservation(true, false, false, 0, true);
  }

  bool probe_teardown_watchdog_obeys_the_end_to_end_deadline() {
    using clock = std::chrono::steady_clock;
    const auto start = clock::time_point {};
    const auto maximum = std::chrono::duration_cast<clock::duration>(5s);
    return amf::lifecycle::teardown_watchdog_within_deadline(
             start + 6100ms, start + 100ms, maximum) == maximum &&
           amf::lifecycle::teardown_watchdog_within_deadline(
             start + 6100ms, start + 6s, maximum) == 100ms &&
           amf::lifecycle::teardown_watchdog_within_deadline(
             start + 6100ms, start + 6100ms, maximum) == clock::duration::zero();
  }

  bool shared_reinit_deadline_defers_instead_of_shortening_driver_watchdog() {
    using clock = std::chrono::steady_clock;
    const auto start = clock::time_point {};
    const auto deadline = start + 8s;
    const auto maximum = std::chrono::duration_cast<clock::duration>(5s);
    return amf::lifecycle::full_watchdog_interval_fits(deadline, start, maximum) &&
           amf::lifecycle::full_watchdog_interval_fits(deadline, start + 1600ms, maximum) &&
           !amf::lifecycle::full_watchdog_interval_fits(deadline, start + 3200ms, maximum);
  }

  bool rejected_fresh_or_idr_input_retries_without_static_wait() {
    using amf::lifecycle::logical_input_requires_immediate_retry;
    return logical_input_requires_immediate_retry(false, true, false) &&
           logical_input_requires_immediate_retry(false, false, true) &&
           !logical_input_requires_immediate_retry(false, false, false) &&
           !logical_input_requires_immediate_retry(true, true, true);
  }

  bool sparse_output_wait_remains_armed_until_image_or_tail_deadline() {
    using clock = std::chrono::steady_clock;
    const auto start = clock::time_point {};
    const auto image_deadline = start + 50ms;
    return amf::lifecycle::output_or_image_wait_deadline(
             image_deadline, false, start, 9ms) == image_deadline &&
           amf::lifecycle::output_or_image_wait_deadline(
             image_deadline, true, start + 5ms, 9ms) == start + 14ms;
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
             every_rate_control_mode_has_a_bounded_first_packet_plan() &&
             rate_control_policy_covers_all_semantic_modes() &&
             live_bitrate_plan_skips_qvbr_and_hevc_static_vbv() &&
             bitrate_worker_contention_defers_and_timeout_abandons() &&
             advanced_rate_control_is_gated_per_codec_runtime() &&
             preanalysis_pipeline_primes_and_drains_in_order() &&
             automatic_h264_coder_preserves_driver_default() &&
             repeated_input_uses_a_replay_source_never_owned_by_amf() &&
             surface_pool_can_prime_a_retaining_driver() &&
             preanalysis_preserves_low_latency_queue_with_compatibility_fallback() &&
             queue_boundary_normalization_and_cache_policy_match_production() &&
             queue_sixteen_retry_is_prepacket_native_only_and_single_shot() &&
             output_pts_filter_rejects_missing_duplicate_and_regressing_packets() &&
             delivery_filter_and_startup_budget_follow_client_visible_packets() &&
             replay_policy_avoids_continuous_copies_and_never_reuses_owned_sources() &&
             saturation_wait_requires_an_actual_surface_release() &&
             output_poll_rearm_survives_concurrent_submission() &&
             asynchronous_pipeline_catches_up_to_current_output() &&
             preanalysis_target_tracks_accepted_indices_with_gaps() &&
             teardown_timeout_returns_control_before_a_wedged_destructor() &&
             runtime_gate_fences_initialization_against_teardown_and_quarantine() &&
             teardown_reservation_closes_the_worker_registration_window() &&
             ordinary_disconnect_defers_a_slow_destructor_without_opening_reconnect() &&
             bitrate_and_one_frame_vbv_tolerate_only_small_normalization() &&
             equivalent_framerates_preserve_the_one_frame_vbv_contract() &&
             timed_out_teardown_quarantines_and_retains_the_runtime_fence() &&
             timed_out_worker_handoff_reaps_on_the_producer_thread() &&
             normal_cancellation_does_not_quarantine_the_runtime() &&
             cancelled_hang_remains_deadline_supervised() &&
             cancelled_handoff_reports_finished_only_after_reaping() &&
             teardown_gate_contention_respects_its_deadline() &&
             startup_deadline_caps_the_caller_not_the_vendor_watchdog() &&
             driver_call_deadlines_separate_latency_from_ownership() &&
             late_driver_return_is_reaped_without_permanent_quarantine() &&
             ownership_deadline_expiry_quarantines_after_caller_return() &&
             ownership_watchdog_outlives_frame_latency_budget() &&
             output_watchdog_rearms_after_caught_up_idle() &&
             query_output_failure_threshold_is_elapsed_time_not_frame_count() &&
             capture_generation_preservation_is_amd_scoped_and_worker_aware() &&
             probe_teardown_watchdog_obeys_the_end_to_end_deadline() &&
             shared_reinit_deadline_defers_instead_of_shortening_driver_watchdog() &&
             rejected_fresh_or_idr_input_retries_without_static_wait() &&
             sparse_output_wait_remains_armed_until_image_or_tail_deadline() &&
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

TEST(SunshineNativeAmfReview, EveryRateControlModeHasBoundedFirstPacketPlan) {
  EXPECT_TRUE(every_rate_control_mode_has_a_bounded_first_packet_plan());
}

TEST(SunshineNativeAmfReview, RateControlPolicyCoversAllSemanticModes) {
  EXPECT_TRUE(rate_control_policy_covers_all_semantic_modes());
}

TEST(SunshineNativeAmfReview, LiveBitratePlanSkipsQvbrAndHevcStaticVbv) {
  EXPECT_TRUE(live_bitrate_plan_skips_qvbr_and_hevc_static_vbv());
}

TEST(SunshineNativeAmfReview, BitrateWorkerContentionDefersAndTimeoutAbandons) {
  EXPECT_TRUE(bitrate_worker_contention_defers_and_timeout_abandons());
}

TEST(SunshineNativeAmfReview, AdvancedRateControlIsGatedPerCodecRuntime) {
  EXPECT_TRUE(advanced_rate_control_is_gated_per_codec_runtime());
}

TEST(SunshineNativeAmfReview, PreAnalysisPipelinePrimesAndDrainsInOrder) {
  EXPECT_TRUE(preanalysis_pipeline_primes_and_drains_in_order());
}

TEST(SunshineNativeAmfReview, AutomaticH264CoderPreservesDriverDefault) {
  EXPECT_TRUE(automatic_h264_coder_preserves_driver_default());
}

TEST(SunshineNativeAmfReview, RepeatedInputUsesReplaySourceNeverOwnedByAmf) {
  EXPECT_TRUE(repeated_input_uses_a_replay_source_never_owned_by_amf());
}

TEST(SunshineNativeAmfReview, SurfacePoolPrimesRetainingDriver) {
  EXPECT_TRUE(surface_pool_can_prime_a_retaining_driver());
}

TEST(SunshineNativeAmfReview, PreanalysisPreservesLowLatencyQueueWithCompatibilityFallback) {
  EXPECT_TRUE(preanalysis_preserves_low_latency_queue_with_compatibility_fallback());
}

TEST(SunshineNativeAmfReview, QueueBoundaryNormalizationAndCachePolicyMatchProduction) {
  EXPECT_TRUE(queue_boundary_normalization_and_cache_policy_match_production());
}

TEST(SunshineNativeAmfReview, QueueSixteenRetryIsPrepacketNativeOnlyAndSingleShot) {
  EXPECT_TRUE(queue_sixteen_retry_is_prepacket_native_only_and_single_shot());
}

TEST(SunshineNativeAmfReview, OutputPtsFilterRejectsMissingDuplicateAndRegressingPackets) {
  EXPECT_TRUE(output_pts_filter_rejects_missing_duplicate_and_regressing_packets());
}

TEST(SunshineNativeAmfReview, DeliveryFilterAndStartupBudgetFollowVisiblePackets) {
  EXPECT_TRUE(delivery_filter_and_startup_budget_follow_client_visible_packets());
}

TEST(SunshineNativeAmfReview, ReplayPolicyAvoidsContinuousCopiesAndOwnedSources) {
  EXPECT_TRUE(replay_policy_avoids_continuous_copies_and_never_reuses_owned_sources());
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

TEST(SunshineNativeAmfReview, TeardownReservationClosesWorkerRegistrationWindow) {
  EXPECT_TRUE(teardown_reservation_closes_the_worker_registration_window());
}

TEST(SunshineNativeAmfReview, OrdinaryDisconnectDefersSlowDestructorWithoutOpeningReconnect) {
  EXPECT_TRUE(ordinary_disconnect_defers_a_slow_destructor_without_opening_reconnect());
}

TEST(SunshineNativeAmfReview, BitrateAndOneFrameVbvAllowOnlySmallNormalization) {
  EXPECT_TRUE(bitrate_and_one_frame_vbv_tolerate_only_small_normalization());
}

TEST(SunshineNativeAmfReview, EquivalentFrameratesPreserveOneFrameVbvContract) {
  EXPECT_TRUE(equivalent_framerates_preserve_the_one_frame_vbv_contract());
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

TEST(SunshineNativeAmfReview, CancelledHangRemainsDeadlineSupervised) {
  EXPECT_TRUE(cancelled_hang_remains_deadline_supervised());
}

TEST(SunshineNativeAmfReview, CancelledHandoffFinishesOnlyAfterReaping) {
  EXPECT_TRUE(cancelled_handoff_reports_finished_only_after_reaping());
}

TEST(SunshineNativeAmfReview, TeardownGateContentionRespectsDeadline) {
  EXPECT_TRUE(teardown_gate_contention_respects_its_deadline());
}

TEST(SunshineNativeAmfReview, StartupDeadlineCapsCallerNotVendorWatchdog) {
  EXPECT_TRUE(startup_deadline_caps_the_caller_not_the_vendor_watchdog());
}

TEST(SunshineNativeAmfReview, DriverCallDeadlinesSeparateLatencyFromOwnership) {
  EXPECT_TRUE(driver_call_deadlines_separate_latency_from_ownership());
}

TEST(SunshineNativeAmfReview, LateDriverReturnIsReapedWithoutPermanentQuarantine) {
  EXPECT_TRUE(late_driver_return_is_reaped_without_permanent_quarantine());
}

TEST(SunshineNativeAmfReview, OwnershipDeadlineExpiryQuarantinesAfterCallerReturn) {
  EXPECT_TRUE(ownership_deadline_expiry_quarantines_after_caller_return());
}

TEST(SunshineNativeAmfReview, OwnershipWatchdogOutlivesFrameLatencyBudget) {
  EXPECT_TRUE(ownership_watchdog_outlives_frame_latency_budget());
}

TEST(SunshineNativeAmfReview, OutputWatchdogRearmsAfterCaughtUpIdle) {
  EXPECT_TRUE(output_watchdog_rearms_after_caught_up_idle());
}

TEST(SunshineNativeAmfReview, QueryOutputFailureThresholdUsesElapsedTime) {
  EXPECT_TRUE(query_output_failure_threshold_is_elapsed_time_not_frame_count());
}

TEST(SunshineNativeAmfReview, CaptureGenerationPreservationIsAmdScopedAndWorkerAware) {
  EXPECT_TRUE(capture_generation_preservation_is_amd_scoped_and_worker_aware());
}

TEST(SunshineNativeAmfReview, ProbeTeardownWatchdogObeysEndToEndDeadline) {
  EXPECT_TRUE(probe_teardown_watchdog_obeys_the_end_to_end_deadline());
}

TEST(SunshineNativeAmfReview, SharedReinitDeadlineDefersInsteadOfShorteningDriverWatchdog) {
  EXPECT_TRUE(shared_reinit_deadline_defers_instead_of_shortening_driver_watchdog());
}

TEST(SunshineNativeAmfReview, RejectedFreshOrIdrInputRetriesWithoutStaticWait) {
  EXPECT_TRUE(rejected_fresh_or_idr_input_retries_without_static_wait());
}

TEST(SunshineNativeAmfReview, SparseOutputWaitRemainsArmedUntilImageOrTailDeadline) {
  EXPECT_TRUE(sparse_output_wait_remains_armed_until_image_or_tail_deadline());
}

TEST(SunshineNativeAmfReview, SmartAccessVideoAvoidsAggressiveLowLatencyDriverPath) {
  EXPECT_TRUE(smart_access_video_avoids_aggressive_low_latency_driver_path());
}

TEST(SunshineNativeAmfReview, ConcurrentNativeSessionRemainsUsable) {
  EXPECT_TRUE(concurrent_native_session_remains_usable());
}

#endif
