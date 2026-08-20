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
   * @brief The controller the VHF driver should present.
   */
  enum class vhf_profile_e {
    automatic,  ///< Best available for the client, preferring a PlayStation pad when it fits.
    xbox_series,
    xbox_one,
    dualshock4,
    dualsense,
    switch_pro
  };

  /**
   * @brief Drives virtual controllers through Vibeshine's own UMDF/VHF gamepad driver.
   * @details This is the alternative to the ViGEmBus backend. Which controller a slot presents
   *          depends on the profile chosen at allocation; the PlayStation profiles additionally
   *          carry a touchpad, motion sensors, a battery, and a lightbar.
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
    int alloc(const gamepad_id_t &id, feedback_queue_t &feedback_queue, vhf_profile_e desired);

    /**
     * @brief Reports whether a slot's controller has a touchpad, motion sensors, and a battery.
     * @param nr The gamepad index.
     * @return `true` for the PlayStation profiles.
     */
    [[nodiscard]] bool slot_has_sensors(int nr) const;

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

    /**
     * @brief Forwards a touchpad event. Ignored unless the slot has a touchpad.
     * @param nr The gamepad index.
     * @param touch_event The touch event.
     */
    void touch(int nr, const gamepad_touch_t &touch_event);

    /**
     * @brief Forwards a motion sample. Ignored unless the slot has sensors.
     * @param nr The gamepad index.
     * @param motion_event The motion event.
     */
    void motion(int nr, const gamepad_motion_t &motion_event);

    /**
     * @brief Forwards a battery update. Ignored unless the slot has a battery.
     * @param nr The gamepad index.
     * @param battery_event The battery event.
     */
    void battery(int nr, const gamepad_battery_t &battery_event);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };

}  // namespace platf
