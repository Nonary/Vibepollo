/**
 * @file src/nvenc/nvenc_event_wait.h
 * @brief NVENC completion-event waiting with an injectable OS boundary.
 */
#pragma once

#include <cstdint>
#include <string>

namespace nvenc::event_wait {
  // Win32 wait results are DWORDs. Keep every bit, including WAIT_FAILED.
  inline constexpr std::uint32_t signaled = 0;
  inline constexpr std::uint32_t timed_out = 0x00000102;
  inline constexpr std::uint32_t failed = 0xffffffff;

  class api_t {
  public:
    virtual ~api_t() = default;
    virtual std::uint32_t wait(void *event, std::uint32_t timeout_ms) = 0;
    virtual std::uint32_t last_error() = 0;
  };

  struct result_t {
    std::uint32_t code;
    std::uint32_t error;

    [[nodiscard]] bool succeeded() const {
      return code == signaled;
    }
  };

  [[nodiscard]] result_t wait(void *event, std::uint32_t timeout_ms, api_t &api);
  [[nodiscard]] std::string describe(const result_t &result, std::uint32_t timeout_ms);
}  // namespace nvenc::event_wait
