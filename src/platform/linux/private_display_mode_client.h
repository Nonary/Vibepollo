/** @file Client for the root-owned Linux requested-mode admission broker. */
#pragma once

#include "private_display_mode_policy.h"

#include <chrono>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace platf::linux_private_display {
  struct mode_admission_result_t {
    bool success {false};
    std::string detail;
  };

  [[nodiscard]] mode_admission_result_t request_managed_mode(
    std::string_view output_name,
    mode_policy::requested_mode_t mode
  );

  namespace detail {
    // The caller owns fd. The explicit peer/deadline allow isolated socket tests;
    // production always uses root and the fixed admission socket.
    [[nodiscard]] mode_admission_result_t transact_requested_mode(
      int fd,
      std::string_view output_name,
      mode_policy::requested_mode_t mode,
      uid_t expected_peer,
      std::chrono::steady_clock::time_point deadline
    );
  }
}  // namespace platf::linux_private_display
