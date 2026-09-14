/**
 * @file src/platform/linux/input/inputtino_ds4.cpp
 * @brief DualShock 4 virtual controller implemented over Linux UHID.
 */

#include "inputtino_ds4.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <inputtino/ds4_usb.hpp>
#include <linux/input.h>
#include <linux/uhid.h>
#include <mutex>
#include <optional>
#include <poll.h>
#include <random>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace std::chrono_literals;

namespace platf::gamepad {
  namespace {

    bool write_uhid_event(int fd, const uhid_event &event) {
      ssize_t written;
      do {
        written = write(fd, &event, sizeof(event));
      } while (written < 0 && errno == EINTR);
      return written == static_cast<ssize_t>(sizeof(event));
    }

    class uhid_device_t {
    public:
      using event_callback_t = std::function<void(const uhid_event &, int)>;

      static inputtino::Result<std::unique_ptr<uhid_device_t>> create(
        const inputtino::DeviceDefinition &definition,
        const std::vector<unsigned char> &report_descriptor,
        const event_callback_t &callback
      ) {
        const auto fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
          return inputtino::Error(std::strerror(errno));
        }

        uhid_event event {};
        event.type = UHID_CREATE2;
        event.u.create2.bus = BUS_USB;
        event.u.create2.vendor = definition.vendor_id;
        event.u.create2.product = definition.product_id;
        event.u.create2.version = definition.version;
        event.u.create2.country = 0;
        event.u.create2.rd_size = static_cast<std::uint16_t>(report_descriptor.size());
        std::ranges::copy(report_descriptor, event.u.create2.rd_data);

        const auto copy_text = [](std::string_view text, unsigned char *target, std::size_t capacity) {
          const auto length = std::min(text.size(), capacity - 1);
          std::memcpy(target, text.data(), length);
          target[length] = '\0';
        };
        copy_text(definition.name, event.u.create2.name, sizeof(event.u.create2.name));
        copy_text(definition.device_phys, event.u.create2.phys, sizeof(event.u.create2.phys));
        copy_text(definition.device_uniq, event.u.create2.uniq, sizeof(event.u.create2.uniq));

        if (!write_uhid_event(fd, event)) {
          const std::string error = std::strerror(errno);
          close(fd);
          return inputtino::Error(error);
        }

        auto device = std::unique_ptr<uhid_device_t>(new uhid_device_t(fd, callback));
        device->thread_ = std::thread([raw = device.get()] {
          raw->event_loop();
        });
        return device;
      }

      uhid_device_t(const uhid_device_t &) = delete;
      uhid_device_t &operator=(const uhid_device_t &) = delete;

      ~uhid_device_t() {
        stop();
        if (fd_ >= 0) {
          uhid_event event {};
          event.type = UHID_DESTROY;
          write_uhid_event(fd_, event);
          close(fd_);
        }
      }

      bool send(const uhid_event &event) const {
        return write_uhid_event(fd_, event);
      }

      void stop() {
        stop_ = true;
        if (thread_.joinable()) {
          thread_.join();
        }
      }

    private:
      uhid_device_t(int fd, event_callback_t callback):
          fd_(fd),
          callback_(std::move(callback)) {
      }

      void event_loop() {
        pollfd descriptor {.fd = fd_, .events = POLLIN};
        while (!stop_.load(std::memory_order_relaxed)) {
          const auto result = poll(&descriptor, 1, 250);
          if (result < 0) {
            if (errno == EINTR) {
              continue;
            }
            break;
          }
          if (result == 0) {
            continue;
          }
          if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
          }
          if (descriptor.revents & POLLIN) {
            uhid_event event {};
            ssize_t bytes_read;
            do {
              bytes_read = read(fd_, &event, sizeof(event));
            } while (bytes_read < 0 && errno == EINTR);
            if (bytes_read != static_cast<ssize_t>(sizeof(event))) {
              break;
            }
            callback_(event, fd_);
          }
        }
      }

      int fd_ {-1};
      event_callback_t callback_;
      std::atomic_bool stop_ {false};
      std::thread thread_;
    };

    // USB DualShock 4 report descriptor. The Linux hid-playstation driver uses the
    // feature reports declared here for calibration, pairing, and firmware data.
    constexpr auto &ds4_usb_report_descriptor = inputtino::ds4_usb::report_descriptor;

    constexpr std::uint8_t ds4_report_input = 0x01;
    constexpr std::uint8_t ds4_report_output = 0x05;
    constexpr std::uint8_t ds4_output_valid_motor = 0x01;
    constexpr std::uint8_t ds4_output_valid_led = 0x02;
    constexpr float standard_gravity = 9.80665f;

#pragma pack(push, 1)

    struct ds4_touch_point_t {
      std::uint8_t contact;
      std::uint8_t x_low;
      std::uint8_t x_high_y_low;
      std::uint8_t y_high;
    };

    struct ds4_touch_report_t {
      std::uint8_t timestamp;
      std::array<ds4_touch_point_t, 2> points;
    };

    struct ds4_input_common_t {
      std::uint8_t x;
      std::uint8_t y;
      std::uint8_t rx;
      std::uint8_t ry;
      std::array<std::uint8_t, 3> buttons;
      std::uint8_t z;
      std::uint8_t rz;
      std::uint16_t sensor_timestamp;
      std::uint8_t sensor_temperature;
      std::array<std::uint16_t, 3> gyro;
      std::array<std::uint16_t, 3> accel;
      std::array<std::uint8_t, 5> reserved;
      std::array<std::uint8_t, 2> status;
      std::uint8_t reserved2;
    };

    struct ds4_input_report_t {
      std::uint8_t report_id;
      ds4_input_common_t common;
      std::uint8_t touch_report_count;
      std::array<ds4_touch_report_t, 3> touch_reports;
      std::array<std::uint8_t, 3> reserved;
    };

    struct ds4_output_common_t {
      std::uint8_t valid_flags;
      std::uint8_t valid_flags2;
      std::uint8_t reserved;
      std::uint8_t motor_right;
      std::uint8_t motor_left;
      std::uint8_t lightbar_red;
      std::uint8_t lightbar_green;
      std::uint8_t lightbar_blue;
      std::uint8_t lightbar_blink_on;
      std::uint8_t lightbar_blink_off;
    };

    struct ds4_output_report_t {
      std::uint8_t report_id;
      ds4_output_common_t common;
      std::array<std::uint8_t, 21> reserved;
    };

#pragma pack(pop)

    static_assert(sizeof(ds4_touch_point_t) == 4);
    static_assert(sizeof(ds4_touch_report_t) == 9);
    static_assert(sizeof(ds4_input_common_t) == 32);
    static_assert(sizeof(ds4_input_report_t) == 64);
    static_assert(sizeof(ds4_output_report_t) == 32);

    std::uint8_t scale_axis(int value, int input_min, int input_max) {
      const auto normalized = static_cast<double>(value - input_min) / (input_max - input_min);
      return static_cast<std::uint8_t>(std::clamp(std::lround(normalized * 255.0), 0l, 255l));
    }

    std::uint16_t signed_to_little_endian(int value) {
      const auto clamped = static_cast<std::int16_t>(std::clamp(value, -32768, 32767));
      return htole16(static_cast<std::uint16_t>(clamped));
    }

    std::array<std::uint8_t, 6> random_mac() {
      std::array<std::uint8_t, 6> result {};
      std::random_device random;
      for (auto &byte : result) {
        byte = static_cast<std::uint8_t>(random());
      }
      result[0] = static_cast<std::uint8_t>((result[0] | 0x02) & 0xfe);
      return result;
    }

    std::string mac_to_string(const std::array<std::uint8_t, 6> &mac) {
      std::ostringstream stream;
      stream << std::hex << std::setfill('0');
      for (std::size_t index = 0; index < mac.size(); ++index) {
        if (index) {
          stream << ':';
        }
        stream << std::setw(2) << static_cast<unsigned int>(mac[index]);
      }
      return stream.str();
    }

    std::array<std::uint8_t, 6> mac_from_string(const std::string &text) {
      if (text.empty()) {
        return random_mac();
      }

      std::array<std::uint8_t, 6> result {};
      std::istringstream stream(text);
      for (std::size_t index = 0; index < result.size(); ++index) {
        unsigned int value = 0;
        if (!(stream >> std::hex >> value) || value > 0xff) {
          return random_mac();
        }
        result[index] = static_cast<std::uint8_t>(value);
        if (index + 1 < result.size() && stream.get() != ':') {
          return random_mac();
        }
      }
      return result;
    }

  }  // namespace

  struct ds4_joypad_t::state_t {
    std::unique_ptr<uhid_device_t> device;
    std::array<std::uint8_t, 6> mac {};
    std::string uniq;
    inputtino::ds4_usb::feature_state features;
    ds4_input_report_t report {};
    std::uint8_t last_touch_id {};
    std::uint8_t touch_timestamp {};
    std::mutex report_mutex;
    std::mutex feedback_mutex;
    std::atomic_bool stop_repeating {false};
    std::thread repeat_thread;
    std::optional<std::function<void(int, int)>> on_rumble;
    std::optional<std::function<void(int, int, int)>> on_led;
  };

  namespace {

    void send_report(const std::shared_ptr<ds4_joypad_t::state_t> &state) {
      std::lock_guard lock(state->report_mutex);
      if (!state->device) {
        return;
      }

      state->report.common.sensor_timestamp = htole16(
        static_cast<std::uint16_t>(le16toh(state->report.common.sensor_timestamp) + 750)
      );
      state->report.touch_reports[0].timestamp = ++state->touch_timestamp;

      uhid_event event {};
      event.type = UHID_INPUT2;
      event.u.input2.size = sizeof(state->report);
      std::memcpy(event.u.input2.data, &state->report, sizeof(state->report));
      state->device->send(event);
    }

    void process_output_report(const std::shared_ptr<ds4_joypad_t::state_t> &state, const std::uint8_t *data, std::size_t size) {
      if (size < sizeof(ds4_output_report_t) || data[0] != ds4_report_output) {
        return;
      }

      ds4_output_report_t output {};
      std::memcpy(&output, data, sizeof(output));

      std::optional<std::function<void(int, int)>> on_rumble;
      std::optional<std::function<void(int, int, int)>> on_led;
      {
        std::lock_guard lock(state->feedback_mutex);
        on_rumble = state->on_rumble;
        on_led = state->on_led;
      }

      if ((output.common.valid_flags & ds4_output_valid_motor) && on_rumble) {
        const auto low = static_cast<int>(std::lround(output.common.motor_left / 255.0 * 65535.0));
        const auto high = static_cast<int>(std::lround(output.common.motor_right / 255.0 * 65535.0));
        (*on_rumble)(low, high);
      }
      if ((output.common.valid_flags & ds4_output_valid_led) && on_led) {
        (*on_led)(output.common.lightbar_red, output.common.lightbar_green, output.common.lightbar_blue);
      }
    }

    void reply_to_feature_request(const std::shared_ptr<ds4_joypad_t::state_t> &state, const uhid_event &request, int fd) {
      uhid_event reply {};
      reply.type = UHID_GET_REPORT_REPLY;
      reply.u.get_report_reply.id = request.u.get_report.id;

      std::reverse_copy(state->mac.begin(), state->mac.end(), state->features.address.begin());
      if (request.u.get_report.rtype == UHID_FEATURE_REPORT) {
        reply.u.get_report_reply.size = inputtino::ds4_usb::get_feature(
          request.u.get_report.rnum, reply.u.get_report_reply.data,
          sizeof(reply.u.get_report_reply.data), state->features);
      }
      if (!reply.u.get_report_reply.size) {
        reply.u.get_report_reply.err = EINVAL;
      }

      write_uhid_event(fd, reply);
    }

    void handle_uhid_event(const std::shared_ptr<ds4_joypad_t::state_t> &state, const uhid_event &event, int fd) {
      switch (event.type) {
        case UHID_GET_REPORT:
          reply_to_feature_request(state, event, fd);
          break;
        case UHID_OUTPUT:
          process_output_report(state, event.u.output.data, event.u.output.size);
          break;
        case UHID_SET_REPORT:
          {
            uhid_event reply {};
            reply.type = UHID_SET_REPORT_REPLY;
            reply.u.set_report_reply.id = event.u.set_report.id;
            if (event.u.set_report.rtype == UHID_FEATURE_REPORT) {
              if (!inputtino::ds4_usb::set_feature(event.u.set_report.rnum, event.u.set_report.data,
                                                event.u.set_report.size, state->features)) {
                reply.u.set_report_reply.err = EINVAL;
              }
            } else if (event.u.set_report.rtype == UHID_OUTPUT_REPORT &&
                       event.u.set_report.rnum == ds4_report_output &&
                       event.u.set_report.size >= sizeof(ds4_output_report_t)) {
              process_output_report(state, event.u.set_report.data, event.u.set_report.size);
            } else {
              reply.u.set_report_reply.err = EINVAL;
            }
            write_uhid_event(fd, reply);
          }
          break;
        default:
          break;
      }
    }

  }  // namespace

  inputtino::Result<ds4_joypad_t> ds4_joypad_t::create(const inputtino::DeviceDefinition &definition) {
    auto state = std::make_shared<state_t>();
    state->mac = mac_from_string(definition.device_uniq);
    state->uniq = mac_to_string(state->mac);

    state->report.report_id = ds4_report_input;
    state->report.common.x = 128;
    state->report.common.y = 128;
    state->report.common.rx = 128;
    state->report.common.ry = 128;
    state->report.common.buttons[0] = 8;
    state->report.common.sensor_temperature = 25;
    state->report.common.status[0] = 0x1b;  // Full battery with USB cable connected.
    state->report.touch_report_count = 1;
    for (auto &touch_report : state->report.touch_reports) {
      for (auto &point : touch_report.points) {
        point.contact = 0x80;
      }
    }

    const auto *descriptor_begin = reinterpret_cast<const unsigned char *>(ds4_usb_report_descriptor);
    inputtino::DeviceDefinition uhid_definition {
      .name = definition.name,
      .vendor_id = definition.vendor_id,
      .product_id = definition.product_id,
      .version = definition.version,
      .device_phys = definition.device_phys.empty() ? "VIBEPOLLO_UHID_DS4" : definition.device_phys,
      .device_uniq = state->uniq,
    };
    const std::vector<unsigned char> report_descriptor {
      descriptor_begin,
      descriptor_begin + sizeof(ds4_usb_report_descriptor) - 1,
    };

    auto device = uhid_device_t::create(
      uhid_definition,
      report_descriptor,
      [state](const uhid_event &event, int fd) {
        handle_uhid_event(state, event, fd);
      }
    );
    if (!device) {
      return inputtino::Error(device.getErrorMessage());
    }

    state->device = std::move(*device);
    state->repeat_thread = std::thread([state] {
      while (!state->stop_repeating.load(std::memory_order_relaxed)) {
        send_report(state);
        std::this_thread::sleep_for(4ms);
      }
    });

    return ds4_joypad_t {std::move(state)};
  }

  ds4_joypad_t::ds4_joypad_t(std::shared_ptr<state_t> state):
      state_(std::move(state)) {
  }

  ds4_joypad_t::ds4_joypad_t(ds4_joypad_t &&other) noexcept:
      state_(std::exchange(other.state_, nullptr)) {
  }

  ds4_joypad_t &ds4_joypad_t::operator=(ds4_joypad_t &&other) noexcept {
    if (this != &other) {
      ds4_joypad_t previous {std::move(*this)};
      state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
  }

  ds4_joypad_t::~ds4_joypad_t() {
    if (!state_) {
      return;
    }
    state_->stop_repeating = true;
    if (state_->repeat_thread.joinable()) {
      state_->repeat_thread.join();
    }
    if (state_->device) {
      state_->device->stop();
      state_->device.reset();
    }
  }

  std::vector<std::string> ds4_joypad_t::get_nodes() const {
    std::vector<std::string> nodes;
    if (!state_) {
      return nodes;
    }

    const std::filesystem::path base_path {"/sys/devices/virtual/misc/uhid"};
    std::error_code error;
    if (!std::filesystem::exists(base_path, error)) {
      return nodes;
    }

    for (const auto &uhid_entry : std::filesystem::directory_iterator(base_path, error)) {
      if (error || !uhid_entry.is_directory()) {
        continue;
      }
      const auto input_path = uhid_entry.path() / "input";
      if (!std::filesystem::exists(input_path, error)) {
        continue;
      }
      for (const auto &input_entry : std::filesystem::directory_iterator(input_path, error)) {
        if (error || !input_entry.is_directory()) {
          continue;
        }
        std::ifstream uniq_file(input_entry.path() / "uniq");
        std::string uniq;
        std::getline(uniq_file, uniq);
        if (uniq != state_->uniq) {
          continue;
        }
        for (const auto &node : std::filesystem::directory_iterator(input_entry.path(), error)) {
          const auto filename = node.path().filename().string();
          if (filename.starts_with("event") || filename.starts_with("js")) {
            nodes.emplace_back((std::filesystem::path {"/dev/input"} / filename).string());
          }
        }
      }
    }
    return nodes;
  }

  void ds4_joypad_t::set_pressed_buttons(unsigned int pressed) {
    std::lock_guard lock(state_->report_mutex);
    auto &buttons = state_->report.common.buttons;
    const auto trigger_buttons = static_cast<std::uint8_t>(buttons[1] & 0x0c);
    buttons = {8, trigger_buttons, 0};

    if (pressed & DPAD_UP) {
      buttons[0] = pressed & DPAD_LEFT ? 7 : pressed & DPAD_RIGHT ? 1 :
                                                                    0;
    } else if (pressed & DPAD_DOWN) {
      buttons[0] = pressed & DPAD_LEFT ? 5 : pressed & DPAD_RIGHT ? 3 :
                                                                    4;
    } else if (pressed & DPAD_LEFT) {
      buttons[0] = 6;
    } else if (pressed & DPAD_RIGHT) {
      buttons[0] = 2;
    }

    if (pressed & X) {
      buttons[0] |= 0x10;
    }
    if (pressed & A) {
      buttons[0] |= 0x20;
    }
    if (pressed & B) {
      buttons[0] |= 0x40;
    }
    if (pressed & Y) {
      buttons[0] |= 0x80;
    }
    if (pressed & LEFT_BUTTON) {
      buttons[1] |= 0x01;
    }
    if (pressed & RIGHT_BUTTON) {
      buttons[1] |= 0x02;
    }
    if (pressed & BACK) {
      buttons[1] |= 0x10;
    }
    if (pressed & START) {
      buttons[1] |= 0x20;
    }
    if (pressed & LEFT_STICK) {
      buttons[1] |= 0x40;
    }
    if (pressed & RIGHT_STICK) {
      buttons[1] |= 0x80;
    }
    if (pressed & HOME) {
      buttons[2] |= 0x01;
    }
    if (pressed & TOUCHPAD_FLAG) {
      buttons[2] |= 0x02;
    }
  }

  void ds4_joypad_t::set_triggers(std::int16_t left, std::int16_t right) {
    std::lock_guard lock(state_->report_mutex);
    auto &report = state_->report.common;
    report.z = scale_axis(left, 0, 255);
    report.rz = scale_axis(right, 0, 255);
    if (left > 0) {
      report.buttons[1] |= 0x04;
    } else {
      report.buttons[1] &= ~0x04;
    }
    if (right > 0) {
      report.buttons[1] |= 0x08;
    } else {
      report.buttons[1] &= ~0x08;
    }
  }

  void ds4_joypad_t::set_stick(STICK_POSITION stick_type, short x, short y) {
    std::lock_guard lock(state_->report_mutex);
    if (stick_type == LS) {
      state_->report.common.x = scale_axis(x, -32768, 32767);
      state_->report.common.y = scale_axis(-static_cast<int>(y), -32768, 32767);
    } else {
      state_->report.common.rx = scale_axis(x, -32768, 32767);
      state_->report.common.ry = scale_axis(-static_cast<int>(y), -32768, 32767);
    }
  }

  void ds4_joypad_t::set_motion(motion_type_e type, float x, float y, float z) {
    std::lock_guard lock(state_->report_mutex);
    auto &target = type == motion_type_e::acceleration ? state_->report.common.accel : state_->report.common.gyro;
    const std::array<float, 3> values {x, y, z};

    for (std::size_t index = 0; index < values.size(); ++index) {
      const auto value = std::isfinite(values[index]) ? values[index] : 0.0f;
      int raw = 0;
      if (type == motion_type_e::acceleration) {
        raw = static_cast<int>(std::lround(std::clamp(value / standard_gravity * 8192.0f, -32768.0f, 32767.0f)));
      } else {
        raw = static_cast<int>(std::lround(std::clamp(value * 16.0f, -32768.0f, 32767.0f)));
      }
      target[index] = signed_to_little_endian(raw);
    }
  }

  void ds4_joypad_t::set_battery(battery_state_e state, int percentage) {
    std::lock_guard lock(state_->report_mutex);
    const auto level = state == battery_state_e::full ? 11 : std::clamp(percentage / 10, 0, 10);
    const auto cable_connected = state == battery_state_e::discharging ? 0 : 0x10;
    state_->report.common.status[0] = static_cast<std::uint8_t>(cable_connected | level);
  }

  void ds4_joypad_t::place_finger(int finger_nr, std::uint16_t x, std::uint16_t y) {
    if (finger_nr < 0 || finger_nr > 1) {
      return;
    }
    std::lock_guard lock(state_->report_mutex);
    auto &point = state_->report.touch_reports[0].points[finger_nr];
    if (point.contact & 0x80) {
      state_->last_touch_id = static_cast<std::uint8_t>((state_->last_touch_id + 1) & 0x7f);
    }
    point.contact = state_->last_touch_id;
    point.x_low = static_cast<std::uint8_t>(x & 0xff);
    point.x_high_y_low = static_cast<std::uint8_t>(((x >> 8) & 0x0f) | ((y & 0x0f) << 4));
    point.y_high = static_cast<std::uint8_t>((y >> 4) & 0xff);
  }

  void ds4_joypad_t::release_finger(int finger_nr) {
    if (finger_nr < 0 || finger_nr > 1) {
      return;
    }
    std::lock_guard lock(state_->report_mutex);
    state_->report.touch_reports[0].points[finger_nr].contact |= 0x80;
  }

  void ds4_joypad_t::set_on_rumble(const std::function<void(int, int)> &callback) {
    std::lock_guard lock(state_->feedback_mutex);
    state_->on_rumble = callback;
  }

  void ds4_joypad_t::set_on_led(const std::function<void(int, int, int)> &callback) {
    std::lock_guard lock(state_->feedback_mutex);
    state_->on_led = callback;
  }

}  // namespace platf::gamepad
