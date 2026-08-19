/**
 * @file src/platform/windows/vhf_gamepad.h
 * @brief Declarations for the Vibeshine VHF virtual gamepad input backend.
 */
#pragma once

// standard includes
#include <memory>

// local includes
#include "src/platform/common.h"

namespace platf {

  /**
   * @brief Drives virtual controllers through Vibeshine's own UMDF/VHF gamepad driver.
   * @details This is the alternative to the ViGEmBus backend. The driver currently exposes a
   *          single generic HID game pad profile, so touch, motion, and battery reports have no
   *          destination here and are dropped by the caller.
   */
  class vhf_gamepad_t {
  public:
    vhf_gamepad_t();
    ~vhf_gamepad_t();

    vhf_gamepad_t(const vhf_gamepad_t &) = delete;
    vhf_gamepad_t &operator=(const vhf_gamepad_t &) = delete;

    /**
     * @brief Checks once whether the driver is installed and speaks a compatible protocol.
     * @return `true` when virtual controllers can be created.
     */
    bool probe();

    /**
     * @brief Reports the result of the last `probe()`.
     * @return `true` when the driver was reachable.
     */
    [[nodiscard]] bool available() const;

    /**
     * @brief Creates a virtual controller in the driver.
     * @param id The gamepad ID.
     * @param feedback_queue The queue for posting messages back to the client.
     * @return 0 on success.
     */
    int alloc(const gamepad_id_t &id, feedback_queue_t &feedback_queue);

    /**
     * @brief Destroys the virtual controller at the given global index.
     * @param nr The gamepad index.
     */
    void free(int nr);

    /**
     * @brief Submits new controller state to the driver.
     * @param nr The gamepad index.
     * @param gamepad_state The gamepad button/axis state sent from the client.
     */
    void update(int nr, const gamepad_state_t &gamepad_state);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };

}  // namespace platf
