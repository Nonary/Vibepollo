#include "virtual_display_cleanup.h"

#ifdef _WIN32

  #include "display_helper_integration.h"
  #include "src/logging.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/platform/windows/virtual_display.h"

  #include <algorithm>
  #include <chrono>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>
  #include <exception>
  #include <memory>
  #include <string>
  #include <thread>

namespace platf::virtual_display_cleanup {
  namespace {
    bool has_active_virtual_display() {
      const auto virtual_displays = VDISPLAY::enumerateSudaVDADisplays();
      return std::any_of(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::SudaVDADisplayInfo &info) {
          return info.is_active;
        }
      );
    }

    std::size_t active_virtual_display_count() {
      const auto virtual_displays = VDISPLAY::enumerateSudaVDADisplays();
      return static_cast<std::size_t>(std::count_if(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::SudaVDADisplayInfo &info) {
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
  }  // namespace

  cleanup_result_t run(
    const std::string_view reason,
    const bool enforce_db_restore,
    const revert_order_t revert_order,
    const bool prefer_golden_if_current_missing
  ) {
    cleanup_result_t result;

    const std::string reason_text = reason.empty() ? "unspecified" : std::string(reason);
    BOOST_LOG(info) << "Virtual display cleanup: begin (reason=" << reason_text
                    << ", enforce_db_restore=" << (enforce_db_restore ? "true" : "false")
                    << ", revert_order="
                    << (revert_order == revert_order_t::restore_before_remove ? "restore_before_remove" : "remove_before_restore")
                    << ")";

    const bool had_active_virtual_display = has_active_virtual_display();
    VDISPLAY::setWatchdogFeedingEnabled(false);

    const auto try_helper_revert = [&]() {
      if (!enforce_db_restore || result.helper_revert_dispatched) {
        return;
      }

      result.helper_revert_dispatched = display_helper_integration::revert(prefer_golden_if_current_missing);
      if (result.helper_revert_dispatched) {
        result.database_restore_applied = true;
      }
    };

    if (enforce_db_restore && revert_order == revert_order_t::restore_before_remove) {
      try_helper_revert();
    }

    result.virtual_displays_removed = VDISPLAY::removeAllVirtualDisplays();
    const bool should_wait_for_teardown_before_restore =
      had_active_virtual_display &&
      enforce_db_restore &&
      (revert_order == revert_order_t::remove_before_restore || !result.helper_revert_dispatched);
    if (should_wait_for_teardown_before_restore) {
      constexpr auto kTeardownSettleTimeout = std::chrono::seconds(5);
      if (wait_for_virtual_display_teardown(kTeardownSettleTimeout)) {
        BOOST_LOG(debug) << "Virtual display cleanup: teardown settled before restore.";
      }
    }

    if (enforce_db_restore) {
      if (revert_order == revert_order_t::remove_before_restore) {
        try_helper_revert();
      }

      if (!result.helper_revert_dispatched) {
        result.database_restore_applied = restore_windows_display_database();
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
}  // namespace platf::virtual_display_cleanup

#endif  // _WIN32
