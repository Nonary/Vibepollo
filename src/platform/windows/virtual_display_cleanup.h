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
    std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes = std::nullopt
  );
}  // namespace platf::virtual_display_cleanup

#endif  // _WIN32
