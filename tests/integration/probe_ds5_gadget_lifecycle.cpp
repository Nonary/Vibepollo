// Manual only: requires the DS5 gadget driver and a free slot 3.
// Run under timeout: a blocked destructor must fail, not hang the test runner.
#include <inputtino/input.hpp>
#include "third-party/libvirtualgamepad/linux/vibeshine-ds5/vibeshine_ds5_uapi.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/hidraw.h>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

static void verify_reports(inputtino::PS5Joypad &pad) {
  using namespace std::chrono_literals;
  int raw = -1;
  for (int attempt = 0; attempt < 100 && raw < 0; ++attempt) {
    for (const auto &entry : std::filesystem::directory_iterator("/sys/class/hidraw")) {
      std::ifstream info(entry.path() / "device/uevent");
      std::string line;
      while (std::getline(info, line)) {
        if (line == "HID_UNIQ=" + pad.get_mac_address())
          raw = open((std::filesystem::path("/dev") / entry.path().filename()).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      }
    }
    if (raw < 0) std::this_thread::sleep_for(20ms);
  }
  require(raw >= 0, "gadget HID identity missing or byte-reversed");
  unsigned char firmware[64] {0x20};
  require(ioctl(raw, HIDIOCGFEATURE(sizeof(firmware)), firmware) == sizeof(firmware),
          "gadget firmware feature read failed");
  require((firmware[44] | (firmware[45] << 8)) >= 0x0390,
          "gadget firmware requests an update in libScePad");
  int reports = 0;
  bool pressed = false, released = false;
  pad.set_pressed_buttons(inputtino::Joypad::A);
  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline && !(released && reports >= 20)) {
    pollfd pending{raw, POLLIN, 0};
    if (poll(&pending, 1, 100) <= 0) continue;
    unsigned char report[64]{};
    if (read(raw, report, sizeof(report)) != 64 || report[0] != 1) continue;
    ++reports;
    if (!pressed && (report[8] & 0x20)) {
      pressed = true;
      pad.set_pressed_buttons(0);
    } else if (pressed && !(report[8] & 0x20)) {
      released = true;
    }
  }
  close(raw);
  if (!(pressed && released && reports >= 20))
    std::fprintf(stderr, "reports=%d pressed=%d released=%d\n", reports, pressed, released);
  require(pressed && released && reports >= 20,
          "gadget must continuously deliver input and Cross press/release");
}

int main() {
  using namespace std::chrono_literals;
  const int fd = open("/dev/vibeshine-ds5", O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) { std::perror("open gadget"); return 1; }
  try {
    vibeshine_ds5_create request{};
    request.slot = 3;
    request.mac[0] = 2;
    request.mac[5] = 3;
    require(ioctl(fd, VIBESHINE_DS5_CREATE, &request) == 0, "slot 3 must be free before test");
    require(ioctl(fd, VIBESHINE_DS5_DESTROY) == 0, "preflight destroy failed");
    for (int iteration = 0; iteration < 20; ++iteration) {
      auto started = std::chrono::steady_clock::now();
      {
        char identity[18];
        std::snprintf(identity, sizeof(identity), "02:76:05:00:%02x:03", iteration);
        auto pad = inputtino::PS5Joypad::create({
          .name = "Vibeshine DS5 lifecycle probe", .vendor_id = 0x054c,
          .product_id = 0x0ce6, .version = 0x8111,
          .device_uniq = identity});
        require(static_cast<bool>(pad), "controller creation failed");
        // Prove the real gadget was acquired, rather than the UHID fallback.
        require(ioctl(fd, VIBESHINE_DS5_CREATE, &request) < 0 && errno == EBUSY,
                "controller did not acquire gadget slot");
        // Cover immediate teardown as well as an idle event reader.
        if (iteration % 2) verify_reports(*pad);
        started = std::chrono::steady_clock::now();
      }
      require(std::chrono::steady_clock::now() - started < 2s, "controller teardown exceeded deadline");
      require(ioctl(fd, VIBESHINE_DS5_CREATE, &request) == 0, "controller leaked gadget slot");
      require(ioctl(fd, VIBESHINE_DS5_DESTROY) == 0, "slot cleanup failed");
    }
    std::puts("PASS: 20 real DS5 gadget lifecycles, continuous HID reports, Cross press/release, identity and slot reuse");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    close(fd);
    return 1;
  }
  close(fd);
}
