#include "virtual_display_cleanup.h"

#ifdef _WIN32

  #include "display_helper_integration.h"
  #include "src/logging.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/platform/windows/virtual_display.h"
  #include "src/process.h"
  #include "src/remote_display_topology.h"

  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cstring>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>
  #include <exception>
  #include <memory>
  #include <mutex>
  #include <string>
  #include <thread>

namespace platf::virtual_display_cleanup {
  namespace {
    std::atomic_uint g_cleanup_reservations {0};
    std::mutex g_terminal_cleanup_mutex;

    class cleanup_reservation_t {
    public:
      cleanup_reservation_t() {
        g_cleanup_reservations.fetch_add(1, std::memory_order_acq_rel);
      }

      ~cleanup_reservation_t() {
        g_cleanup_reservations.fetch_sub(1, std::memory_order_acq_rel);
      }

      cleanup_reservation_t(const cleanup_reservation_t &) = delete;
      cleanup_reservation_t &operator=(const cleanup_reservation_t &) = delete;
    };

    bool has_active_virtual_display() {
      const auto virtual_displays = VDISPLAY::enumerateVirtualDisplays();
      return std::any_of(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::VirtualDisplayInfo &info) {
          return info.is_active;
        }
      );
    }

    std::size_t active_virtual_display_count() {
      const auto virtual_displays = VDISPLAY::enumerateVirtualDisplays();
      return static_cast<std::size_t>(std::count_if(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::VirtualDisplayInfo &info) {
          return info.is_active;
        }
      ));
    }

    bool wait_for_virtual_display_teardown(std::chrono::steady_clock::duration timeout) {
      constexpr auto kPollInterval = std::chrono::milliseconds(100);

      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (true) {
        const auto remaining = active_virtual_display_count();
        if (remaining == 0) {
          return true;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
          BOOST_LOG(warning) << "Virtual display cleanup: teardown wait expired with "
                             << remaining << " virtual display(s) still enumerated.";
          return false;
        }

        std::this_thread::sleep_for(kPollInterval);
      }
    }

    bool restore_windows_display_database() {
      try {
        auto api = std::make_shared<display_device::WinApiLayer>();
        auto win_dd = std::make_shared<display_device::WinDisplayDevice>(api);
        auto impersonating_dd = std::make_shared<display_device::ImpersonatingDisplayDevice>(win_dd);
        return impersonating_dd->restoreMonitorSettings();
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Virtual display cleanup: direct database restore threw exception: " << e.what();
      } catch (...) {
        BOOST_LOG(warning) << "Virtual display cleanup: direct database restore threw unknown exception.";
      }
      return false;
    }

    bool guid_bytes_are_empty(const std::array<std::uint8_t, 16> &guid_bytes) {
      return std::all_of(guid_bytes.begin(), guid_bytes.end(), [](std::uint8_t byte) {
        return byte == 0;
      });
    }

    bool remove_specific_virtual_display(const std::optional<std::array<std::uint8_t, 16>> &guid_bytes) {
      if (!guid_bytes || guid_bytes_are_empty(*guid_bytes)) {
        return true;
      }

      GUID guid {};
      static_assert(sizeof(guid) == 16);
      std::memcpy(&guid, guid_bytes->data(), sizeof(guid));
      return VDISPLAY::removeVirtualDisplay(guid);
    }

    void disengage_recovery_monitors(const std::optional<std::array<std::uint8_t, 16>> &guid_bytes) {
      if (!guid_bytes || guid_bytes_are_empty(*guid_bytes)) {
        VDISPLAY::cancel_all_virtual_display_recovery_monitors();
        return;
      }

      GUID guid {};
      static_assert(sizeof(guid) == 16);
      std::memcpy(&guid, guid_bytes->data(), sizeof(guid));
      VDISPLAY::cancel_virtual_display_recovery_monitor(guid);
    }
  }  // namespace

  cleanup_result_t run(
    const std::string_view reason,
    const bool enforce_db_restore,
    const revert_order_t revert_order,
    const bool prefer_golden_if_current_missing,
    const std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes,
    const recovery_monitor_policy_t recovery_monitor_policy,
    const cleanup_admission_policy_t cleanup_admission_policy
  ) {
    cleanup_reservation_t cleanup_reservation;
    cleanup_result_t result;

    const std::string reason_text = reason.empty() ? "unspecified" : std::string(reason);
    if (recovery_monitor_policy == recovery_monitor_policy_t::disengage_before_admission) {
      // Terminal intent is authoritative even while a managed session still
      // owns the display. Cancel before the ownership guard so an intentionally
      // expired or externally removed lease can never be classified as a crash
      // and recreated by that ended session's recovery worker.
      disengage_recovery_monitors(virtual_display_guid_bytes);
      BOOST_LOG(info) << "Virtual display cleanup: recovery monitors disengaged before terminal cleanup admission (reason="
                      << reason_text << ").";
    }
    const bool managed_cleanup_allowed = remote_display_topology::instance().generic_virtual_display_cleanup_allowed();
    if (!cleanup_admitted(managed_cleanup_allowed, cleanup_admission_policy)) {
      if (enforce_db_restore) {
        proc::defer_display_revert();
      }
      BOOST_LOG(info) << "Virtual display cleanup: deferred (reason=" << reason_text
                      << ") until the remaining managed client display sessions release ownership.";
      return result;
    }
    if (!managed_cleanup_allowed) {
      BOOST_LOG(warning) << "Virtual display cleanup: overriding managed display ownership for terminal user action (reason="
                         << reason_text << ").";
    }

    BOOST_LOG(info) << "Virtual display cleanup: begin (reason=" << reason_text
                    << ", enforce_db_restore=" << (enforce_db_restore ? "true" : "false")
                    << ", revert_order="
                    << (revert_order == revert_order_t::restore_before_remove ? "restore_before_remove" : "remove_before_restore")
                    << ", prefer_golden_if_current_missing=" << (prefer_golden_if_current_missing ? "true" : "false")
                    << ")";

    const bool had_active_virtual_display = has_active_virtual_display();
    VDISPLAY::setWatchdogFeedingEnabled(false);

    const auto try_helper_revert = [&]() {
      if (!enforce_db_restore || result.helper_revert_dispatched) {
        return;
      }

      result.helper_revert_dispatched = display_helper_integration::revert(
        prefer_golden_if_current_missing,
        cleanup_admission_policy == cleanup_admission_policy_t::override_managed_owners
      );
      if (result.helper_revert_dispatched) {
        result.database_restore_applied = true;
      }
    };

    bool teardown_completed = false;
    bool teardown_waited = false;
    const auto wait_for_teardown_before_restore = [&]() {
      if (teardown_waited || result.helper_revert_dispatched || !teardown_completed ||
          !had_active_virtual_display || !enforce_db_restore) {
        return;
      }
      constexpr auto kTeardownSettleTimeout = std::chrono::seconds(5);
      if (wait_for_virtual_display_teardown(kTeardownSettleTimeout)) {
        BOOST_LOG(debug) << "Virtual display cleanup: teardown settled before restore.";
      }
      teardown_waited = true;
    };

    // Keep the retained probe display alive for restore-before-remove callers,
    // but remove it in the normal remove-before-restore order with the other
    // virtual displays. This also covers a driver-accepted target that has
    // not yet appeared in Windows enumeration.
    for (const auto step : ordered_restore_steps(revert_order)) {
      switch (step) {
        case cleanup_step_t::helper_revert:
          wait_for_teardown_before_restore();
          if (enforce_db_restore) {
            try_helper_revert();
          }
          break;
        case cleanup_step_t::retained_probe_remove:
          VDISPLAY::cleanup_retained_ensure_display();
          break;
        case cleanup_step_t::explicit_display_remove: {
          const bool specific_display_removed = remove_specific_virtual_display(virtual_display_guid_bytes);
          const bool tracked_displays_removed = VDISPLAY::removeAllVirtualDisplays();
          result.virtual_displays_removed = specific_display_removed && tracked_displays_removed;
          teardown_completed = true;
          break;
        }
        case cleanup_step_t::database_restore:
          wait_for_teardown_before_restore();
          if (enforce_db_restore && !result.helper_revert_dispatched) {
            result.database_restore_applied = restore_windows_display_database();
          }
          break;
      }
    }

    BOOST_LOG(info) << "Virtual display cleanup: finished (reason=" << reason_text
                    << ", had_active_virtual_display=" << (had_active_virtual_display ? "true" : "false")
                    << ", virtual_displays_removed=" << (result.virtual_displays_removed ? "true" : "false")
                    << ", helper_revert_dispatched=" << (result.helper_revert_dispatched ? "true" : "false")
                    << ", database_restore_applied=" << (result.database_restore_applied ? "true" : "false")
                    << ")";
    return result;
  }

  cleanup_result_t terminate_all(const std::string_view reason) {
    std::lock_guard terminal_lock {g_terminal_cleanup_mutex};
    // A previous ordinary cleanup may have queued a restore behind the same
    // managed-owner gate this terminal action intentionally overrides. This
    // action consumes that intent now, so it must not fire again later.
    proc::clear_deferred_display_revert();
    const auto result = run(
      reason,
      true,
      revert_order_t::restore_before_remove,
      true,
      std::nullopt,
      recovery_monitor_policy_t::disengage_before_admission,
      cleanup_admission_policy_t::override_managed_owners
    );

    // A terminal user action must also end the helper restart loop. Forced
    // stop is safe here because run() has already completed the synchronous
    // REVERT attempt and display teardown. Closing the driver transport stops
    // its ping/watchdog worker; a later new session may open it again.
    VDISPLAY::closeVDisplayDevice();
    display_helper_integration::stop_watchdog(true);
    BOOST_LOG(info) << "Virtual display cleanup: terminal driver and helper watchdog shutdown completed (reason="
                    << (reason.empty() ? "unspecified" : std::string(reason)) << ").";
    return result;
  }

  bool in_progress() {
    return g_cleanup_reservations.load(std::memory_order_acquire) != 0;
  }
}  // namespace platf::virtual_display_cleanup

#endif  // _WIN32
