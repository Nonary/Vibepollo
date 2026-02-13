#pragma once

#ifdef _WIN32

  #include <string_view>

namespace platf::virtual_display_cleanup {
  struct cleanup_result_t {
    bool virtual_displays_removed {false};
    bool helper_revert_dispatched {false};
    bool database_restore_applied {false};
  };

  cleanup_result_t run(std::string_view reason, bool enforce_db_restore = true);
}  // namespace platf::virtual_display_cleanup

#endif  // _WIN32
