/**
 * @file src/platform/linux/input/inputtino_ds4.h
 * @brief DualShock 4 virtual controller implemented over Linux UHID.
 */
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <inputtino/input.hpp>
#include <inputtino/result.hpp>
#include <memory>
#include <string>
#include <vector>

namespace platf::gamepad {

  class ds4_joypad_t final: public inputtino::Joypad {
  public:
    struct state_t;

    enum class motion_type_e : std::uint8_t {
      acceleration,
      gyroscope,
    };

    enum class battery_state_e : std::uint8_t {
      charging,
      discharging,
      full,
    };

    static inputtino::Result<ds4_joypad_t> create(
      const inputtino::DeviceDefinition &device = {
        .name = "Sony Computer Entertainment Wireless Controller",
        .vendor_id = 0x054C,
        .product_id = 0x05C4,
        .version = 0x0100,
      }
    );

    ds4_joypad_t(ds4_joypad_t &&other) noexcept;
    ds4_joypad_t &operator=(ds4_joypad_t &&other) noexcept;
    ds4_joypad_t(const ds4_joypad_t &) = delete;
    ds4_joypad_t &operator=(const ds4_joypad_t &) = delete;
    ~ds4_joypad_t() override;

    std::vector<std::string> get_nodes() const override;

    void set_pressed_buttons(unsigned int pressed) override;
    void set_triggers(std::int16_t left, std::int16_t right) override;
    void set_stick(STICK_POSITION stick_type, short x, short y) override;

    void set_motion(motion_type_e type, float x, float y, float z);
    void set_battery(battery_state_e state, int percentage);
    void place_finger(int finger_nr, std::uint16_t x, std::uint16_t y);
    void release_finger(int finger_nr);

    void set_on_rumble(const std::function<void(int low_frequency, int high_frequency)> &callback);
    void set_on_led(const std::function<void(int red, int green, int blue)> &callback);

    static constexpr int touchpad_width = 1920;
    static constexpr int touchpad_height = 942;

  private:
    explicit ds4_joypad_t(std::shared_ptr<state_t> state);

    std::shared_ptr<state_t> state_;
  };

}  // namespace platf::gamepad
