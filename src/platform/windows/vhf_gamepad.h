/**
 * @file src/platform/windows/vhf_gamepad.h
 * @brief Declarations for the Vibepollo VHF virtual gamepad input backend.
 */
#pragma once

// standard includes
#include <functional>
#include <memory>

// local includes
#include "src/platform/common.h"
#include "vhf_gamepad_policy.h"

namespace platf {

  /**
   * @brief Drives virtual controllers through Vibepollo's own UMDF/VHF gamepad driver.
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

    // Keyboard/mouse shares the controller connection and its lifetime lock.
    // Initialization returns false before any HID input when unavailable.
    bool open_keyboard_mouse();
    // A failed report may be followed by fallback only after the VHF child has
    // been synchronously removed; the failed event itself is never replayed.
    [[nodiscard]] bool keyboard_mouse_fallback_safe();
    // Keep the recovery state stable until a Win32 fallback event has been sent.
    // The callback must not call back into the VHF transport.
    [[nodiscard]] bool keyboard_mouse_fallback(const std::function<void()> &send);
    void keyboard_mouse_key(std::uint16_t usage, bool pressed);
    [[nodiscard]] bool keyboard_mouse_consumer_supported() const;
    void keyboard_mouse_consumer(std::uint16_t usage, bool pressed);
    void keyboard_mouse_button(std::uint8_t button, bool pressed);
    void keyboard_mouse_relative(int x, int y, int wheel, int pan);
    // False means a healthy VHF device cannot represent this coordinate.
    // A faulted device consumes the event until its removal is confirmed.
    bool keyboard_mouse_absolute(std::uint16_t x, std::uint16_t y);

    /**
     * @brief Creates a virtual controller in the driver.
     * @param id The gamepad ID.
     * @param feedback_queue The queue for posting messages back to the client.
     * @param desired The profile the configuration or the client's controller type asks for.
     * @param client_capabilities The client's `LI_CCAP_*` flags for this controller; the Steam
     *        Controller profile uses them to tell a two-touchpad client from one that sends both
     *        pads as halves of a single touchpad.
     * @return 0 on success.
     */
    int alloc(const gamepad_id_t &id, feedback_queue_t &feedback_queue, vhf_profile_e desired, std::uint16_t client_capabilities = 0);

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

    /**
     * @brief Hands the real Steam Controller's firmware attributes to the driver.
     * @details Ignored unless the slot runs the Steam Controller profile for a client that
     *          advertised LI_CCAP_STEAM_ATTRIBUTES. A driver that predates the call is logged
     *          once and the virtual controller keeps its default attributes.
     * @param nr The gamepad index.
     * @param attributes_event The attributes.
     */
    void attributes(int nr, const steam_controller_attributes_t &attributes_event);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };

}  // namespace platf
