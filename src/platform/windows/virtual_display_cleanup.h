#pragma once

#ifdef _WIN32

  #include <array>
  #include <cstdint>
  #include <optional>
  #include <string_view>

namespace platf::virtual_display_cleanup {
  enum class revert_order_t {
    remove_before_restore,
    restore_before_remove,
  };

  enum class recovery_monitor_policy_t {
    // Ordinary speculative cleanup keeps managed-session recovery alive when
    // ownership admission defers the cleanup.
    preserve_if_deferred,
    // A terminal lease/restore boundary cancels recovery before admission, so
    // an ownership deferral cannot revive an intentionally ended display.
    disengage_before_admission,
  };

  enum class cleanup_admission_policy_t {
    respect_managed_owners,
    // The explicit user killswitch is authoritative over every managed owner.
    override_managed_owners,
  };

  constexpr bool cleanup_admitted(
    const bool managed_cleanup_allowed,
    const cleanup_admission_policy_t policy
  ) noexcept {
    return managed_cleanup_allowed || policy == cleanup_admission_policy_t::override_managed_owners;
  }

  enum class cleanup_step_t {
    helper_revert,
    retained_probe_remove,
    explicit_display_remove,
    database_restore,
  };

  // The final database_restore entry is a fallback marker: its action is
  // taken only when helper REVERT was not dispatched, after topology teardown
  // in either ordering.
  constexpr std::array<cleanup_step_t, 4> ordered_restore_steps(const revert_order_t order) noexcept {
    if (order == revert_order_t::restore_before_remove) {
      return {
        cleanup_step_t::helper_revert,
        cleanup_step_t::retained_probe_remove,
        cleanup_step_t::explicit_display_remove,
        cleanup_step_t::database_restore,
      };
    }
    return {
      cleanup_step_t::retained_probe_remove,
      cleanup_step_t::explicit_display_remove,
      cleanup_step_t::helper_revert,
      cleanup_step_t::database_restore,
    };
  }

  struct cleanup_result_t {
    bool virtual_displays_removed {false};
    bool helper_revert_dispatched {false};
    bool database_restore_applied {false};
  };

  cleanup_result_t run(
    std::string_view reason,
    bool enforce_db_restore = true,
    revert_order_t revert_order = revert_order_t::remove_before_restore,
    bool prefer_golden_if_current_missing = true,
    std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes = std::nullopt,
    recovery_monitor_policy_t recovery_monitor_policy = recovery_monitor_policy_t::preserve_if_deferred,
    cleanup_admission_policy_t cleanup_admission_policy = cleanup_admission_policy_t::respect_managed_owners
  );

  // Execute the complete user-requested kill-switch contract. Unlike ordinary
  // cleanup, this terminal action overrides managed ownership so recovery,
  // restore, display removal, and watchdog shutdown cannot be deferred.
  cleanup_result_t terminate_all(std::string_view reason);

  // Nonblocking observation for callers that must not begin display probing
  // while any cleanup path is removing a virtual display or restoring topology.
  bool in_progress();
}  // namespace platf::virtual_display_cleanup

#endif  // _WIN32
