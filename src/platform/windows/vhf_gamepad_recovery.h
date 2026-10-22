/**
 * @file src/platform/windows/vhf_gamepad_recovery.h
 * @brief Transactional recovery of a removed VHF connection without changing its owners.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace platf::vhf_gamepad {

  // request()/pending() may run under a shared lifetime lock. attempt()/reset()
  // require the exclusive lifetime lock, including across every client call.
  // This bounds retry frequency, not synchronous driver-call completion time.
  class connection_recovery_t {
  public:
    using clock = std::chrono::steady_clock;

    void request() noexcept {
      requested.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool pending() const noexcept {
      return requested.load(std::memory_order_acquire);
    }

    void reset() noexcept {
      retry_at = {};
      requested.store(false, std::memory_order_release);
    }

    template<class Client, class Slots, class ResetTransient>
    std::optional<std::uint32_t> attempt(
      Client &client,
      Slots &slots,
      const clock::time_point now,
      ResetTransient reset_transient
    ) {
      if (!pending() || now < retry_at) {
        return std::nullopt;
      }
      retry_at = now + std::chrono::seconds(1);

      // Closing the owning connection removes any surviving or partially recreated
      // controllers. Never replay a destroy IOCTL against the removed device.
      client.close();
      for (std::uint32_t nr = 0; nr < slots.size(); ++nr) {
        if (slots[nr].active) {
          reset_transient(nr, slots[nr]);
        }
      }
      std::uint32_t status = client.connect();
      if (status == 0) {
        for (std::uint32_t nr = 0; nr < slots.size(); ++nr) {
          if (slots[nr].active) {
            // Preserve the exact profile and slot ID, not an automatic fallback.
            status = client.create_controller(nr, slots[nr].profile);
            if (status == 0 && slots[nr].last_input) {
              // Clients need not repeat unchanged held controls. Restore the latest
              // complete report before submissions resume; failure rolls everything back.
              status = client.submit_input_state(*slots[nr].last_input);
            }
            if (status != 0) {
              break;
            }
          }
        }
      }
      if (status != 0) {
        // All or nothing: do not expose a partially restored connection to input.
        client.close();
      } else {
        reset();
      }
      return status;
    }

  private:
    std::atomic<bool> requested {false};
    clock::time_point retry_at {};
  };

}  // namespace platf::vhf_gamepad
