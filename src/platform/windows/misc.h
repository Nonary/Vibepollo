/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <functional>
#include <string_view>
#include <system_error>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  void print_status(const std::string_view &prefix, HRESULT status);
  HDESK syncThreadDesktop();

  int64_t qpc_counter();

  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Convert a UTF-8 string into a UTF-16 wide string.
   * @param string The UTF-8 string.
   * @return The converted UTF-16 wide string.
   */
  std::wstring from_utf8(const std::string &string);

  /**
   * @brief Convert a UTF-16 wide string into a UTF-8 string.
   * @param string The UTF-16 wide string.
   * @return The converted UTF-8 string.
   */
  std::string to_utf8(const std::wstring &string);

  /**
   * @brief Obtain the current sessions user's primary token with elevated privileges if available.
   * @param elevated Request an elevated token if the user has one.
   * @return User token handle or nullptr on failure (caller must CloseHandle on success).
   */
  HANDLE retrieve_users_token(bool elevated);

  /**
   * @brief Check if the current process is running under the SYSTEM account.
   */
  bool is_running_as_system();

  /**
   * @brief Impersonate the specified user during the callback lifespan.
   * @return std::error_code set to permission_denied on failure, empty on success.
   */
  std::error_code impersonate_current_user(HANDLE user_token, std::function<void()> callback);
}  // namespace platf
