/** @file src/platform/windows/end_stream_hotkey_protocol.h
 * @brief Shared contract for the local interactive hotkey helper.
 */
#pragma once
#include <cstdint>

namespace platf::hotkey::end_stream {
  enum class message : std::uint8_t {
    ready = 1,
    terminate = 2,
    heartbeat = 3,
    registration_failed = 4
  };

  // Require a modifier for this destructive shortcut; the Win32 MOD_* values
  // exclude registration-only MOD_NOREPEAT.
  constexpr bool valid_binding(int key, unsigned int modifiers) {
    return ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') ||
            (key >= 0x70 && key <= 0x87)) &&
           modifiers != 0 && (modifiers & ~0xFU) == 0;
  }

  constexpr bool authorized_sender(std::uint32_t expected_pid, std::uint32_t actual_pid, std::uint32_t expected_session, std::uint32_t actual_session, std::uint32_t console_session) {
    return expected_pid != 0 && actual_pid == expected_pid && expected_session != 0 &&
           expected_session != 0xFFFFFFFFU && actual_session == expected_session && console_session == expected_session;
  }
}  // namespace platf::hotkey::end_stream
