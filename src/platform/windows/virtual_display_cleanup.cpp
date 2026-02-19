#include "virtual_display_cleanup.h"

#ifdef _WIN32

  #include "display_helper_integration.h"
  #include "src/logging.h"
  #include "src/platform/windows/impersonating_display_device.h"
  #include "src/platform/windows/virtual_display.h"

  #include <algorithm>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>
  #include <exception>
  #include <memory>
  #include <string>

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

  cleanup_result_t run(const std::string_view reason, const bool enforce_db_restore) {
    cleanup_result_t result;

    const std::string reason_text = reason.empty() ? "unspecified" : std::string(reason);
    BOOST_LOG(info) << "Virtual display cleanup: begin (reason=" << reason_text
                    << ", enforce_db_restore=" << (enforce_db_restore ? "true" : "false") << ")";

    const bool had_active_virtual_display = has_active_virtual_display();
    VDISPLAY::setWatchdogFeedingEnabled(false);
    result.virtual_displays_removed = VDISPLAY::removeAllVirtualDisplays();

    if (enforce_db_restore) {
      result.helper_revert_dispatched = display_helper_integration::revert();
      if (result.helper_revert_dispatched) {
        result.database_restore_applied = true;
      } else {
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
