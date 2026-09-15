// Manual live-device test. Creates only its own temporary UHID controllers.
#include "src/platform/linux/input/inputtino_ds4.h"
#include <inputtino/ds4_usb.hpp>
#include <inputtino/ds5_usb.hpp>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/ioctl.h>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}

struct fd_t {
  int value = -1;
  ~fd_t() { if (value >= 0) close(value); }
};

static int find_device(const std::string &identity) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    for (const auto &entry : fs::directory_iterator("/sys/class/hidraw")) {
      std::ifstream file(entry.path() / "device/uevent");
      std::string line;
      while (std::getline(file, line)) {
        if (line == "HID_UNIQ=" + identity) {
          int fd = open((fs::path("/dev") / entry.path().filename()).c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
          if (fd >= 0) return fd;
        }
      }
    }
    std::this_thread::sleep_for(50ms);
  }
  throw std::runtime_error("temporary controller hidraw node unavailable");
}

static void probe(int profile) {
  char address[18];
  std::snprintf(address, sizeof(address), "02:76:%02x:%02x:%02x:%02x", profile,
                (getpid() >> 16) & 255, (getpid() >> 8) & 255, getpid() & 255);
  inputtino::DeviceDefinition definition{
    .name = "Vibepollo PlayStation USB probe",
    .vendor_id = 0x054c,
    .product_id = profile == 4 ? 0x05c4 : 0x0ce6,
    .version = 0x0100,
    .device_uniq = address,
  };
  std::unique_ptr<inputtino::Joypad> pad;
  if (profile == 4) {
    auto result = platf::gamepad::ds4_joypad_t::create(definition);
    require(bool(result), "DS4 creation failed");
    pad = std::make_unique<platf::gamepad::ds4_joypad_t>(std::move(*result));
  } else {
    auto result = inputtino::PS5Joypad::create(definition);
    require(bool(result), "DS5 creation failed");
    pad = std::make_unique<inputtino::PS5Joypad>(std::move(*result));
  }
  fd_t fd{find_device(address)};
  hidraw_devinfo info{};
  require(ioctl(fd.value, HIDIOCGRAWINFO, &info) == 0 && info.bustype == BUS_USB,
          "controller is not USB");
  hidraw_report_descriptor descriptor{};
  require(ioctl(fd.value, HIDIOCGRDESCSIZE, &descriptor.size) == 0, "descriptor size read failed");
  require(ioctl(fd.value, HIDIOCGRDESC, &descriptor) == 0, "descriptor read failed");
  const auto *expected = profile == 4
    ? reinterpret_cast<const unsigned char *>(inputtino::ds4_usb::report_descriptor)
    : inputtino::ds5_usb::report_descriptor;
  const auto expected_size = profile == 4 ? inputtino::ds4_usb::report_descriptor_size : inputtino::ds5_usb::report_descriptor_size;
  require(descriptor.size == expected_size && std::memcmp(descriptor.value, expected, expected_size) == 0,
          "live descriptor differs from validated USB contract");

  std::array<unsigned char, 64> report{};
  const int firmware = profile == 4 ? 0xa3 : 0x20;
  report[0] = firmware;
  require(ioctl(fd.value, HIDIOCGFEATURE(report.size()), report.data()) == (profile == 4 ? 49 : 64),
          "firmware read failed");
  if (profile == 4) require((report[35] | report[36] << 8) >= 0x3100, "DS4 native firmware gate failed");
  else require((report[28] | report[29] << 8 | report[30] << 16 | report[31] << 24) >= 0x1003e,
               "DS5 native firmware gate failed");
  report.fill(0); report[0] = profile == 4 ? 0x12 : 0x09;
  require(ioctl(fd.value, HIDIOCGFEATURE(report.size()), report.data()) == (profile == 4 ? 16 : 20),
          "pairing read failed");
  require(report[6] == 2 && report[5] == 0x76 && report[4] == profile, "pairing address mismatch");
  report.fill(0); report[0] = profile == 4 ? 2 : 5;
  require(ioctl(fd.value, HIDIOCGFEATURE(report.size()), report.data()) == (profile == 4 ? 37 : 41),
          "calibration read failed");
  report.fill(0); report[0] = profile == 4 ? 0x14 : 8; report[1] = 2;
  int command_size = profile == 4 ? 17 : 48;
  require(ioctl(fd.value, HIDIOCSFEATURE(command_size), report.data()) == command_size,
          "native sensor initialization failed");
  report[1] = 0xff;
  require(ioctl(fd.value, HIDIOCSFEATURE(command_size), report.data()) < 0, "unknown sensor command accepted");
  report[0] = 0x7c;
  require(ioctl(fd.value, HIDIOCGFEATURE(report.size()), report.data()) < 0, "unknown feature accepted");

  const auto pressed = inputtino::Joypad::A |
    (profile == 4 ? inputtino::Joypad::HOME | inputtino::Joypad::TOUCHPAD_FLAG : 0);
  pad->set_pressed_buttons(pressed);
  if (profile == 4) {
    auto &ds4 = static_cast<platf::gamepad::ds4_joypad_t &>(*pad);
    ds4.set_motion(platf::gamepad::ds4_joypad_t::motion_type_e::gyroscope, 1, 0, 0);
    ds4.set_motion(platf::gamepad::ds4_joypad_t::motion_type_e::acceleration, 9.80665f, 0, 0);
  } else {
    auto &ds5 = static_cast<inputtino::PS5Joypad &>(*pad);
    ds5.set_motion(inputtino::PS5Joypad::GYROSCOPE, 3.14159265358979323846f / 180, 0, 0);
    ds5.set_motion(inputtino::PS5Joypad::ACCELERATION, 9.80665f, 0, 0);
  }
  bool seen = false;
  int previous_counter = -1;
  int sequenced_reports = 0;
  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd event{fd.value, POLLIN, 0};
    if (poll(&event, 1, 100) <= 0) continue;
    int size = read(fd.value, report.data(), report.size());
    int gyro = profile == 4 ? 13 : 16;
    int accel = profile == 4 ? 19 : 22;
    if (size == 64 && report[0] == 1 && (report[profile == 4 ? 5 : 8] & 0x20) &&
        report[gyro] == 16 && report[gyro + 1] == 0 && report[accel] == 0 && report[accel + 1] == 0x20) {
      seen = true;
      if (profile == 4) {
        require((report[7] & 3) == 3, "DS4 counter overwrote PS/touchpad buttons");
        const int counter = report[7] >> 2;
        if (previous_counter >= 0) {
          require(counter == ((previous_counter + 1) & 63), "DS4 report counter stalled, reset, or skipped");
        }
        previous_counter = counter;
        // Exercise two wraps while button writes race the repeating reports.
        pad->set_pressed_buttons(pressed);
        if (++sequenced_reports < 130) continue;
      }
      break;
    }
  }
  pad->set_pressed_buttons(0);
  require(seen, "USB Cross/motion input report missing or incorrectly scaled");
  require(profile != 4 || sequenced_reports == 130, "insufficient DS4 reports to verify counter wraparound");
  std::printf("DS%d live USB descriptor, firmware, pairing, sensor initialization, rejection, Cross and motion: PASS\n", profile);
}

int main() {
  try { probe(4); probe(5); }
  catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
